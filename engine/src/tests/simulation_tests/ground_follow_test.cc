#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/ground_follow.h"

namespace {

namespace ground_follow = network_example::ground_follow;
using network_example::physics::CollisionLayer;
using network_example::physics::CollisionObjectDescriptor;
using network_example::physics::CollisionObjectIdentity;
using network_example::physics::CollisionObjectKind;
using network_example::physics::CollisionShapeType;
using network_example::physics::PhysicsWorld;
using network_example::physics::collision_layer_bit;

constexpr float kDt = 1.0f / 30.0f;
constexpr float kPi = 3.14159265358979f;

bool near(float actual, float expected, float tolerance = 0.01f) {
    return std::fabs(actual - expected) <= tolerance;
}

struct Scene {
    PhysicsWorld physics;
    std::uint32_t next_collider_id = 1;

    void add_box(
        const glm::vec3& center,
        const glm::vec3& half_extents,
        const glm::quat& rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f}) {
        CollisionObjectDescriptor object{};
        object.identity = CollisionObjectIdentity{
            0,
            next_collider_id++,
            0,
            CollisionObjectKind::kTerrain,
            CollisionLayer::kTerrain,
        };
        object.shape.type = CollisionShapeType::kBox;
        object.shape.half_extents = half_extents;
        object.position = center;
        object.rotation = rotation;
        std::string error;
        assert(physics.upsert_object(object, &error));
    }

    // Ground with its top at y = 0 for x in [min_x, max_x].
    void add_flat(float min_x, float max_x, float top = 0.0f) {
        add_box(
            glm::vec3{(min_x + max_x) * 0.5f, top - 0.5f, 0.0f},
            glm::vec3{(max_x - min_x) * 0.5f, 0.5f, 10.0f});
    }

    // A ramp whose top face starts at (start_x, 0) and rises towards +x at
    // the given angle for `length` metres along the slope. The flat ground
    // before it should run on underneath: a probe landing exactly on the seam
    // of two boxes that only meet edge to edge hits neither, which reads as a
    // one-tick cliff. Authored terrain overlaps its pieces the same way.
    void add_ramp(float start_x, float degrees, float length = 10.0f) {
        const float angle = degrees * kPi / 180.0f;
        const glm::quat rotation =
            glm::angleAxis(angle, glm::vec3{0.0f, 0.0f, 1.0f});
        const glm::vec3 top_center{
            start_x + 0.5f * length * std::cos(angle),
            0.5f * length * std::sin(angle),
            0.0f};
        const glm::vec3 up = rotation * glm::vec3{0.0f, 1.0f, 0.0f};
        add_box(
            top_center - up * 0.5f,
            glm::vec3{0.5f * length, 0.5f, 10.0f},
            rotation);
    }
};

ground_follow::Config config_moving(float vx, float sweep_radius = 0.0f) {
    ground_follow::Config config{};
    config.horizontal_velocity = glm::vec3{vx, 0.0f, 0.0f};
    config.hover_height = 1.0f;
    config.max_slope_degrees = 50.0f;
    config.step_up = 0.5f;
    config.probe_depth = 0.5f;
    config.sweep_radius = sweep_radius;
    config.filter.collision_mask = collision_layer_bit(CollisionLayer::kTerrain);
    return config;
}

// Steps until x passes `until_x` (or the field parks), checking every tick.
template <typename Check>
void run_until(
    const Scene& scene,
    const ground_follow::Config& config,
    ground_follow::State* state,
    float until_x,
    Check check) {
    for (int tick = 0; tick < 600; ++tick) {
        const ground_follow::StepResult result =
            ground_follow::step(scene.physics, config, kDt, state);
        check(*state, result);
        if (state->parked) {
            return;
        }
        if ((config.horizontal_velocity.x > 0.0f && state->position.x >= until_x) ||
            (config.horizontal_velocity.x < 0.0f && state->position.x <= until_x)) {
            return;
        }
    }
    assert(false && "never reached until_x");
}

void rides_flat_ground_at_hover_height() {
    Scene scene;
    scene.add_flat(-20.0f, 20.0f);
    const ground_follow::Config config = config_moving(6.0f);
    ground_follow::State state{glm::vec3{0.0f, 1.0f, 0.0f}};
    int ticks = 0;
    // 5.99, not 6: thirty steps of 0.2 add up to a hair under six in float.
    run_until(scene, config, &state, 5.99f, [&](const auto& s, const auto& r) {
        ++ticks;
        assert(r.grounded);
        assert(!r.stopped);
        assert(near(s.position.y, 1.0f, 0.0001f));
        assert(near(r.velocity.x, 6.0f, 0.001f));
    });
    assert(ticks == 30);
    assert(near(state.position.x, 6.0f, 0.001f));
}

