#include "game_server/src/game_server.h"

#include <array>
#include <cmath>
#include <cstring>
#include <spdlog/spdlog.h>
#include <utility>
#include <vector>

namespace network_example::game_server {

GameServer::GameServer(KernelHandle* kernel, GameServerGameplayConfig config)
    : kernel_(kernel),
      config_(std::move(config)),
      agent_runtime_manager_(kernel, config_),
      respawn_(config_.player.respawn),
      shelter_(kernel),
      loadout_(kernel, config_, [this](std::uint32_t player) {
          const ActorTemplateConfig* actor_template =
              find_actor_template(config_, config_.player.actor_template_id);
          return actor_template != nullptr &&
              configure_player_inventory(player, *actor_template, true) &&
              configure_player_weapons(player, true);
      }) {
    load_kernel_gameplay_catalog(kernel_, config_);
    // The default loadout as weapon items: every template weapon needs one.
    if (const ActorTemplateConfig* player_template =
            find_actor_template(config_, config_.player.actor_template_id)) {
        weapons_are_items_ = true;
        for (std::uint8_t slot = 0; slot < player_template->weapon_slot_count; ++slot) {
            const std::uint32_t weapon_id = player_template->weapon_ids[slot];
            std::uint32_t item_template_id = 0;
            for (const ItemTemplateConfig& item : config_.item_templates) {
                if (item.definition.is_weapon != 0u &&
                    item.definition.weapon_id == weapon_id) {
                    item_template_id = item.definition.item_template_id;
                }
            }
            if (item_template_id == 0u) {
                weapons_are_items_ = false;
                default_weapon_items_.clear();
                break;
            }
            default_weapon_items_.push_back(item_template_id);
        }
    }
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
    } else if (event.type == KernelEventType_EntitySpawned) {
        stock_camp(event.net_id);
    } else if (event.type == KernelEventType_EntityDied &&
               players_.find(event.net_id) != players_.end()) {
        drop_tagged_items(event.net_id);
        respawn_.on_player_died(event.net_id);
    }
    shelter_.handle_event(event);
    loadout_.handle_event(event);
    agent_runtime_manager_.handle_event(event);
}

void GameServer::tick(float delta_seconds) {
    place_scene_props();
    if (kernel_ != nullptr) {
        KernelGameMessage messages[8]{};
        std::uint32_t count = 0;
        while ((count = Kernel_ServerPollGameMessages(kernel_, messages, 8u)) != 0u) {
            for (std::uint32_t index = 0; index < count; ++index) {
                loadout_.handle_message(messages[index]);
            }
        }
    }
    for (const std::uint32_t net_id : respawn_.advance(delta_seconds)) {
        revive_player(net_id, delta_seconds);
    }
    agent_runtime_manager_.tick(delta_seconds);
}

