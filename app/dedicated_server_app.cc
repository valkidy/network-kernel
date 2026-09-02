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
#include "protocol/public/network_packets.h"
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

KernelConfig default_config(const TickConfig& tick) {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick = tick;
    config.max_render_states = 2048;
    config.max_events = 2048;
    config.network_stats.mode = GetAppNetworkStatsMode();
    return config;
}

// What the operator actually chose, in the units the choice is made in. The
// rate on its own says neither of these, and both move with it.
void log_netcode_preset(const char* name, const TickConfig& tick) {
    const std::uint64_t snapshot_bits_per_second =
        static_cast<std::uint64_t>(network_example::kSnapshotSendBudgetBytes) *
        tick.snapshot_rate * 8u;
    spdlog::info(
        "[NetworkExample] Dedicated Server: netcode preset={} tick={} Hz "
        "snapshots={} Hz budget={} B ceiling={} kbit/s per client "
        "interpolation_delay={} ms",
        name,
        tick.server_tick_rate,
        tick.snapshot_rate,
        network_example::kSnapshotSendBudgetBytes,
        snapshot_bits_per_second / 1000u,
        network_example::interpolation_delay_ms(tick));
}

void log_dedicated_server_build_info(
    std::uint32_t physics_simulation,
    std::uint32_t physics_workers,
    std::uint32_t actor_blocking) {
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
        "compiler_info           = {}\n "
        "network-stats           = {}\n "
        "physics_simulation      = {}\n "
        "physics-workers         = {}\n "
        "actor-blocking          = {}\n ",
        info.module_name,
        info.module_file_name,
        info.module_version,
        info.protocol_version,
        info.snapshot_schema_version,
        info.packet_schema_version,
        info.git_commit,
        info.build_platform,
        info.build_config,
        info.compiler_info,
        static_cast<unsigned int>(GetAppNetworkStatsMode()),
        physics_simulation,
        physics_workers,
        actor_blocking);
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
    const char* gameplay_catalog_content_namespace,
    std::uint32_t physics_simulation,
    std::uint32_t physics_workers,
    std::uint32_t actor_blocking,
    const char* netcode_preset) {
    log_dedicated_server_build_info(
        physics_simulation,
        physics_workers,
        actor_blocking);

    network_example::game_server::GameServerGameplayConfig gameplay_config;
    std::vector<std::uint8_t> bundle_bytes;
    std::vector<std::uint8_t> collision_scene_bytes;
    try {
        if (gameplay_catalog_bundle_path != nullptr &&
            gameplay_catalog_bundle_path[0] != '\0') {
            bundle_bytes = read_binary_file(gameplay_catalog_bundle_path);
            gameplay_config =
                network_example::game_server::load_gameplay_config_from_bundle_memory(
                    bundle_bytes.data(),
                    static_cast<std::uint32_t>(bundle_bytes.size()),
                    gameplay_catalog_entry_path);
            if (!gameplay_config.static_collision_scene.entry_path.empty()) {
                collision_scene_bytes =
                    network_example::game_server::load_gameplay_bundle_entry_bytes(
                        bundle_bytes.data(),
                        static_cast<std::uint32_t>(bundle_bytes.size()),
                        gameplay_config.static_collision_scene.entry_path);
            }
            // The navmesh goes to game_server rather than to the kernel, so
            // unlike the collision scene it rides along on the config it was
            // named in.
            if (!gameplay_config.navigation_mesh.entry_path.empty()) {
                gameplay_config.navigation_mesh.artifact =
                    network_example::game_server::load_gameplay_bundle_entry_bytes(
                        bundle_bytes.data(),
                        static_cast<std::uint32_t>(bundle_bytes.size()),
                        gameplay_config.navigation_mesh.entry_path);
            }
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

    TickConfig tick{};
    if (!network_example::find_netcode_preset(netcode_preset, &tick)) {
        spdlog::error(
            "unknown netcode preset: {} (expected standard or responsive)",
            netcode_preset);
        return 1;
    }
    tick = network_example::with_tick_defaults(tick);
    log_netcode_preset(netcode_preset, tick);

    KernelConfig config = default_config(tick);
    KernelHandle* kernel = Kernel_Create(&config);
    KernelPhysicsConfig physics_config{};
    physics_config.struct_size = sizeof(physics_config);
    physics_config.physics_simulation = physics_simulation;
    physics_config.physics_workers = physics_workers;
    KernelSessionRulesConfig session_rules{};
    session_rules.struct_size = sizeof(session_rules);
    session_rules.actor_blocking_mode = actor_blocking;
    if (kernel == nullptr || !Kernel_SetPhysicsConfig(kernel, &physics_config) ||
        !Kernel_SetSessionRules(kernel, &session_rules) ||
        !network_example::game_server::load_kernel_gameplay_catalog(
            kernel,
            gameplay_config)) {
        spdlog::error("failed to start dedicated server");
        Kernel_Destroy(kernel);
        return 1;
    }
    if (!collision_scene_bytes.empty()) {
        KernelStaticCollisionSceneConfig scene_config{};
        scene_config.struct_size = sizeof(scene_config);
        scene_config.artifact_bytes = collision_scene_bytes.data();
        scene_config.artifact_size =
            static_cast<std::uint32_t>(collision_scene_bytes.size());
        scene_config.scene_id = gameplay_config.static_collision_scene.scene_id;
        scene_config.collider_id =
            gameplay_config.static_collision_scene.collider_id;
        scene_config.collision_layer =
            gameplay_config.static_collision_scene.collision_layer;
        if (!Kernel_SetStaticCollisionScene(kernel, &scene_config)) {
            spdlog::error("failed to register static collision scene");
            Kernel_Destroy(kernel);
            return 1;
        }
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

    network_example::game_server::GameServer game_server(kernel, gameplay_config);
    if (!game_server.preload_directors()) {
        spdlog::error("failed to preload dedicated server directors");
        Kernel_Destroy(kernel);
        return 1;
    }
    spdlog::info("dedicated server listening on 127.0.0.1:{}", port);

    constexpr float kDeltaSeconds = 1.0f / 30.0f;
    // Paced against absolute deadlines on a steady clock, not a fixed sleep.
    // The kernel is told 33.333 ms passed on every iteration, so if the loop
    // actually takes longer than that in real time -- sleep_for(33ms) is a
    // MINIMUM and the OS overshoots it, and the tick's own work lands on top --
    // the simulated clock falls behind the wall clock and never catches up.
    //
    // That drift is not cosmetic. A client derives its render instant from its
    // own real-time clock plus a smoothed constant offset, and a constant
    // cannot absorb a rate difference: the target outruns the newest snapshot,
    // client_render_server_time_us clamps to the buffer every frame, and the
    // interpolation delay collapses to zero with the render instant quantised
    // to whole ticks. Root and pose then step instead of moving continuously.
    // Measured before this: 28.6 ticks/s against a nominal 30, i.e. 4.8% slow.
    const auto tick_period =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(kDeltaSeconds));
    // Far enough behind that catching up tick-by-tick would only dig deeper, so
    // the schedule is rebased instead of spiralling.
    const auto resync_threshold = tick_period * 5;
    auto next_tick_deadline = std::chrono::steady_clock::now();
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

        next_tick_deadline += tick_period;
        const auto now = std::chrono::steady_clock::now();
        if (now < next_tick_deadline) {
            std::this_thread::sleep_until(next_tick_deadline);
        } else if (now - next_tick_deadline > resync_threshold) {
            next_tick_deadline = now;
        }
    }

    Kernel_Destroy(kernel);
    return 0;
}
