#ifndef SIMULATION_SRC_ITEM_GAMEPLAY_SYSTEM_H_
#define SIMULATION_SRC_ITEM_GAMEPLAY_SYSTEM_H_

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
};

}  // namespace network_example

#endif  // SIMULATION_SRC_ITEM_GAMEPLAY_SYSTEM_H_
