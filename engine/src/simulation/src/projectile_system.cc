#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

        QueryFilter filter;
        filter.ignored_net_id = projectile.shooter_net_id;
        filter.ignored_owner_peer = identity.owner_peer;
        filter.collision_mask = projectile.collision_mask;
        const std::vector<QueryHit> hits = collect_segment_hits(
            world,
            projectile.previous_position,
            transform.position,
            filter);
        const bool hit_target = !hits.empty();
        const glm::vec3 impact_position =
            hit_target ? hits.front().position : transform.position;
        const bool expired = projectile.max_lifetime_seconds > 0.0f &&
                             projectile.age_seconds >= projectile.max_lifetime_seconds;
        if (!hit_target && !expired) {
            continue;
        }

        const std::uint64_t hit_time_us =
            tick_time_us(current_tick, fixed_delta_seconds);
        if (hit_target &&
            projectile.hit_response == ProjectileHitResponse::kContinue &&
            projectile.damage_shape == ProjectileDamageShape::kPiercingSegment &&
            projectile.damage > 0) {
            std::uint32_t sequence_id = 0;
            for (const QueryHit& hit : hits) {
                if (projectile.hit_count >= projectile.max_hit_count) {
                    break;
                }
                active_damage_pipeline->submit_damage_request(DamageRequest{
                    current_tick,
                    sequence_id++,
                    identity.net_id,
                    hit.net_id,
                    identity.owner_peer,
                    projectile.weapon_id,
                    projectile.damage,
                    hit_time_us,
                    hit.position,
                });
                ++projectile.hit_count;
            }
            if (projectile.hit_count >= projectile.max_hit_count) {
                projectiles_to_destroy.push_back(identity.net_id);
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
                hit_target ? impact_position : transform.position,
                current_tick,
                hit_time_us,
                events,
                active_damage_pipeline);
        } else if (hit_target &&
                   projectile.damage_shape == ProjectileDamageShape::kDirectHit &&
                   projectile.damage > 0) {
            active_damage_pipeline->submit_damage_request(DamageRequest{
                current_tick,
                0,
                identity.net_id,
                hits.front().net_id,
                identity.owner_peer,
                projectile.weapon_id,
                projectile.damage,
                hit_time_us,
                impact_position,
            });
        }
        projectiles_to_destroy.push_back(identity.net_id);
    }

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
