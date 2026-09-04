#include "game_server/src/game_server.h"

#include <cstring>
#include <utility>

namespace network_example::game_server {

GameServer::GameServer(KernelHandle* kernel, GameServerGameplayConfig config)
    : kernel_(kernel),
      config_(std::move(config)),
      agent_runtime_manager_(kernel, config_) {
    load_kernel_gameplay_catalog(kernel_, config_);
}

void GameServer::handle_event(const KernelEvent& event) {
    if (event.type == KernelEventType_PlayerJoined && event.net_id != 0) {
        configure_player(event.net_id);
    }
    agent_runtime_manager_.handle_event(event);
}

void GameServer::tick(float delta_seconds) {
    agent_runtime_manager_.tick(delta_seconds);
}

bool GameServer::preload_directors() {
    return agent_runtime_manager_.preload_directors();
}

AgentRuntimeManager& GameServer::agent_runtime_manager() {
    return agent_runtime_manager_;
}

const AgentRuntimeManager& GameServer::agent_runtime_manager() const {
    return agent_runtime_manager_;
}

bool GameServer::query_weapon_template(
    std::uint8_t weapon_id,
    GameServerWeaponTemplateInfo* out_info) const {
    if (out_info == nullptr ||
        out_info->struct_size < sizeof(GameServerWeaponTemplateInfo) ||
        !config_.weapons.configured[weapon_id]) {
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
    out_info->valid = 1u;
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
    if (!Kernel_ServerSetEntityActorTemplate(
            kernel_,
            net_id,
            actor_template->actor_template_id)) {
        return;
    }
    Kernel_ServerSetEntityVisionConfig(kernel_, net_id, &actor_template->vision);
    for (std::uint8_t slot = 0; slot < actor_template->weapon_slot_count; ++slot) {
        const KernelWeaponMechanicsDefinition& weapon =
            config_.weapons.definitions[actor_template->weapon_ids[slot]];
        Kernel_ServerSetEntityWeaponMechanics(kernel_, net_id, &weapon);
    }
    configure_player_inventory(net_id, *actor_template);
}

bool GameServer::configure_player_inventory(
    std::uint32_t net_id,
    const ActorTemplateConfig& actor_template) const {
    if (actor_template.inventory_slot_capacity == 0) {
        return true;
    }
    if (Kernel_CopyOwnedInventoryContainers(kernel_, net_id, nullptr, 0) != 0) {
        return true;
    }
    KernelInventoryContainerId container_id = 0;
    if (!Kernel_ServerCreateInventoryContainer(
            kernel_,
            net_id,
            actor_template.inventory_slot_capacity,
            &container_id)) {
        return false;
    }
    for (const InventorySlotConfig& slot : actor_template.inventory_slots) {
        KernelItemInstanceId item_instance_id = 0;
        if (!Kernel_ServerCreateInventoryItem(
                kernel_,
                slot.item_template_id,
                slot.quantity,
                container_id,
                &item_instance_id)) {
            return false;
        }
    }
    return true;
}

}  // namespace network_example::game_server
