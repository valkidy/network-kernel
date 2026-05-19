#include "dedicated_server_app.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>

#include <spdlog/spdlog.h>

#include "game_server/game_server.h"
#include "kernel/public/kernel_api.h"
#include "kernel/src/tick_loop.h"

namespace {

KernelConfig default_config() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick = network_example::current_netcode_preset();
    config.max_render_states = 256;
    config.max_events = 256;
    return config;
}

}  // namespace

int RunDedicatedServer(std::uint16_t port) {
    KernelConfig config = default_config();
    KernelHandle* kernel = Kernel_Create(&config);
    if (kernel == nullptr || !Kernel_StartDedicatedServer(kernel, port)) {
        spdlog::error("failed to start dedicated server");
        Kernel_Destroy(kernel);
        return 1;
    }

    spdlog::info("dedicated server listening on 127.0.0.1:{}", port);
    network_example::game_server::GameServer game_server(kernel);

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
            game_server.handle_event(events[index]);
        }
        game_server.tick(kDeltaSeconds);
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    Kernel_Destroy(kernel);
    return 0;
}
