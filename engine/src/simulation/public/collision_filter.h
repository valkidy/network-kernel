#ifndef SIMULATION_PUBLIC_COLLISION_FILTER_H_
#define SIMULATION_PUBLIC_COLLISION_FILTER_H_

#include <cstdint>

#include "kernel/public/kernel_types.h"
#include "physics/public/collision_types.h"

namespace network_example {

// What separates "this hit an actor" from "this hit the world".
//
// Every query that resolves a hit into a target asks this question, and until
// these existed each one answered it by spelling out kActorHitbox inline -- in
// seven places across four systems. That is fine while an actor has exactly one
// damageable volume and becomes a trap the moment it has more: a rig's per-bone
// limbs are a second kind of actor volume, and a site left behind does not fail,
// it silently reclassifies a leg as scenery.
//
// So the answer lives here once. Widening what counts as hitting an actor is an
// edit to these three functions and nothing else.
//
// The pair is a pair on purpose: a broad phase query is filtered on the layer
// AND on the object kind, so anything that narrows one has to narrow the other
// to match, or it silently keeps volumes it meant to drop.
constexpr std::uint32_t actor_hit_layer_mask() {
    return physics::collision_layer_bit(physics::CollisionLayer::kDamageable);
}

constexpr std::uint32_t actor_hit_kind_mask() {
    return 1u << static_cast<std::uint32_t>(
               physics::CollisionObjectKind::kActorHitbox);
}

constexpr bool is_actor_hit(physics::CollisionObjectKind kind) {
    return (actor_hit_kind_mask() &
            (1u << static_cast<std::uint32_t>(kind))) != 0u;
}

// Pins today's answer, so that changing it is a deliberate edit here rather
// than something that drifts. The limb line is the load-bearing one: a rig's
// bones are an actor's volumes but are NOT damageable volumes yet, and nothing
// downstream is ready for them to become so -- lag compensation rewinds
// hitboxes only, so a leg hit on a moving rig would resolve against a leg that
// is no longer there.
static_assert(is_actor_hit(physics::CollisionObjectKind::kActorHitbox));
static_assert(!is_actor_hit(physics::CollisionObjectKind::kActorLimb));
static_assert(!is_actor_hit(physics::CollisionObjectKind::kActorMovement));
static_assert(!is_actor_hit(physics::CollisionObjectKind::kTerrain));
static_assert(!is_actor_hit(physics::CollisionObjectKind::kStaticObstacle));

inline physics::CollisionQueryFilter collision_filter_from_mask(
    std::uint32_t mask) {
    physics::CollisionQueryFilter filter{};
    filter.collision_mask = 0u;
    filter.object_kind_mask = 0u;
    filter.gameplay_category_mask = mask & KERNEL_COLLISION_MASK_ACTOR;
    if ((mask & KERNEL_COLLISION_MASK_ACTOR) != 0u) {
        filter.collision_mask |= actor_hit_layer_mask();
        filter.object_kind_mask |= actor_hit_kind_mask();
    }
    if ((mask & KERNEL_COLLISION_MASK_PROP) != 0u) {
        filter.collision_mask |= physics::collision_layer_bit(
            physics::CollisionLayer::kStaticObstacle);
        filter.object_kind_mask |=
            1u << static_cast<std::uint32_t>(
                physics::CollisionObjectKind::kStaticObstacle);
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
