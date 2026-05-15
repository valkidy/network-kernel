#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/public/kernel_api.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

void presentation_gate_releases_at_render_time() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    network_example::KernelEngine engine(config);
    engine.pending_presentation_events_.push_back(KernelEvent{
        KernelEventType_DamageApplied,
        3,
        11,
        0,
        7,
        100000,
        100000});

    std::array<KernelEvent, 4> events{};
    assert(engine.poll_events(events.data(), static_cast<std::uint32_t>(events.size())) == 0);

    network_example::WorldSnapshot early_snapshot;
    early_snapshot.header.server_tick = 2;
    engine.handle_client_snapshot(early_snapshot);
    engine.rebuild_render_states();
    assert(engine.poll_events(events.data(), static_cast<std::uint32_t>(events.size())) == 0);

    network_example::WorldSnapshot later_snapshot;
    later_snapshot.header.server_tick = 10;
    engine.handle_client_snapshot(later_snapshot);
    engine.rebuild_render_states();

    const std::uint32_t event_count =
        engine.poll_events(events.data(), static_cast<std::uint32_t>(events.size()));
    assert(event_count == 1);
    assert(events[0].type == KernelEventType_DamageApplied);
    assert(events[0].event_time_us == 100000);
    assert(events[0].presentation_time_us == 100000);
}

}  // namespace

int main() {
    presentation_gate_releases_at_render_time();

    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartClient(kernel, "127.0.0.1:9"));

    PlayerInput input{};
    input.input_seq = 1;
    input.client_action_time_us = 33333;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    Kernel_SubmitInput(kernel, 0, &input);

    std::array<KernelEvent, 8> events{};
    const std::uint32_t event_count =
        Kernel_PollEvents(kernel, events.data(), static_cast<std::uint32_t>(events.size()));
    bool saw_error = false;
    for (std::uint32_t index = 0; index < event_count; ++index) {
        saw_error = saw_error || events[index].type == KernelEventType_Error;
    }
    assert(saw_error);

    Kernel_Destroy(kernel);
    return 0;
}
