#ifndef KERNEL_SRC_KERNEL_RPC_WORLD_HANDLERS_H_
#define KERNEL_SRC_KERNEL_RPC_WORLD_HANDLERS_H_

#include <cstdint>
#include <string_view>

#include "kernel/public/kernel_types.h"
#include "kernel/src/kernel_rpc.h"

namespace network_example {

class KernelRpcWorldHandlers {
public:
    static KernelRpcMethodHandler handler_for_symbol(std::string_view symbol);
    static void complete_simulation_command(
        KernelRpcResponseStore& response_store,
        std::uint64_t completion_token,
        simulation::CommandId command_id,
        const simulation::CommandResult& result);
    static bool server_get_entity_state(
        const KernelEngine& engine,
        std::uint32_t net_id,
        KernelServerEntityState* out_state);
    static bool enqueue_simulation_command(
        KernelEngine& engine,
        const simulation::Command& command);
};

}  // namespace network_example

#endif  // KERNEL_SRC_KERNEL_RPC_WORLD_HANDLERS_H_
