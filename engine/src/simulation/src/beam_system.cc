#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "physics/public/physics_world.h"
#include "simulation/public/collision_filter.h"

namespace network_example {
namespace {

constexpr std::uint32_t kDamageScale = 1000000u;

void push_event(
    std::vector<KernelEvent>* events,
    KernelEventType type,
    std::uint32_t tick,
    NetId net_id,
    PeerId peer_id,
    std::uint32_t code = 0) {
    if (events == nullptr) {
        return;
    }
    events->push_back(KernelEvent{type, tick, net_id, peer_id, code});
}

// Maps the collider's local +Z -- the axis beam_oriented_box's half_extents.z
// runs along, and the axis the presentation prefabs are built on -- onto the
// beam's aim. Nothing else writes a projectile's rotation: World::spawn_projectile
// leaves it identity and the weapon refresh only moves the origin, so without
// this a beam's replicated transform never turns with the shooter.
glm::quat beam_rotation(const glm::vec3& direction) {
    const float length = glm::length(direction);
    if (length <= 0.0001f) {
        return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    }
    const glm::vec3 from{0.0f, 0.0f, 1.0f};
    const glm::vec3 to = direction / length;
    const float dot = std::clamp(glm::dot(from, to), -1.0f, 1.0f);
    if (dot > 0.999f) {
        return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    }
    if (dot < -0.999f) {
        // Antiparallel: cross() is degenerate, so name the half turn outright.
        return glm::quat{0.0f, 0.0f, 1.0f, 0.0f};
    }
    const glm::vec3 axis = glm::normalize(glm::cross(from, to));
    return glm::angleAxis(std::acos(dot), axis);
}

std::uint32_t tick_damage_units(const ProjectileBeamRuntime& beam) {
    const double units =
        static_cast<double>(beam.damage_per_tick) *
        static_cast<double>(kDamageScale);
    return static_cast<std::uint32_t>(std::max(0.0, std::round(units)));
}

}  // namespace

void simulate_beams(
    World& world,
    std::uint32_t current_tick,
    float fixed_delta_seconds,
    std::uint64_t server_time_us,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    (void)fixed_delta_seconds;
    DamagePipeline local_damage_pipeline;
    DamagePipeline* active_damage_pipeline = damage_pipeline;
    if (active_damage_pipeline == nullptr) {
        active_damage_pipeline = &local_damage_pipeline;
    }

    std::vector<NetId> beams_to_destroy;
    auto view = world.registry().view<
        NetworkIdentity,
        Transform,
        ProjectileState,
        ProjectileBeamRuntime,
        ProjectileTag>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        Transform& transform = view.get<Transform>(entity);
        ProjectileBeamRuntime& beam = view.get<ProjectileBeamRuntime>(entity);

        if (beam.expire_tick != 0 && current_tick >= beam.expire_tick) {
            beams_to_destroy.push_back(identity.net_id);
            continue;
        }
        if (beam.length <= 0.0f || beam.radius <= 0.0f ||
            beam.damage_per_tick == 0) {
            continue;
        }

        transform.position = beam.origin;
        transform.rotation = beam_rotation(beam.direction);
        physics::PhysicsWorld* collision_world = world.collision_world();
        if (collision_world == nullptr) {
            continue;
        }
        physics::ShapeCastRequest request{};
        request.shape.type = physics::CollisionShapeType::kSphere;
        request.shape.radius = beam.radius;
        request.start = beam.origin;
        request.displacement = beam.direction * beam.length;
        request.filter = collision_filter_from_mask(beam.collision_mask);
        request.filter.ignored_entity_net_id = beam.shooter_net_id;
        const std::vector<physics::CollisionHit> hits =
            collision_world->shape_cast_all(request);

        const std::uint32_t damage_units = tick_damage_units(beam);
        std::uint32_t sequence_id = 0;
        for (const physics::CollisionHit& hit : hits) {
            if (hit.identity.kind != physics::CollisionObjectKind::kActorHitbox) {
                break;
            }
            const NetId target_net_id = hit.identity.entity_net_id;
            std::uint32_t& remainder =
                beam.damage_remainder_by_target[target_net_id];
            const std::uint64_t accumulated =
                static_cast<std::uint64_t>(remainder) + damage_units;
            const auto damage =
                static_cast<std::uint16_t>(accumulated / kDamageScale);
            remainder = static_cast<std::uint32_t>(accumulated % kDamageScale);
            if (damage == 0) {
                continue;
            }
            active_damage_pipeline->submit_damage_request(DamageRequest{
                current_tick,
                sequence_id++,
                identity.net_id,
                target_net_id,
                identity.owner_peer,
                beam.source_code,
                damage,
                server_time_us,
                hit.position,
            });
        }
    }

    for (NetId beam : beams_to_destroy) {
        if (world.destroy(beam)) {
            push_event(
                events,
                KernelEventType_EntityDestroyed,
                current_tick,
                beam,
                0,
                KernelDespawnReason_Destroyed);
        }
    }

    if (damage_pipeline == nullptr) {
        const std::vector<ConfirmedDamage> ready_damage =
            active_damage_pipeline->drain_ready_damage(world, server_time_us);
        apply_damage_applications(world, ready_damage, current_tick, events);
    }
}

}  // namespace network_example
