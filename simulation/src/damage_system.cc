#include "simulation/public/simulation.h"

namespace network_example {

void destroy_dead_entities(
    World& world,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events) {
    std::vector<NetId> dead_entities;
    auto view = world.registry().view<NetworkIdentity, Health>();
    for (const entt::entity entity : view) {
        const Health& health = view.get<Health>(entity);
        if (health.hp == 0) {
            dead_entities.push_back(view.get<NetworkIdentity>(entity).net_id);
        }
    }

    for (NetId net_id : dead_entities) {
        if (world.destroy(net_id) && events != nullptr) {
            events->push_back(
                KernelEvent{KernelEventType_EntityDestroyed, current_tick, net_id, 0, 0});
        }
    }
}

}  // namespace network_example
