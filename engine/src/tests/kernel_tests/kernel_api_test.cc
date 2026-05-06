#include <array>
#include <cassert>

#include "kernel/public/kernel_api.h"

int main() {
    KernelAbiInfo abi_info{};
    assert(Kernel_GetAbiInfo(&abi_info, sizeof(abi_info)));
    assert(abi_info.struct_size == sizeof(KernelAbiInfo));
    assert(abi_info.abi_version == KERNEL_ABI_VERSION);
    assert(abi_info.kernel_config_size == sizeof(KernelConfig));
    assert(abi_info.player_input_size == sizeof(PlayerInput));
    assert(abi_info.render_entity_state_size == sizeof(RenderEntityState));
    assert(abi_info.kernel_event_size == sizeof(KernelEvent));
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_CLIENT_MODE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LISTEN_SERVER_MODE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_DEDICATED_SERVER_MODE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_INPUT_SUBMISSION) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_RENDER_STATES) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_EVENT_POLLING) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_CLIENT_PREDICTION) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SNAPSHOT_INTERPOLATION) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LAG_COMPENSATED_HITSCAN) != 0);
    assert(!Kernel_GetAbiInfo(nullptr, sizeof(abi_info)));
    assert(!Kernel_GetAbiInfo(&abi_info, sizeof(abi_info) - 1));

    assert(Kernel_Create(nullptr) == nullptr);
    assert(!Kernel_StartClient(nullptr, "127.0.0.1:9"));
    assert(!Kernel_StartListenServer(nullptr, 7777));
    assert(!Kernel_StartDedicatedServer(nullptr, 7777));
    Kernel_Update(nullptr, 1.0f / 30.0f);
    Kernel_SubmitInput(nullptr, 1, nullptr);
    assert(Kernel_GetRenderStates(nullptr, nullptr, 0) == 0);
    assert(Kernel_PollEvents(nullptr, nullptr, 0) == 0);

    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(!Kernel_StartClient(kernel, nullptr));
    assert(!Kernel_StartClient(kernel, ""));
    assert(Kernel_StartDedicatedServer(kernel, 7777));

    PlayerInput input{};
    input.input_seq = 1;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    Kernel_SubmitInput(kernel, 1, &input);
    Kernel_Update(kernel, 1.0f / 30.0f);

    std::array<RenderEntityState, 8> states{};
    assert(Kernel_GetRenderStates(kernel, nullptr, states.size()) == 0);
    assert(Kernel_GetRenderStates(kernel, states.data(), 0) == 0);
    assert(Kernel_GetRenderStates(kernel, states.data(), states.size()) >= 1);

    std::array<KernelEvent, 16> events{};
    assert(Kernel_PollEvents(kernel, nullptr, events.size()) == 0);
    assert(Kernel_PollEvents(kernel, events.data(), 0) == 0);
    assert(Kernel_PollEvents(kernel, events.data(), events.size()) >= 1);

    Kernel_Destroy(kernel);
    return 0;
}
