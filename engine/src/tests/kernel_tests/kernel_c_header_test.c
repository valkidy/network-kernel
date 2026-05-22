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

    assert(KERNEL_ABI_VERSION == 8u);
    assert(sizeof(KernelAbiInfo) > 0u);
    assert(sizeof(KernelLocalPlayerInfo) > 0u);
    assert(sizeof(KernelConfig) > 0u);
    assert(sizeof(PlayerInput) > 0u);
    assert(sizeof(RenderEntityState) > 0u);
    assert(sizeof(KernelEvent) > 0u);
    assert(sizeof(KernelServerEntityCreateInfo) > 0u);
    assert(sizeof(KernelServerEntityState) > 0u);
    assert((KERNEL_CAPABILITY_CLIENT_MODE & KERNEL_CAPABILITY_RENDER_STATES) == 0u);
    assert((KERNEL_CAPABILITY_LAG_COMPENSATED_PROJECTILE &
            KERNEL_CAPABILITY_EVENT_PRESENTATION_TIME) == 0u);
    assert((KERNEL_CAPABILITY_RENDER_STATES_AT_TIME &
            KERNEL_CAPABILITY_RENDER_STATES) == 0u);
    assert((KERNEL_VISUAL_FLAG_MOVING & KERNEL_VISUAL_FLAG_RELOADING) == 0u);
    assert((KERNEL_VISUAL_FLAG_MOVING & KERNEL_VISUAL_FLAG_DEAD) == 0u);
    assert((InputButton_Dodge & InputButton_Parry) == 0u);
    assert((InputButton_Dodge & InputButton_Reload) == 0u);
    assert(sizeof(abi_info.abi_version) == sizeof(uint32_t));
    assert(offsetof(PlayerInput, client_action_time_us) > offsetof(PlayerInput, input_seq));
    assert(offsetof(PlayerInput, client_action_id) > offsetof(PlayerInput, client_action_time_us));
    assert(offsetof(RenderEntityState, entity_id) == 0u);
    assert(offsetof(RenderEntityState, hp) > offsetof(RenderEntityState, velocity));
    assert(offsetof(RenderEntityState, max_hp) > offsetof(RenderEntityState, hp));
    assert(offsetof(KernelEvent, event_time_us) > offsetof(KernelEvent, code));
    assert(offsetof(KernelEvent, presentation_time_us) > offsetof(KernelEvent, event_time_us));
    assert(sizeof(state.entity_id) == sizeof(uint64_t));

    return 0;
}
