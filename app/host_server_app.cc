#include "host_server_app.h"

#include <array>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "game_server/game_server.h"
#include "kernel/public/kernel_api.h"

namespace {

KernelConfig default_config() {
    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;
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
    input.buttons = sequence == 2 ? InputButton_Fire : 0;
    return input;
}

}  // namespace

int RunHostServer(std::uint16_t port) {
    KernelConfig config = default_config();
    KernelHandle* kernel = Kernel_Create(&config);
    if (kernel == nullptr || !Kernel_StartListenServer(kernel, port)) {
        spdlog::error("failed to start listen server");
        Kernel_Destroy(kernel);
        return 1;
    }

    constexpr std::array<float, 4> kFrameDeltas = {
        1.0f / 30.0f,
        1.0f / 30.0f,
        1.0f / 30.0f,
        1.0f / 30.0f,
    };

    network_example::game_server::GameServer game_server(kernel);
    std::uint32_t sequence = 1;
    for (float delta_seconds : kFrameDeltas) {
        const PlayerInput input = scripted_input(sequence++);
        Kernel_SubmitInput(kernel, 1, &input);
        Kernel_Update(kernel, delta_seconds);

        std::array<KernelEvent, 32> events{};
        const std::uint32_t event_count =
            Kernel_PollEvents(kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < event_count; ++index) {
            spdlog::info(
                "event type={} tick={} net_id={} peer={} code={}",
                static_cast<int>(events[index].type),
                events[index].tick,
                events[index].net_id,
                events[index].peer_id,
                events[index].code);
            game_server.handle_event(events[index]);
        }
        game_server.tick(delta_seconds);
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
    return 0;
}
