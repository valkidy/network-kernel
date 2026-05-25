#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "simulation/public/collision_query.h"

namespace network_example {
namespace {

void push_event(
    std::vector<KernelEvent>* events,
    KernelEventType type,
    std::uint32_t tick,
    NetId net_id,
    PeerId peer_id,
    std::uint32_t code = 0,
    std::uint64_t event_time_us = 0,
    std::uint64_t presentation_time_us = 0) {
    if (events == nullptr) {
        return;
    }
    events->push_back(KernelEvent{
        type,
        tick,
        net_id,
        peer_id,
        code,
        event_time_us,
        presentation_time_us});
}

float distance_to_hitbox(
    const glm::vec3& point,
    const Transform& transform,
    const Hitbox& hitbox) {
    const glm::vec3 center = transform.position + hitbox.center;
    const glm::vec3 min_corner = center - hitbox.half_extents;
    const glm::vec3 max_corner = center + hitbox.half_extents;
    const glm::vec3 closest = glm::clamp(point, min_corner, max_corner);
    return glm::length(point - closest);
}

float distance_to_historical_hitbox(
    const glm::vec3& point,
    const HitVolumeSnapshot& volume) {
    const glm::vec3 min_corner = volume.center - volume.half_extents;
    const glm::vec3 max_corner = volume.center + volume.half_extents;
    const glm::vec3 closest = glm::clamp(point, min_corner, max_corner);
    return glm::length(point - closest);
}

void apply_historical_explosion_damage(
    World& world,
    const HistoryFrame& frame,
    NetId projectile_net_id,
    PeerId owner_peer,
    const ProjectileState& projectile,
    const glm::vec3& explosion_center,
    std::uint32_t current_tick,
    std::uint64_t hit_time_us,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    if (projectile.explosion_radius <= 0.0f || projectile.damage == 0) {
        return;
    }

    std::vector<std::pair<NetId, std::uint16_t>> damage_results;
    for (const HitVolumeSnapshot& volume : frame.volumes) {
        if (volume.net_id == projectile_net_id || volume.alive == 0) {
            continue;
        }

        const float distance = distance_to_historical_hitbox(explosion_center, volume);
        if (distance > projectile.explosion_radius) {
            continue;
        }

        const float falloff = 1.0f - (distance / projectile.explosion_radius);
        const auto damage =
            static_cast<std::uint16_t>(std::max(1.0f, std::round(projectile.damage * falloff)));
        damage_results.push_back({volume.net_id, damage});
    }

    for (const auto& [target_net_id, damage] : damage_results) {
        if (damage_pipeline != nullptr) {
            damage_pipeline->submit_damage_request(DamageRequest{
                current_tick,
                0,
                projectile_net_id,
                target_net_id,
                owner_peer,
                projectile.weapon_id,
                damage,
                hit_time_us,
                explosion_center,
            });
        }
    }
}

void explode_projectile(
    World& world,
    NetId projectile_net_id,
    PeerId owner_peer,
    const ProjectileState& projectile,
    const glm::vec3& explosion_center,
    std::uint32_t current_tick,
    std::uint64_t hit_time_us,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    if (projectile.explosion_radius <= 0.0f || projectile.damage == 0) {
        return;
    }

    push_event(
        events,
        KernelEventType_Explosion,
        current_tick,
        projectile_net_id,
        owner_peer,
        static_cast<std::uint32_t>(projectile.explosion_radius * 100.0f),
        hit_time_us,
        hit_time_us);

    QueryFilter filter;
    filter.ignored_net_id = projectile_net_id;
    filter.ignored_owner_peer = owner_peer;
    filter.collision_mask = projectile.collision_mask;
    const std::vector<QueryHit> hits = collect_sphere_overlaps(
        world,
        explosion_center,
        projectile.explosion_radius,
        filter);
    std::vector<std::pair<NetId, std::uint16_t>> damage_results;
    for (const QueryHit& hit : hits) {
        const float falloff = 1.0f - (hit.distance / projectile.explosion_radius);
        const auto damage =
            static_cast<std::uint16_t>(std::max(1.0f, std::round(projectile.damage * falloff)));
        damage_results.push_back({hit.net_id, damage});
    }

    for (const auto& [target_net_id, damage] : damage_results) {
        if (damage_pipeline != nullptr) {
            damage_pipeline->submit_damage_request(DamageRequest{
                current_tick,
                0,
                projectile_net_id,
                target_net_id,
                owner_peer,
                projectile.weapon_id,
                damage,
                hit_time_us,
                explosion_center,
            });
        }
    }
}

std::uint64_t tick_time_us(std::uint32_t tick, float fixed_delta_seconds) {
    return static_cast<std::uint64_t>(
        static_cast<double>(tick) * static_cast<double>(fixed_delta_seconds) *
        1000000.0);
}

glm::vec3 normalized_or(const glm::vec3& value, const glm::vec3& fallback) {
    if (glm::length(value) <= 0.0001f) {
        return fallback;
    }
    return glm::normalize(value);
}

float degrees_to_radians(float degrees) {
    return degrees * 0.017453292519943295f;
}

glm::vec3 hitbox_center(const Transform& transform, const Hitbox& hitbox) {
    return transform.position + hitbox.center;
}

bool homing_target_is_valid(
    World& world,
    NetId target_net_id,
    PeerId owner_peer,
    std::uint32_t collision_mask,
    const glm::vec3& projectile_position,
    const HomingState& homing,
    glm::vec3* out_target_center) {
    const auto target_entity = world.find_entity(target_net_id);
    if (!target_entity.has_value() ||
        !world.registry().all_of<NetworkIdentity, Transform, Hitbox, Health>(
            *target_entity)) {
        return false;
    }
    const NetworkIdentity& identity =
        world.registry().get<NetworkIdentity>(*target_entity);
    if (owner_peer != 0 && identity.owner_peer == owner_peer) {
        return false;
    }
    const Health& health = world.registry().get<Health>(*target_entity);
    if (health.hp == 0) {
        return false;
    }
    const std::uint32_t layer = entity_collision_layer(world, target_net_id);
    if (layer != 0 && (layer & collision_mask) == 0) {
        return false;
    }
    const Transform& transform = world.registry().get<Transform>(*target_entity);
    const Hitbox& hitbox = world.registry().get<Hitbox>(*target_entity);
    const glm::vec3 center = hitbox_center(transform, hitbox);
    if (glm::length(center - projectile_position) > homing.lose_target_range) {
        return false;
    }
    if (out_target_center != nullptr) {
        *out_target_center = center;
    }
    return true;
}

NetId acquire_homing_target(
    World& world,
    NetId shooter_net_id,
    PeerId owner_peer,
    std::uint32_t collision_mask,
    const glm::vec3& origin,
    const glm::vec3& forward,
    const HomingState& homing) {
    const glm::vec3 aim = normalized_or(forward, glm::vec3{1.0f, 0.0f, 0.0f});
    const float cone_cos =
        std::cos(degrees_to_radians(homing.lock_cone_degrees) * 0.5f);
    NetId best_net_id = 0;
    float best_dot = -std::numeric_limits<float>::max();
    float best_distance = std::numeric_limits<float>::max();

    auto view = world.registry().view<NetworkIdentity, Transform, Hitbox, Health>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        if (identity.net_id == shooter_net_id ||
            (owner_peer != 0 && identity.owner_peer == owner_peer)) {
            continue;
        }
        const Health& health = view.get<Health>(entity);
        if (health.hp == 0) {
            continue;
        }
        const std::uint32_t layer = entity_collision_layer(world, identity.net_id);
        if (layer != 0 && (layer & collision_mask) == 0) {
            continue;
        }
        const glm::vec3 center =
            hitbox_center(view.get<Transform>(entity), view.get<Hitbox>(entity));
        const glm::vec3 to_target = center - origin;
        const float distance = glm::length(to_target);
        if (distance <= 0.0001f || distance > homing.lock_on_range) {
            continue;
        }
        const float dot = glm::dot(aim, to_target / distance);
        if (dot < cone_cos) {
            continue;
        }
        const bool better_dot = dot > best_dot + 0.0001f;
        const bool tied_dot = std::abs(dot - best_dot) <= 0.0001f;
        if (better_dot ||
            (tied_dot && distance < best_distance - 0.0001f) ||
            (tied_dot && std::abs(distance - best_distance) <= 0.0001f &&
             (best_net_id == 0 || identity.net_id < best_net_id))) {
            best_net_id = identity.net_id;
            best_dot = dot;
            best_distance = distance;
        }
    }
    return best_net_id;
}

glm::vec3 steer_direction(
    const glm::vec3& current_direction,
    const glm::vec3& desired_direction,
    float max_turn_radians) {
    const glm::vec3 current =
        normalized_or(current_direction, glm::vec3{1.0f, 0.0f, 0.0f});
    const glm::vec3 desired = normalized_or(desired_direction, current);
    const float dot = glm::clamp(glm::dot(current, desired), -1.0f, 1.0f);
    const float angle = std::acos(dot);
    if (angle <= 0.0001f || angle <= max_turn_radians) {
        return desired;
    }
    const float t = max_turn_radians / angle;
    return normalized_or(current + (desired - current) * t, current);
}

void advance_homing_projectile(
    World& world,
    PeerId owner_peer,
    ProjectileState& projectile,
    Transform& transform,
    Velocity& velocity,
    HomingState& homing,
    float fixed_delta_seconds,
    std::uint32_t current_tick) {
    const float next_age_seconds = projectile.age_seconds + fixed_delta_seconds;
    if (homing.phase == MissileGuidancePhase::kBoost) {
        transform.position = projectile_position_at(
            projectile.spawn_position,
            projectile.initial_velocity,
            ProjectileMotionModel::kLinear,
            glm::vec3{0.0f},
            next_age_seconds);
        velocity.linear = projectile.initial_velocity;
        projectile.age_seconds = next_age_seconds;
        if (current_tick >= homing.guidance_start_tick) {
            const NetId target = acquire_homing_target(
                world,
                projectile.shooter_net_id,
                owner_peer,
                projectile.collision_mask,
                transform.position,
                velocity.linear,
                homing);
            homing.target_net_id = target;
            homing.phase = target == 0 ? MissileGuidancePhase::kLostTarget
                                       : MissileGuidancePhase::kGuided;
        }
        return;
    }

    glm::vec3 target_center{0.0f, 0.0f, 0.0f};
    if (homing.phase == MissileGuidancePhase::kGuided &&
        !homing_target_is_valid(
            world,
            homing.target_net_id,
            owner_peer,
            projectile.collision_mask,
            transform.position,
            homing,
            &target_center)) {
        homing.target_net_id = 0;
        homing.phase = MissileGuidancePhase::kLostTarget;
    }

    if (homing.phase == MissileGuidancePhase::kGuided) {
        const float max_turn =
            degrees_to_radians(homing.max_turn_rate_degrees_per_second) *
            fixed_delta_seconds;
        const glm::vec3 direction = steer_direction(
            velocity.linear,
            target_center - transform.position,
            max_turn);
        const float speed = std::min(
            homing.max_speed,
            glm::length(velocity.linear) + homing.acceleration * fixed_delta_seconds);
        velocity.linear = direction * speed;
    }

    transform.position += velocity.linear * fixed_delta_seconds;
    projectile.age_seconds = next_age_seconds;
}

struct ProjectileHitRecord {
    NetId projectile_net_id = 0;
    NetId target_net_id = 0;
    QueryHit hit{};
    std::uint32_t sequence_id = 0;
};

struct ProjectileHitOutcome {
    bool destroy_projectile = false;
};

struct ProjectileEntityRef {
    NetId net_id = 0;
    entt::entity entity = entt::null;
};

struct ProjectileInteractionMatch {
    NetId lhs_net_id = 0;
    NetId rhs_net_id = 0;
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    const ProjectileInteractionRule* rule = nullptr;
    bool lhs_is_rule_lhs = true;
};

std::vector<ProjectileEntityRef> sorted_projectile_entities(World& world) {
    std::vector<ProjectileEntityRef> projectiles;
    auto view = world.registry().view<NetworkIdentity, ProjectileState, ProjectileTag>();
    for (const entt::entity entity : view) {
        projectiles.push_back(ProjectileEntityRef{
            view.get<NetworkIdentity>(entity).net_id,
            entity,
        });
    }
    std::sort(
        projectiles.begin(),
        projectiles.end(),
        [](const ProjectileEntityRef& lhs, const ProjectileEntityRef& rhs) {
            return lhs.net_id < rhs.net_id;
        });
    return projectiles;
}

bool contains_net_id(const std::vector<NetId>& net_ids, NetId net_id) {
    return std::find(net_ids.begin(), net_ids.end(), net_id) != net_ids.end();
}

void push_unique_net_id(std::vector<NetId>* net_ids, NetId net_id) {
    if (!contains_net_id(*net_ids, net_id)) {
        net_ids->push_back(net_id);
    }
}

std::vector<ProjectileHitRecord> make_projectile_hit_records(
    NetId projectile_net_id,
    const std::vector<QueryHit>& hits) {
    std::vector<ProjectileHitRecord> records;
    records.reserve(hits.size());
    std::uint32_t sequence_id = 0;
    for (const QueryHit& hit : hits) {
        records.push_back(ProjectileHitRecord{
            projectile_net_id,
            hit.net_id,
            hit,
            sequence_id++,
        });
    }
    return records;
}

ProjectileHitOutcome process_projectile_hit_records(
    World& world,
    const NetworkIdentity& identity,
    ProjectileState& projectile,
    const std::vector<ProjectileHitRecord>& records,
    const glm::vec3& fallback_position,
    std::uint32_t current_tick,
    std::uint64_t hit_time_us,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    ProjectileHitOutcome outcome{};
    if (records.empty()) {
        return outcome;
    }

    if (projectile.hit_response == ProjectileHitResponse::kContinue &&
        projectile.damage_shape == ProjectileDamageShape::kPiercingSegment &&
        projectile.damage > 0) {
        for (const ProjectileHitRecord& record : records) {
            if (projectile.hit_count >= projectile.max_hit_count) {
                break;
            }
            damage_pipeline->submit_damage_request(DamageRequest{
                current_tick,
                record.sequence_id,
                identity.net_id,
                record.target_net_id,
                identity.owner_peer,
                projectile.weapon_id,
                projectile.damage,
                hit_time_us,
                record.hit.position,
            });
            ++projectile.hit_count;
        }
        outcome.destroy_projectile = projectile.hit_count >= projectile.max_hit_count;
        return outcome;
    }

    const glm::vec3 impact_position =
        records.empty() ? fallback_position : records.front().hit.position;
    if (projectile.damage_shape == ProjectileDamageShape::kExplosion ||
        projectile.explosion_radius > 0.0f) {
        explode_projectile(
            world,
            identity.net_id,
            identity.owner_peer,
            projectile,
            impact_position,
            current_tick,
            hit_time_us,
            events,
            damage_pipeline);
    } else if (projectile.damage_shape == ProjectileDamageShape::kDirectHit &&
               projectile.damage > 0) {
        damage_pipeline->submit_damage_request(DamageRequest{
            current_tick,
            0,
            identity.net_id,
            records.front().target_net_id,
            identity.owner_peer,
            projectile.weapon_id,
            projectile.damage,
            hit_time_us,
            impact_position,
        });
    }

    outcome.destroy_projectile = true;
    return outcome;
}

std::optional<ProjectileInteractionMatch> match_projectile_interaction_rule(
    const std::vector<ProjectileInteractionRule>& rules,
    NetId lhs_net_id,
    const ProjectileState& lhs_projectile,
    NetId rhs_net_id,
    const ProjectileState& rhs_projectile,
    const glm::vec3& position) {
    for (const ProjectileInteractionRule& rule : rules) {
        if (rule.lhs_weapon_id == lhs_projectile.weapon_id &&
            rule.rhs_weapon_id == rhs_projectile.weapon_id) {
            return ProjectileInteractionMatch{
                lhs_net_id,
                rhs_net_id,
                position,
                &rule,
                true,
            };
        }
        if (rule.symmetric &&
            rule.lhs_weapon_id == rhs_projectile.weapon_id &&
            rule.rhs_weapon_id == lhs_projectile.weapon_id) {
            return ProjectileInteractionMatch{
                lhs_net_id,
                rhs_net_id,
                position,
                &rule,
                false,
            };
        }
    }
    return std::nullopt;
}

bool projectile_can_interact_with_projectiles(const ProjectileState& projectile) {
    return (projectile.collision_mask & kCollisionLayerProjectile) != 0;
}

void collect_projectile_interaction_matches(
    World& world,
    std::vector<ProjectileInteractionMatch>* matches) {
    matches->clear();
    if (world.projectile_interaction_rules().empty()) {
        return;
    }

    const std::vector<ProjectileEntityRef> projectiles =
        sorted_projectile_entities(world);
    for (const ProjectileEntityRef& ref : projectiles) {
        if (!world.registry().valid(ref.entity) ||
            !world.registry().all_of<NetworkIdentity, Transform, ProjectileState>(
                ref.entity)) {
            continue;
        }
        const NetworkIdentity& identity =
            world.registry().get<NetworkIdentity>(ref.entity);
        const Transform& transform = world.registry().get<Transform>(ref.entity);
        const ProjectileState& projectile =
            world.registry().get<ProjectileState>(ref.entity);
        if (!projectile_can_interact_with_projectiles(projectile)) {
            continue;
        }

        QueryFilter filter;
        filter.ignored_net_id = identity.net_id;
        filter.ignored_owner_peer = identity.owner_peer;
        filter.collision_mask = projectile.collision_mask;
        filter.include_projectiles = true;
        const std::vector<QueryHit> hits = collect_segment_hits(
            world,
            projectile.previous_position,
            transform.position,
            filter);
        for (const QueryHit& hit : hits) {
            if (hit.entity_type != EntityType::kProjectile) {
                continue;
            }
            const NetId first = std::min(identity.net_id, hit.net_id);
            const NetId second = std::max(identity.net_id, hit.net_id);
            const auto duplicate = std::find_if(
                matches->begin(),
                matches->end(),
                [first, second](const ProjectileInteractionMatch& match) {
                    return std::min(match.lhs_net_id, match.rhs_net_id) == first &&
                           std::max(match.lhs_net_id, match.rhs_net_id) == second;
                });
            if (duplicate != matches->end()) {
                continue;
            }

            const auto target_entity = world.find_entity(hit.net_id);
            if (!target_entity.has_value() ||
                !world.registry().all_of<NetworkIdentity, ProjectileState>(
                    *target_entity)) {
                continue;
            }
            const NetworkIdentity& target_identity =
                world.registry().get<NetworkIdentity>(*target_entity);
            if (identity.owner_peer != 0 &&
                identity.owner_peer == target_identity.owner_peer) {
                continue;
            }
            const ProjectileState& target_projectile =
                world.registry().get<ProjectileState>(*target_entity);
            if (!projectile_can_interact_with_projectiles(target_projectile)) {
                continue;
            }

            const std::optional<ProjectileInteractionMatch> match =
                match_projectile_interaction_rule(
                    world.projectile_interaction_rules(),
                    identity.net_id,
                    projectile,
                    hit.net_id,
                    target_projectile,
                    hit.position);
            if (match.has_value()) {
                matches->push_back(*match);
            }
        }
    }

    std::sort(
        matches->begin(),
        matches->end(),
        [](const ProjectileInteractionMatch& lhs,
           const ProjectileInteractionMatch& rhs) {
            const NetId lhs_min = std::min(lhs.lhs_net_id, lhs.rhs_net_id);
            const NetId lhs_max = std::max(lhs.lhs_net_id, lhs.rhs_net_id);
            const NetId rhs_min = std::min(rhs.lhs_net_id, rhs.rhs_net_id);
            const NetId rhs_max = std::max(rhs.lhs_net_id, rhs.rhs_net_id);
            if (lhs_min != rhs_min) {
                return lhs_min < rhs_min;
            }
            return lhs_max < rhs_max;
        });
}

void process_projectile_interactions(
    World& world,
    std::uint32_t current_tick,
    float fixed_delta_seconds,
    std::vector<KernelEvent>* events,
    std::vector<NetId>* projectiles_to_destroy) {
    std::vector<ProjectileInteractionMatch> matches;
    collect_projectile_interaction_matches(world, &matches);
    std::vector<NetId> consumed_projectiles;
    for (const ProjectileInteractionMatch& match : matches) {
        if (contains_net_id(consumed_projectiles, match.lhs_net_id) ||
            contains_net_id(consumed_projectiles, match.rhs_net_id)) {
            continue;
        }
        if (match.rule == nullptr) {
            continue;
        }

        const bool destroy_lhs = match.lhs_is_rule_lhs ? match.rule->destroy_lhs
                                                       : match.rule->destroy_rhs;
        const bool destroy_rhs = match.lhs_is_rule_lhs ? match.rule->destroy_rhs
                                                       : match.rule->destroy_lhs;
        if (destroy_lhs) {
            push_unique_net_id(projectiles_to_destroy, match.lhs_net_id);
            push_unique_net_id(&consumed_projectiles, match.lhs_net_id);
        }
        if (destroy_rhs) {
            push_unique_net_id(projectiles_to_destroy, match.rhs_net_id);
            push_unique_net_id(&consumed_projectiles, match.rhs_net_id);
        }

        if (match.rule->area_effect.enabled) {
            const NetId source_net_id =
                match.lhs_is_rule_lhs ? match.lhs_net_id : match.rhs_net_id;
            PeerId owner_peer = 0;
            const auto source_entity = world.find_entity(source_net_id);
            if (source_entity.has_value() &&
                world.registry().all_of<NetworkIdentity>(*source_entity)) {
                owner_peer =
                    world.registry().get<NetworkIdentity>(*source_entity).owner_peer;
            }
            const ProjectileInteractionAreaEffectSpawn& spawn =
                match.rule->area_effect;
            const NetId area_effect = world.spawn_area_effect(
                owner_peer,
                match.position,
                spawn.radius,
                spawn.damage_interval_ticks,
                current_tick + spawn.lifetime_ticks,
                spawn.damage_per_interval,
                spawn.source_code,
                spawn.collision_mask);
            push_event(
                events,
                KernelEventType_EntitySpawned,
                current_tick,
                area_effect,
                owner_peer,
                static_cast<std::uint32_t>(EntityType::kAreaEffect),
                tick_time_us(current_tick, fixed_delta_seconds),
                tick_time_us(current_tick, fixed_delta_seconds));
        }
    }
}

}  // namespace

