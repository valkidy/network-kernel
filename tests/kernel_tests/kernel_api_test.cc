#include <array>
#include <cassert>

#include "kernel/public/kernel_api.h"

int main() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartDedicatedServer(kernel, 7777));

    PlayerInput input{};
    input.input_seq = 1;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    Kernel_SubmitInput(kernel, 1, &input);
    Kernel_Update(kernel, 1.0f / 30.0f);

    std::array<RenderEntityState, 8> states{};
    assert(Kernel_GetRenderStates(kernel, states.data(), states.size()) >= 2);

    std::array<KernelEvent, 16> events{};
    assert(Kernel_PollEvents(kernel, events.data(), events.size()) >= 1);

    Kernel_Destroy(kernel);
    return 0;
}
