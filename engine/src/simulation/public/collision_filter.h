#ifndef SIMULATION_PUBLIC_COLLISION_FILTER_H_
#define SIMULATION_PUBLIC_COLLISION_FILTER_H_

#include <cstdint>

#include "kernel/public/kernel_types.h"
#include "physics/public/collision_types.h"

namespace network_example {

inline physics::CollisionQueryFilter collision_filter_from_mask(
    std::uint32_t mask) {
    physics::CollisionQueryFilter filter{};
    filter.collision_mask = 0u;
    filter.object_kind_mask = 0u;
    filter.gameplay_category_mask = mask & KERNEL_COLLISION_MASK_ACTOR;
    if ((mask & KERNEL_COLLISION_MASK_ACTOR) != 0u) {
        filter.collision_mask |= physics::collision_layer_bit(
            physics::CollisionLayer::kDamageable);
        filter.object_kind_mask |=
            1u << static_cast<std::uint32_t>(
                physics::CollisionObjectKind::kActorHitbox);
    }
    if ((mask & KERNEL_COLLISION_LAYER_TERRAIN) != 0u) {
        filter.collision_mask |= physics::collision_layer_bit(
            physics::CollisionLayer::kTerrain);
        filter.object_kind_mask |=
            1u << static_cast<std::uint32_t>(
                physics::CollisionObjectKind::kTerrain);
    }
    if ((mask & KERNEL_COLLISION_LAYER_STATIC_OBSTACLE) != 0u) {
        filter.collision_mask |= physics::collision_layer_bit(
            physics::CollisionLayer::kStaticObstacle);
        filter.object_kind_mask |=
            1u << static_cast<std::uint32_t>(
                physics::CollisionObjectKind::kStaticObstacle);
    }
    return filter;
}

}  // namespace network_example

#endif  // SIMULATION_PUBLIC_COLLISION_FILTER_H_
