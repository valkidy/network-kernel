#include "game_server/public/game_server_api.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::uint16_t kMaxReserveMagazines =
    std::numeric_limits<std::uint16_t>::max();
// Measured at 121 frames (~4 s) from the player joining to the first
// gingerbread; the cap leaves room without letting a stuck mission spin.
constexpr int kMaxFramesUntilEnemies = 30 * 20;

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
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> read_binary_file(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good());
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

// The collision mesh the catalog names, read from the catalog itself so the
// missing-entry check follows the terrain the game actually ships with.
std::string static_collision_entry_path() {
    std::istringstream catalog(read_text_file(
        runfiles_root() / "game_server" / "gameplay_catalog" /
        "gameplay_catalog.yaml"));
    bool in_scene = false;
    for (std::string line; std::getline(catalog, line);) {
        if (line.rfind("static_collision_scene:", 0) == 0) {
            in_scene = true;
            continue;
        }
        if (!in_scene) {
            continue;
        }
        if (!line.empty() && line[0] != ' ') {
            break;
        }
        const std::size_t key = line.find_first_not_of(' ');
        const std::string entry = "entry_path:";
        if (key != std::string::npos && line.compare(key, entry.size(), entry) == 0) {
            const std::size_t value =
                line.find_first_not_of(' ', key + entry.size());
            return value == std::string::npos ? std::string() : line.substr(value);
        }
    }
    return std::string();
}

std::filesystem::path catalog_path(const std::string& relative) {
    return runfiles_root() / "game_server" / "gameplay_catalog" / relative;
}

// The value of a top-level `key: value` line, comment stripped; empty if absent.
std::string yaml_top_level_value(
    const std::filesystem::path& path,
    const std::string& key) {
    std::istringstream yaml(read_text_file(path));
    const std::string prefix = key + ":";
    for (std::string line; std::getline(yaml, line);) {
        if (line.rfind(prefix, 0) != 0) {
            continue;
        }
        std::string value = line.substr(prefix.size());
        value = value.substr(0, value.find('#'));
        const std::size_t begin = value.find_first_not_of(' ');
        const std::size_t end = value.find_last_not_of(' ');
        return begin == std::string::npos ? std::string()
                                          : value.substr(begin, end - begin + 1);
    }
    return std::string();
}

std::uint32_t yaml_top_level_uint(
    const std::filesystem::path& path,
    const std::string& key) {
    const std::string value = yaml_top_level_value(path, key);
    require(!value.empty());
    return static_cast<std::uint32_t>(std::stoul(value));
}

struct LoadoutSlot {
    std::uint32_t item_template_id = 0;
    std::uint32_t quantity = 0;
    bool stateful = false;
};

// The player template's starting inventory, in slot order. Item names resolve
// to ids through the item template file names ($id_$name.yaml), so the loadout
// can change in the catalog without this test following by hand.
std::vector<LoadoutSlot> player_loadout() {
    std::unordered_map<std::string, LoadoutSlot> items_by_name;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(catalog_path("item_templates"))) {
        const std::string stem = entry.path().stem().string();
        const std::size_t separator = stem.find('_');
        if (entry.path().extension() != ".yaml" || separator == std::string::npos) {
            continue;
        }
        LoadoutSlot item;
        item.item_template_id =
            static_cast<std::uint32_t>(std::stoul(stem.substr(0, separator)));
        item.stateful =
            read_text_file(entry.path()).find("\nportable_state:") != std::string::npos;
        items_by_name[stem.substr(separator + 1)] = item;
    }

    std::istringstream yaml(
        read_text_file(catalog_path("entity_templates/1_player.yaml")));
    std::vector<LoadoutSlot> loadout;
    bool in_slots = false;
    const std::string item_key = "- item_template:";
    const std::string quantity_key = "quantity:";
    for (std::string line; std::getline(yaml, line);) {
        if (line.rfind("inventory_slots:", 0) == 0) {
            in_slots = true;
            continue;
        }
        if (!in_slots) {
            continue;
        }
        if (!line.empty() && line[0] != ' ' && line[0] != '#') {
            break;
        }
        const std::size_t text = line.find_first_not_of(' ');
        if (text == std::string::npos) {
            continue;
        }
        if (line.compare(text, item_key.size(), item_key) == 0) {
            const std::string name = line.substr(
                line.find_first_not_of(' ', text + item_key.size()));
            const auto found = items_by_name.find(name);
            require(found != items_by_name.end());
            loadout.push_back(found->second);
        } else if (line.compare(text, quantity_key.size(), quantity_key) == 0) {
            require(!loadout.empty());
            loadout.back().quantity = static_cast<std::uint32_t>(
                std::stoul(line.substr(text + quantity_key.size())));
        }
    }
    return loadout;
}

