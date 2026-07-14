#include "host_server_app.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "game_server/game_server.h"
#include "client_app.h"
#include "kernel/public/kernel_api.h"
#include "kernel/src/tick_loop.h"

namespace {

constexpr std::uint64_t kCatalogBundleChunkBytes = 32u * 1024u;
constexpr std::uint64_t kCatalogSyncFixedProtocolBytes = 362u;
constexpr std::uint64_t kCatalogChunkProtocolBytes = 72u;

std::uint64_t catalog_bundle_chunk_count(std::uint64_t bundle_size) {
    return (bundle_size + kCatalogBundleChunkBytes - 1u) /
           kCatalogBundleChunkBytes;
}

std::uint64_t estimated_catalog_sync_protocol_bytes(std::uint64_t bundle_size) {
    return bundle_size + kCatalogSyncFixedProtocolBytes +
           catalog_bundle_chunk_count(bundle_size) * kCatalogChunkProtocolBytes;
}

KernelConfig default_config() {
    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick = network_example::current_netcode_preset();
    // The short host-server smoke sample reads render states after only a few
    // scripted frames, so keep per-tick snapshots for that local presentation path.
    config.tick.snapshot_rate = config.tick.server_tick_rate;
    config.max_render_states = 2048;
    config.max_events = 2048;
    config.network_stats.mode = GetAppNetworkStatsMode();
    return config;
}

PlayerInput scripted_input(std::uint32_t sequence) {
    PlayerInput input{};
    input.input_seq = sequence;
    input.client_action_time_us = static_cast<std::uint64_t>(sequence) * 33333u;
    input.move = KernelVec2{0.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    if (sequence == 2) {
        input.action_intent = ActionIntent{
            sequence, KernelActionBinding_PrimaryFire, 0u, 0u};
    }
    return input;
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

std::vector<std::uint8_t> read_binary_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        throw std::runtime_error(std::string("failed to open file: ") + path);
    }
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

}  // namespace

int RunHostServer(
    std::uint16_t port,
    const char* gameplay_catalog_path,
    const char* gameplay_catalog_bundle_path,
    const char* gameplay_catalog_entry_path,
    const char* gameplay_catalog_content_namespace,
    std::uint32_t frame_count) {
    log_native_build_info();

    network_example::game_server::GameServerGameplayConfig gameplay_config;
    std::vector<std::uint8_t> bundle_bytes;
    try {
        if (gameplay_catalog_bundle_path != nullptr &&
            gameplay_catalog_bundle_path[0] != '\0') {
            bundle_bytes = read_binary_file(gameplay_catalog_bundle_path);
            gameplay_config =
                network_example::game_server::load_gameplay_config_from_bundle_memory(
                    bundle_bytes.data(),
                    static_cast<std::uint32_t>(bundle_bytes.size()),
                    gameplay_catalog_entry_path);
        } else {
            gameplay_config =
                network_example::game_server::load_gameplay_config_from_catalog_file(
                    gameplay_catalog_path);
        }
    } catch (const std::exception& error) {
        spdlog::error("failed to load gameplay catalog: {}", error.what());
        return 1;
    }

    KernelConfig config = default_config();
    KernelHandle* kernel = Kernel_Create(&config);
    if (kernel == nullptr ||
        !network_example::game_server::load_kernel_gameplay_catalog(
            kernel,
            gameplay_config)) {
        spdlog::error("failed to start listen server");
        Kernel_Destroy(kernel);
        return 1;
    }
    if (!bundle_bytes.empty()) {
        KernelGameplayCatalogSyncServerConfig sync_config{};
        sync_config.struct_size = sizeof(sync_config);
        sync_config.bundle_bytes = bundle_bytes.data();
        sync_config.bundle_size =
            static_cast<std::uint32_t>(bundle_bytes.size());
        sync_config.entry_path = gameplay_catalog_entry_path;
        sync_config.content_namespace = gameplay_catalog_content_namespace;
        KernelGameplayCatalogManifest manifest{};
        manifest.struct_size = sizeof(manifest);
        if (!Kernel_SetGameplayCatalogSyncBundle(
                kernel,
                &sync_config,
                &manifest)) {
            spdlog::error(
                "failed to register gameplay catalog sync bundle "
                "bundle_bytes={} limit_bytes={}",
                bundle_bytes.size(),
                KERNEL_GAMEPLAY_CATALOG_SYNC_MAX_BUNDLE_SIZE);
            Kernel_Destroy(kernel);
            return 1;
        }
        spdlog::info(
            "registered gameplay catalog sync bundle bundle_bytes={} "
            "limit_bytes={} chunks={} estimated_cache_miss_protocol_bytes={}",
            bundle_bytes.size(),
            KERNEL_GAMEPLAY_CATALOG_SYNC_MAX_BUNDLE_SIZE,
            catalog_bundle_chunk_count(bundle_bytes.size()),
            estimated_catalog_sync_protocol_bytes(bundle_bytes.size()));
    }
    if (!Kernel_StartListenServer(kernel, port)) {
        spdlog::error("failed to start listen server");
        Kernel_Destroy(kernel);
        return 1;
    }

    network_example::game_server::GameServer game_server(kernel, gameplay_config);
    std::uint32_t sequence = 1;
    bool observed_agent_render = false;
    constexpr float kDeltaSeconds = 1.0f / 30.0f;
    for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
        const PlayerInput input = scripted_input(sequence++);
        Kernel_SubmitInput(kernel, 1, &input);
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
        std::array<RenderEntityState, 64> frame_states{};
        const std::uint32_t frame_state_count = Kernel_GetRenderStates(
            kernel,
            frame_states.data(),
            static_cast<std::uint32_t>(frame_states.size()));
        for (std::uint32_t index = 0; index < frame_state_count; ++index) {
            observed_agent_render =
                observed_agent_render ||
                (frame_states[index].entity_type == KernelEntityType_Actor &&
                 frame_states[index].actor_type == KernelActorType_Agent);
        }
        if (frame_count > 12) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    }

    spdlog::info(
        "host_server observed_agent_render={}",
        observed_agent_render ? 1 : 0);

    std::array<RenderEntityState, 64> states{};
    const std::uint32_t state_count =
        Kernel_GetRenderStates(kernel, states.data(), static_cast<std::uint32_t>(states.size()));
    for (std::uint32_t index = 0; index < state_count; ++index) {
        const RenderEntityState& state = states[index];
        spdlog::info(
            "render_state net_id={} type={} actor={} pos=({}, {}, {})",
            state.net_id,
            state.entity_type,
            state.actor_type,
            state.position.x,
            state.position.y,
            state.position.z);
    }

    Kernel_Destroy(kernel);
    return 0;
}
