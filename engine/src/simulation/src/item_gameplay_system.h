#ifndef SIMULATION_SRC_ITEM_GAMEPLAY_SYSTEM_H_
#define SIMULATION_SRC_ITEM_GAMEPLAY_SYSTEM_H_

#include "kernel/public/kernel_types.h"

namespace network_example {

class KernelEngine;

class ItemGameplaySystem {
public:
    bool submit_request(
        KernelEngine& engine,
        const KernelGameplayRequest& request) const;
    void update_carried_props(KernelEngine& engine) const;
};

}  // namespace network_example

#endif  // SIMULATION_SRC_ITEM_GAMEPLAY_SYSTEM_H_
