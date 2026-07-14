#include "dedicated_server_app.h"

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
    config.mode = KernelMode_DedicatedServer;
    config.tick = network_example::current_netcode_preset();
    config.max_render_states = 2048;
    config.max_events = 2048;
    config.network_stats.mode = GetAppNetworkStatsMode();
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

int RunDedicatedServer(
    std::uint16_t port,
    const char* gameplay_catalog_path,
    const char* gameplay_catalog_bundle_path,
    const char* gameplay_catalog_entry_path,
    const char* gameplay_catalog_content_namespace) {
    log_dedicated_server_build_info();

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
            spdlog::info(
                "loaded gameplay catalog bundle={} entry={} version={} hash={}",
                gameplay_catalog_bundle_path,
                gameplay_catalog_entry_path,
                gameplay_config.weapons.catalog_version,
                gameplay_config.weapons.catalog_hash);
        } else {
            gameplay_config =
                network_example::game_server::load_gameplay_config_from_catalog_file(
                    gameplay_catalog_path);
            spdlog::info(
                "loaded gameplay catalog file={} version={} hash={}",
                gameplay_catalog_path,
                gameplay_config.weapons.catalog_version,
                gameplay_config.weapons.catalog_hash);
        }
    } catch (const network_example::game_server::DataLoadError& error) {
        spdlog::error(
            "failed to load gameplay catalog error_code={} diagnostic={} "
            "path={} field={} line={} column={} template_kind={} "
            "template_id={}",
            error.error_code,
            error.what(),
            error.path,
            error.field,
            error.line,
            error.column,
            error.template_kind,
            error.template_id);
        return 1;
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
        spdlog::error("failed to start dedicated server");
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
    if (!Kernel_StartDedicatedServer(kernel, port)) {
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
