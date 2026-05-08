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

    (void)config;
    (void)local_player_info;
    (void)input;
    (void)state;
    (void)event;

    assert(KERNEL_ABI_VERSION == 2u);
    assert(sizeof(KernelAbiInfo) > 0u);
    assert(sizeof(KernelLocalPlayerInfo) > 0u);
    assert(sizeof(KernelConfig) > 0u);
    assert(sizeof(PlayerInput) > 0u);
    assert(sizeof(RenderEntityState) > 0u);
    assert(sizeof(KernelEvent) > 0u);
    assert((KERNEL_CAPABILITY_CLIENT_MODE & KERNEL_CAPABILITY_RENDER_STATES) == 0u);
    assert(sizeof(abi_info.abi_version) == sizeof(uint32_t));

    return 0;
}
