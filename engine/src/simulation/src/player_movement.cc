#include "simulation/public/simulation.h"

#include "simulation/public/movement_solver.h"

namespace network_example {

void simulate_player_movement(
    World& world,
    const std::vector<QueuedInput>& inputs,
    float fixed_delta_seconds) {
    for (const QueuedInput& queued_input : inputs) {
        auto view = world.registry()
                        .view<NetworkIdentity, Transform, Velocity, MovementState, PlayerTag>();
        for (const entt::entity entity : view) {
            const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
            if (identity.owner_peer != queued_input.owner_peer) {
                continue;
            }
            const MovementState& movement = view.get<MovementState>(entity);
            Velocity& velocity = view.get<Velocity>(entity);
            Transform& transform = view.get<Transform>(entity);
            velocity.linear = movement_solver::input_move_to_world(queued_input.input) *
                              movement.speed_meters_per_second;
            transform.position += velocity.linear * fixed_delta_seconds;
        }
    }
}

void simulate_velocity_movement(World& world, float fixed_delta_seconds) {
    if (fixed_delta_seconds <= 0.0f) {
        return;
    }

    auto view = world.registry().view<Transform, Velocity>();
    for (const entt::entity entity : view) {
        if (world.registry().all_of<PlayerTag>(entity) ||
            world.registry().all_of<ProjectileTag>(entity)) {
            continue;
        }
        Transform& transform = view.get<Transform>(entity);
        const Velocity& velocity = view.get<Velocity>(entity);
        transform.position += velocity.linear * fixed_delta_seconds;
    }
}

}  // namespace network_example
