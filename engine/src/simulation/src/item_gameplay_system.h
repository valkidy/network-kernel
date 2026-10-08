#ifndef SIMULATION_SRC_ITEM_GAMEPLAY_SYSTEM_H_
#define SIMULATION_SRC_ITEM_GAMEPLAY_SYSTEM_H_

#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "simulation/public/item_system.h"
#include "world/public/components.h"

namespace network_example {

class KernelEngine;

class ItemGameplaySystem {
public:
    bool decorate_item_prop(
        KernelEngine& engine,
        std::uint32_t prop_id,
        const ItemInstanceRecord& item) const;
    bool submit_request(
        KernelEngine& engine,
        const KernelGameplayRequest& request) const;
    void update_carried_props(KernelEngine& engine) const;
    // Sets down whatever `carrier_net_id` is carrying at `position`, as a
    // Place request would: placed, colliding again, and replicated. For a
    // carrier that is about to stop being where its load can follow it.
    void drop_carried_props(
        KernelEngine& engine,
        NetId carrier_net_id,
        const glm::vec3& position) const;
    // Lets go of whatever `carrier_net_id` is carrying where it is held, each
    // on the ground beneath it (design: carried props drop at death and at a
    // disconnect, tagged or not). For a carrier that dies or goes away.
    void drop_carried_props(KernelEngine& engine, NetId carrier_net_id) const;

private:
    // Every prop `carrier_net_id` is carrying, in registry order.
    std::vector<entt::entity> carried_by(
        KernelEngine& engine, NetId carrier_net_id) const;
    void set_down_carried_prop(
        KernelEngine& engine,
        entt::entity entity,
        const glm::vec3& position) const;
};

}  // namespace network_example

#endif  // SIMULATION_SRC_ITEM_GAMEPLAY_SYSTEM_H_
