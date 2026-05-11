#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/public/kernel_api.h"

int main(void) {
    KernelAbiInfo abi_info;
    KernelConfig config;
    KernelLocalPlayerInfo local_player_info;
    PlayerInput input;
    RenderEntityState state;
    KernelEvent event;
    KernelServerEntityCreateInfo create_info;
    KernelServerEntityState server_state;

    (void)config;
    (void)local_player_info;
    (void)input;
    (void)state;
    (void)event;
    (void)create_info;
    (void)server_state;

    assert(KERNEL_ABI_VERSION == 4u);
    assert(sizeof(KernelAbiInfo) > 0u);
    assert(sizeof(KernelLocalPlayerInfo) > 0u);
    assert(sizeof(KernelConfig) > 0u);
    assert(sizeof(PlayerInput) > 0u);
    assert(sizeof(RenderEntityState) > 0u);
    assert(sizeof(KernelEvent) > 0u);
    assert(sizeof(KernelServerEntityCreateInfo) > 0u);
    assert(sizeof(KernelServerEntityState) > 0u);
    assert((KERNEL_CAPABILITY_CLIENT_MODE & KERNEL_CAPABILITY_RENDER_STATES) == 0u);
    assert((KERNEL_VISUAL_FLAG_MOVING & KERNEL_VISUAL_FLAG_RELOADING) == 0u);
    assert((KERNEL_VISUAL_FLAG_MOVING & KERNEL_VISUAL_FLAG_DEAD) == 0u);
    assert(sizeof(abi_info.abi_version) == sizeof(uint32_t));

    return 0;
}
