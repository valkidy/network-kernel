#ifndef KERNEL_SRC_KERNEL_RPC_DEV_HANDLERS_H_
#define KERNEL_SRC_KERNEL_RPC_DEV_HANDLERS_H_

#include <string_view>

#include "kernel/src/kernel_rpc.h"

namespace network_example {

KernelRpcMethodHandler kernel_rpc_dev_handler_for_symbol(
    std::string_view symbol);

}  // namespace network_example

#endif  // KERNEL_SRC_KERNEL_RPC_DEV_HANDLERS_H_
