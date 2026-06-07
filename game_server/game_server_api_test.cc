#include "game_server/public/game_server_api.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>

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
    std::array<KernelServerEntityState, 8> states{};
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
    assert(info.weapon_template_info_size == sizeof(GameServerWeaponTemplateInfo));
    assert(!GameServer_GetAbiInfo(nullptr, sizeof(info)));
    assert(!GameServer_GetAbiInfo(&info, sizeof(info) - 1));

    assert(GameServer_Create(nullptr) == nullptr);
    assert(GameServer_CreateWithWeaponTemplateDirectory(nullptr, "x") == nullptr);
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
    assert(GameServer_GetEnemyCount(game_server) == 1);
    assert(query_enemy_count(kernel) == 1);

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
    Kernel_Destroy(kernel);
    return 0;
}
