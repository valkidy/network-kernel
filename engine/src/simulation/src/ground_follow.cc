#include "simulation/public/ground_follow.h"

#include <cmath>
#include <optional>
#include <vector>

namespace network_example::ground_follow {
namespace {

bool walkable(const glm::vec3& normal, float max_slope_degrees) {
    return normal.y >= std::cos(glm::radians(max_slope_degrees));
}

enum class Ground : std::uint8_t {
    // Nothing within reach: a cliff, or a descent steeper than probe_depth.
    kNone,
    // Ground the field can ride.
    kWalkable,
    // Too steep to ride, or the probe started inside something -- an obstacle
    // taller than step_up. Either way the field does not go there.
    kBlocked,
};

struct Probe {
    Ground ground = Ground::kNone;
    float height = 0.0f;
};

Probe probe_ground(
    const physics::PhysicsWorld& physics_world,
    const Config& config,
    const glm::vec3& at,
    float ground_level) {
    physics::RayCastRequest request{};
    request.origin = glm::vec3{at.x, ground_level + config.step_up, at.z};
    request.direction = glm::vec3{0.0f, -1.0f, 0.0f};
    request.max_distance = config.step_up + config.probe_depth;
    request.filter = config.filter;
    physics::CollisionHit hit{};
    if (!physics_world.ray_cast_closest(request, &hit)) {
        return {};
    }
    // A ray that starts inside a solid reports it at once, with whatever normal
    // the inside of the shape gives. That is not a floor, it is a wall the
    // probe was started behind.
    if (hit.fraction <= 0.0f ||
        !walkable(hit.normal, config.max_slope_degrees)) {
        return Probe{Ground::kBlocked, 0.0f};
    }
    return Probe{Ground::kWalkable, hit.position.y};
}

// Where along from -> to the first thing too steep to ride was met, as a
// fraction of the step. Walkable contacts are passed over: on a slope the
// swept volume runs alongside the ground it is riding, and that ground is the
// probe's business, not a reason to stop.
std::optional<float> first_blocking_fraction(
    const physics::PhysicsWorld& physics_world,
    const Config& config,
    const glm::vec3& from,
    const glm::vec3& to) {
    const glm::vec3 displacement = to - from;
    const float length = glm::length(displacement);
    if (length <= 0.000001f) {
        return std::nullopt;
    }
    std::vector<physics::CollisionHit> hits;
    if (config.sweep_radius > 0.0f) {
        physics::ShapeCastRequest request{};
        request.shape.type = physics::CollisionShapeType::kSphere;
        request.shape.radius = config.sweep_radius;
        request.start = from;
        request.displacement = displacement;
        request.filter = config.filter;
        hits = physics_world.shape_cast_all(request);
    } else {
        physics::RayCastRequest request{};
        request.origin = from;
        request.direction = displacement / length;
        request.max_distance = length;
        request.filter = config.filter;
        hits = physics_world.ray_cast_all(request);
    }
    // Sorted nearest first by the physics world.
    for (const physics::CollisionHit& hit : hits) {
        if (!walkable(hit.normal, config.max_slope_degrees)) {
            return hit.fraction;
        }
    }
    return std::nullopt;
}

}  // namespace

bool settle(
    const physics::PhysicsWorld& physics_world,
    const Config& config,
    float max_drop,
    State* state) {
    if (state == nullptr || max_drop <= 0.0f) {
        return false;
    }
    // From step_up above the spawn point, not the point itself: a field
    // spawned where something broke on the ground starts on the surface, and
    // a ray from there reads as starting inside it.
    physics::RayCastRequest request{};
    request.origin = state->position + glm::vec3{0.0f, config.step_up, 0.0f};
    request.direction = glm::vec3{0.0f, -1.0f, 0.0f};
    request.max_distance = config.step_up + max_drop;
    request.filter = config.filter;
    physics::CollisionHit hit{};
    if (!physics_world.ray_cast_closest(request, &hit) ||
        hit.fraction <= 0.0f ||
        !walkable(hit.normal, config.max_slope_degrees)) {
        return false;
    }
    state->position.y = hit.position.y + config.hover_height;
    return true;
}

StepResult step(
    const physics::PhysicsWorld& physics_world,
    const Config& config,
    float fixed_delta_seconds,
    State* state) {
    StepResult result{};
    if (state == nullptr || state->parked || fixed_delta_seconds <= 0.0f) {
        return result;
    }
    const glm::vec3 horizontal{
        config.horizontal_velocity.x, 0.0f, config.horizontal_velocity.z};
    if (glm::dot(horizontal, horizontal) <= 0.0f) {
        return result;
    }

    const glm::vec3 from = state->position;
    const glm::vec3 ahead = from + horizontal * fixed_delta_seconds;
    const Probe ground =
        probe_ground(physics_world, config, ahead, from.y - config.hover_height);

    glm::vec3 to = ahead;
    if (ground.ground == Ground::kWalkable) {
        to.y = ground.height + config.hover_height;
        result.grounded = true;
    }
    // kNone holds the height `ahead` already has. kBlocked still sweeps
    // towards it: the sweep says where the obstacle was met, and only if it
    // finds nothing does the field stop where it stands.

    const std::optional<float> blocked_at =
        first_blocking_fraction(physics_world, config, from, to);
    if (blocked_at.has_value() || ground.ground == Ground::kBlocked) {
        const float fraction = blocked_at.value_or(0.0f);
        state->position = from + (to - from) * fraction;
        state->parked = true;
        result.stopped = true;
        result.grounded = false;
    } else {
        state->position = to;
    }
    result.velocity = (state->position - from) / fixed_delta_seconds;
    return result;
}

}  // namespace network_example::ground_follow