glm::vec3 projectile_position_at(
    const glm::vec3& origin,
    const glm::vec3& initial_velocity,
    ProjectileMotionModel motion_model,
    const glm::vec3& gravity,
    float elapsed_seconds) {
    if (motion_model == ProjectileMotionModel::kParabolic) {
        return origin + initial_velocity * elapsed_seconds +
               0.5f * gravity * elapsed_seconds * elapsed_seconds;
    }
    return origin + initial_velocity * elapsed_seconds;
}

glm::vec3 projectile_velocity_at(
    const glm::vec3& initial_velocity,
    ProjectileMotionModel motion_model,
    const glm::vec3& gravity,
    float elapsed_seconds) {
    if (motion_model == ProjectileMotionModel::kParabolic) {
        return initial_velocity + gravity * elapsed_seconds;
    }
    return initial_velocity;
}

void simulate_projectiles(World& world, float fixed_delta_seconds) {
    simulate_projectiles(world, fixed_delta_seconds, 0, nullptr);
}

void simulate_projectiles(
    World& world,
    float fixed_delta_seconds,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events) {
    simulate_projectiles(world, fixed_delta_seconds, current_tick, events, nullptr);
}

void simulate_projectiles(
    World& world,
    float fixed_delta_seconds,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    DamagePipeline local_damage_pipeline;
    DamagePipeline* active_damage_pipeline = damage_pipeline;
    if (active_damage_pipeline == nullptr) {
        active_damage_pipeline = &local_damage_pipeline;
    }
    std::vector<NetId> projectiles_to_destroy;

    auto view = world.registry().view<NetworkIdentity, Transform, Velocity, ProjectileState, ProjectileTag>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        Transform& transform = view.get<Transform>(entity);
        Velocity& velocity = view.get<Velocity>(entity);
        ProjectileState& projectile = view.get<ProjectileState>(entity);

        projectile.previous_position = transform.position;
        if (projectile.motion_model == ProjectileMotionModel::kHoming &&
            world.registry().all_of<HomingState>(entity)) {
            advance_homing_projectile(
                world,
                identity.owner_peer,
                projectile,
                transform,
                velocity,
                world.registry().get<HomingState>(entity),
                fixed_delta_seconds,
                current_tick);
        } else {
            const float next_age_seconds =
                projectile.age_seconds + fixed_delta_seconds;
            transform.position = projectile_position_at(
                projectile.spawn_position,
                projectile.initial_velocity,
                projectile.motion_model,
                projectile.gravity,
                next_age_seconds);
            velocity.linear = projectile_velocity_at(
                projectile.initial_velocity,
                projectile.motion_model,
                projectile.gravity,
                next_age_seconds);
            projectile.age_seconds = next_age_seconds;
        }
    }

    process_projectile_interactions(
        world,
        current_tick,
        fixed_delta_seconds,
        events,
        &projectiles_to_destroy);

    auto hit_view = world.registry().view<NetworkIdentity, Transform, ProjectileState, ProjectileTag>();
    for (const entt::entity entity : hit_view) {
        const NetworkIdentity& identity = hit_view.get<NetworkIdentity>(entity);
        if (contains_net_id(projectiles_to_destroy, identity.net_id)) {
            continue;
        }
        const Transform& transform = hit_view.get<Transform>(entity);
        ProjectileState& projectile = hit_view.get<ProjectileState>(entity);

        QueryFilter filter;
        filter.ignored_net_id = projectile.shooter_net_id;
        filter.ignored_owner_peer = identity.owner_peer;
        filter.collision_mask = projectile.collision_mask;
        const std::vector<QueryHit> hits = collect_segment_hits(
            world,
            projectile.previous_position,
            transform.position,
            filter);
        const bool expired = projectile.max_lifetime_seconds > 0.0f &&
                             projectile.age_seconds >= projectile.max_lifetime_seconds;
        if (hits.empty() && !expired) {
            continue;
        }

        const std::uint64_t hit_time_us =
            tick_time_us(current_tick, fixed_delta_seconds);
        const std::vector<ProjectileHitRecord> records =
            make_projectile_hit_records(identity.net_id, hits);
        if (!records.empty()) {
            const ProjectileHitOutcome outcome = process_projectile_hit_records(
                world,
                identity,
                projectile,
                records,
                transform.position,
                current_tick,
                hit_time_us,
                events,
                active_damage_pipeline);
            if (outcome.destroy_projectile) {
                push_unique_net_id(&projectiles_to_destroy, identity.net_id);
            }
            continue;
        }

        if (
            projectile.damage_shape == ProjectileDamageShape::kExplosion ||
            projectile.explosion_radius > 0.0f) {
            explode_projectile(
                world,
                identity.net_id,
                identity.owner_peer,
                projectile,
                transform.position,
                current_tick,
                hit_time_us,
                events,
                active_damage_pipeline);
        }
        push_unique_net_id(&projectiles_to_destroy, identity.net_id);
    }

    std::sort(projectiles_to_destroy.begin(), projectiles_to_destroy.end());
    projectiles_to_destroy.erase(
        std::unique(projectiles_to_destroy.begin(), projectiles_to_destroy.end()),
        projectiles_to_destroy.end());
    for (NetId projectile : projectiles_to_destroy) {
        world.destroy(projectile);
    }
    if (damage_pipeline == nullptr) {
        active_damage_pipeline->confirm_ready(
            world,
            tick_time_us(current_tick, fixed_delta_seconds),
            current_tick,
            events);
    }
}

