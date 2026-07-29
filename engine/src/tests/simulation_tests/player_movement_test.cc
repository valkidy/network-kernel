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
    movement_solver_clamps_diagonal_input();
    movement_solver_applies_player_input_to_snapshot();
    simulate_player_movement_uses_solver_formula();
    return 0;
}
