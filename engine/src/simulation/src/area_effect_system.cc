#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "physics/public/physics_world.h"
#include "simulation/public/collision_filter.h"

namespace network_example {
namespace {

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

}  // namespace

void simulate_area_effects(
    World& world,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    simulate_area_effects(world, current_tick, current_tick, events, damage_pipeline);
}

void simulate_area_effects(
    World& world,
    std::uint32_t current_tick,
    std::uint64_t server_time_us,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    DamagePipeline local_damage_pipeline;
    DamagePipeline* active_damage_pipeline = damage_pipeline;
    if (active_damage_pipeline == nullptr) {
        active_damage_pipeline = &local_damage_pipeline;
    }

    std::vector<NetId> area_effects_to_destroy;
    auto view = world.registry().view<
        NetworkIdentity,
        Transform,
        ProjectileState,
        ProjectileAreaEffectRuntime,
        ProjectileTag>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        const Transform& transform = view.get<Transform>(entity);
        ProjectileAreaEffectRuntime& area_effect =
            view.get<ProjectileAreaEffectRuntime>(entity);
        const ProjectileState& projectile = view.get<ProjectileState>(entity);

        if (area_effect.expire_tick != 0 && current_tick >= area_effect.expire_tick) {
            area_effects_to_destroy.push_back(identity.net_id);
            continue;
        }
        if (area_effect.radius <= 0.0f || area_effect.damage_per_interval == 0) {
            continue;
        }

        physics::PhysicsWorld* collision_world = world.collision_world();
        if (collision_world == nullptr) {
            continue;
        }
        physics::OverlapRequest request{};
        request.shape.type = physics::CollisionShapeType::kSphere;
        request.shape.radius = area_effect.radius;
        request.position = transform.position;
        request.filter = collision_filter_from_mask(area_effect.collision_mask);
        request.filter.ignored_entity_net_id = projectile.shooter_net_id;
        std::vector<physics::CollisionHit> hits =
            collision_world->overlap_all(request);
        std::sort(
            hits.begin(),
            hits.end(),
            [](const physics::CollisionHit& lhs,
               const physics::CollisionHit& rhs) {
                return lhs.identity.entity_net_id < rhs.identity.entity_net_id;
            });

        std::uint32_t sequence_id = 0;
        for (const physics::CollisionHit& hit : hits) {
            const NetId target_net_id = hit.identity.entity_net_id;
            const auto next_damage_tick =
                area_effect.next_damage_tick_by_target.find(target_net_id);
            if (next_damage_tick != area_effect.next_damage_tick_by_target.end() &&
                current_tick < next_damage_tick->second) {
                continue;
            }

            std::uint16_t damage = area_effect.damage_per_interval;
            if (area_effect.damage_falloff == ProjectileDamageFalloff::kLinear &&
                area_effect.radius > 0.0f) {
                const float falloff =
                    1.0f - std::min(1.0f, hit.distance / area_effect.radius);
                damage = static_cast<std::uint16_t>(
                    std::max(1.0f, std::round(area_effect.damage_per_interval * falloff)));
            }

            active_damage_pipeline->submit_damage_request(DamageRequest{
                current_tick,
                sequence_id++,
                identity.net_id,
                target_net_id,
                identity.owner_peer,
                area_effect.source_code,
                damage,
                server_time_us,
                hit.position,
            });
            area_effect.next_damage_tick_by_target[target_net_id] =
                current_tick + std::max(1u, area_effect.damage_interval_ticks);
        }
    }

    for (NetId area_effect : area_effects_to_destroy) {
        if (world.destroy(area_effect)) {
            push_event(
                events,
                KernelEventType_EntityDestroyed,
                current_tick,
                area_effect,
                0,
                KernelDespawnReason_Destroyed);
        }
    }

    if (damage_pipeline == nullptr) {
        active_damage_pipeline->confirm_ready(
            world,
            server_time_us,
            current_tick,
            events);
    }
}

}  // namespace network_example
