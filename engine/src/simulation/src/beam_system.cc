#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "simulation/public/collision_query.h"

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

std::uint32_t tick_damage_units(
    const BeamState& beam,
    float fixed_delta_seconds) {
    if (fixed_delta_seconds <= 0.0f) {
        return 0;
    }
    const double units =
        static_cast<double>(beam.damage_per_second) *
        static_cast<double>(fixed_delta_seconds) *
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
    DamagePipeline local_damage_pipeline;
    DamagePipeline* active_damage_pipeline = damage_pipeline;
    if (active_damage_pipeline == nullptr) {
        active_damage_pipeline = &local_damage_pipeline;
    }

    std::vector<NetId> beams_to_destroy;
    auto view = world.registry().view<NetworkIdentity, Transform, BeamState, BeamTag>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        Transform& transform = view.get<Transform>(entity);
        BeamState& beam = view.get<BeamState>(entity);

        if (beam.expire_tick != 0 && current_tick >= beam.expire_tick) {
            beams_to_destroy.push_back(identity.net_id);
            continue;
        }
        if (beam.length <= 0.0f || beam.radius <= 0.0f ||
            beam.damage_per_second == 0) {
            continue;
        }

        transform.position = beam.origin;
        QueryFilter filter;
        filter.ignored_net_id = beam.shooter_net_id;
        filter.ignored_owner_peer = identity.owner_peer;
        filter.collision_mask = beam.collision_mask;
        const std::vector<QueryHit> hits = collect_swept_sphere_hits(
            world,
            beam.origin,
            beam.origin + beam.direction * beam.length,
            beam.radius,
            filter);

        const std::uint32_t damage_units =
            tick_damage_units(beam, fixed_delta_seconds);
        std::uint32_t sequence_id = 0;
        for (const QueryHit& hit : hits) {
            std::uint32_t& remainder = beam.damage_remainder_by_target[hit.net_id];
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
                hit.net_id,
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
