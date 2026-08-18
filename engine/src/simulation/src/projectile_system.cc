#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "physics/public/physics_world.h"
#include "simulation/public/collision_filter.h"
#include "simulation/public/action_graph.h"

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

std::uint64_t tick_time_us(std::uint32_t tick, float fixed_delta_seconds);
glm::vec3 normalized_or(const glm::vec3& value, const glm::vec3& fallback);

bool spawn_projectile_from_template(
    World& world,
    const RuntimeProjectileTemplate& projectile_template,
    PeerId owner_peer,
    NetId shooter_net_id,
    std::uint8_t source_weapon_id,
    std::uint32_t action_instance_id,
    const glm::vec3& position,
    const glm::vec3& direction,
    std::uint32_t current_tick,
    float fixed_delta_seconds,
    std::vector<KernelEvent>* events,
    std::uint32_t* out_entity_type) {
    const std::uint8_t weapon_id =
        projectile_template.weapon_id == 0 ? source_weapon_id
                                           : projectile_template.weapon_id;
    const glm::vec3 velocity =
        projectile_template.projectile_type == ProjectileType::kAreaEffect
            ? glm::vec3{0.0f, 0.0f, 0.0f}
            : normalized_or(direction, glm::vec3{1.0f, 0.0f, 0.0f}) *
                  projectile_template.speed;
    const NetId projectile_net_id = world.spawn_projectile(
        owner_peer,
        position,
        velocity);
    const auto projectile_entity = world.find_entity(projectile_net_id);
    if (!projectile_entity.has_value()) {
        return false;
    }
    ProjectileState& projectile =
        world.registry().get<ProjectileState>(*projectile_entity);
    projectile.weapon_id = weapon_id;
    projectile.projectile_template_id = projectile_template.projectile_template_id;
    projectile.damage = projectile_template.damage;
    projectile.spawn_tick = current_tick;
    projectile.action_instance_id = action_instance_id;
    projectile.shooter_net_id = shooter_net_id;
    projectile.motion_model = projectile_template.motion_model;
    projectile.hit_response = projectile_template.hit_response;
    projectile.damage_shape = projectile_template.damage_shape;
    projectile.collision_query_mode = projectile_template.collision_query_mode;
    projectile.collision_geometry = projectile_template.collision_geometry;
    projectile.has_collision_geometry = projectile_template.has_collision_geometry;
    projectile.collision_mask = projectile_template.collision_mask;
    projectile.max_hit_count = std::max(1u, projectile_template.max_hit_count);
    projectile.max_lifetime_ticks = projectile_template.lifetime_ticks;
    projectile.spawn_position = position;
    projectile.initial_velocity = velocity;
    projectile.gravity = projectile_template.gravity;
    projectile.previous_position = position;
    if (projectile_template.projectile_impact_binding.has_value()) {
        world.registry().emplace<OnProjectileImpactTriggerTag>(
            *projectile_entity);
    }
    if (projectile_template.expired_binding.has_value()) {
        world.registry().emplace<OnExpiredTriggerTag>(*projectile_entity);
    }
    if (projectile_template.projectile_type == ProjectileType::kAreaEffect) {
        world.registry().replace<Hitbox>(
            *projectile_entity,
            Hitbox{
                {0.0f, 0.0f, 0.0f},
                {projectile_template.area_radius,
                 projectile_template.area_radius,
                 projectile_template.area_radius},
                projectile_template.collider_template_id});
        world.registry().emplace<ProjectileAreaEffectRuntime>(
            *projectile_entity,
            ProjectileAreaEffectRuntime{
                projectile_template.area_radius,
                projectile_template.damage,
                projectile_template.damage_interval_ticks == 0
                    ? 1u
                    : projectile_template.damage_interval_ticks,
                current_tick + std::max(1u, projectile_template.lifetime_ticks),
                weapon_id,
                projectile_template.collision_mask,
                projectile_template.damage_falloff,
                {},
                projectile_template.projectile_impact_binding,
            });
    }
    if (projectile_template.projectile_type == ProjectileType::kBeam) {
        world.registry().emplace<ProjectileBeamRuntime>(
            *projectile_entity,
            ProjectileBeamRuntime{
                shooter_net_id,
                position,
                normalized_or(direction, glm::vec3{1.0f, 0.0f, 0.0f}),
                projectile_template.beam_length,
                projectile_template.beam_radius,
                projectile_template.damage,
                current_tick + std::max(1u, projectile_template.lifetime_ticks),
                weapon_id,
                projectile_template.collision_mask,
                {},
            });
    }
    push_event(
        events,
        KernelEventType_EntitySpawned,
        current_tick,
        projectile_net_id,
        owner_peer,
        static_cast<std::uint32_t>(EntityType::kProjectile),
        tick_time_us(current_tick, fixed_delta_seconds),
        tick_time_us(current_tick, fixed_delta_seconds));
    if (out_entity_type != nullptr) {
        *out_entity_type = static_cast<std::uint32_t>(EntityType::kProjectile);
    }
    return true;
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

std::uint32_t entity_gameplay_collision_category(
    const World& world,
    NetId net_id) {
    std::uint32_t category = 0;
    for (const ColliderInstance& collider : world.collider_registry().instances()) {
        if (collider.entity_net_id == net_id && collider.lifetime_ticks == 0 &&
            collider.enabled) {
            category |= collider.layer_mask;
        }
    }
    return category;
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
    const std::uint32_t layer =
        entity_gameplay_collision_category(world, target_net_id);
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
        const std::uint32_t layer =
            entity_gameplay_collision_category(world, identity.net_id);
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
    const std::uint32_t next_age_ticks = projectile.age_ticks + 1;
    const float next_age_duration =
        static_cast<float>(next_age_ticks) * fixed_delta_seconds;
    if (homing.phase == MissileGuidancePhase::kBoost) {
        transform.position = projectile_position_at(
            projectile.spawn_position,
            projectile.initial_velocity,
            ProjectileMotionModel::kLinear,
            glm::vec3{0.0f},
            next_age_duration);
        velocity.linear = projectile.initial_velocity;
        projectile.age_ticks = next_age_ticks;
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
            degrees_to_radians(homing.max_turn_degrees_per_tick);
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
    projectile.age_ticks = next_age_ticks;
}

enum class ProjectileCollisionQueryType {
    kSegment,
    kSweptSphere,
    kSphereOverlap,
    kSweptBox,
    kBoxOverlap,
    kUnsupported,
};

struct ProjectileCollisionQuery {
    ProjectileCollisionQueryType query_type = ProjectileCollisionQueryType::kSegment;
    glm::vec3 previous_position{0.0f, 0.0f, 0.0f};
    glm::vec3 current_position{0.0f, 0.0f, 0.0f};
    glm::vec3 center{0.0f, 0.0f, 0.0f};
    glm::vec3 half_extents{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    glm::vec3 bounds_center{0.0f, 0.0f, 0.0f};
    glm::vec3 bounds_half_extents{0.0f, 0.0f, 0.0f};
};

struct ProjectileHitRecord {
    NetId projectile_net_id = 0;
    NetId target_net_id = 0;
    physics::CollisionHit hit{};
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

ProjectileCollisionQuery make_segment_projectile_query(
    const glm::vec3& previous_position,
    const glm::vec3& current_position) {
    const glm::vec3 min_corner = glm::min(previous_position, current_position);
    const glm::vec3 max_corner = glm::max(previous_position, current_position);
    ProjectileCollisionQuery query;
    query.query_type = ProjectileCollisionQueryType::kSegment;
    query.previous_position = previous_position;
    query.current_position = current_position;
    query.bounds_center = (min_corner + max_corner) * 0.5f;
    query.bounds_half_extents = (max_corner - min_corner) * 0.5f;
    return query;
}

ProjectileCollisionQuery build_projectile_collision_query(
    const ProjectileState& projectile,
    const glm::vec3& previous_position,
    const glm::vec3& current_position) {
    if (!projectile.has_collision_geometry) {
        return make_segment_projectile_query(previous_position, current_position);
    }

    const ProjectileCollisionGeometry& geometry = projectile.collision_geometry;
    const glm::vec3 previous_center = previous_position + geometry.center;
    const glm::vec3 current_center = current_position + geometry.center;
    const glm::vec3 displacement = current_center - previous_center;
    const bool moved = glm::length(displacement) > 0.0001f;

    ProjectileCollisionQuery query;
    query.previous_position = previous_center;
    query.current_position = current_center;
    query.center = current_center;
    query.half_extents = geometry.half_extents;
    query.radius = geometry.radius;

    const ProjectileCollisionQueryMode mode = projectile.collision_query_mode;
    const bool force_overlap = mode == ProjectileCollisionQueryMode::kOverlap;
    const bool force_sweep = mode == ProjectileCollisionQueryMode::kSweep;
    const bool force_ray = mode == ProjectileCollisionQueryMode::kRay;

    if (force_ray || geometry.shape_type == ColliderShapeType::kSegment) {
        if (geometry.length > 0.0f && !moved) {
            query.current_position =
                current_center + glm::vec3{geometry.length, 0.0f, 0.0f};
        }
        return make_segment_projectile_query(
            query.previous_position,
            query.current_position);
    }

    if (geometry.shape_type == ColliderShapeType::kSphere && geometry.radius > 0.0f) {
        if (force_overlap || (!force_sweep && !moved)) {
            query.query_type = ProjectileCollisionQueryType::kSphereOverlap;
            query.bounds_center = current_center;
            query.bounds_half_extents = glm::vec3{geometry.radius};
            return query;
        }
        query.query_type = ProjectileCollisionQueryType::kSweptSphere;
        const glm::vec3 min_corner =
            glm::min(previous_center, current_center) - glm::vec3{geometry.radius};
        const glm::vec3 max_corner =
            glm::max(previous_center, current_center) + glm::vec3{geometry.radius};
        query.bounds_center = (min_corner + max_corner) * 0.5f;
        query.bounds_half_extents = (max_corner - min_corner) * 0.5f;
        return query;
    }

    if ((geometry.shape_type == ColliderShapeType::kAabb ||
         geometry.shape_type == ColliderShapeType::kOrientedBox) &&
        geometry.half_extents.x >= 0.0f && geometry.half_extents.y >= 0.0f &&
        geometry.half_extents.z >= 0.0f) {
        if (force_overlap || (!force_sweep && !moved)) {
            query.query_type = ProjectileCollisionQueryType::kBoxOverlap;
            query.bounds_center = current_center;
            query.bounds_half_extents = geometry.half_extents;
            return query;
        }
        query.query_type = ProjectileCollisionQueryType::kSweptBox;
        const glm::vec3 min_corner =
            glm::min(previous_center, current_center) - geometry.half_extents;
        const glm::vec3 max_corner =
            glm::max(previous_center, current_center) + geometry.half_extents;
        query.bounds_center = (min_corner + max_corner) * 0.5f;
        query.bounds_half_extents = (max_corner - min_corner) * 0.5f;
        return query;
    }

    query.query_type = ProjectileCollisionQueryType::kUnsupported;
    return query;
}

std::vector<physics::CollisionHit> query_projectile_collision_hits_impl(
    const physics::PhysicsWorld& collision_world,
    const ProjectileState& projectile,
    const glm::vec3& previous_position,
    const glm::vec3& current_position,
    const physics::CollisionQueryFilter& filter) {
    const ProjectileCollisionQuery query = build_projectile_collision_query(
        projectile,
        previous_position,
        current_position);
    switch (query.query_type) {
        case ProjectileCollisionQueryType::kSweptSphere:
        {
            physics::ShapeCastRequest request{};
            request.shape.type = physics::CollisionShapeType::kSphere;
            request.shape.radius = query.radius;
            request.start = query.previous_position;
            request.displacement =
                query.current_position - query.previous_position;
            request.filter = filter;
            return collision_world.shape_cast_all(request);
        }
        case ProjectileCollisionQueryType::kSphereOverlap:
        {
            physics::OverlapRequest request{};
            request.shape.type = physics::CollisionShapeType::kSphere;
            request.shape.radius = query.radius;
            request.position = query.center;
            request.filter = filter;
            return collision_world.overlap_all(request);
        }
        case ProjectileCollisionQueryType::kSweptBox:
        {
            physics::ShapeCastRequest request{};
            request.shape.type = physics::CollisionShapeType::kBox;
            request.shape.half_extents = query.half_extents;
            request.start = query.previous_position;
            request.displacement =
                query.current_position - query.previous_position;
            request.filter = filter;
            return collision_world.shape_cast_all(request);
        }
        case ProjectileCollisionQueryType::kBoxOverlap:
        {
            physics::OverlapRequest request{};
            request.shape.type = physics::CollisionShapeType::kBox;
            request.shape.half_extents = query.half_extents;
            request.position = query.center;
            request.filter = filter;
            return collision_world.overlap_all(request);
        }
        case ProjectileCollisionQueryType::kUnsupported:
            return {};
        case ProjectileCollisionQueryType::kSegment:
        default:
        {
            const glm::vec3 displacement =
                query.current_position - query.previous_position;
            physics::RayCastRequest request{};
            request.origin = query.previous_position;
            request.direction = displacement;
            request.max_distance = glm::length(displacement);
            request.filter = filter;
            return collision_world.ray_cast_all(request);
        }
    }
}

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
    const std::vector<physics::CollisionHit>& hits) {
    std::vector<ProjectileHitRecord> records;
    records.reserve(hits.size());
    std::uint32_t sequence_id = 0;
    for (const physics::CollisionHit& hit : hits) {
        records.push_back(ProjectileHitRecord{
            projectile_net_id,
            hit.identity.entity_net_id,
            hit,
            sequence_id++,
        });
    }
    return records;
}

std::uint64_t trigger_request_id(
    std::uint32_t current_tick,
    NetId subject,
    TriggerEventType event_type,
    std::uint32_t sequence) {
    std::uint64_t hash = UINT64_C(1469598103934665603);
    const auto mix = [&](std::uint32_t value) {
        for (std::uint32_t shift = 0; shift < 32u; shift += 8u) {
            hash ^= (value >> shift) & 0xffu;
            hash *= UINT64_C(1099511628211);
        }
    };
    mix(current_tick);
    mix(subject);
    mix(static_cast<std::uint32_t>(event_type));
    mix(sequence);
    return hash;
}

std::optional<CompiledActionGraphBinding> projectile_trigger_binding(
    World& world,
    const ProjectileState& projectile,
    TriggerEventType event_type) {
    const RuntimeProjectileTemplate* projectile_template =
        world.find_projectile_template(projectile.projectile_template_id);
    if (projectile_template == nullptr) {
        return std::nullopt;
    }
    return event_type == TriggerEventType::kExpired
        ? projectile_template->expired_binding
        : projectile_template->projectile_impact_binding;
}

void queue_projectile_trigger(
    World& world,
    const NetworkIdentity& identity,
    const ProjectileState& projectile,
    TriggerEventType event_type,
    NetId target,
    const glm::vec3& position,
    const glm::vec3& direction,
    std::uint32_t current_tick,
    std::uint32_t sequence,
    bool historical,
    std::vector<ActionGraphQueuedTrigger>* trigger_events) {
    if (trigger_events == nullptr) {
        return;
    }
    const auto entity = world.find_entity(identity.net_id);
    const bool subscribed = entity.has_value() &&
        (event_type == TriggerEventType::kExpired
             ? world.registry().all_of<OnExpiredTriggerTag>(*entity)
             : world.registry().all_of<OnProjectileImpactTriggerTag>(*entity));
    if (!subscribed) {
        return;
    }
    std::optional<CompiledActionGraphBinding> binding =
        projectile_trigger_binding(world, projectile, event_type);
    if (!binding.has_value()) {
        return;
    }
    const TriggerEvent event{
        event_type,
        identity.net_id,
        projectile.shooter_net_id,
        target,
        position,
        direction,
        event_type == TriggerEventType::kProjectileImpact
            ? std::optional<ProjectileImpactPayload>{ProjectileImpactPayload{
                  projectile.projectile_template_id,
                  projectile.action_instance_id,
                  projectile.weapon_id,
                  historical,
              }}
            : std::nullopt,
    };
    trigger_events->push_back(ActionGraphQueuedTrigger{
        std::move(*binding),
        identity.net_id,
        event,
        ActionExecutionProvenance{
            trigger_request_id(
                current_tick, identity.net_id, event_type, sequence),
            projectile.action_instance_id,
            current_tick,
            projectile.shooter_net_id,
            identity.owner_peer,
            projectile.weapon_id,
            ActionAuthoritySource::kAuthoritativeSimulation,
        },
        sequence,
    });
}

void execute_queued_trigger_events(
    World& world,
    std::vector<ActionGraphQueuedTrigger>* trigger_events,
    float fixed_delta_seconds,
    std::vector<KernelEvent>* events) {
    if (trigger_events == nullptr) {
        return;
    }
    std::vector<ActionGraphCommandBatch> command_batches;
    if (!dispatch_action_graph_triggers(
            trigger_events, &command_batches, nullptr)) {
        return;
    }
    for (const ActionGraphCommandBatch& batch : command_batches) {
        const World::ActionGraphDedupKey dedup_key{
            batch.provenance.requester_peer != 0u
                ? batch.provenance.requester_peer
                : batch.provenance.owner_peer,
            batch.provenance.request_id,
            batch.event.type,
            batch.sequence,
        };
        if (batch.provenance.request_id == 0u ||
            world.action_graph_batch_processed(
                dedup_key.requester_peer,
                dedup_key.request_id,
                dedup_key.event_type,
                dedup_key.sequence)) {
            continue;
        }
        const bool valid = std::all_of(
            batch.commands.begin(),
            batch.commands.end(),
            [&](const ActionGraphCommand& graph_command) {
                const auto* command =
                    std::get_if<ActionSpawnProjectileCommand>(&graph_command);
                return command != nullptr &&
                    world.find_projectile_template(
                        command->projectile_template_id) != nullptr;
            });
        if (!valid) {
            continue;
        }
        if (!world.reserve_action_graph_batch_capacity(1u)) {
            continue;
        }
        const World::ActionGraphDedupReservationResult reservation =
            world.reserve_action_graph_batch(dedup_key);
        if (reservation != World::ActionGraphDedupReservationResult::kReserved) {
            world.release_action_graph_batch_capacity(1u);
            continue;
        }
        bool committed = true;
        for (const ActionGraphCommand& graph_command : batch.commands) {
            const auto* command =
                std::get_if<ActionSpawnProjectileCommand>(&graph_command);
            const RuntimeProjectileTemplate* projectile_template =
                world.find_projectile_template(command->projectile_template_id);
            if (!spawn_projectile_from_template(
                world,
                *projectile_template,
                command->provenance.owner_peer,
                command->provenance.instigator,
                command->provenance.source_weapon_id,
                command->provenance.action_instance_id,
                command->position,
                command->direction,
                command->provenance.server_tick,
                fixed_delta_seconds,
                events,
                nullptr)) {
                committed = false;
                break;
            }
        }
        if (committed) {
            world.commit_action_graph_batch(
                dedup_key, batch.provenance.server_tick);
        } else {
            world.cancel_action_graph_batch(dedup_key);
        }
    }
}

ProjectileHitOutcome process_projectile_hit_records(
    World& world,
    const NetworkIdentity& identity,
    ProjectileState& projectile,
    const std::vector<ProjectileHitRecord>& records,
    const glm::vec3& fallback_position,
    std::uint32_t current_tick,
    float fixed_delta_seconds,
    std::uint64_t hit_time_us,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline,
    std::vector<ActionGraphQueuedTrigger>* trigger_events) {
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
            if (record.hit.identity.kind !=
                physics::CollisionObjectKind::kActorHitbox) {
                queue_projectile_trigger(
                    world,
                    identity,
                    projectile,
                    TriggerEventType::kProjectileImpact,
                    0,
                    record.hit.position,
                    normalized_or(
                        record.hit.position - projectile.previous_position,
                        glm::vec3{1.0f, 0.0f, 0.0f}),
                    current_tick,
                    record.sequence_id,
                    false,
                    trigger_events);
                outcome.destroy_projectile = true;
                return outcome;
            }
            if (!damage_source_may_damage(
                    world, projectile.collision_mask, record.target_net_id)) {
                continue;
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
    const glm::vec3 impact_direction =
        normalized_or(
            impact_position - projectile.previous_position,
            glm::vec3{1.0f, 0.0f, 0.0f});
    queue_projectile_trigger(
        world,
        identity,
        projectile,
        TriggerEventType::kProjectileImpact,
        records.front().hit.identity.kind ==
                physics::CollisionObjectKind::kActorHitbox
            ? records.front().target_net_id
            : 0,
        impact_position,
        impact_direction,
        current_tick,
        records.front().sequence_id,
        false,
        trigger_events);

    // No-op while only actors are reachable here -- they carry no GameplaySide,
    // so the rule admits every one of them. It is in place so that widening what
    // a direct hit can touch cannot silently reintroduce a weapon that levels
    // its own side's cover.
    if (projectile.damage_shape == ProjectileDamageShape::kDirectHit &&
        projectile.damage > 0 &&
        records.front().hit.identity.kind ==
            physics::CollisionObjectKind::kActorHitbox &&
        damage_source_may_damage(
            world, projectile.collision_mask, records.front().target_net_id)) {
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

float projectile_interaction_radius(const ProjectileState& projectile) {
    if (!projectile.has_collision_geometry) {
        return 0.25f;
    }
    if (projectile.collision_geometry.shape_type == ColliderShapeType::kSphere) {
        return std::max(0.0f, projectile.collision_geometry.radius);
    }
    if (projectile.collision_geometry.shape_type == ColliderShapeType::kAabb ||
        projectile.collision_geometry.shape_type ==
            ColliderShapeType::kOrientedBox) {
        return glm::length(projectile.collision_geometry.half_extents);
    }
    return 0.25f;
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
    for (std::size_t lhs_index = 0; lhs_index < projectiles.size(); ++lhs_index) {
        const ProjectileEntityRef& lhs_ref = projectiles[lhs_index];
        if (!world.registry().valid(lhs_ref.entity) ||
            !world.registry().all_of<NetworkIdentity, Transform, ProjectileState>(
                lhs_ref.entity)) {
            continue;
        }
        const NetworkIdentity& lhs_identity =
            world.registry().get<NetworkIdentity>(lhs_ref.entity);
        const Transform& lhs_transform =
            world.registry().get<Transform>(lhs_ref.entity);
        const ProjectileState& lhs_projectile =
            world.registry().get<ProjectileState>(lhs_ref.entity);
        if (!projectile_can_interact_with_projectiles(lhs_projectile)) {
            continue;
        }
        for (std::size_t rhs_index = lhs_index + 1;
             rhs_index < projectiles.size();
             ++rhs_index) {
            const ProjectileEntityRef& rhs_ref = projectiles[rhs_index];
            if (!world.registry().valid(rhs_ref.entity) ||
                !world.registry().all_of<NetworkIdentity, Transform, ProjectileState>(
                    rhs_ref.entity)) {
                continue;
            }
            const NetworkIdentity& rhs_identity =
                world.registry().get<NetworkIdentity>(rhs_ref.entity);
            const Transform& rhs_transform =
                world.registry().get<Transform>(rhs_ref.entity);
            const ProjectileState& rhs_projectile =
                world.registry().get<ProjectileState>(rhs_ref.entity);
            if (!projectile_can_interact_with_projectiles(rhs_projectile) ||
                (lhs_identity.owner_peer != 0 &&
                 lhs_identity.owner_peer == rhs_identity.owner_peer)) {
                continue;
            }

            const glm::vec3 relative_start =
                lhs_projectile.previous_position -
                rhs_projectile.previous_position;
            const glm::vec3 relative_delta =
                (lhs_transform.position - rhs_transform.position) -
                relative_start;
            const float delta_length_squared = glm::dot(
                relative_delta, relative_delta);
            const float fraction = delta_length_squared <= 0.000001f
                ? 0.0f
                : glm::clamp(
                      -glm::dot(relative_start, relative_delta) /
                          delta_length_squared,
                      0.0f,
                      1.0f);
            const glm::vec3 separation =
                relative_start + relative_delta * fraction;
            const float radius = projectile_interaction_radius(lhs_projectile) +
                                 projectile_interaction_radius(rhs_projectile);
            if (glm::dot(separation, separation) > radius * radius) {
                continue;
            }
            const glm::vec3 lhs_position = glm::mix(
                lhs_projectile.previous_position,
                lhs_transform.position,
                fraction);
            const glm::vec3 rhs_position = glm::mix(
                rhs_projectile.previous_position,
                rhs_transform.position,
                fraction);
            const std::optional<ProjectileInteractionMatch> match =
                match_projectile_interaction_rule(
                    world.projectile_interaction_rules(),
                    lhs_identity.net_id,
                    lhs_projectile,
                    rhs_identity.net_id,
                    rhs_projectile,
                    (lhs_position + rhs_position) * 0.5f);
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

        if (match.rule->spawn_projectile_template_id != 0u) {
            const NetId source_net_id =
                match.lhs_is_rule_lhs ? match.lhs_net_id : match.rhs_net_id;
            PeerId owner_peer = 0;
            const auto source_entity = world.find_entity(source_net_id);
            if (source_entity.has_value() &&
                world.registry().all_of<NetworkIdentity>(*source_entity)) {
                owner_peer =
                    world.registry().get<NetworkIdentity>(*source_entity).owner_peer;
            }
            const RuntimeProjectileTemplate* spawn_template =
                world.find_projectile_template(
                    match.rule->spawn_projectile_template_id);
            if (spawn_template == nullptr) {
                continue;
            }
            (void)spawn_projectile_from_template(
                world,
                *spawn_template,
                owner_peer,
                source_net_id,
                0,
                0,
                match.position,
                glm::vec3{1.0f, 0.0f, 0.0f},
                current_tick,
                fixed_delta_seconds,
                events,
                nullptr);
        }
    }
}

}  // namespace

bool spawn_action_graph_projectile(
    World& world,
    std::uint32_t projectile_template_id,
    PeerId owner_peer,
    NetId instigator,
    std::uint32_t action_instance_id,
    const glm::vec3& position,
    const glm::vec3& direction,
    std::uint32_t current_tick,
    float fixed_delta_seconds) {
    const RuntimeProjectileTemplate* projectile_template =
        world.find_projectile_template(projectile_template_id);
    if (projectile_template == nullptr ||
        (projectile_template->projectile_type != ProjectileType::kAreaEffect &&
         (!std::isfinite(projectile_template->speed) ||
          projectile_template->speed <= 0.0f))) {
        return false;
    }
    return spawn_projectile_from_template(
        world,
        *projectile_template,
        owner_peer,
        instigator,
        0u,
        action_instance_id,
        position,
        direction,
        current_tick,
        fixed_delta_seconds,
        nullptr,
        nullptr);
}

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

std::vector<physics::CollisionHit> query_projectile_collision_hits(
    const physics::PhysicsWorld& collision_world,
    const ProjectileState& projectile,
    const glm::vec3& previous_position,
    const glm::vec3& current_position,
    const physics::CollisionQueryFilter& filter) {
    return query_projectile_collision_hits_impl(
        collision_world,
        projectile,
        previous_position,
        current_position,
        filter);
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
    std::vector<ActionGraphQueuedTrigger> trigger_events;

    auto view = world.registry().view<NetworkIdentity, Transform, Velocity, ProjectileState, ProjectileTag>();
    for (const entt::entity entity : view) {
        // Beams carry ProjectileState so they replicate like any other
        // projectile, but simulate_beams owns their lifetime and their
        // transform. Ageing them here expires them on max_lifetime_ticks --
        // which apply_projectile_mechanics sets from beam.lifetime_ticks --
        // while the weapon is still refreshing them, and the refresh never
        // reset age_ticks. The beam was destroyed and respawned under a new
        // net_id every other tick.
        if (world.registry().all_of<ProjectileBeamRuntime>(entity)) {
            continue;
        }
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
            const std::uint32_t next_age_ticks = projectile.age_ticks + 1;
            const float next_age_duration =
                static_cast<float>(next_age_ticks) * fixed_delta_seconds;
            transform.position = projectile_position_at(
                projectile.spawn_position,
                projectile.initial_velocity,
                projectile.motion_model,
                projectile.gravity,
                next_age_duration);
            velocity.linear = projectile_velocity_at(
                projectile.initial_velocity,
                projectile.motion_model,
                projectile.gravity,
                next_age_duration);
            projectile.age_ticks = next_age_ticks;
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
        // See the ageing loop above: simulate_beams resolves beam hits and
        // beam expiry. A beam is also the wrong shape for this loop -- it does
        // not travel, so the swept query degenerates -- and any hit it did
        // report would destroy it outright, which is not how a held beam that
        // rests against a wall should behave.
        if (world.registry().all_of<ProjectileBeamRuntime>(entity)) {
            continue;
        }
        const NetworkIdentity& identity = hit_view.get<NetworkIdentity>(entity);
        if (contains_net_id(projectiles_to_destroy, identity.net_id)) {
            continue;
        }
        const Transform& transform = hit_view.get<Transform>(entity);
        ProjectileState& projectile = hit_view.get<ProjectileState>(entity);

        physics::CollisionQueryFilter filter =
            collision_filter_from_mask(projectile.collision_mask);
        filter.ignored_entity_net_id = projectile.shooter_net_id;
        const physics::PhysicsWorld* collision_world = world.collision_world();
        const std::vector<physics::CollisionHit> hits =
            collision_world == nullptr
                ? std::vector<physics::CollisionHit>{}
                : query_projectile_collision_hits(
                      *collision_world,
                      projectile,
                      projectile.previous_position,
                      transform.position,
                      filter);
        const bool expired = projectile.max_lifetime_ticks > 0u &&
                             projectile.age_ticks >= projectile.max_lifetime_ticks;
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
                fixed_delta_seconds,
                hit_time_us,
                events,
                active_damage_pipeline,
                &trigger_events);
            if (outcome.destroy_projectile) {
                push_unique_net_id(&projectiles_to_destroy, identity.net_id);
            }
            continue;
        }

        queue_projectile_trigger(
            world,
            identity,
            projectile,
            TriggerEventType::kExpired,
            0,
            transform.position,
            normalized_or(
                transform.position - projectile.previous_position,
                glm::vec3{1.0f, 0.0f, 0.0f}),
            current_tick,
            0,
            false,
            &trigger_events);
        push_unique_net_id(&projectiles_to_destroy, identity.net_id);
    }

    execute_queued_trigger_events(
        world, &trigger_events, fixed_delta_seconds, events);

    std::sort(projectiles_to_destroy.begin(), projectiles_to_destroy.end());
    projectiles_to_destroy.erase(
        std::unique(projectiles_to_destroy.begin(), projectiles_to_destroy.end()),
        projectiles_to_destroy.end());
    for (NetId projectile : projectiles_to_destroy) {
        if (world.destroy(projectile)) {
            push_event(
                events,
                KernelEventType_EntityDestroyed,
                current_tick,
                projectile,
                0,
                KernelDespawnReason_Destroyed);
        }
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
                const NetworkIdentity identity{projectile_net_id, owner_peer};
                std::vector<ActionGraphQueuedTrigger> trigger_events;
                queue_projectile_trigger(
                    world,
                    identity,
                    projectile,
                    TriggerEventType::kProjectileImpact,
                    hit.net_id,
                    hit.impact_position,
                    normalized_or(
                        current_position - previous_position,
                        glm::vec3{1.0f, 0.0f, 0.0f}),
                    current_tick,
                    0,
                    true,
                    &trigger_events);
                if (projectile.damage_shape == ProjectileDamageShape::kDirectHit &&
                    projectile.damage > 0 &&
                    damage_pipeline != nullptr) {
                    damage_pipeline->submit_damage_request(DamageRequest{
                        current_tick,
                        0,
                        projectile_net_id,
                        hit.net_id,
                        owner_peer,
                        projectile.weapon_id,
                        projectile.damage,
                        hit_time_us,
                        hit.impact_position,
                    });
                }
                execute_queued_trigger_events(
                    world, &trigger_events, fixed_delta_seconds, events);
                world.destroy(projectile_net_id);
                return true;
            }
        }
        previous_position = current_position;
    }

    return false;
}

}  // namespace network_example
