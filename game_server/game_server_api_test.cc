#include "game_server/public/game_server_api.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint16_t kMaxReserveMagazines =
    std::numeric_limits<std::uint16_t>::max();

KernelConfig listen_server_config() {
    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;
    config.max_events = 64;
    config.max_render_states = 64;
    return config;
}

void handle_pending_events(
    KernelHandle* kernel,
    GameServerHandle* game_server) {
    std::array<KernelEvent, 32> events{};
    const std::uint32_t count = Kernel_PollEvents(
        kernel,
        events.data(),
        static_cast<std::uint32_t>(events.size()));
    for (std::uint32_t index = 0; index < count; ++index) {
        GameServer_HandleEvent(game_server, &events[index]);
    }
}

void run_game_server_frames(
    KernelHandle* kernel,
    GameServerHandle* game_server,
    int count) {
    for (int index = 0; index < count; ++index) {
        GameServer_Tick(game_server, 1.0f / 30.0f);
        Kernel_Update(kernel, 1.0f / 30.0f);
        handle_pending_events(kernel, game_server);
    }
    GameServer_Tick(game_server, 1.0f / 30.0f);
}

std::uint32_t query_enemy_count(KernelHandle* kernel) {
    std::array<KernelServerEntityState, 16> states{};
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    const std::uint32_t count = Kernel_ServerQueryEntities(
        kernel,
        1,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    std::uint32_t enemy_count = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].actor_type == KernelActorType_Agent) {
            ++enemy_count;
        }
    }
    return enemy_count;
}

bool pump_until_catalog_sync_state(
    KernelHandle* server,
    KernelHandle* client,
    KernelGameplayCatalogSyncState expected_state) {
    for (int iteration = 0; iteration < 2000; ++iteration) {
        Kernel_Update(server, 1.0f / 60.0f);
        Kernel_Update(client, 1.0f / 60.0f);
        KernelGameplayCatalogSyncStatus status{};
        status.struct_size = sizeof(status);
        if (!Kernel_GetGameplayCatalogSyncStatus(client, &status) ||
            status.state == KernelGameplayCatalogSyncState_Failed ||
            status.state == KernelGameplayCatalogSyncState_Disconnected) {
            return false;
        }
        if (status.state == expected_state) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

std::filesystem::path runfiles_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    assert(test_srcdir != nullptr);
    assert(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file.good());
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> read_binary_file(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

void append_u16(std::vector<std::uint8_t>* out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xffu));
    out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void append_u32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    append_u16(out, static_cast<std::uint16_t>(value & 0xffffu));
    append_u16(out, static_cast<std::uint16_t>((value >> 16u) & 0xffffu));
}

std::uint32_t crc32(const std::string& text) {
    std::uint32_t crc = 0xffffffffu;
    for (const unsigned char byte : text) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

std::vector<std::uint8_t> make_store_zip(
    const std::vector<std::pair<std::string, std::string>>& files) {
    struct CentralEntry {
        std::string path;
        std::string data;
        std::uint32_t crc = 0;
        std::uint32_t local_offset = 0;
    };

    std::vector<CentralEntry> central_entries;
    std::vector<std::uint8_t> zip;
    for (const auto& [path, data] : files) {
        const std::uint32_t entry_crc = crc32(data);
        const std::uint32_t local_offset =
            static_cast<std::uint32_t>(zip.size());
        append_u32(&zip, 0x04034b50u);
        append_u16(&zip, 20);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u32(&zip, entry_crc);
        append_u32(&zip, static_cast<std::uint32_t>(data.size()));
        append_u32(&zip, static_cast<std::uint32_t>(data.size()));
        append_u16(&zip, static_cast<std::uint16_t>(path.size()));
        append_u16(&zip, 0);
        zip.insert(zip.end(), path.begin(), path.end());
        zip.insert(zip.end(), data.begin(), data.end());
        central_entries.push_back(CentralEntry{path, data, entry_crc, local_offset});
    }

    const std::uint32_t central_offset = static_cast<std::uint32_t>(zip.size());
    for (const CentralEntry& entry : central_entries) {
        append_u32(&zip, 0x02014b50u);
        append_u16(&zip, 20);
        append_u16(&zip, 20);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u32(&zip, entry.crc);
        append_u32(&zip, static_cast<std::uint32_t>(entry.data.size()));
        append_u32(&zip, static_cast<std::uint32_t>(entry.data.size()));
        append_u16(&zip, static_cast<std::uint16_t>(entry.path.size()));
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u32(&zip, 0);
        append_u32(&zip, entry.local_offset);
        zip.insert(zip.end(), entry.path.begin(), entry.path.end());
    }
    const std::uint32_t central_size =
        static_cast<std::uint32_t>(zip.size()) - central_offset;
    append_u32(&zip, 0x06054b50u);
    append_u16(&zip, 0);
    append_u16(&zip, 0);
    append_u16(&zip, static_cast<std::uint16_t>(central_entries.size()));
    append_u16(&zip, static_cast<std::uint16_t>(central_entries.size()));
    append_u32(&zip, central_size);
    append_u32(&zip, central_offset);
    append_u16(&zip, 0);
    return zip;
}

std::vector<std::uint8_t> make_gameplay_bundle_zip() {
    const std::filesystem::path root = runfiles_root();
    std::vector<std::pair<std::string, std::string>> files;
    files.push_back({
        "gameplay_catalog.yaml",
        read_text_file(root / "game_server" / "gameplay_catalog.yaml")});

    // Enumerated rather than listed by hand. This fixture's whole point is a
    // bundle complete except for the collision mesh, and hardcoded lists drift
    // out of the real directories as templates are added -- which shows up as a
    // load failing on the wrong thing, long after the fact.
    const std::vector<std::string> template_dirs = {
        "collider_templates",
        "entity_templates",
        "weapon_templates",
        "action_templates",
        "action_graph_templates",
        "item_templates",
        "projectile_templates",
    };
    for (const std::string& directory : template_dirs) {
        const std::filesystem::path source = root / "game_server" / directory;
        std::vector<std::filesystem::path> entries;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(source)) {
            if (entry.is_regular_file() &&
                entry.path().extension() == ".yaml") {
                entries.push_back(entry.path());
            }
        }
        std::sort(entries.begin(), entries.end());
        for (const std::filesystem::path& entry : entries) {
            files.push_back({
                directory + "/" + entry.filename().string(),
                read_text_file(entry)});
        }
    }

    // The catalog resolves its skeletons before its collision scene, so a
    // bundle without these fails on a missing manifest and never reaches the
    // missing joltmesh.
    const std::filesystem::path generated =
        root / "game_server" / "skeleton_assets" / "generated";
    std::vector<std::filesystem::path> skeleton_entries;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(generated)) {
        if (entry.is_regular_file()) {
            skeleton_entries.push_back(entry.path());
        }
    }
    std::sort(skeleton_entries.begin(), skeleton_entries.end());
    for (const std::filesystem::path& entry : skeleton_entries) {
        files.push_back({
            "skeleton_assets/generated/" + entry.filename().string(),
            read_text_file(entry)});
    }
    return make_store_zip(files);
}

}  // namespace

