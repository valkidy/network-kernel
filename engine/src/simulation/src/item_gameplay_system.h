#ifndef SIMULATION_SRC_ITEM_GAMEPLAY_SYSTEM_H_
#define SIMULATION_SRC_ITEM_GAMEPLAY_SYSTEM_H_

#include "kernel/public/kernel_types.h"
#include "simulation/public/item_system.h"

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
};

}  // namespace network_example

#endif  // SIMULATION_SRC_ITEM_GAMEPLAY_SYSTEM_H_