bool resolve_projectile_historical_hit(
    World& world,
    const HistoryBuffer& history_buffer,
    NetId projectile_net_id,
    NetId ignored_net_id,
    PeerId owner_peer,
    const ProjectileState& projectile,
    const glm::vec3& origin,
    const glm::vec3& velocity,
    std::uint32_t rewind_tick,
    std::uint32_t current_tick,
    float fixed_delta_seconds,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    if (fixed_delta_seconds <= 0.0f || current_tick <= rewind_tick) {
        return false;
    }

    // Only query historical hitboxes along the deterministic catch-up path; the
    // surviving projectile's current transform remains the fast-forwarded state.
    glm::vec3 previous_position = origin;
    for (std::uint32_t tick = rewind_tick + 1; tick <= current_tick; ++tick) {
        const float elapsed_seconds =
            static_cast<float>(tick - rewind_tick) * fixed_delta_seconds;
        const glm::vec3 current_position = projectile_position_at(
            origin,
            velocity,
            projectile.motion_model,
            projectile.gravity,
            elapsed_seconds);
        const HistoryFrame* frame = history_buffer.find_frame(tick);
        if (frame != nullptr) {
            HistoricalHitResult hit;
            if (sweep_history_frame(
                    *frame,
                    previous_position,
                    current_position,
                    ignored_net_id,
                    &hit)) {
                const std::uint64_t hit_time_us =
                    tick_time_us(frame->server_tick, fixed_delta_seconds);
                push_event(
                    events,
                    KernelEventType_Explosion,
                    current_tick,
                    projectile_net_id,
                    owner_peer,
                    static_cast<std::uint32_t>(
                        projectile.explosion_radius * 100.0f),
                    hit_time_us,
                    hit_time_us);
                apply_historical_explosion_damage(
                    world,
                    *frame,
                    projectile_net_id,
                    owner_peer,
                    projectile,
                    hit.impact_position,
                    current_tick,
                    hit_time_us,
                    events,
                    damage_pipeline);
                world.destroy(projectile_net_id);
                return true;
            }
        }
        previous_position = current_position;
    }

    return false;
}

}  // namespace network_example
