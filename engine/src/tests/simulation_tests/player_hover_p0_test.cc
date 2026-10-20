// P0 of docs/PLAYER_HOVER_PLAN.md: can a player hover on the character
// controller itself -- step_character_at_velocity with a vertical velocity that
// seeks a fixed clearance -- instead of switching to the kHover controller?
//
// Bare PhysicsWorld and movement_solver only, no World and no ABI: what is
// measured is the step the plan proposes, before anything is built around it.
// The clearance probe here is a copy of player_movement.cc's probe_ground; P2
// moves that one somewhere both the authority and the prediction can call.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/movement_solver.h"

namespace {

using namespace network_example;

using physics::CharacterGroundState;
using physics::CollisionLayer;
using physics::CollisionObjectDescriptor;
using physics::CollisionObjectIdentity;
using physics::CollisionObjectKind;
using physics::CollisionShapeType;
using physics::PhysicsWorld;

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "player_hover_p0_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr float kTick = 1.0f / 30.0f;
constexpr float kClearance = 3.0f;      // the plan's hover height
constexpr float kVerticalSpeed = 4.0f;  // the plan's vertical speed
constexpr float kWalkSpeed = 5.0f;

void add_box(PhysicsWorld& physics, std::uint32_t id, glm::vec3 center, glm::vec3 half) {
    CollisionObjectDescriptor object{};
    object.identity = CollisionObjectIdentity{
        0, id, 0, CollisionObjectKind::kStaticObstacle, CollisionLayer::kStaticObstacle};
    object.shape.type = CollisionShapeType::kBox;
    object.shape.half_extents = half;
    object.position = center;
    std::string error;
    require(physics.upsert_object(object, &error));
}

// Feet at the position, 1.8 m tall, as the status_suspension_test player.
movement_solver::CharacterMovementConfig player_config() {
    movement_solver::CharacterMovementConfig config{};
    config.character_id = 7;
    config.shape.type = CollisionShapeType::kCapsule;
    config.shape.local_center = glm::vec3{0.0f, 0.9f, 0.0f};
    config.shape.radius = 0.35f;
    config.shape.capsule_half_height = 0.55f;
    config.gravity = glm::vec3{0.0f, -9.81f, 0.0f};
    config.max_slope_degrees = 50.0f;
    config.step_height = 0.4f;
    config.ground_snap_distance = 0.5f;
    config.filter.collision_mask =
        physics::collision_layer_bit(CollisionLayer::kTerrain) |
        physics::collision_layer_bit(CollisionLayer::kStaticObstacle);
    config.filter.ignored_entity_net_id = 7;
    return config;
}

// player_movement.cc's probe_ground and step_hover's use of it: the first
// walkable hit, else the first hit, max(3h, h + 10) down from the capsule.
struct Probe {
    bool hit = false;
    float below = 0.0f;
};

Probe probe_clearance(
    const PhysicsWorld& physics,
    const movement_solver::CharacterMovementConfig& config,
    const movement_solver::CharacterMovementState& state) {
    const float distance = std::max(3.0f * kClearance, kClearance + 10.0f);
    physics::ShapeCastRequest request{};
    request.shape = config.shape;
    request.start = state.position;
    request.rotation = state.rotation;
    request.displacement = glm::vec3{0.0f, -distance, 0.0f};
    request.filter = config.filter;
    const float walkable_y = std::cos(glm::radians(config.max_slope_degrees));
    Probe probe;
    for (const physics::CollisionHit& hit : physics.shape_cast_all(request)) {
        if (!probe.hit) {
            probe.hit = true;
            probe.below = distance * hit.fraction;
        }
        if (hit.normal.y >= walkable_y) {
            probe.hit = true;
            probe.below = distance * hit.fraction;
            break;
        }
    }
    return probe;
}

// The step the plan proposes (§2): horizontal as usual, vertical toward the
// clearance at no more than the vertical speed, held where it cannot see a
// floor, and never grounded.
void hover_step(
    PhysicsWorld& physics,
    const movement_solver::CharacterMovementConfig& config,
    glm::vec3 desired_horizontal,
    movement_solver::CharacterMovementState* state) {
    const Probe probe = probe_clearance(physics, config, *state);
    const float max_step = kVerticalSpeed;
    const float rise = probe.hit
        ? std::clamp((kClearance - probe.below) / kTick, -max_step, max_step)
        : 0.0f;
    state->ground_state = CharacterGroundState::kAirborne;
    std::string error;
    require(movement_solver::step_character_at_velocity(
        physics, config,
        glm::vec3{desired_horizontal.x, rise, desired_horizontal.z}, kTick,
        state, &error));
    state->ground_state = CharacterGroundState::kAirborne;
}

void walk_step(
    PhysicsWorld& physics,
    const movement_solver::CharacterMovementConfig& config,
    glm::vec3 desired_horizontal,
    movement_solver::CharacterMovementState* state) {
    std::string error;
    require(movement_solver::step_character(
        physics, config, desired_horizontal, kTick, state, &error));
}

// Standing on the ground the way a player is before it drinks the potion.
movement_solver::CharacterMovementState settled(
    PhysicsWorld& physics, const movement_solver::CharacterMovementConfig& config,
    glm::vec3 at) {
    movement_solver::CharacterMovementState state{};
    state.position = at;
    for (int i = 0; i < 30; ++i) {
        walk_step(physics, config, glm::vec3{0.0f}, &state);
    }
    require(state.ground_state == CharacterGroundState::kGrounded);
    require(std::fabs(state.position.y - at.y) < 0.05f);
    return state;
}

// (a) Flat ground: lifts off to the clearance and holds it while moving.
void flat_ground_holds_clearance() {
    PhysicsWorld physics;
    add_box(physics, 100, glm::vec3{0.0f, -0.5f, 0.0f}, glm::vec3{50.0f, 0.5f, 50.0f});
    const auto config = player_config();
    auto state = settled(physics, config, glm::vec3{-20.0f, 0.0f, 0.0f});

    int reached_tick = -1;
    float worst = 0.0f;
    for (int tick = 0; tick < 240; ++tick) {
        hover_step(physics, config, glm::vec3{kWalkSpeed * 0.15f, 0.0f, 0.0f}, &state);
        if (reached_tick < 0 && std::fabs(state.position.y - kClearance) < 0.05f) {
            reached_tick = tick;
        }
        if (reached_tick >= 0) {
            worst = std::max(worst, std::fabs(state.position.y - kClearance));
        }
    }
    std::printf("[a] flat: reached %.1f m at tick %d, worst hold error %.4f m, y=%.4f\n",
                kClearance, reached_tick, worst, state.position.y);
    // 3 m at 4 m/s is 0.75 s: 23 ticks, and a little for the last one.
    require(reached_tick >= 20 && reached_tick <= 26);
    require(worst < 0.05f);
}

// (b) A 1 m box on the way: rides up over it, comes back down after.
void climbs_over_box_and_back() {
    PhysicsWorld physics;
    add_box(physics, 100, glm::vec3{0.0f, -0.5f, 0.0f}, glm::vec3{50.0f, 0.5f, 50.0f});
    add_box(physics, 101, glm::vec3{0.0f, 0.5f, 0.0f}, glm::vec3{2.0f, 0.5f, 5.0f});
    const auto config = player_config();
    auto state = settled(physics, config, glm::vec3{-15.0f, 0.0f, 0.0f});
    for (int tick = 0; tick < 40; ++tick) {
        hover_step(physics, config, glm::vec3{0.0f}, &state);
    }
    require(std::fabs(state.position.y - kClearance) < 0.05f);

    float over_box_min = 1e9f;
    float over_box_max = -1e9f;
    for (int tick = 0; tick < 240; ++tick) {
        hover_step(physics, config, glm::vec3{kWalkSpeed, 0.0f, 0.0f}, &state);
        // Over the middle of the top, well clear of its edges.
        if (state.position.x > -1.0f && state.position.x < 1.0f) {
            over_box_min = std::min(over_box_min, state.position.y);
            over_box_max = std::max(over_box_max, state.position.y);
        }
    }
    std::printf("[b] 1 m box: y over its middle %.3f..%.3f, end x=%.2f y=%.4f\n",
                over_box_min, over_box_max, state.position.x, state.position.y);
    // Passed it, unblocked: 240 ticks at 5 m/s is 40 m from x = -15.
    require(state.position.x > 20.0f);
    // Rode up toward clearance above the top (1 + 3 m); at 5 m/s across a 4 m
    // box it gets most of the way, not all.
    require(over_box_max > kClearance + 0.5f);
    require(over_box_max < kClearance + 1.0f + 0.05f);
    require(std::fabs(state.position.y - kClearance) < 0.05f);
}

// A wall taller than the clearance: measured, not decided -- the plan reports
// what happens. The probe only sees the wall's top once over it, so the climb
// never starts from beside it.
void taller_wall_is_measured() {
    PhysicsWorld physics;
    add_box(physics, 100, glm::vec3{0.0f, -0.5f, 0.0f}, glm::vec3{50.0f, 0.5f, 50.0f});
    add_box(physics, 101, glm::vec3{0.0f, 2.5f, 0.0f}, glm::vec3{1.0f, 2.5f, 5.0f});
    const auto config = player_config();
    auto state = settled(physics, config, glm::vec3{-10.0f, 0.0f, 0.0f});
    for (int tick = 0; tick < 120; ++tick) {
        hover_step(physics, config, glm::vec3{kWalkSpeed, 0.0f, 0.0f}, &state);
    }
    std::printf("[wall] 5 m wall at x=-1: end x=%.2f y=%.3f\n",
                state.position.x, state.position.y);
    require(std::isfinite(state.position.x) && std::isfinite(state.position.y));
}

// Off a cliff: past the probe's reach it holds the height it has.
void holds_height_past_a_cliff() {
    PhysicsWorld physics;
    // Ground only for x < 0, its top at y = 0; nothing beneath beyond it.
    add_box(physics, 100, glm::vec3{-25.0f, -0.5f, 0.0f}, glm::vec3{25.0f, 0.5f, 25.0f});
    const auto config = player_config();
    auto state = settled(physics, config, glm::vec3{-10.0f, 0.0f, 0.0f});
    for (int tick = 0; tick < 40; ++tick) {
        hover_step(physics, config, glm::vec3{0.0f}, &state);
    }
    // Crossing the edge the capsule's round bottom is cast against the corner,
    // which reads as more clearance than there is, so it sinks a little on
    // the way over -- step_hover's probe does the same for an airship. Once
    // nothing is in reach it holds whatever height that left it at.
    float lowest_past = 1e9f;
    float highest_past = -1e9f;
    for (int tick = 0; tick < 150; ++tick) {
        hover_step(physics, config, glm::vec3{kWalkSpeed, 0.0f, 0.0f}, &state);
        if (state.position.x > 1.0f) {
            lowest_past = std::min(lowest_past, state.position.y);
            highest_past = std::max(highest_past, state.position.y);
        }
    }
    std::printf("[cliff] end x=%.2f y=%.4f past the edge y %.4f..%.4f\n",
                state.position.x, state.position.y, lowest_past, highest_past);
    require(state.position.x > 10.0f);
    require(lowest_past > kClearance - 0.25f);
    require(highest_past - lowest_past < 0.001f);
}

// (d) The switch both ways on one CharacterVirtual: walk, hover, then the
// status ends and plain step_character takes it down -- no reset in between.
void switches_both_ways_without_reset() {
    PhysicsWorld physics;
    add_box(physics, 100, glm::vec3{0.0f, -0.5f, 0.0f}, glm::vec3{50.0f, 0.5f, 50.0f});
    const auto config = player_config();
    auto state = settled(physics, config, glm::vec3{0.0f, 0.0f, 0.0f});
    for (int tick = 0; tick < 60; ++tick) {
        hover_step(physics, config, glm::vec3{kWalkSpeed, 0.0f, 0.0f}, &state);
    }
    require(std::fabs(state.position.y - kClearance) < 0.05f);
    // Leaves hover with whatever vertical velocity the last step resolved.
    require(std::fabs(state.velocity.y) < 0.5f);

    int landed_tick = -1;
    for (int tick = 0; tick < 90; ++tick) {
        walk_step(physics, config, glm::vec3{kWalkSpeed, 0.0f, 0.0f}, &state);
        if (landed_tick < 0 && state.ground_state == CharacterGroundState::kGrounded) {
            landed_tick = tick;
        }
    }
    std::printf("[switch] landed after %d ticks, y=%.4f vy=%.4f\n",
                landed_tick, state.position.y, state.velocity.y);
    // sqrt(2 * 3 / 9.81) = 0.78 s, 23 ticks, give or take the snap distance.
    require(landed_tick >= 18 && landed_tick <= 26);
    require(std::fabs(state.position.y) < 0.05f);
}

// (c) The authority and an owner's prediction run this step on separate
// physics worlds with the same static scene. Same inputs, same answers. (The
// real two-process check is P4; this rules out the step itself diverging.)
void two_worlds_agree() {
    PhysicsWorld authority;
    PhysicsWorld prediction;
    for (PhysicsWorld* physics : {&authority, &prediction}) {
        add_box(*physics, 100, glm::vec3{0.0f, -0.5f, 0.0f}, glm::vec3{50.0f, 0.5f, 50.0f});
        add_box(*physics, 101, glm::vec3{0.0f, 0.5f, 0.0f}, glm::vec3{2.0f, 0.5f, 5.0f});
        add_box(*physics, 102, glm::vec3{6.0f, 2.5f, 3.0f}, glm::vec3{1.0f, 2.5f, 1.0f});
    }
    const auto config = player_config();
    auto a = settled(authority, config, glm::vec3{-10.0f, 0.0f, 0.0f});
    auto p = settled(prediction, config, glm::vec3{-10.0f, 0.0f, 0.0f});
    float worst = 0.0f;
    for (int tick = 0; tick < 300; ++tick) {
        const float angle = 0.02f * static_cast<float>(tick);
        const glm::vec3 move{std::cos(angle) * kWalkSpeed, 0.0f, std::sin(angle) * kWalkSpeed};
        hover_step(authority, config, move, &a);
        hover_step(prediction, config, move, &p);
        worst = std::max(worst, glm::length(a.position - p.position));
    }
    std::printf("[parity] worst authority/prediction gap over 300 ticks %.6f m\n", worst);
    require(worst < 0.0001f);

    // Control: one tick of different input must show up, or the gap above
    // proves nothing.
    hover_step(authority, config, glm::vec3{kWalkSpeed, 0.0f, 0.0f}, &a);
    hover_step(prediction, config, glm::vec3{0.0f, 0.0f, kWalkSpeed}, &p);
    const float control_gap = glm::length(a.position - p.position);
    std::printf("[parity] control gap after one differing input %.4f m\n", control_gap);
    require(control_gap > 0.1f);
}

}  // namespace

int main() {
    flat_ground_holds_clearance();
    climbs_over_box_and_back();
    taller_wall_is_measured();
    holds_height_past_a_cliff();
    switches_both_ways_without_reset();
    two_worlds_agree();
    std::printf("player_hover_p0_test passed\n");
    return 0;
}
