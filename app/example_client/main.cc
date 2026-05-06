#include <array>
#include <chrono>
#include <cstdint>
#include <thread>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "kernel/public/kernel_api.h"

namespace {

KernelConfig default_config() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    config.tick.history_ms = 500;
    config.tick.max_ticks_per_update = 4;
    config.max_render_states = 256;
    config.max_events = 256;
    return config;
}

PlayerInput scripted_input(std::uint32_t sequence) {
    PlayerInput input{};
    input.input_seq = sequence;
    input.client_tick = sequence;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.buttons = 0;
    return input;
}

}  // namespace

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    const char* address = argc > 1 ? argv[1] : "127.0.0.1:7777";
    KernelConfig config = default_config();
    KernelHandle* kernel = Kernel_Create(&config);
    if (kernel == nullptr || !Kernel_StartClient(kernel, address)) {
        spdlog::error("failed to start example client for {}", address);
        Kernel_Destroy(kernel);
        return 1;
    }

    spdlog::info("example client connecting to {}", address);

    constexpr float kDeltaSeconds = 1.0f / 30.0f;
    bool ready_for_input = false;
    std::uint32_t sequence = 1;

    for (std::uint32_t frame = 0; frame < 180; ++frame) {
        Kernel_Update(kernel, kDeltaSeconds);

        std::array<KernelEvent, 32> events{};
        const std::uint32_t event_count =
            Kernel_PollEvents(kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < event_count; ++index) {
            const KernelEvent& event = events[index];
            spdlog::info(
                "event type={} tick={} net_id={} peer={} code={}",
                static_cast<int>(event.type),
                event.tick,
                event.net_id,
                event.peer_id,
                event.code);
            if (event.type == KernelEventType_PlayerJoined && event.peer_id != 0) {
                ready_for_input = true;
            }
        }

        if (ready_for_input) {
            const PlayerInput input = scripted_input(sequence++);
            Kernel_SubmitInput(kernel, 0, &input);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    std::array<RenderEntityState, 16> states{};
    const std::uint32_t state_count =
        Kernel_GetRenderStates(kernel, states.data(), static_cast<std::uint32_t>(states.size()));
    for (std::uint32_t index = 0; index < state_count; ++index) {
        const RenderEntityState& state = states[index];
        spdlog::info(
            "render_state net_id={} type={} pos=({}, {}, {})",
            state.net_id,
            state.entity_type,
            state.position.x,
            state.position.y,
            state.position.z);
    }

    Kernel_Destroy(kernel);
    return state_count > 0 ? 0 : 2;
}