int main() {
    GameServerAbiInfo info{};
    assert(GameServer_GetAbiInfo(&info, sizeof(info)));
    assert(info.struct_size == sizeof(GameServerAbiInfo));
    assert(info.abi_version == GAME_SERVER_ABI_VERSION);
    assert(info.abi_version == 5u);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_ENEMY_MANAGER) != 0);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_EVENT_HANDLING) != 0);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_DESPAWN_ALL) != 0);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_DIRECTORY) != 0);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_QUERY) != 0);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_GAMEPLAY_CATALOG_BUNDLE) != 0);
    assert(info.weapon_template_info_size == sizeof(GameServerWeaponTemplateInfo));
    assert(info.gameplay_catalog_load_result_size ==
           sizeof(KernelGameplayCatalogLoadResult));
    assert(!GameServer_GetAbiInfo(nullptr, sizeof(info)));
    assert(!GameServer_GetAbiInfo(&info, sizeof(info) - 1));

    assert(GameServer_Create(nullptr) == nullptr);
    assert(GameServer_CreateWithWeaponTemplateDirectory(nullptr, "x") == nullptr);
    KernelGameplayCatalogLoadResult load_result{};
    assert(GameServer_CreateWithGameplayCatalogFromMemory(
               nullptr,
               nullptr,
               0,
               "gameplay_catalog.yaml",
               &load_result) == nullptr);
    assert(load_result.status == KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_FAILED);
    assert(load_result.error_code ==
           KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_ARGUMENT);
    assert(load_result.diagnostic[0] != '\0');
    GameServer_Destroy(nullptr);
    GameServer_HandleEvent(nullptr, nullptr);
    GameServer_Tick(nullptr, 1.0f / 30.0f);
    assert(GameServer_GetEnemyCount(nullptr) == 0);
    GameServerWeaponTemplateInfo template_info{};
    template_info.struct_size = sizeof(template_info);
    assert(!GameServer_QueryWeaponTemplate(nullptr, 0, &template_info));
    GameServer_DespawnAll(nullptr, KernelDespawnReason_Destroyed);

    KernelConfig config = listen_server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);

    const std::vector<std::uint8_t> missing_collision_bundle =
        make_gameplay_bundle_zip();
    load_result = KernelGameplayCatalogLoadResult{};
    assert(!Kernel_LoadGameplayCatalogFromMemory(
        kernel,
        missing_collision_bundle.data(),
        static_cast<std::uint32_t>(missing_collision_bundle.size()),
        "gameplay_catalog.yaml",
        &load_result));
    assert(load_result.status == KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_FAILED);
    assert(
        load_result.error_code ==
        KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_MISSING_BUNDLE_ENTRY);
    assert(
        std::string(load_result.path) ==
        "mesh_assets/jolt/undulating.joltmesh");
    assert(load_result.diagnostic[0] != '\0');

    const std::vector<std::uint8_t> gameplay_bundle = read_binary_file(
        runfiles_root() / "game_server" / "gameplay_catalog_bundle" /
        "bundle.zip");
    load_result = KernelGameplayCatalogLoadResult{};
    const bool loaded_catalog = Kernel_LoadGameplayCatalogFromMemory(
        kernel,
        gameplay_bundle.data(),
        static_cast<std::uint32_t>(gameplay_bundle.size()),
        "gameplay_catalog.yaml",
        &load_result);
    if (!loaded_catalog) {
        std::fprintf(
            stderr,
            "catalog load failed: %s path=%s field=%s\n",
            load_result.diagnostic,
            load_result.path,
            load_result.field);
    }
    assert(loaded_catalog);
    assert(load_result.status == KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_SUCCESS);
    assert(load_result.error_code == KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_NONE);
    assert(load_result.catalog_version == 16);
    assert(load_result.catalog_hash != 0);
    assert(load_result.projectile_template_count > 0);
    assert(load_result.collider_template_count == 14);
    assert(load_result.collider_binding_count == 0);
    KernelSessionRulesConfig session_rules{};
    session_rules.struct_size = sizeof(session_rules);
    session_rules.actor_blocking_mode = KernelActorBlockingMode_Predicted;
    assert(Kernel_SetSessionRules(kernel, &session_rules));
    KernelGameplayCatalogSyncServerConfig sync_server_config{};
    sync_server_config.struct_size = sizeof(sync_server_config);
    sync_server_config.bundle_bytes = gameplay_bundle.data();
    sync_server_config.bundle_size =
        static_cast<std::uint32_t>(gameplay_bundle.size());
    sync_server_config.entry_path = "gameplay_catalog.yaml";
    sync_server_config.content_namespace = "regression";
    KernelGameplayCatalogManifest sync_manifest{};
    sync_manifest.struct_size = sizeof(sync_manifest);
    assert(Kernel_SetGameplayCatalogSyncBundle(
        kernel,
        &sync_server_config,
        &sync_manifest));
    assert(Kernel_StartListenServer(kernel, 7777));

    KernelConfig client_config = config;
    client_config.mode = KernelMode_Client;
    KernelHandle* catalog_client = Kernel_Create(&client_config);
    assert(catalog_client != nullptr);
    KernelGameplayCatalogSyncClientConfig sync_client_config{};
    sync_client_config.struct_size = sizeof(sync_client_config);
    sync_client_config.max_bundle_size =
        static_cast<std::uint32_t>(gameplay_bundle.size());
    sync_client_config.timeout_ms = 5000u;
    assert(Kernel_StartClientCatalogSync(
        catalog_client,
        "127.0.0.1:7777",
        &sync_client_config));
    assert(pump_until_catalog_sync_state(
        kernel,
        catalog_client,
        KernelGameplayCatalogSyncState_ManifestReady));
    load_result = KernelGameplayCatalogLoadResult{};
    assert(Kernel_LoadGameplayCatalogFromMemory(
        catalog_client,
        gameplay_bundle.data(),
        static_cast<std::uint32_t>(gameplay_bundle.size()),
        "gameplay_catalog.yaml",
        &load_result));
    assert(Kernel_ContinueClientHandshake(catalog_client));
    assert(pump_until_catalog_sync_state(
        kernel,
        catalog_client,
        KernelGameplayCatalogSyncState_Ready));
    KernelLocalPlayerInfo local_player_info{};
    assert(Kernel_GetLocalPlayerInfo(catalog_client, &local_player_info));
    assert(local_player_info.connected != 0u);
    assert(local_player_info.has_welcome != 0u);
    assert(local_player_info.peer_id != 0u);
    assert(local_player_info.player_net_id != 0u);

    GameServerHandle* game_server = GameServer_Create(kernel);
    assert(game_server != nullptr);
    assert(GameServer_QueryWeaponTemplate(game_server, 2, &template_info));
    assert(template_info.weapon_id == 2);
    assert(template_info.fire_mode == KernelWeaponFireMode_Projectile);
    assert(template_info.mechanics.damage == 1);
    assert(template_info.mechanics.magazine_size == 120);
    assert(template_info.mechanics.reserve_magazines == kMaxReserveMagazines);
    assert(template_info.name[0] == 'P');
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(game_server, 4, &template_info));
    assert(template_info.weapon_id == 4);
    assert(template_info.fire_mode == KernelWeaponFireMode_Projectile);
    assert(template_info.mechanics.projectile_template_id == 4);
    assert(template_info.name[0] == 'F');
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(game_server, 5, &template_info));
    assert(template_info.weapon_id == 5);
    assert(template_info.fire_mode == KernelWeaponFireMode_Projectile);
    assert(template_info.mechanics.projectile_template_id == 5);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(game_server, 6, &template_info));
    assert(template_info.weapon_id == 6);
    assert(template_info.fire_mode == KernelWeaponFireMode_Projectile);
    assert(template_info.mechanics.projectile_template_id == 6);
    handle_pending_events(kernel, game_server);
    std::array<KernelInventoryContainerView, 2> inventory_containers{};
    for (KernelInventoryContainerView& container : inventory_containers) {
        container.struct_size = sizeof(KernelInventoryContainerView);
    }
    assert(Kernel_CopyOwnedInventoryContainers(
               kernel,
               local_player_info.player_net_id,
               inventory_containers.data(),
               static_cast<std::uint32_t>(inventory_containers.size())) == 1);
    assert(inventory_containers[0].slot_capacity == 8);
    assert(inventory_containers[0].occupied_slot_count == 3);
    std::array<KernelItemInstanceView, 8> inventory_items{};
    for (KernelItemInstanceView& item : inventory_items) {
        item.struct_size = sizeof(KernelItemInstanceView);
    }
    assert(Kernel_CopyInventorySlots(
               kernel,
               inventory_containers[0].inventory_container_id,
               inventory_items.data(),
               static_cast<std::uint32_t>(inventory_items.size())) == 3);
    assert(inventory_items[0].slot == 0);
    assert(inventory_items[0].item_template_id == 3002);
    assert(inventory_items[0].quantity == 5);
    assert(inventory_items[1].slot == 1);
    assert(inventory_items[1].item_template_id == 3003);
    assert(inventory_items[1].quantity == 1);
    assert(inventory_items[1].portable_state_field_count == 1);
    assert(inventory_items[1].portable_state_fields[0].uint32_default == 3);
    assert(inventory_items[2].slot == 2);
    assert(inventory_items[2].item_template_id == 3004);
    assert(inventory_items[2].quantity == 1);
    assert(inventory_items[2].portable_state_field_count == 1);
    assert(inventory_items[2].portable_state_fields[0].uint32_default == 1);

    KernelEvent duplicate_player_joined{};
    duplicate_player_joined.type = KernelEventType_PlayerJoined;
    duplicate_player_joined.net_id = local_player_info.player_net_id;
    GameServer_HandleEvent(game_server, &duplicate_player_joined);
    assert(Kernel_CopyOwnedInventoryContainers(
               kernel,
               local_player_info.player_net_id,
               inventory_containers.data(),
               static_cast<std::uint32_t>(inventory_containers.size())) == 1);
    assert(Kernel_CopyInventorySlots(
               kernel,
               inventory_containers[0].inventory_container_id,
               inventory_items.data(),
               static_cast<std::uint32_t>(inventory_items.size())) == 3);
    Kernel_Destroy(catalog_client);
    run_game_server_frames(kernel, game_server, 3);
    assert(GameServer_GetEnemyCount(game_server) == 10);
    assert(query_enemy_count(kernel) == 2);

    GameServer_DespawnAll(game_server, KernelDespawnReason_Destroyed);
    GameServer_Tick(game_server, 1.0f / 30.0f);
    assert(GameServer_GetEnemyCount(game_server) == 0);
    Kernel_Update(kernel, 1.0f / 30.0f);
    assert(query_enemy_count(kernel) == 0);

    GameServer_Destroy(game_server);
    game_server = nullptr;

    const std::filesystem::path template_dir =
        runfiles_root() / "game_server" / "weapon_templates";
    GameServerHandle* yaml_game_server =
        GameServer_CreateWithWeaponTemplateDirectory(kernel, template_dir.string().c_str());
    assert(yaml_game_server != nullptr);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(yaml_game_server, 2, &template_info));
    assert(template_info.mechanics.damage == 1);
    assert(template_info.mechanics.magazine_size == 120);
    assert(template_info.mechanics.reserve_magazines == kMaxReserveMagazines);
    assert(template_info.mechanics.projectile_template_id == 2);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(yaml_game_server, 4, &template_info));
    assert(template_info.mechanics.fire_mode == KernelWeaponFireMode_Projectile);
    assert(template_info.mechanics.projectile_template_id == 4);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(yaml_game_server, 5, &template_info));
    assert(template_info.mechanics.fire_mode == KernelWeaponFireMode_Projectile);
    assert(template_info.mechanics.projectile_template_id == 5);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(yaml_game_server, 6, &template_info));
    assert(template_info.mechanics.fire_mode == KernelWeaponFireMode_Projectile);
    assert(template_info.mechanics.projectile_template_id == 6);
    GameServer_Destroy(yaml_game_server);

    KernelHandle* bundle_kernel = Kernel_Create(&config);
    assert(bundle_kernel != nullptr);
    load_result = KernelGameplayCatalogLoadResult{};
    GameServerHandle* bundle_game_server =
        GameServer_CreateWithGameplayCatalogFromMemory(
            bundle_kernel,
            gameplay_bundle.data(),
            static_cast<std::uint32_t>(gameplay_bundle.size()),
            "gameplay_catalog.yaml",
            &load_result);
    assert(bundle_game_server != nullptr);
    assert(load_result.status == KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_SUCCESS);
    assert(load_result.catalog_hash != 0);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(bundle_game_server, 2, &template_info));
    assert(template_info.mechanics.pellet_count == 3);
    assert(template_info.mechanics.pellet_spread == 15.0f);
    GameServer_Destroy(bundle_game_server);
    Kernel_Destroy(bundle_kernel);
    Kernel_Destroy(kernel);

    KernelConfig dedicated_config = listen_server_config();
    dedicated_config.mode = KernelMode_DedicatedServer;
    KernelHandle* dedicated_kernel = Kernel_Create(&dedicated_config);
    assert(dedicated_kernel != nullptr);
    load_result = KernelGameplayCatalogLoadResult{};
    const std::vector<std::uint8_t> unsupported_version_bundle = make_store_zip({
        {"gameplay_catalog.yaml", "catalog_version: 1\n"},
    });
    assert(!Kernel_LoadGameplayCatalogFromMemory(
        dedicated_kernel,
        unsupported_version_bundle.data(),
        static_cast<std::uint32_t>(unsupported_version_bundle.size()),
        "gameplay_catalog.yaml",
        &load_result));
    assert(load_result.status == KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_FAILED);
    assert(
        load_result.error_code ==
        KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNSUPPORTED_CATALOG_VERSION);
    assert(std::string(load_result.path) == "gameplay_catalog.yaml");
    assert(std::string(load_result.field) == "catalog_version");
    assert(load_result.diagnostic[0] != '\0');

    load_result = KernelGameplayCatalogLoadResult{};
    assert(Kernel_LoadGameplayCatalogFromMemory(
        dedicated_kernel,
        gameplay_bundle.data(),
        static_cast<std::uint32_t>(gameplay_bundle.size()),
        "gameplay_catalog.yaml",
        &load_result));
    assert(Kernel_StartDedicatedServer(dedicated_kernel, 7798));
    Kernel_Destroy(dedicated_kernel);
    return 0;
}
