#include "simulation/public/lock_strike.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "physics/public/physics_world.h"
#include "simulation/public/collision_filter.h"

namespace network_example {
namespace {

// Who a lock may take, before its volume has a say: living actors on a side
// the shot hits, other than the shooter and anything its own peer
// controls. Positions come from the rewound frame when there is one -- where
// the shooter saw them -- and from the live world otherwise.
//
// The rewound frame carries no side, and it carries every hitbox, props
// included, so both are looked up live: an actor's side does not change while
// a shot is in flight. The side comes from the registry's per-entity index,
// not from entity_gameplay_collision_category, which walks every collider in
// the world for each candidate.
struct LockCandidates {
    std::vector<lock_targets::Candidate> candidates;
    std::vector<std::pair<NetId, std::uint16_t>> hit_zones;
};

LockCandidates lock_candidates(
    World& world,
    const HistoryFrame* rewind_frame,
    NetId shooter_net_id,
    PeerId shooter_peer_id,
    std::uint32_t collision_mask) {
    const std::uint32_t mask =
        collision_mask != 0u ? collision_mask : KERNEL_COLLISION_MASK_DAMAGEABLE;
    const World::ColliderRegistry& colliders = world.collider_registry();
    const auto side_of = [&colliders](NetId net_id) {
        std::uint32_t side = 0;
        for (const std::uint32_t index :
             colliders.entity_collider_indices(net_id)) {
            const ColliderInstance& collider = colliders.instances()[index];
            if (collider.lifetime_ticks == 0 && collider.enabled) {
                side |= collider.layer_mask;
            }
        }
        return side;
    };
    entt::registry& registry = world.registry();
    const auto may_lock = [&](NetId net_id) {
        if (net_id == 0 || net_id == shooter_net_id) {
            return false;
        }
        const std::optional<entt::entity> entity = world.find_entity(net_id);
        if (!entity.has_value() || !registry.all_of<EntityKind>(*entity) ||
            registry.get<EntityKind>(*entity).type != EntityType::kActor) {
            return false;
        }
        if (shooter_peer_id != 0 && registry.all_of<NetworkIdentity>(*entity) &&
            registry.get<NetworkIdentity>(*entity).owner_peer == shooter_peer_id) {
            return false;
        }
        const std::uint32_t side = side_of(net_id);
        return side == 0u || (side & mask) != 0u;
    };

    LockCandidates out;
    if (rewind_frame != nullptr && rewind_frame->valid) {
        for (const HitVolumeSnapshot& volume : rewind_frame->volumes) {
            if (volume.is_limb != 0u || volume.alive == 0u ||
                !may_lock(volume.net_id)) {
                continue;
            }
            out.candidates.push_back({volume.net_id, volume.center});
            out.hit_zones.emplace_back(volume.net_id, volume.hit_zone);
        }
        return out;
    }
    auto view = registry.view<NetworkIdentity, Transform, Hitbox>();
    for (const entt::entity entity : view) {
        const NetId net_id = view.get<NetworkIdentity>(entity).net_id;
        const bool alive =
            (!registry.all_of<Health>(entity) ||
             registry.get<Health>(entity).hp > 0u) &&
            !status_untargetable(world, entity);
        if (!alive || !may_lock(net_id)) {
            continue;
        }
        const Hitbox& hitbox = view.get<Hitbox>(entity);
        out.candidates.push_back(
            {net_id, view.get<Transform>(entity).position + hitbox.center});
        out.hit_zones.emplace_back(net_id, hitbox.hit_zone);
    }
    return out;
}

}  // namespace

std::uint16_t LockSelection::hit_zone_of(NetId net_id) const {
    for (const auto& [candidate, zone] : hit_zones) {
        if (candidate == net_id) {
            return zone;
        }
    }
    return kHitZoneUnscaled;
}

LockSelection select_lock_targets(
    World& world,
    const LockStrikeSpec& spec,
    const HistoryFrame* rewind_frame,
    NetId shooter_net_id,
    PeerId shooter_peer_id) {
    LockSelection selection;
    if (spec.volume.range <= 0.0f) {
        return selection;
    }

    const physics::PhysicsWorld* collision_world = world.collision_world();
    const lock_targets::LineBlocked behind_a_wall =
        [collision_world,
         ignored = spec.carrier_net_id != 0u ? spec.carrier_net_id
                                             : shooter_net_id](
            const glm::vec3& from, const glm::vec3& to) {
            if (collision_world == nullptr) {
                return false;
            }
            const glm::vec3 offset = to - from;
            const float distance = glm::length(offset);
            if (distance <= 0.0001f) {
                return false;
            }
            physics::RayCastRequest ray{};
            ray.origin = from;
            ray.direction = offset / distance;
            ray.max_distance = distance;
            ray.filter = collision_filter_from_mask(
                KERNEL_COLLISION_LAYER_TERRAIN |
                KERNEL_COLLISION_LAYER_STATIC_OBSTACLE);
            ray.filter.ignored_entity_net_id = ignored;
            physics::CollisionHit hit{};
            return collision_world->ray_cast_closest(ray, &hit) &&
                   hit.distance < distance;
        };

    LockCandidates found = lock_candidates(
        world,
        rewind_frame,
        shooter_net_id,
        shooter_peer_id,
        spec.collision_mask);
    selection.locked = lock_targets::select(
        std::move(found.candidates), spec.volume, behind_a_wall);
    selection.hit_zones = std::move(found.hit_zones);
    return selection;
}

void apply_lock_instant_damage(
    World& world,
    const LockStrikeSpec& spec,
    const LockSelection& selection,
    std::uint32_t current_tick,
    NetId shooter_net_id,
    PeerId shooter_peer_id,
    std::uint64_t hit_time_us,
    DamagePipeline* damage_pipeline) {
    if (damage_pipeline == nullptr) {
        return;
    }
    std::uint32_t sequence_id = 0;
    for (const lock_targets::Candidate& target : selection.locked) {
        (void)damage_pipeline->submit_damage_request(with_projectile_stagger(
            damage_request_at(
                current_tick,
                sequence_id++,
                shooter_net_id,
                target.net_id,
                shooter_peer_id,
                spec.weapon_id,
                spec.damage,
                hit_time_us,
                target.center,
                selection.hit_zone_of(target.net_id)),
            world,
            spec.projectile_template_id));
    }
}

bool launch_lock_missile(
    World& world,
    const LockStrikeSpec& spec,
    const lock_targets::Candidate& target,
    std::uint16_t hit_zone,
    std::uint16_t commit_index,
    std::uint32_t current_tick,
    float fixed_delta_seconds,
    std::uint64_t action_time_us,
    NetId shooter_net_id,
    PeerId shooter_peer_id,
    std::uint32_t action_instance_id,
    DamagePipeline* damage_pipeline,
    std::vector<LockMissileLaunch>* launches,
    bool report_origin) {
    if (damage_pipeline == nullptr || spec.volume.range <= 0.0f) {
        return false;
    }
    const RuntimeProjectileTemplate* shot =
        world.find_projectile_template(spec.projectile_template_id);
    const std::uint32_t max_flight_ticks = std::clamp<std::uint32_t>(
        shot == nullptr ? 1u : shot->lifetime_ticks, 1u, UINT8_MAX);
    const float meters_per_tick =
        spec.volume.range / static_cast<float>(max_flight_ticks);
    const float distance = glm::length(target.center - spec.volume.origin);
    const std::uint32_t flight_ticks = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::ceil(distance / meters_per_tick)),
        1u,
        max_flight_ticks);
    const std::uint32_t arrival_tick = current_tick + flight_ticks;
    // The dodge window is judged against this, so it is the arrival.
    const std::uint64_t arrival_time_us = fixed_delta_seconds > 0.0f
        ? static_cast<std::uint64_t>(
              static_cast<double>(arrival_tick) *
              static_cast<double>(fixed_delta_seconds) * 1000000.0)
        : action_time_us;

    DamageRequest request = with_projectile_stagger(
        damage_request_at(
            current_tick,
            commit_index,
            shooter_net_id,
            target.net_id,
            shooter_peer_id,
            spec.weapon_id,
            spec.damage,
            arrival_time_us,
            target.center,
            hit_zone),
        world,
        spec.projectile_template_id);
    request.apply_tick = arrival_tick;
    if (!damage_pipeline->submit_damage_request(request)) {
        return false;
    }
    if (launches != nullptr) {
        launches->push_back(LockMissileLaunch{
            shooter_net_id,
            shooter_peer_id,
            action_instance_id,
            commit_index,
            0u,
            target.net_id,
            current_tick,
            static_cast<std::uint8_t>(flight_ticks),
            report_origin,
            spec.volume.origin,
            spec.projectile_template_id,
        });
    }
    return true;
}

}  // namespace network_example