void climbs_a_walkable_slope() {
    Scene scene;
    scene.add_flat(-20.0f, 3.0f);
    scene.add_ramp(2.0f, 30.0f);
    // A swept sphere as well as the probe: on the slope it runs alongside the
    // ground, and walkable contacts must not stop it.
    const ground_follow::Config config = config_moving(6.0f, 0.3f);
    ground_follow::State state{glm::vec3{0.0f, 1.0f, 0.0f}};
    run_until(scene, config, &state, 6.0f, [](const auto& s, const auto& r) {
        assert(r.grounded);
        assert(!s.parked);
        const float ground = s.position.x <= 2.0f
            ? 0.0f
            : std::tan(30.0f * kPi / 180.0f) * (s.position.x - 2.0f);
        assert(near(s.position.y, ground + 1.0f));
    });
    assert(state.position.y > 2.5f);
}

void stops_at_a_slope_too_steep_to_climb() {
    Scene scene;
    scene.add_flat(-20.0f, 3.0f);
    scene.add_ramp(2.0f, 60.0f);
    const ground_follow::Config config = config_moving(6.0f);
    ground_follow::State state{glm::vec3{0.0f, 1.0f, 0.0f}};
    run_until(scene, config, &state, 20.0f, [](const auto&, const auto&) {});
    assert(state.parked);
    // Never onto the face: the probe meets it at half a metre up, the ray at
    // the centre's metre would meet it at x = 2 + 1/tan(60).
    assert(state.position.x < 2.0f + 1.0f / std::tan(60.0f * kPi / 180.0f));
    assert(near(state.position.y, 1.0f, 0.0001f));

    // Parked is for good: no movement and no claim of ground.
    const glm::vec3 parked_at = state.position;
    const ground_follow::StepResult after =
        ground_follow::step(scene.physics, config, kDt, &state);
    assert(state.position == parked_at);
    assert(after.velocity == glm::vec3{0.0f});
    assert(!after.stopped);
}

void follows_a_walkable_slope_down() {
    Scene scene;
    scene.add_flat(-20.0f, 3.0f);
    scene.add_ramp(2.0f, 30.0f);
    const float tan30 = std::tan(30.0f * kPi / 180.0f);
    const ground_follow::Config config = config_moving(-6.0f, 0.3f);
    ground_follow::State state{glm::vec3{7.0f, tan30 * 5.0f + 1.0f, 0.0f}};
    run_until(scene, config, &state, -2.0f, [&](const auto& s, const auto& r) {
        assert(r.grounded);
        assert(!s.parked);
        const float ground =
            s.position.x <= 2.0f ? 0.0f : tan30 * (s.position.x - 2.0f);
        assert(near(s.position.y, ground + 1.0f));
    });
    assert(near(state.position.y, 1.0f, 0.0001f));
}

void holds_its_height_off_a_cliff() {
    Scene scene;
    scene.add_flat(-20.0f, 0.0f, 3.0f);
    scene.add_flat(0.0f, 20.0f, 0.0f);
    const ground_follow::Config config = config_moving(6.0f);
    ground_follow::State state{glm::vec3{-2.0f, 4.0f, 0.0f}};
    bool left_the_edge = false;
    run_until(scene, config, &state, 4.0f, [&](const auto& s, const auto& r) {
        assert(!s.parked);
        assert(near(s.position.y, 4.0f, 0.0001f));
        if (s.position.x > 0.1f) {
            assert(!r.grounded);
            left_the_edge = true;
        }
    });
    assert(left_the_edge);
}

void stops_at_a_wall_with_its_sweep_against_it() {
    Scene scene;
    scene.add_flat(-20.0f, 20.0f);
    // Face at x = 3, far taller than the field.
    scene.add_box(glm::vec3{3.5f, 3.0f, 0.0f}, glm::vec3{0.5f, 3.0f, 10.0f});
    const ground_follow::Config config = config_moving(6.0f, 0.3f);
    ground_follow::State state{glm::vec3{0.0f, 1.0f, 0.0f}};
    int stopped_ticks = 0;
    run_until(scene, config, &state, 20.0f, [&](const auto&, const auto& r) {
        stopped_ticks += r.stopped ? 1 : 0;
    });
    assert(state.parked);
    assert(stopped_ticks == 1);
    // The sphere touching the face, not the centre in it.
    assert(near(state.position.x, 3.0f - 0.3f, 0.02f));
    assert(near(state.position.y, 1.0f, 0.0001f));
}

