#include <array>
#include <cassert>

#include "kernel/public/kernel_api.h"

namespace {

RenderEntityState find_player(const std::array<RenderEntityState, 16>& states, std::uint32_t count) {
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].entity_type == 1) {
            return states[index];
        }
    }
    assert(false);
    return RenderEntityState{};
}

}  // namespace

int main() {
    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;

    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartListenServer(kernel, 7777));

    std::array<KernelEvent, 16> events{};
    const std::uint32_t event_count =
        Kernel_PollEvents(kernel, events.data(), static_cast<std::uint32_t>(events.size()));
    bool saw_player_joined = false;
    bool saw_player_spawned = false;
    for (std::uint32_t index = 0; index < event_count; ++index) {
        saw_player_joined =
            saw_player_joined || events[index].type == KernelEventType_PlayerJoined;
        saw_player_spawned =
            saw_player_spawned || events[index].type == KernelEventType_EntitySpawned;
    }
    assert(saw_player_joined);
    assert(saw_player_spawned);

    std::array<RenderEntityState, 16> before_states{};
    const std::uint32_t before_count = Kernel_GetRenderStates(
        kernel,
        before_states.data(),
        static_cast<std::uint32_t>(before_states.size()));
    assert(before_count >= 2);
    const RenderEntityState before_player = find_player(before_states, before_count);

    PlayerInput input{};
    input.input_seq = 1;
    input.client_tick = 0;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    Kernel_SubmitInput(kernel, 1, &input);

    Kernel_Update(kernel, 0.0f);
    std::array<RenderEntityState, 16> predicted_states{};
    const std::uint32_t predicted_count = Kernel_GetRenderStates(
        kernel,
        predicted_states.data(),
        static_cast<std::uint32_t>(predicted_states.size()));
    assert(predicted_count >= 2);
    const RenderEntityState predicted_player =
        find_player(predicted_states, predicted_count);
    assert(predicted_player.position.x > before_player.position.x);

    Kernel_Update(kernel, 1.0f / 30.0f);

    std::array<RenderEntityState, 16> after_states{};
    const std::uint32_t after_count = Kernel_GetRenderStates(
        kernel,
        after_states.data(),
        static_cast<std::uint32_t>(after_states.size()));
    assert(after_count >= 2);
    const RenderEntityState after_player = find_player(after_states, after_count);
    assert(after_player.position.x > before_player.position.x);

    PlayerInput fire_input{};
    fire_input.input_seq = 2;
    fire_input.client_tick = 2;
    fire_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    fire_input.buttons = InputButton_Fire;
    fire_input.selected_weapon = 0;
    Kernel_SubmitInput(kernel, 1, &fire_input);
    Kernel_Update(kernel, 1.0f / 30.0f);

    std::array<KernelEvent, 16> combat_events{};
    const std::uint32_t combat_event_count = Kernel_PollEvents(
        kernel,
        combat_events.data(),
        static_cast<std::uint32_t>(combat_events.size()));
    bool saw_fire_confirmed = false;
    bool saw_damage_applied = false;
    for (std::uint32_t index = 0; index < combat_event_count; ++index) {
        saw_fire_confirmed =
            saw_fire_confirmed ||
            combat_events[index].type == KernelEventType_FireConfirmed;
        saw_damage_applied =
            saw_damage_applied ||
            combat_events[index].type == KernelEventType_DamageApplied;
    }
    assert(saw_fire_confirmed);
    assert(saw_damage_applied);

    Kernel_Destroy(kernel);
    return 0;
}
