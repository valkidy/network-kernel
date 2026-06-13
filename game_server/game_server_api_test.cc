#include "game_server/public/game_server_api.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

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

std::uint32_t query_enemy_count(KernelHandle* kernel) {
    std::array<KernelServerEntityState, 16> states{};
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    return Kernel_ServerQueryEntities(
        kernel,
        2,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
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
    files.push_back({
        "collider_templates/default.yaml",
        read_text_file(root / "game_server" / "collider_templates" / "default.yaml")});
    const std::vector<std::string> actor_files = {
        "enemy_grunt.yaml",
        "player.yaml",
    };
    for (const std::string& file : actor_files) {
        files.push_back({
            "actor_templates/" + file,
            read_text_file(root / "game_server" / "actor_templates" / file)});
    }
    const std::vector<std::string> weapon_files = {
        "beam_rifle.yaml",
        "fire_floor.yaml",
        "homing_missile.yaml",
        "rifle.yaml",
        "rocket.yaml",
        "shotgun.yaml",
        "spammer.yaml",
    };
    for (const std::string& file : weapon_files) {
        files.push_back({
            "weapon_templates/" + file,
            read_text_file(root / "game_server" / "weapon_templates" / file)});
    }
    const std::vector<std::string> projectile_files = {
        "homing_missile.yaml",
        "rocket.yaml",
        "rocket_explosion.yaml",
        "spammer.yaml",
    };
    for (const std::string& file : projectile_files) {
        files.push_back({
            "projectile_templates/" + file,
            read_text_file(root / "game_server" / "projectile_templates" / file)});
    }
    return make_store_zip(files);
}

}  // namespace

int main() {
    GameServerAbiInfo info{};
    assert(GameServer_GetAbiInfo(&info, sizeof(info)));
    assert(info.struct_size == sizeof(GameServerAbiInfo));
    assert(info.abi_version == GAME_SERVER_ABI_VERSION);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_ENEMY_MANAGER) != 0);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_EVENT_HANDLING) != 0);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_DESPAWN_ALL) != 0);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_DIRECTORY) != 0);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_QUERY) != 0);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_GAMEPLAY_CATALOG_BUNDLE) != 0);
    assert(info.weapon_template_info_size == sizeof(GameServerWeaponTemplateInfo));
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
    assert(Kernel_StartListenServer(kernel, 7777));

    const std::vector<std::uint8_t> gameplay_bundle = make_gameplay_bundle_zip();
    load_result = KernelGameplayCatalogLoadResult{};
    assert(Kernel_LoadGameplayCatalogFromMemory(
        kernel,
        gameplay_bundle.data(),
        static_cast<std::uint32_t>(gameplay_bundle.size()),
        "gameplay_catalog.yaml",
        &load_result));
    assert(load_result.success);
    assert(load_result.catalog_version == 1);
    assert(load_result.catalog_hash != 0);
    assert(load_result.projectile_template_count > 0);
    assert(load_result.collider_template_count == 8);
    assert(load_result.collider_binding_count == 4);

    GameServerHandle* game_server = GameServer_Create(kernel);
    assert(game_server != nullptr);
    assert(GameServer_QueryWeaponTemplate(game_server, 2, &template_info));
    assert(template_info.weapon_id == 2);
    assert(template_info.fire_mode == KernelWeaponFireMode_Projectile);
    assert(template_info.mechanics.damage == 1);
    assert(template_info.mechanics.magazine_size == 120);
    assert(template_info.mechanics.cooldown_ticks == 1);
    assert(template_info.name[0] == 'P');
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(game_server, 4, &template_info));
    assert(template_info.weapon_id == 4);
    assert(template_info.fire_mode == KernelWeaponFireMode_AreaEffect);
    assert(template_info.mechanics.area_effect.radius == 2.0f);
    assert(template_info.name[0] == 'F');
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(game_server, 5, &template_info));
    assert(template_info.weapon_id == 5);
    assert(template_info.fire_mode == KernelWeaponFireMode_Beam);
    assert(template_info.mechanics.beam.length == 8.0f);
    assert(template_info.mechanics.beam.damage_per_second == 30);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(game_server, 6, &template_info));
    assert(template_info.weapon_id == 6);
    assert(template_info.fire_mode == KernelWeaponFireMode_Projectile);
    assert(template_info.mechanics.projectile.motion_model ==
           KernelProjectileMotionModel_Homing);
    assert(template_info.mechanics.projectile.homing.lock_on_range == 25.0f);
    handle_pending_events(kernel, game_server);
    GameServer_Tick(game_server, 1.0f / 30.0f);
    assert(GameServer_GetEnemyCount(game_server) == 10);
    assert(query_enemy_count(kernel) == 10);

    GameServer_DespawnAll(game_server, KernelDespawnReason_Destroyed);
    GameServer_Tick(game_server, 1.0f / 30.0f);
    assert(GameServer_GetEnemyCount(game_server) == 0);
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
    assert(template_info.mechanics.cooldown_ticks == 1);
    assert(template_info.mechanics.projectile.motion_model ==
           KernelProjectileMotionModel_Linear);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(yaml_game_server, 4, &template_info));
    assert(template_info.mechanics.fire_mode == KernelWeaponFireMode_AreaEffect);
    assert(template_info.mechanics.area_effect.collision_mask == KERNEL_COLLISION_LAYER_ENEMY);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(yaml_game_server, 5, &template_info));
    assert(template_info.mechanics.fire_mode == KernelWeaponFireMode_Beam);
    assert(template_info.mechanics.beam.collision_mask == KERNEL_COLLISION_LAYER_ENEMY);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(yaml_game_server, 6, &template_info));
    assert(template_info.mechanics.fire_mode == KernelWeaponFireMode_Projectile);
    assert(template_info.mechanics.projectile.motion_model ==
           KernelProjectileMotionModel_Homing);
    assert(template_info.mechanics.projectile.homing.max_speed == 30.0f);
    GameServer_Destroy(yaml_game_server);

    load_result = KernelGameplayCatalogLoadResult{};
    GameServerHandle* bundle_game_server =
        GameServer_CreateWithGameplayCatalogFromMemory(
            kernel,
            gameplay_bundle.data(),
            static_cast<std::uint32_t>(gameplay_bundle.size()),
            "gameplay_catalog.yaml",
            &load_result);
    assert(bundle_game_server != nullptr);
    assert(load_result.success);
    assert(load_result.catalog_hash != 0);
    template_info = GameServerWeaponTemplateInfo{};
    template_info.struct_size = sizeof(template_info);
    assert(GameServer_QueryWeaponTemplate(bundle_game_server, 2, &template_info));
    assert(template_info.mechanics.pellet_count == 3);
    assert(template_info.mechanics.pellet_spread == 15.0f);
    GameServer_Destroy(bundle_game_server);
    Kernel_Destroy(kernel);

    KernelConfig dedicated_config = listen_server_config();
    dedicated_config.mode = KernelMode_DedicatedServer;
    KernelHandle* dedicated_kernel = Kernel_Create(&dedicated_config);
    assert(dedicated_kernel != nullptr);
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
