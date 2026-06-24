#include "client_app.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "game_server/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/src/tick_loop.h"
#include "protocol/public/sha256.h"

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
    } else if (sequence >= 72 && sequence < 96) {
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

std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        return {};
    }
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

std::string digest_hex(const std::uint8_t* digest, std::size_t size) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(kDigits[digest[index] >> 4]);
        result.push_back(kDigits[digest[index] & 0x0f]);
    }
    return result;
}

bool bundle_matches_manifest(
    const std::vector<std::uint8_t>& bundle,
    const KernelGameplayCatalogManifest& manifest) {
    if (bundle.size() != manifest.bundle_size) {
        return false;
    }
    const std::array<std::uint8_t, 32> digest =
        network_example::compute_sha256(bundle.data(), bundle.size());
    return std::equal(
        digest.begin(),
        digest.end(),
        std::begin(manifest.bundle_sha256));
}

bool wait_for_sync_state(
    KernelHandle* kernel,
    KernelGameplayCatalogSyncState first_terminal,
    KernelGameplayCatalogSyncState second_terminal,
    KernelGameplayCatalogSyncStatus* out_status) {
    constexpr float kWaitDeltaSeconds = 1.0f / 60.0f;
    for (std::uint32_t attempt = 0; attempt < 1800; ++attempt) {
        Kernel_Update(kernel, kWaitDeltaSeconds);
        out_status->struct_size = sizeof(*out_status);
        if (!Kernel_GetGameplayCatalogSyncStatus(kernel, out_status)) {
            return false;
        }
        if (out_status->state == first_terminal ||
            out_status->state == second_terminal) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return false;
}

bool start_client_with_catalog_sync(
    KernelHandle* kernel,
    const char* address,
    const char* cache_directory) {
    KernelGameplayCatalogSyncClientConfig sync_config{};
    sync_config.struct_size = sizeof(sync_config);
    if (!Kernel_StartClientCatalogSync(kernel, address, &sync_config)) {
        return false;
    }

    KernelGameplayCatalogSyncStatus status{};
    if (!wait_for_sync_state(
            kernel,
            KernelGameplayCatalogSyncState_ManifestReady,
            KernelGameplayCatalogSyncState_Failed,
            &status) ||
        status.state != KernelGameplayCatalogSyncState_ManifestReady) {
        spdlog::error(
            "catalog manifest sync failed error={}",
            static_cast<int>(status.error));
        return false;
    }

    const std::array<std::uint8_t, 32> endpoint_digest =
        network_example::compute_sha256(
            reinterpret_cast<const std::uint8_t*>(address),
            std::strlen(address));
    const std::filesystem::path bundle_path =
        std::filesystem::path(cache_directory) /
        digest_hex(endpoint_digest.data(), 16) /
        status.manifest.content_namespace /
        digest_hex(
            status.manifest.bundle_sha256,
            KERNEL_GAMEPLAY_CATALOG_SHA256_SIZE) /
        "bundle.zip";

    std::vector<std::uint8_t> bundle = read_binary_file(bundle_path);
    if (!bundle_matches_manifest(bundle, status.manifest)) {
        bundle.clear();
        if (!Kernel_RequestGameplayCatalogBundle(kernel) ||
            !wait_for_sync_state(
                kernel,
                KernelGameplayCatalogSyncState_BundleReady,
                KernelGameplayCatalogSyncState_Failed,
                &status) ||
            status.state != KernelGameplayCatalogSyncState_BundleReady) {
            spdlog::error(
                "catalog bundle download failed error={}",
                static_cast<int>(status.error));
            return false;
        }
        bundle.resize(status.manifest.bundle_size);
        std::uint32_t copied_size = 0;
        if (!Kernel_CopyGameplayCatalogBundle(
                kernel,
                bundle.data(),
                static_cast<std::uint32_t>(bundle.size()),
                &copied_size) ||
            copied_size != bundle.size() ||
            !bundle_matches_manifest(bundle, status.manifest)) {
            spdlog::error("downloaded catalog bundle verification failed");
            return false;
        }

        std::error_code filesystem_error;
        std::filesystem::create_directories(
            bundle_path.parent_path(),
            filesystem_error);
        if (!filesystem_error) {
            const std::filesystem::path temporary_path =
                bundle_path.string() + ".tmp";
            {
                std::ofstream output(temporary_path, std::ios::binary);
                output.write(
                    reinterpret_cast<const char*>(bundle.data()),
                    static_cast<std::streamsize>(bundle.size()));
            }
            std::filesystem::remove(bundle_path, filesystem_error);
            filesystem_error.clear();
            std::filesystem::rename(
                temporary_path,
                bundle_path,
                filesystem_error);
            if (filesystem_error) {
                spdlog::warn(
                    "catalog cache write failed: {}",
                    filesystem_error.message());
            }
        } else {
            spdlog::warn(
                "catalog cache directory unavailable: {}",
                filesystem_error.message());
        }
    }

    KernelGameplayCatalogLoadResult load_result{};
    load_result.struct_size = sizeof(load_result);
    if (!Kernel_LoadGameplayCatalogFromMemory(
            kernel,
            bundle.data(),
            static_cast<std::uint32_t>(bundle.size()),
            status.manifest.entry_path,
            &load_result) ||
        load_result.catalog_version != status.manifest.catalog_version ||
        load_result.catalog_hash != status.manifest.catalog_hash) {
        spdlog::error(
            "catalog bundle load failed error={} diagnostic={}",
            load_result.error_code,
            load_result.diagnostic);
        return false;
    }
    return Kernel_ContinueClientHandshake(kernel);
}

}  // namespace

int RunClient(
    const char* address,
    const char* gameplay_catalog_path,
    const char* gameplay_catalog_cache_directory) {
    log_native_build_info();

    KernelConfig config = default_config();
    KernelHandle* kernel = Kernel_Create(&config);
    if (kernel == nullptr) {
        spdlog::error("failed to create example client");
        return 1;
    }
    bool started = false;
    if (gameplay_catalog_cache_directory != nullptr &&
        gameplay_catalog_cache_directory[0] != '\0') {
        started = start_client_with_catalog_sync(
            kernel,
            address,
            gameplay_catalog_cache_directory);
    } else {
        try {
            const network_example::game_server::GameServerGameplayConfig
                gameplay_config =
                    network_example::game_server::
                        load_gameplay_config_from_catalog_file(
                            gameplay_catalog_path);
            started =
                network_example::game_server::load_kernel_gameplay_catalog(
                    kernel,
                    gameplay_config) &&
                Kernel_StartClient(kernel, address);
        } catch (const std::exception& error) {
            spdlog::error("failed to load gameplay catalog: {}", error.what());
        }
    }
    if (!started) {
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
