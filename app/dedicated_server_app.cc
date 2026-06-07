#include "dedicated_server_app.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
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

void log_dedicated_server_build_info() {
    KernelBuildInfo info{};
    if (!Kernel_GetBuildInfo(&info, sizeof(info))) {
        spdlog::error("[NetworkExample] Dedicated Server: Kernel_GetBuildInfo failed");
        return;
    }
    spdlog::info(
        "[NetworkExample] Dedicated Server:\n "
        "module_name             = {}\n "
        "module_file             = {}\n "
        "server_version          = {}\n "
        "protocol_version        = {}\n "
        "snapshot_schema_version = {}\n "
        "packet_schema_version   = {}\n "
        "git_commit              = {}\n "
        "build_platform          = {}\n "
        "build_config            = {}\n "
        "compiler_info           = {}",
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

int RunDedicatedServer(std::uint16_t port) {
    log_dedicated_server_build_info();

    network_example::game_server::GameServerGameplayConfig gameplay_config;
    try {
        gameplay_config =
            network_example::game_server::load_gameplay_config_from_catalog_file(
                "game_server/gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        spdlog::error("failed to load gameplay catalog: {}", error.what());
        return 1;
    }

    KernelConfig config = default_config();
    KernelHandle* kernel = Kernel_Create(&config);
    if (kernel == nullptr ||
        !network_example::game_server::load_kernel_gameplay_catalog(
            kernel,
            gameplay_config) ||
        !Kernel_StartDedicatedServer(kernel, port)) {
        spdlog::error("failed to start dedicated server");
        Kernel_Destroy(kernel);
        return 1;
    }

    spdlog::info("dedicated server listening on 127.0.0.1:{}", port);
    network_example::game_server::GameServer game_server(kernel, gameplay_config);

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