void GameServer::place_scene_props() {
    if (scene_props_placed_ || kernel_ == nullptr) {
        return;
    }
    // All or nothing per attempt: a kernel not yet serving refuses the first,
    // and nothing has been placed to place twice.
    for (const ScenePropConfig& scene_prop : config_.scene_props) {
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_type = KernelEntityType_Prop;
        create.entity_template_id = scene_prop.entity_template_id;
        create.position = scene_prop.position;
        create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t net_id = 0;
        if (!Kernel_ServerCreateEntity(kernel_, &create, &net_id)) {
            if (&scene_prop == &config_.scene_props.front()) {
                return;
            }
            spdlog::warn(
                "scene prop not placed template={}", scene_prop.entity_template_id);
        }
    }
    for (const SceneItemConfig& scene_item : config_.scene_items) {
        KernelItemInstanceId item = 0;
        std::uint32_t prop = 0;
        if (!Kernel_ServerCreateWorldItem(
                kernel_, scene_item.item_template_id, scene_item.quantity,
                &scene_item.position, &item, &prop)) {
            spdlog::warn("scene item not placed template={}", scene_item.item_template_id);
            continue;
        }
        // A weapon lying on the map is a map weapon: it outlives a loadout
        // being reapplied (D23) and, later, drops on death (D19).
        for (const ItemTemplateConfig& candidate : config_.item_templates) {
            if (candidate.definition.item_template_id == scene_item.item_template_id &&
                candidate.definition.is_weapon != 0u) {
                Kernel_ServerSetItemDropTag(kernel_, item, KERNEL_DROP_TAG_MAP_WEAPON);
            }
        }
    }
    scene_props_placed_ = true;
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

void GameServer::drop_tagged_items(std::uint32_t net_id) const {
    if (kernel_ == nullptr) {
        return;
    }
    KernelServerEntityState me{};
    me.struct_size = sizeof(me);
    if (!Kernel_ServerGetEntityState(kernel_, net_id, &me)) {
        return;
    }
    std::vector<KernelItemInstanceId> tagged;
    KernelInventoryContainerView owned[4]{};
    for (KernelInventoryContainerView& view : owned) view.struct_size = sizeof(view);
    const std::uint32_t count =
        Kernel_CopyOwnedInventoryContainers(kernel_, net_id, owned, 4u);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::vector<KernelItemInstanceView> held(owned[index].slot_capacity);
        for (KernelItemInstanceView& view : held) view.struct_size = sizeof(view);
        held.resize(Kernel_CopyInventorySlots(
            kernel_, owned[index].inventory_container_id, held.data(),
            static_cast<std::uint32_t>(held.size())));
        for (const KernelItemInstanceView& item : held) {
            if (item.drop_tag != KERNEL_DROP_TAG_NONE) {
                tagged.push_back(item.item_instance_id);
            }
        }
    }
    // Spread on a ring round where they fell, so no two land in one spot; the
    // kernel puts each on the ground beneath its point.
    constexpr float kRadius = 1.0f;
    constexpr float kTwoPi = 6.28318530718f;
    for (std::size_t index = 0; index < tagged.size(); ++index) {
        const float angle = kTwoPi * static_cast<float>(index) /
            static_cast<float>(tagged.size());
        const KernelVec3 at{
            me.position.x + kRadius * std::cos(angle),
            me.position.y,
            me.position.z + kRadius * std::sin(angle)};
        std::uint32_t prop = 0;
        if (!Kernel_ServerDropInventoryItem(kernel_, tagged[index], &at, &prop)) {
            spdlog::warn("death drop failed player={} item={}", net_id, tagged[index]);
        }
    }
}

void GameServer::stock_camp(std::uint32_t net_id) const {
    if (kernel_ == nullptr || net_id == 0u) {
        return;
    }
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    if (!Kernel_ServerGetEntityState(kernel_, net_id, &state) ||
        state.entity_type != KernelEntityType_Prop) {
        return;
    }
    const EntityTemplateConfig* camp = nullptr;
    for (const EntityTemplateConfig& candidate : config_.entity_templates) {
        if (candidate.actor_template_id == state.entity_template_id &&
            !candidate.camp_stock.empty()) {
            camp = &candidate;
        }
    }
    if (camp == nullptr) {
        return;
    }
    // A listen host hears of a spawn twice (the server's and its own client's
    // copy); one stock.
    if (Kernel_CopyOwnedInventoryContainers(kernel_, net_id, nullptr, 0u) != 0u) {
        return;
    }
    KernelInventoryContainerId stock = 0;
    if (!Kernel_ServerCreateStockContainer(
            kernel_, net_id, static_cast<std::uint32_t>(camp->camp_stock.size()),
            &stock)) {
        spdlog::warn("camp stock container not made camp={}", net_id);
        return;
    }
    for (const InventorySlotConfig& entry : camp->camp_stock) {
        KernelItemInstanceId item = 0;
        if (!Kernel_ServerCreateInventoryItem(
                kernel_, entry.item_template_id, entry.quantity, stock, &item)) {
            spdlog::warn(
                "camp stock item not made camp={} item_template={}",
                net_id, entry.item_template_id);
        }
    }
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
    // Any weapon item may end up in hand, so every weapon with a category is
    // configured up front. Mechanics alone fire nothing: only the loadout --
    // the weapon container -- decides what can fire.
    for (const ItemTemplateConfig& item : config_.item_templates) {
        if (item.definition.is_weapon == 0u) continue;
        const KernelWeaponMechanicsDefinition& weapon =
            config_.weapons.definitions[item.definition.weapon_id];
        Kernel_ServerSetEntityWeaponMechanics(kernel_, net_id, &weapon);
    }
    configure_player_inventory(net_id, *actor_template, reset_inventory);
    configure_player_weapons(net_id, reset_inventory);
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
        // Only what the loadout gave goes; a quest item stays (D23).
        if (!Kernel_ServerClearUntaggedItems(kernel_, container_id)) {
            return false;
        }
    } else if (!Kernel_ServerCreateInventoryContainer(
                   kernel_,
                   net_id,
                   actor_template.inventory_slot_capacity,
                   &container_id)) {
        return false;
    }
    // The loadout picked at a camp, or the template's default.
    const PlayerLoadout* picked = loadout_.loadout_of(net_id);
    const std::vector<InventorySlotConfig>& slots =
        picked != nullptr ? picked->items : actor_template.inventory_slots;
    for (const InventorySlotConfig& slot : slots) {
        KernelItemInstanceId item_instance_id = 0;
        // A slot a kept quest item holds is not freed for the loadout: what
        // no longer fits is not given.
        if (!Kernel_ServerCreateInventoryItem(
                kernel_,
                slot.item_template_id,
                slot.quantity,
                container_id,
                &item_instance_id)) {
            spdlog::info(
                "loadout item did not fit player={} template={}",
                net_id,
                slot.item_template_id);
        }
    }
    return true;
}

