#include "client_app.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>

#include <spdlog/spdlog.h>

#include "kernel/public/kernel_api.h"
#include "kernel/src/tick_loop.h"

namespace {

KernelConfig default_config() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick = network_example::current_netcode_preset();
    config.max_render_states = 256;
    config.max_events = 256;
    return config;
}

PlayerInput scripted_input(std::uint32_t sequence) {
    PlayerInput input{};
    input.input_seq = sequence;
    input.client_action_time_us = static_cast<std::uint64_t>(sequence) * 33333u;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.buttons = 0;
    input.selected_weapon = 0;

    if (sequence == 2) {
        input.buttons = InputButton_Fire;
        input.selected_weapon = 0;
    } else if (sequence == 12) {
        input.buttons = InputButton_Reload;
        input.selected_weapon = 0;
    } else if (sequence == 36) {
        input.buttons = InputButton_Fire;
        input.selected_weapon = 1;
    } else if (sequence == 72) {
        input.buttons = InputButton_Fire;
        input.selected_weapon = 2;
    }

    return input;
}

bool sample_player_x(KernelHandle* kernel, float* out_x) {
    std::array<RenderEntityState, 16> states{};
    const std::uint32_t state_count =
        Kernel_GetRenderStates(kernel, states.data(), static_cast<std::uint32_t>(states.size()));
    for (std::uint32_t index = 0; index < state_count; ++index) {
        if (states[index].entity_type == 1) {
            if (out_x != nullptr) {
                *out_x = states[index].position.x;
            }
            return true;
        }
    }
    return false;
}

void log_native_build_info() {
    KernelBuildInfo info{};
    if (!Kernel_GetBuildInfo(&info, sizeof(info))) {
        spdlog::error("[NetworkExample] Native Module: Kernel_GetBuildInfo failed");
        return;
    }
    spdlog::info(
        "[NetworkExample] Native Module: module_name={} module_file={} "
        "module_version={} protocol_version={} snapshot_schema_version={} "
        "packet_schema_version={} git_commit={} build_platform={} build_config={} "
        "compiler_info={}",
        info.module_name,
        info.module_file_name,
        info.module_version,
        info.protocol_version,
        info.snapshot_schema_version,
        info.packet_schema_version,
        info.git_commit,
        info.build_platform,
        info.build_config,
        info.compiler_info);
}

}  // namespace

int RunClient(const char* address) {
    log_native_build_info();

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
    std::uint32_t combat_input_count = 0;
    std::uint32_t fire_confirmed_count = 0;
    std::uint32_t damage_applied_count = 0;
    std::uint32_t explosion_count = 0;
    std::uint32_t client_render_sample_count = 0;
    std::uint32_t client_player_move_sample_count = 0;
    float first_player_x = 0.0f;
    float last_player_x = 0.0f;
    bool has_first_player_x = false;

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
            if (event.type == KernelEventType_FireConfirmed) {
                ++fire_confirmed_count;
            }
            if (event.type == KernelEventType_DamageApplied) {
                ++damage_applied_count;
            }
            if (event.type == KernelEventType_Explosion) {
                ++explosion_count;
            }
        }

        if (ready_for_input) {
            const PlayerInput input = scripted_input(sequence++);
            if ((input.buttons & (InputButton_Fire | InputButton_Reload)) != 0) {
                ++combat_input_count;
                spdlog::info(
                    "client submitting combat input seq={} buttons={} weapon={}",
                    input.input_seq,
                    input.buttons,
                    static_cast<int>(input.selected_weapon));
            }
            Kernel_SubmitInput(kernel, 0, &input);
        }

        float sampled_player_x = 0.0f;
        if (ready_for_input && sample_player_x(kernel, &sampled_player_x)) {
            ++client_render_sample_count;
            if (!has_first_player_x) {
                first_player_x = sampled_player_x;
                has_first_player_x = true;
            }
            if (sampled_player_x > last_player_x) {
                ++client_player_move_sample_count;
            }
            last_player_x = sampled_player_x;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    spdlog::info(
        "client combat test summary submitted={} observed_fire_confirmed={} "
        "observed_damage_applied={} observed_explosions={}",
        combat_input_count,
        fire_confirmed_count,
        damage_applied_count,
        explosion_count);
    spdlog::info(
        "client side test summary render_samples={} move_samples={} first_x={} last_x={}",
        client_render_sample_count,
        client_player_move_sample_count,
        first_player_x,
        last_player_x);

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
    if (state_count == 0) {
        return 2;
    }
    if (client_render_sample_count == 0 || client_player_move_sample_count == 0) {
        return 3;
    }
    return 0;
}
