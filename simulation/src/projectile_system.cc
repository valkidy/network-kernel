#include "simulation/public/simulation.h"

namespace network_example {

void simulate_projectiles(World& world, float fixed_delta_seconds) {
    auto view = world.registry().view<Transform, Velocity, ProjectileTag>();
    for (const entt::entity entity : view) {
        Transform& transform = view.get<Transform>(entity);
        const Velocity& velocity = view.get<Velocity>(entity);
        transform.position += velocity.linear * fixed_delta_seconds;
    }
}

}  // namespace network_example