bool GameServer::configure_player_weapons(std::uint32_t net_id, bool reset) const {
    if (kernel_ == nullptr || !weapons_are_items_) {
        return true;
    }
    KernelInventoryContainerId weapons = 0;
    KernelInventoryContainerView owned[4]{};
    for (KernelInventoryContainerView& view : owned) view.struct_size = sizeof(view);
    const std::uint32_t count =
        Kernel_CopyOwnedInventoryContainers(kernel_, net_id, owned, 4u);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (owned[index].container_kind == KernelInventoryContainerKind_Weapons) {
            weapons = owned[index].inventory_container_id;
        }
    }
    if (weapons != 0u) {
        // A join keeps what it has; a respawn or a new pick starts fresh --
        // except for map weapons, which stay (D23).
        if (!reset) {
            return true;
        }
        if (!Kernel_ServerClearUntaggedItems(kernel_, weapons)) {
            return false;
        }
    } else if (!Kernel_ServerCreateWeaponContainer(kernel_, net_id, &weapons)) {
        return false;
    }
    const PlayerLoadout* picked = loadout_.loadout_of(net_id);
    const std::vector<std::uint32_t>& weapon_items =
        picked != nullptr ? picked->weapon_items : default_weapon_items_;
    // What is still in the container is a kept map weapon, by category.
    std::array<KernelItemInstanceId, KERNEL_WEAPON_CATEGORY_COUNT> kept{};
    {
        KernelItemInstanceView held[KERNEL_WEAPON_CATEGORY_COUNT]{};
        for (KernelItemInstanceView& view : held) view.struct_size = sizeof(view);
        const std::uint32_t held_count = Kernel_CopyInventorySlots(
            kernel_, weapons, held, KERNEL_WEAPON_CATEGORY_COUNT);
        for (std::uint32_t index = 0; index < held_count; ++index) {
            if (held[index].slot < KERNEL_WEAPON_CATEGORY_COUNT) {
                kept[held[index].slot] = held[index].item_instance_id;
            }
        }
    }
    bool all_placed = true;
    for (const std::uint32_t item_template_id : weapon_items) {
        // The picked weapon takes its slot; a map weapon kept there goes to the
        // player's feet, as a same-category pickup would put it (D23, D11).
        std::uint8_t category = KERNEL_WEAPON_CATEGORY_COUNT;
        for (const ItemTemplateConfig& candidate : config_.item_templates) {
            if (candidate.definition.item_template_id == item_template_id) {
                category = candidate.definition.weapon_category;
            }
        }
        if (category < KERNEL_WEAPON_CATEGORY_COUNT && kept[category] != 0u) {
            KernelServerEntityState me{};
            me.struct_size = sizeof(me);
            std::uint32_t prop = 0;
            if (Kernel_ServerGetEntityState(kernel_, net_id, &me) &&
                Kernel_ServerDropInventoryItem(kernel_, kept[category], &me.position, &prop)) {
                kept[category] = 0u;
            }
        }
        KernelItemInstanceId item = 0;
        // Two default weapons of one category: the second has no slot.
        all_placed &= Kernel_ServerCreateInventoryItem(
            kernel_, item_template_id, 1u, weapons, &item);
    }
    return all_placed;
}

}  // namespace network_example::game_server
