#include <cassert>
#include <cmath>
#include <optional>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "simulation/public/movement_solver.h"
#include "simulation/public/simulation.h"

namespace {

bool nearly_equal(float lhs, float rhs) {
    return std::fabs(lhs - rhs) < 0.0001f;
}

// A grounded state does not imply a walkable normal. Jolt hands back OnGround
// together with a near-horizontal ground normal, measured live at 0.0095
// against a 0.6428 limit, and the slope-following division's gain is 1/n.y --
// so believing the state there turned a 2.5 m/s walk into 146 m/s upward and
// launched the actor 750 m. The normal has to be the thing that decides.
void ground_following_velocity_ignores_unwalkable_normals() {
    constexpr float kDelta = 1.0f / 30.0f;
    constexpr float kGravityY = -9.81f;
    constexpr float kMaxSlope = 50.0f;
    const glm::vec3 walk{2.5f, 0.0f, 0.0f};

    // The live failure: grounded, but the normal is nearly horizontal.
    const glm::vec3 launching_normal =
        glm::normalize(glm::vec3{0.99995f, 0.0095f, 0.0f});
    const glm::vec3 handled =
        network_example::movement_solver::ground_following_velocity(
            walk,
            network_example::physics::CharacterGroundState::kGrounded,
            launching_normal,
            kMaxSlope,
            0.0f,
            kGravityY,
            kDelta);
    // Falls, rather than being flung: one frame of gravity, nothing more.
    assert(std::abs(handled.y - kGravityY * kDelta) < 0.0001f);

    // A walkable slope still follows the ground: 2.5 m/s up a 50 degree face
    // rises at 2.5 * tan(50) = 2.979 m/s, and that is the most this can ever
    // produce now, because the guard IS the slope limit.
    const float slope_radians = kMaxSlope * 3.14159265358979323846f / 180.0f;
    const glm::vec3 limit_normal{
        -std::sin(slope_radians), std::cos(slope_radians), 0.0f};
    const glm::vec3 followed =
        network_example::movement_solver::ground_following_velocity(
            walk,
            network_example::physics::CharacterGroundState::kGrounded,
            limit_normal,
            kMaxSlope,
            0.0f,
            kGravityY,
            kDelta);
    assert(std::abs(followed.y - 2.5f * std::tan(slope_radians)) < 0.001f);
    assert(followed.y > 0.0f);

    // Airborne always falls, whatever normal is left over from before.
    const glm::vec3 falling =
        network_example::movement_solver::ground_following_velocity(
            walk,
            network_example::physics::CharacterGroundState::kAirborne,
            glm::vec3{0.0f, 1.0f, 0.0f},
            kMaxSlope,
            -3.0f,
            kGravityY,
            kDelta);
    assert(std::abs(falling.y - (-3.0f + kGravityY * kDelta)) < 0.0001f);
}

void movement_solver_clamps_diagonal_input() {
    KernelPlayerInput input{};
    input.move = KernelVec2{3.0f, 4.0f};

    const glm::vec3 movement = network_example::movement_solver::input_move_to_world(input);

    assert(nearly_equal(movement.x, 0.6f));
    assert(nearly_equal(movement.y, 0.0f));
    assert(nearly_equal(movement.z, 0.8f));
}

void movement_solver_applies_player_input_to_snapshot() {
    network_example::EntitySnapshot entity{};
    entity.position = glm::vec3{1.0f, 2.0f, 3.0f};
    KernelPlayerInput input{};
    input.move = KernelVec2{3.0f, 4.0f};

    network_example::movement_solver::apply_player_input(entity, input, 0.5f, 10.0f);

    assert(nearly_equal(entity.velocity.x, 6.0f));
    assert(nearly_equal(entity.velocity.y, 0.0f));
    assert(nearly_equal(entity.velocity.z, 8.0f));
    assert(nearly_equal(entity.position.x, 4.0f));
    assert(nearly_equal(entity.position.y, 2.0f));
    assert(nearly_equal(entity.position.z, 7.0f));
}

void simulate_player_movement_uses_solver_formula() {
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(7, glm::vec3{1.0f, 2.0f, 3.0f});
    const std::optional<entt::entity> entity = world.find_entity(player);
    assert(entity.has_value());
    world.registry().get<network_example::MovementState>(*entity)
        .speed_meters_per_second = 10.0f;

    KernelPlayerInput input{};
    input.move = KernelVec2{3.0f, 4.0f};
    const std::vector<network_example::QueuedInput> inputs{
        network_example::QueuedInput{7, input, 0, 0, false, 0},
    };

    network_example::simulate_player_movement(world, inputs, 0.5f);

    const network_example::Transform& transform =
        world.registry().get<network_example::Transform>(*entity);
    const network_example::Velocity& velocity =
        world.registry().get<network_example::Velocity>(*entity);
    assert(nearly_equal(velocity.linear.x, 6.0f));
    assert(nearly_equal(velocity.linear.y, 0.0f));
    assert(nearly_equal(velocity.linear.z, 8.0f));
    assert(nearly_equal(transform.position.x, 4.0f));
    assert(nearly_equal(transform.position.y, 2.0f));
    assert(nearly_equal(transform.position.z, 7.0f));
}

}  // namespace

int main() {
    ground_following_velocity_ignores_unwalkable_normals();
    movement_solver_clamps_diagonal_input();
    movement_solver_applies_player_input_to_snapshot();
    simulate_player_movement_uses_solver_formula();
    return 0;
}