void steps_onto_a_ledge_under_step_up_and_stops_at_one_over_it() {
    {
        Scene scene;
        scene.add_flat(-20.0f, 20.0f);
        scene.add_box(glm::vec3{5.0f, 0.15f, 0.0f}, glm::vec3{2.0f, 0.15f, 10.0f});
        const ground_follow::Config config = config_moving(6.0f);
        ground_follow::State state{glm::vec3{0.0f, 1.0f, 0.0f}};
        run_until(scene, config, &state, 5.0f, [](const auto& s, const auto&) {
            assert(!s.parked);
        });
        assert(near(state.position.y, 1.3f, 0.0001f));
    }
    {
        // Shorter than the field's centre, so the centre ray passes over it;
        // taller than step_up, so the probe starts inside it.
        Scene scene;
        scene.add_flat(-20.0f, 20.0f);
        scene.add_box(glm::vec3{5.0f, 0.4f, 0.0f}, glm::vec3{2.0f, 0.4f, 10.0f});
        const ground_follow::Config config = config_moving(6.0f);
        ground_follow::State state{glm::vec3{0.0f, 1.0f, 0.0f}};
        run_until(scene, config, &state, 20.0f, [](const auto&, const auto&) {});
        assert(state.parked);
        assert(state.position.x < 3.0f);
        assert(near(state.position.y, 1.0f, 0.0001f));
    }
}

void settles_onto_ground_and_refuses_without_any() {
    Scene scene;
    scene.add_flat(-20.0f, 0.0f);
    ground_follow::Config config = config_moving(6.0f);
    config.hover_height = 2.0f;

    ground_follow::State over_ground{glm::vec3{-5.0f, 1.5f, 0.0f}};
    assert(ground_follow::settle(scene.physics, config, 3.0f, &over_ground));
    assert(near(over_ground.position.y, 2.0f, 0.0001f));

    ground_follow::State over_nothing{glm::vec3{5.0f, 1.5f, 0.0f}};
    assert(!ground_follow::settle(scene.physics, config, 3.0f, &over_nothing));
    assert(over_nothing.position.y == 1.5f);

    // Spawned exactly on the ground -- where a bottle that broke there puts
    // it. A probe from the spawn point itself would start on the surface and
    // read as starting inside it.
    ground_follow::State on_ground{glm::vec3{-5.0f, 0.0f, 0.0f}};
    assert(ground_follow::settle(scene.physics, config, 3.0f, &on_ground));
    assert(near(on_ground.position.y, 2.0f, 0.0001f));
    // And then rides rather than parking at once.
    ground_follow::Config moving = config;
    moving.horizontal_velocity = glm::vec3{-6.0f, 0.0f, 0.0f};
    ground_follow::step(scene.physics, moving, kDt, &on_ground);
    assert(!on_ground.parked);
    assert(near(on_ground.position.y, 2.0f, 0.0001f));
}

void a_replay_over_the_same_course_is_identical() {
    Scene scene;
    scene.add_flat(-20.0f, 3.0f);
    scene.add_ramp(2.0f, 30.0f, 4.0f);
    scene.add_box(glm::vec3{12.0f, 3.0f, 0.0f}, glm::vec3{0.5f, 3.0f, 10.0f});
    const ground_follow::Config config = config_moving(6.0f, 0.3f);
    std::vector<glm::vec3> first;
    std::vector<glm::vec3> second;
    for (std::vector<glm::vec3>* trace : {&first, &second}) {
        ground_follow::State state{glm::vec3{0.0f, 1.0f, 0.0f}};
        for (int tick = 0; tick < 90; ++tick) {
            ground_follow::step(scene.physics, config, kDt, &state);
            trace->push_back(state.position);
        }
    }
    assert(first == second);
}

}  // namespace

int main() {
    rides_flat_ground_at_hover_height();
    climbs_a_walkable_slope();
    stops_at_a_slope_too_steep_to_climb();
    follows_a_walkable_slope_down();
    holds_its_height_off_a_cliff();
    stops_at_a_wall_with_its_sweep_against_it();
    steps_onto_a_ledge_under_step_up_and_stops_at_one_over_it();
    settles_onto_ground_and_refuses_without_any();
    a_replay_over_the_same_course_is_identical();
    return 0;
}
