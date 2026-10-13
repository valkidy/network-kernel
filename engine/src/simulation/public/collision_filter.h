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
    return physics::collision_layer_bit(physics::CollisionLayer::kDamageable) |
        physics::collision_layer_bit(physics::CollisionLayer::kActorLimb);
}

constexpr std::uint32_t actor_hit_kind_mask() {
    return (1u << static_cast<std::uint32_t>(
                physics::CollisionObjectKind::kActorHitbox)) |
        (1u << static_cast<std::uint32_t>(
             physics::CollisionObjectKind::kActorLimb));
}

constexpr bool is_actor_hit(physics::CollisionObjectKind kind) {
    return (actor_hit_kind_mask() &
            (1u << static_cast<std::uint32_t>(kind))) != 0u;
}

// Pins the answer, so that changing it is a deliberate edit here rather than
// something that drifts.
//
// A limb counts: a leg resolves to the actor wearing it rather than to
// scenery. Since ABI 106 every limb belongs to an actor hit on its bones, and
// collision_filter_from_mask hands those bones to any mask that reaches actors.
//
// The movement capsule stays out. It is where an actor stands, not what it can
// be hit on, and admitting it would let every weapon shoot the volume that
// exists to stop people walking through each other.
static_assert(is_actor_hit(physics::CollisionObjectKind::kActorHitbox));
static_assert(is_actor_hit(physics::CollisionObjectKind::kActorLimb));
static_assert(!is_actor_hit(physics::CollisionObjectKind::kActorMovement));
static_assert(!is_actor_hit(physics::CollisionObjectKind::kTerrain));
static_assert(!is_actor_hit(physics::CollisionObjectKind::kStaticObstacle));

inline physics::CollisionQueryFilter collision_filter_from_mask(
    std::uint32_t mask) {
    physics::CollisionQueryFilter filter{};
    filter.collision_mask = 0u;
    filter.object_kind_mask = 0u;
    filter.gameplay_category_mask = mask & KERNEL_COLLISION_MASK_ACTOR;
    // Spelled out rather than reusing actor_hit_*_mask(), which answers a
    // different question: what counts as hitting an actor when a result is
    // classified. This says what the word "actor" turns on in an authored mask.
    //
    // Since ABI 106 that includes a rig's bones. Every limb belongs to an
    // actor whose bounds are not a body (KernelColliderPurpose_Bounds), so its
    // bones are the only thing there is to hit; a weapon that reached actors
    // and not limbs would pass straight through it. A limb carries its actor's
    // side as its gameplay category, so the side bits still decide whose bones.
    if ((mask & KERNEL_COLLISION_MASK_ACTOR) != 0u) {
        filter.collision_mask |=
            physics::collision_layer_bit(physics::CollisionLayer::kDamageable) |
            physics::collision_layer_bit(physics::CollisionLayer::kActorLimb);
        filter.object_kind_mask |=
            (1u << static_cast<std::uint32_t>(
                 physics::CollisionObjectKind::kActorHitbox)) |
            (1u << static_cast<std::uint32_t>(
                 physics::CollisionObjectKind::kActorLimb));
    }
    // The explicit token, kept for a limb without bounds should one ever
    // exist; every limb today is already reached by the side bits above.
    if ((mask & KERNEL_COLLISION_LAYER_LIMB) != 0u) {
        filter.collision_mask |=
            physics::collision_layer_bit(physics::CollisionLayer::kActorLimb);
        filter.object_kind_mask |=
            1u << static_cast<std::uint32_t>(
                physics::CollisionObjectKind::kActorLimb);
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
