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
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    config.tick.history_ms = 500;
    config.tick.max_ticks_per_update = 4;
    config.max_render_states = 256;
    config.max_events = 256;
    return config;
}

}  // namespace

int main() {
    spdlog::set_level(spdlog::level::info);

    KernelConfig config = default_config();
    KernelHandle* kernel = Kernel_Create(&config);
    if (kernel == nullptr || !Kernel_StartDedicatedServer(kernel, 7777)) {
        spdlog::error("failed to start dedicated server");
        Kernel_Destroy(kernel);
        return 1;
    }

    spdlog::info("dedicated server listening on 127.0.0.1:7777");

    constexpr float kDeltaSeconds = 1.0f / 30.0f;
    while (true) {
        Kernel_Update(kernel, kDeltaSeconds);

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
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    Kernel_Destroy(kernel);
    return 0;
}
