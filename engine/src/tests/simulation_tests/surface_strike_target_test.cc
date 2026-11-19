// Where a surface strike lands (KernelWeaponFireMode_TargetedStrikeSurface,
// ABI 110, docs/PLAYER_WEAPON_REVAMP_PLAN.md I3): the first wall or floor
// along the aim, through actors; else the ground under the end of the reach;
// else nowhere, which refuses the shot. The face's normal comes with it.
//
// A 100 m floor whose top is y = 1, and a wall whose near face is x = 10.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <source_location>
#include <string>

#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/simulation.h"

namespace {

namespace ne = network_example;
namespace phys = network_example::physics;

void require(
    bool condition,
    const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(
            stderr,
            "require failed at %s:%u\n",
            location.file_name(),
            location.line());
        std::abort();
    }
}

bool near(const glm::vec3& a, const glm::vec3& b, float tolerance) {
    return glm::length(a - b) <= tolerance;
}

void add_box(phys::PhysicsWorld* physics,
             std::uint32_t collider_id,
             std::uint32_t entity_net_id,
             phys::CollisionObjectKind kind,
             phys::CollisionLayer layer,
             const glm::vec3& center,
             const glm::vec3& half_extents) {
    phys::CollisionObjectDescriptor box;
    box.identity = phys::CollisionObjectIdentity{entity_net_id, collider_id, 0, kind, layer};
    box.shape.type = phys::CollisionShapeType::kBox;
    box.shape.half_extents = half_extents;
    box.position = center;
    std::string error;
    require(physics->upsert_object(box, &error));
}

}  // namespace

int main() {
    phys::PhysicsWorld physics;
    add_box(&physics, 200, 0, phys::CollisionObjectKind::kTerrain,
            phys::CollisionLayer::kTerrain, {0.0f, 0.5f, 0.0f}, {50.0f, 0.5f, 50.0f});
    add_box(&physics, 201, 0, phys::CollisionObjectKind::kStaticObstacle,
            phys::CollisionLayer::kStaticObstacle, {11.0f, 4.0f, 0.0f}, {1.0f, 3.0f, 5.0f});
    // An actor standing between the shooter and the wall: the ray passes it.
    add_box(&physics, 202, 9, phys::CollisionObjectKind::kActorHitbox,
            phys::CollisionLayer::kDamageable, {5.0f, 2.0f, 0.0f}, {0.4f, 1.0f, 0.4f});

    const glm::vec3 eye{0.0f, 2.0f, 0.0f};
    constexpr ne::NetId kShooter = 1;

    // A wall straight ahead, through the actor: its face and its normal.
    {
        const std::optional<ne::SurfaceStrikeTarget> target =
            ne::resolve_surface_strike_target(&physics, kShooter, eye, {1.0f, 0.0f, 0.0f}, 45.0f);
        require(target.has_value());
        require(near(target->point, {10.0f, 2.0f, 0.0f}, 1e-3f));
        require(near(target->normal, {-1.0f, 0.0f, 0.0f}, 1e-3f));
    }

    // Beyond reach, the wall is not seen: the ground under the end of it.
    {
        const std::optional<ne::SurfaceStrikeTarget> target =
            ne::resolve_surface_strike_target(&physics, kShooter, eye, {1.0f, 0.0f, 0.0f}, 8.0f);
        require(target.has_value());
        require(near(target->point, {8.0f, 1.0f, 0.0f}, 1e-3f));
        require(near(target->normal, {0.0f, 1.0f, 0.0f}, 1e-3f));
    }

    // Aimed down at the floor within reach: that floor point (plan D4).
    {
        const glm::vec3 down = glm::normalize(glm::vec3{1.0f, -1.0f, 0.0f});
        const std::optional<ne::SurfaceStrikeTarget> target =
            ne::resolve_surface_strike_target(&physics, kShooter, eye, down, 45.0f);
        require(target.has_value());
        require(near(target->point, {1.0f, 1.0f, 0.0f}, 1e-3f));
        require(near(target->normal, {0.0f, 1.0f, 0.0f}, 1e-3f));
    }

    // Level into open ground, away from the wall: the ground at max range.
    {
        const std::optional<ne::SurfaceStrikeTarget> target =
            ne::resolve_surface_strike_target(&physics, kShooter, eye, {-1.0f, 0.0f, 0.0f}, 20.0f);
        require(target.has_value());
        require(near(target->point, {-20.0f, 1.0f, 0.0f}, 1e-3f));
    }

    // Up into the sky still lands, on the ground under the end of the reach
    // (rule c): 45 m at 45 degrees ends ~31.8 m out and ~33.8 m up, and the
    // drop reaches as far as the reach does.
    {
        const glm::vec3 up = glm::normalize(glm::vec3{-1.0f, 1.0f, 0.0f});
        const std::optional<ne::SurfaceStrikeTarget> target =
            ne::resolve_surface_strike_target(&physics, kShooter, eye, up, 45.0f);
        require(target.has_value());
        require(near(target->point, {-45.0f / std::sqrt(2.0f), 1.0f, 0.0f}, 1e-3f));
    }
    // Too high for the drop to reach the ground, or past the edge of the
    // floor: nowhere to land, so the weapon refuses.
    require(!ne::resolve_surface_strike_target(
                 &physics, kShooter, eye, glm::normalize(glm::vec3{-0.2f, 1.0f, 0.0f}), 45.0f)
                 .has_value());
    require(!ne::resolve_surface_strike_target(
                 &physics, kShooter, eye, {-1.0f, 0.0f, 0.0f}, 80.0f)
                 .has_value());

    // Degenerate input.
    require(!ne::resolve_surface_strike_target(nullptr, kShooter, eye, {1.0f, 0.0f, 0.0f}, 45.0f)
                 .has_value());
    require(!ne::resolve_surface_strike_target(&physics, kShooter, eye, {1.0f, 0.0f, 0.0f}, 0.0f)
                 .has_value());
    require(!ne::resolve_surface_strike_target(&physics, kShooter, eye, {0.0f, 0.0f, 0.0f}, 45.0f)
                 .has_value());

    std::puts("surface_strike_target_test passed");
    return 0;
}
