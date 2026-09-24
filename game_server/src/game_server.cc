#include "game_server/src/game_server.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace network_example::game_server {

GameServer::GameServer(KernelHandle* kernel, GameServerGameplayConfig config)
    : kernel_(kernel),
      config_(std::move(config)),
      agent_runtime_manager_(kernel, config_),
      respawn_(config_.player.respawn) {
    load_kernel_gameplay_catalog(kernel_, config_);
}

void GameServer::handle_event(const KernelEvent& event) {
    if (event.type == KernelEventType_PlayerJoined && event.net_id != 0) {
        players_.insert(event.net_id);
        configure_player(event.net_id);
        // Out of revives: arriving late is not a way back in. The kernel emits
        // no EntityDied for this, so it neither splats nor queues a revive.
        if (kernel_ != nullptr && respawn_.joins_dead()) {
            Kernel_ServerSetEntityHealth(kernel_, event.net_id, 0u);
        }
    } else if (event.type == KernelEventType_PlayerLeft) {
        players_.erase(event.net_id);
        respawn_.on_player_left(event.net_id);
    } else if (event.type == KernelEventType_EntityDied &&
               players_.find(event.net_id) != players_.end()) {
        respawn_.on_player_died(event.net_id);
    }
    agent_runtime_manager_.handle_event(event);
}

void GameServer::tick(float delta_seconds) {
    for (const std::uint32_t net_id : respawn_.advance(delta_seconds)) {
        revive_player(net_id, delta_seconds);
    }
    agent_runtime_manager_.tick(delta_seconds);
}

void GameServer::revive_player(std::uint32_t net_id, float delta_seconds) {
    if (kernel_ == nullptr) {
        return;
    }
    const PlayerRespawnConfig& respawn = config_.player.respawn;
    KernelServerReviveInfo info{};
    info.struct_size = sizeof(info);
    info.net_id = net_id;
    info.lift_meters = respawn.height_offset_meters;
    // game_server ticks once per kernel tick, so its delta is the tick length.
    info.invulnerable_ticks = delta_seconds > 0.0f
        ? static_cast<std::uint32_t>(
              std::ceil(respawn.invulnerable_seconds / delta_seconds))
        : 0u;
    if (!Kernel_ServerReviveEntity(kernel_, &info)) {
        return;
    }
    configure_player(net_id, true);
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

void GameServer::configure_player(std::uint32_t net_id, bool reset_inventory) const {
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
    configure_player_inventory(net_id, *actor_template, reset_inventory);
}

bool GameServer::configure_player_inventory(
    std::uint32_t net_id,
    const ActorTemplateConfig& actor_template,
    bool reset_inventory) const {
    if (actor_template.inventory_slot_capacity == 0) {
        return true;
    }
    KernelInventoryContainerId container_id = 0;
    KernelInventoryContainerView existing{};
    existing.struct_size = sizeof(existing);
    if (Kernel_CopyOwnedInventoryContainers(kernel_, net_id, &existing, 1) != 0) {
        if (!reset_inventory) {
            return true;
        }
        container_id = existing.inventory_container_id;
        if (!Kernel_ServerClearInventoryContainer(kernel_, container_id)) {
            return false;
        }
    } else if (!Kernel_ServerCreateInventoryContainer(
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
