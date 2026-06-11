#include "game_server/game_server.h"

#include <cstring>
#include <utility>

namespace network_example::game_server {

GameServer::GameServer(KernelHandle* kernel, GameServerGameplayConfig config)
    : kernel_(kernel),
      config_(std::move(config)),
      enemy_manager_(kernel, config_) {
    load_kernel_gameplay_catalog(kernel_, config_);
}

void GameServer::handle_event(const KernelEvent& event) {
    if (event.type == KernelEventType_PlayerJoined && event.net_id != 0) {
        configure_player(event.net_id);
    }
    enemy_manager_.handle_event(event);
}

void GameServer::tick(float delta_seconds) {
    enemy_manager_.tick(delta_seconds);
}

EnemyManager& GameServer::enemy_manager() {
    return enemy_manager_;
}

const EnemyManager& GameServer::enemy_manager() const {
    return enemy_manager_;
}

bool GameServer::query_weapon_template(
    std::uint8_t weapon_id,
    GameServerWeaponTemplateInfo* out_info) const {
    if (out_info == nullptr ||
        out_info->struct_size < sizeof(GameServerWeaponTemplateInfo) ||
        weapon_id >= kWeaponCount) {
        return false;
    }
    const KernelWeaponMechanicsDefinition& mechanics =
        config_.weapons.definitions[weapon_id];
    if (mechanics.struct_size < sizeof(KernelWeaponMechanicsDefinition)) {
        return false;
    }
    std::memset(out_info, 0, sizeof(GameServerWeaponTemplateInfo));
    out_info->struct_size = sizeof(GameServerWeaponTemplateInfo);
    out_info->weapon_id = weapon_id;
    out_info->fire_mode = mechanics.fire_mode;
    const std::string& name = config_.weapons.names[weapon_id];
    std::strncpy(out_info->name, name.c_str(), sizeof(out_info->name) - 1);
    out_info->mechanics = mechanics;
    out_info->valid = true;
    return true;
}

void GameServer::configure_player(std::uint32_t net_id) const {
    if (kernel_ == nullptr) {
        return;
    }
    const ActorTemplateConfig* actor_template =
        find_actor_template(config_, config_.player.actor_template_id);
    if (actor_template == nullptr) {
        return;
    }
    KernelCombatStateDefinition combat_state = make_player_combat_state(config_);
    if (!Kernel_ServerSetEntityCombatState(kernel_, net_id, &combat_state)) {
        return;
    }
    for (std::uint8_t slot = 0; slot < actor_template->weapon_slot_count; ++slot) {
        const KernelWeaponMechanicsDefinition& weapon =
            config_.weapons.definitions[actor_template->weapon_slots[slot]];
        Kernel_ServerSetEntityWeaponMechanics(kernel_, net_id, &weapon);
    }
}

}  // namespace network_example::game_server