std::uint32_t count_yaml_files(const std::filesystem::path& directory) {
    std::uint32_t count = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".yaml") {
            ++count;
        }
    }
    return count;
}

std::vector<std::uint8_t> make_gameplay_bundle_zip() {
    const std::filesystem::path root = runfiles_root();
    std::vector<std::pair<std::string, std::string>> files;
    files.push_back({
        "gameplay_catalog.yaml",
        read_text_file(
            root / "game_server" / "gameplay_catalog" /
            "gameplay_catalog.yaml")});

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
        "status_effect_templates",
        "item_templates",
        "projectile_templates",
    };
    for (const std::string& directory : template_dirs) {
        const std::filesystem::path source =
            root / "game_server" / "gameplay_catalog" / directory;
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
        root / "game_server" / "gameplay_catalog" / "skeleton_assets" /
        "generated";
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
    require(GameServer_GetAbiInfo(&info, sizeof(info)));
    require(info.struct_size == sizeof(GameServerAbiInfo));
    require(info.abi_version == GAME_SERVER_ABI_VERSION);
    require(info.abi_version == 5u);
    require((info.capability_flags & GAME_SERVER_CAPABILITY_ENEMY_MANAGER) != 0);
    require((info.capability_flags & GAME_SERVER_CAPABILITY_EVENT_HANDLING) != 0);
    require((info.capability_flags & GAME_SERVER_CAPABILITY_DESPAWN_ALL) != 0);
    require((info.capability_flags & GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_DIRECTORY) != 0);
    require((info.capability_flags & GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_QUERY) != 0);
    require((info.capability_flags & GAME_SERVER_CAPABILITY_GAMEPLAY_CATALOG_BUNDLE) != 0);
    require(info.weapon_template_info_size == sizeof(GameServerWeaponTemplateInfo));
    require(info.gameplay_catalog_load_result_size ==
           sizeof(KernelGameplayCatalogLoadResult));
    require(!GameServer_GetAbiInfo(nullptr, sizeof(info)));
    require(!GameServer_GetAbiInfo(&info, sizeof(info) - 1));

    require(GameServer_Create(nullptr) == nullptr);
    require(GameServer_CreateWithWeaponTemplateDirectory(nullptr, "x") == nullptr);
    KernelGameplayCatalogLoadResult load_result{};
    require(GameServer_CreateWithGameplayCatalogFromMemory(
               nullptr,
               nullptr,
               0,
               "gameplay_catalog.yaml",
               &load_result) == nullptr);
    require(load_result.status == KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_FAILED);
    require(load_result.error_code ==
           KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_ARGUMENT);
    require(load_result.diagnostic[0] != '\0');
    GameServer_Destroy(nullptr);
    GameServer_HandleEvent(nullptr, nullptr);
    GameServer_Tick(nullptr, 1.0f / 30.0f);
    require(GameServer_GetEnemyCount(nullptr) == 0);
    GameServerWeaponTemplateInfo template_info{};
    template_info.struct_size = sizeof(template_info);
    require(!GameServer_QueryWeaponTemplate(nullptr, 0, &template_info));
    GameServer_DespawnAll(nullptr, KernelDespawnReason_Destroyed);

    KernelConfig config = listen_server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);

    const std::vector<std::uint8_t> missing_collision_bundle =
        make_gameplay_bundle_zip();
    load_result = KernelGameplayCatalogLoadResult{};
    require(!Kernel_LoadGameplayCatalogFromMemory(
        kernel,
        missing_collision_bundle.data(),
        static_cast<std::uint32_t>(missing_collision_bundle.size()),
        "gameplay_catalog.yaml",
        &load_result));
    require(load_result.status == KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_FAILED);
    require(
        load_result.error_code ==
        KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_MISSING_BUNDLE_ENTRY);
    const std::string collision_entry = static_collision_entry_path();
    require(!collision_entry.empty());
    require(std::string(load_result.path) == collision_entry);
    require(load_result.diagnostic[0] != '\0');

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
    require(loaded_catalog);
    require(load_result.status == KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_SUCCESS);
    require(load_result.error_code == KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_NONE);
    require(load_result.catalog_version == 16);
    require(load_result.catalog_hash != 0);
    require(load_result.projectile_template_count > 0);
    require(
        load_result.collider_template_count ==
        count_yaml_files(
            runfiles_root() / "game_server" / "gameplay_catalog" /
            "collider_templates"));
    require(load_result.collider_binding_count == 0);
    KernelSessionRulesConfig session_rules{};
    session_rules.struct_size = sizeof(session_rules);
    session_rules.actor_blocking_mode = KernelActorBlockingMode_Predicted;
    require(Kernel_SetSessionRules(kernel, &session_rules));
    KernelGameplayCatalogSyncServerConfig sync_server_config{};
    sync_server_config.struct_size = sizeof(sync_server_config);
    sync_server_config.bundle_bytes = gameplay_bundle.data();
    sync_server_config.bundle_size =
        static_cast<std::uint32_t>(gameplay_bundle.size());
    sync_server_config.entry_path = "gameplay_catalog.yaml";
    sync_server_config.content_namespace = "regression";
    KernelGameplayCatalogManifest sync_manifest{};
    sync_manifest.struct_size = sizeof(sync_manifest);
    require(Kernel_SetGameplayCatalogSyncBundle(
        kernel,
        &sync_server_config,
        &sync_manifest));
    require(Kernel_StartListenServer(kernel, 8046));

    KernelConfig client_config = config;
    client_config.mode = KernelMode_Client;
    KernelHandle* catalog_client = Kernel_Create(&client_config);
    require(catalog_client != nullptr);
    KernelGameplayCatalogSyncClientConfig sync_client_config{};
    sync_client_config.struct_size = sizeof(sync_client_config);
    sync_client_config.max_bundle_size =
        static_cast<std::uint32_t>(gameplay_bundle.size());
    sync_client_config.timeout_ms = 5000u;
    require(Kernel_StartClientCatalogSync(
        catalog_client,
        "127.0.0.1:8046",
        &sync_client_config));
    require(pump_until_catalog_sync_state(
        kernel,
        catalog_client,
        KernelGameplayCatalogSyncState_ManifestReady));
    load_result = KernelGameplayCatalogLoadResult{};
    require(Kernel_LoadGameplayCatalogFromMemory(
        catalog_client,
        gameplay_bundle.data(),
        static_cast<std::uint32_t>(gameplay_bundle.size()),
        "gameplay_catalog.yaml",
        &load_result));
    require(Kernel_ContinueClientHandshake(catalog_client));
    require(pump_until_catalog_sync_state(
        kernel,
        catalog_client,
        KernelGameplayCatalogSyncState_Ready));
    KernelLocalPlayerInfo local_player_info{};
    require(Kernel_GetLocalPlayerInfo(catalog_client, &local_player_info));
    require(local_player_info.connected != 0u);
    require(local_player_info.has_welcome != 0u);
    require(local_player_info.peer_id != 0u);
    require(local_player_info.player_net_id != 0u);

    GameServerHandle* game_server = GameServer_Create(kernel);
    require(game_server != nullptr);
    require(GameServer_QueryWeaponTemplate(game_server, 2, &template_info));
    require(template_info.weapon_id == 2);
    require(template_info.fire_mode == KernelWeaponFireMode_Projectile);
    require(template_info.mechanics.damage == 1);
    require(
        template_info.mechanics.magazine_size ==
        yaml_top_level_uint(
            catalog_path("weapon_templates/2_weapon_spammer.yaml"),
            "magazine_size"));
    require(template_info.mechanics.reserve_magazines == kMaxReserveMagazines);
    require(template_info.name[0] == 'P');
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    require(GameServer_QueryWeaponTemplate(game_server, 4, &template_info));
    require(template_info.weapon_id == 4);
    require(template_info.fire_mode == KernelWeaponFireMode_Projectile);
    require(template_info.mechanics.projectile_template_id == 4);
    require(template_info.name[0] == 'F');
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    require(GameServer_QueryWeaponTemplate(game_server, 5, &template_info));
    require(template_info.weapon_id == 5);
    require(template_info.fire_mode == KernelWeaponFireMode_Projectile);
    require(template_info.mechanics.projectile_template_id == 5);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    require(GameServer_QueryWeaponTemplate(game_server, 6, &template_info));
    require(template_info.weapon_id == 6);
    require(template_info.fire_mode == KernelWeaponFireMode_Projectile);
    require(template_info.mechanics.projectile_template_id == 6);
    handle_pending_events(kernel, game_server);
    std::array<KernelInventoryContainerView, 2> inventory_containers{};
    for (KernelInventoryContainerView& container : inventory_containers) {
        container.struct_size = sizeof(KernelInventoryContainerView);
    }
    // The item container and, since P3, the weapon container that holds the
    // template's weapons as weapon items.
    require(Kernel_CopyOwnedInventoryContainers(
               kernel,
               local_player_info.player_net_id,
               inventory_containers.data(),
               static_cast<std::uint32_t>(inventory_containers.size())) == 2);
    if (inventory_containers[0].container_kind != KernelInventoryContainerKind_Items) {
        std::swap(inventory_containers[0], inventory_containers[1]);
    }
    require(inventory_containers[0].container_kind == KernelInventoryContainerKind_Items);
    require(inventory_containers[1].container_kind == KernelInventoryContainerKind_Weapons);
    const std::filesystem::path player_template =
        catalog_path("entity_templates/1_player.yaml");
    const std::vector<LoadoutSlot> loadout = player_loadout();
    require(!loadout.empty());
    require(
        inventory_containers[0].slot_capacity ==
        yaml_top_level_uint(player_template, "inventory_slot_capacity"));
    require(inventory_containers[0].occupied_slot_count == loadout.size());
    std::array<KernelItemInstanceView, 16> inventory_items{};
    for (KernelItemInstanceView& item : inventory_items) {
        item.struct_size = sizeof(KernelItemInstanceView);
    }
    require(Kernel_CopyInventorySlots(
               kernel,
               inventory_containers[0].inventory_container_id,
               inventory_items.data(),
               static_cast<std::uint32_t>(inventory_items.size())) ==
           loadout.size());
    for (std::size_t slot = 0; slot < loadout.size(); ++slot) {
        const KernelItemInstanceView& item = inventory_items[slot];
        require(item.slot == slot);
        require(item.item_template_id == loadout[slot].item_template_id);
        require(item.quantity == loadout[slot].quantity);
        require((item.portable_state_field_count != 0) == loadout[slot].stateful);
    }

    KernelEvent duplicate_player_joined{};
    duplicate_player_joined.type = KernelEventType_PlayerJoined;
    duplicate_player_joined.net_id = local_player_info.player_net_id;
    GameServer_HandleEvent(game_server, &duplicate_player_joined);
    // A second join makes neither container again.
    require(Kernel_CopyOwnedInventoryContainers(
               kernel,
               local_player_info.player_net_id,
               inventory_containers.data(),
               static_cast<std::uint32_t>(inventory_containers.size())) == 2);
    if (inventory_containers[0].container_kind != KernelInventoryContainerKind_Items) {
        std::swap(inventory_containers[0], inventory_containers[1]);
    }
    require(Kernel_CopyInventorySlots(
               kernel,
               inventory_containers[0].inventory_container_id,
               inventory_items.data(),
               static_cast<std::uint32_t>(inventory_items.size())) ==
           loadout.size());
    // Enemies come from the catalog's game_rule director: it waits for a
    // player, places the nests, and the nests' spawners put gingerbread out.
    // So the client stays connected until they do, and the count is whatever
    // the mission produces rather than a number this test would have to track.
    int frames_until_enemies = 0;
    while (frames_until_enemies < kMaxFramesUntilEnemies &&
           (GameServer_GetEnemyCount(game_server) == 0 ||
            query_enemy_count(kernel) == 0)) {
        run_game_server_frames(kernel, game_server, 1);
        Kernel_Update(catalog_client, 1.0f / 30.0f);
        ++frames_until_enemies;
    }
    require(GameServer_GetEnemyCount(game_server) > 0);
    require(query_enemy_count(kernel) > 0);
    Kernel_Destroy(catalog_client);

    GameServer_DespawnAll(game_server, KernelDespawnReason_Destroyed);
    GameServer_Tick(game_server, 1.0f / 30.0f);
    require(GameServer_GetEnemyCount(game_server) == 0);
    Kernel_Update(kernel, 1.0f / 30.0f);
    require(query_enemy_count(kernel) == 0);

    GameServer_Destroy(game_server);
    game_server = nullptr;

    const std::filesystem::path template_dir =
        runfiles_root() / "game_server" / "gameplay_catalog" / "weapon_templates";
    GameServerHandle* yaml_game_server =
        GameServer_CreateWithWeaponTemplateDirectory(kernel, template_dir.string().c_str());
    require(yaml_game_server != nullptr);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    require(GameServer_QueryWeaponTemplate(yaml_game_server, 2, &template_info));
    require(template_info.mechanics.damage == 1);
    require(
        template_info.mechanics.magazine_size ==
        yaml_top_level_uint(
            catalog_path("weapon_templates/2_weapon_spammer.yaml"),
            "magazine_size"));
    require(template_info.mechanics.reserve_magazines == kMaxReserveMagazines);
    require(template_info.mechanics.projectile_template_id == 2);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    require(GameServer_QueryWeaponTemplate(yaml_game_server, 4, &template_info));
    require(template_info.mechanics.fire_mode == KernelWeaponFireMode_Projectile);
    require(template_info.mechanics.projectile_template_id == 4);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    require(GameServer_QueryWeaponTemplate(yaml_game_server, 5, &template_info));
    require(template_info.mechanics.fire_mode == KernelWeaponFireMode_Projectile);
    require(template_info.mechanics.projectile_template_id == 5);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    require(GameServer_QueryWeaponTemplate(yaml_game_server, 6, &template_info));
    require(template_info.mechanics.fire_mode == KernelWeaponFireMode_Projectile);
    require(template_info.mechanics.projectile_template_id == 6);
    GameServer_Destroy(yaml_game_server);

    KernelHandle* bundle_kernel = Kernel_Create(&config);
    require(bundle_kernel != nullptr);
    load_result = KernelGameplayCatalogLoadResult{};
    GameServerHandle* bundle_game_server =
        GameServer_CreateWithGameplayCatalogFromMemory(
            bundle_kernel,
            gameplay_bundle.data(),
            static_cast<std::uint32_t>(gameplay_bundle.size()),
            "gameplay_catalog.yaml",
            &load_result);
    require(bundle_game_server != nullptr);
    require(load_result.status == KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_SUCCESS);
    require(load_result.catalog_hash != 0);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    require(GameServer_QueryWeaponTemplate(bundle_game_server, 2, &template_info));
    require(template_info.mechanics.pellet_count == 3);
    require(template_info.mechanics.pellet_spread == 15.0f);
    GameServer_Destroy(bundle_game_server);
    Kernel_Destroy(bundle_kernel);
    Kernel_Destroy(kernel);

    KernelConfig dedicated_config = listen_server_config();
    dedicated_config.mode = KernelMode_DedicatedServer;
    KernelHandle* dedicated_kernel = Kernel_Create(&dedicated_config);
    require(dedicated_kernel != nullptr);
    load_result = KernelGameplayCatalogLoadResult{};
    const std::vector<std::uint8_t> unsupported_version_bundle = make_store_zip({
        {"gameplay_catalog.yaml", "catalog_version: 1\n"},
    });
    require(!Kernel_LoadGameplayCatalogFromMemory(
        dedicated_kernel,
        unsupported_version_bundle.data(),
        static_cast<std::uint32_t>(unsupported_version_bundle.size()),
        "gameplay_catalog.yaml",
        &load_result));
    require(load_result.status == KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_FAILED);
    require(
        load_result.error_code ==
        KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNSUPPORTED_CATALOG_VERSION);
    require(std::string(load_result.path) == "gameplay_catalog.yaml");
    require(std::string(load_result.field) == "catalog_version");
    require(load_result.diagnostic[0] != '\0');

    load_result = KernelGameplayCatalogLoadResult{};
    require(Kernel_LoadGameplayCatalogFromMemory(
        dedicated_kernel,
        gameplay_bundle.data(),
        static_cast<std::uint32_t>(gameplay_bundle.size()),
        "gameplay_catalog.yaml",
        &load_result));
    require(Kernel_StartDedicatedServer(dedicated_kernel, 7798));
    Kernel_Destroy(dedicated_kernel);
    return 0;
}
