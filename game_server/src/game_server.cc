#include "game_server/src/game_server.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <spdlog/spdlog.h>
#include <utility>
#include <vector>

#include "game_server/src/scene_placement.h"

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
    // The camp's level gate ends, for a config never laid out against its
    // ground (finish_scene_placement does both when it is).
    if (!config_.level_gate.resolved) {
        fill_level_gate_defaults(&config_);
    }
    // Hunters leave alone a player a tent hides (AI plan Q3).
    agent_runtime_manager_.set_shelter_director(&shelter_);
    load_kernel_gameplay_catalog(kernel_, config_);
    if (kernel_ != nullptr &&
        !Kernel_ServerSetFallDeathHeight(
            kernel_, config_.player.fall_death_height_meters)) {
        spdlog::warn(
            "fall death height not set height={}",
            config_.player.fall_death_height_meters);
    }
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
        // Everyone gets the scene table's state on arrival; a level already
        // under way is in it, to load and then walk into.
        peer_players_[event.peer_id] = event.net_id;
        if (scene_table_enabled_) {
            broadcast_scene_state();
        }
        configure_player(event.net_id);
        if (kernel_ != nullptr && config_.player.has_spawn_position) {
            const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
            Kernel_ServerSetEntityTransform(
                kernel_, event.net_id, &config_.player.spawn_position, &facing);
        }
        grant_late_joiner_revives();
        // Out of revives: arriving late is not a way back in. The kernel emits
        // no EntityDied for this, so it neither splats nor queues a revive.
        // With levels, newcomers arrive in the camp, which a level's lost
        // revives say nothing about.
        if (kernel_ != nullptr && !scene_table_enabled_ && respawn_.joins_dead()) {
            Kernel_ServerSetEntityHealth(kernel_, event.net_id, 0u);
        }
    } else if (event.type == KernelEventType_PlayerLeft) {
        players_.erase(event.net_id);
        peer_players_.erase(event.peer_id);
        level_ready_.erase(event.net_id);
        if (scene_table_enabled_) {
            broadcast_scene_state();  // the owner may have changed
        }
        respawn_.on_player_left(event.net_id);
    } else if (event.type == KernelEventType_EntitySpawned) {
        stock_camp(event.net_id);
    } else if (event.type == KernelEventType_EntityDied &&
               players_.find(event.net_id) != players_.end()) {
        drop_tagged_items(event.net_id);
        respawn_.on_player_died(event.net_id);
    }
    if (event.type == KernelEventType_EntityDied) {
        note_level_enemy_death(event.net_id);
    }
    if (event.type == KernelEventType_UiOpened && event.code == GAME_SERVER_UI_LEVEL_SELECT) {
        // Not a building to go inside: the level list is the player's to see.
        answer_level_select(event);
    } else {
        shelter_.handle_event(event);
    }
    loadout_.handle_event(event);
    agent_runtime_manager_.handle_event(event);
    for (SceneSlot& slot : scene_slots_) {
        if (slot.manager != nullptr) {
            slot.manager->handle_event(event);
        }
    }
}

void GameServer::tick(float delta_seconds) {
    place_scene_props();
    if (kernel_ != nullptr) {
        KernelGameMessage messages[8]{};
        std::uint32_t count = 0;
        while ((count = Kernel_ServerPollGameMessages(kernel_, messages, 8u)) != 0u) {
            for (std::uint32_t index = 0; index < count; ++index) {
                loadout_.handle_message(messages[index]);
                handle_scene_message(messages[index]);
            }
        }
        // Players' orders to their side's agents (docs/AI_BEHAVIOR_PLAN.md
        // §8), each answered whether or not it was obeyed.
        KernelAgentCommand commands[8]{};
        while ((count = Kernel_ServerPollAgentCommands(kernel_, commands, 8u)) != 0u) {
            for (std::uint32_t index = 0; index < count; ++index) {
                // The agent's own slot answers: a level's AI, or the camp's.
                const KernelAgentCommandOutcome outcome =
                    manager_for_agent(commands[index].agent_net_id)
                        .handle_agent_command(commands[index]);
                Kernel_ServerSendAgentCommandOutcome(kernel_, &outcome);
            }
        }
    }
    for (const std::uint32_t net_id : respawn_.advance(delta_seconds)) {
        revive_player(net_id, delta_seconds);
    }
    agent_runtime_manager_.tick(delta_seconds);
    tick_scene_slots(delta_seconds);
    tick_scene_flow(delta_seconds);
}

void GameServer::place_scene_props() {
    if (scene_props_placed_ || kernel_ == nullptr) {
        return;
    }
    scene_props_placed_ = place_props_of(config_);
}

bool GameServer::place_props_of(const GameServerGameplayConfig& config) const {
    // All or nothing per attempt: a kernel not yet serving refuses the first,
    // and nothing has been placed to place twice.
    for (const ScenePropConfig& scene_prop : config.scene_props) {
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_type = KernelEntityType_Prop;
        create.entity_template_id = scene_prop.entity_template_id;
        create.position = scene_prop.position;
        create.rotation = scene_prop.rotation;
        std::uint32_t net_id = 0;
        if (!Kernel_ServerCreateEntity(kernel_, &create, &net_id)) {
            if (&scene_prop == &config.scene_props.front()) {
                return false;
            }
            spdlog::warn(
                "scene prop not placed template={}", scene_prop.entity_template_id);
        }
    }
    for (const SceneItemConfig& scene_item : config.scene_items) {
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
        for (const ItemTemplateConfig& candidate : config.item_templates) {
            if (candidate.definition.item_template_id == scene_item.item_template_id &&
                candidate.definition.is_weapon != 0u) {
                Kernel_ServerSetItemDropTag(kernel_, item, KERNEL_DROP_TAG_MAP_WEAPON);
            }
        }
    }
    return true;
}

void GameServer::revive_player(std::uint32_t net_id, float delta_seconds) {
    if (kernel_ == nullptr) {
        return;
    }
    const PlayerRespawnConfig& respawn = config_.player.respawn;
    // A revive lifts the player off where it died. One that died off the map
    // has nowhere under it to stand, and would only fall to its death again
    // -- once per revive, until the team's pool is gone -- so it comes back at
    // the player spawn instead (the world origin on a map without one).
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    if (Kernel_ServerGetEntityState(kernel_, net_id, &state) &&
        !(state.position.y >= config_.player.fall_death_height_meters)) {
        // The spawn of the slot it fell from: a level's own, offset to its
        // slot, or the camp's.
        const std::uint32_t slot = scene_slot_of(state.position.x);
        const GameServerGameplayConfig* where =
            slot != 0u ? scene_config(slot) : nullptr;
        const GameServerGameplayConfig& map = where != nullptr ? *where : config_;
        const KernelVec3 spawn = map.player.has_spawn_position
            ? map.player.spawn_position
            : map.scene_offset;
        const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
        Kernel_ServerSetEntityTransform(kernel_, net_id, &spawn, &facing);
    }
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
    // Round where the player fell; the kernel spreads and grounds them.
    std::uint32_t dropped = 0;
    if (!Kernel_ServerDropTaggedItems(kernel_, net_id, nullptr, &dropped)) {
        spdlog::warn("death drop failed player={}", net_id);
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
            // A pick is made from inside the camp, where the player stands at
            // the camp's centre: its feet are where it went in.
            KernelVec3 feet{};
            bool have_feet = shelter_.entry_position_of(net_id, &feet);
            if (!have_feet) {
                KernelServerEntityState me{};
                me.struct_size = sizeof(me);
                have_feet = Kernel_ServerGetEntityState(kernel_, net_id, &me);
                feet = me.position;
            }
            std::uint32_t prop = 0;
            if (have_feet &&
                Kernel_ServerDropInventoryItem(kernel_, kept[category], &feet, &prop)) {
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

void GameServer::enable_scene_table(SceneCatalogSource source) {
    scene_source_ = std::move(source);
    scene_table_enabled_ = true;
    agent_runtime_manager_.set_scene_bounds(scene_slot_bounds(0u));
}

bool GameServer::load_scene(std::uint32_t slot, const std::string& scene_template) {
    if (!scene_table_enabled_ || slot == 0u || slot >= kSceneSlotCount ||
        scene_slots_[slot].state != SceneSlotState::kEmpty) {
        return false;
    }
    SceneSlot& target = scene_slots_[slot];
    target.state = SceneSlotState::kLoading;
    target.recipe.clear();
    target.recipe_hash = 0u;
    target.scene_template = scene_template;
    target.error.clear();
    // Only immutable inputs cross: the shared bundle, names and the slot.
    target.pending = std::async(
        std::launch::async,
        [source = scene_source_, scene_template, slot]() {
            return prepare_scene(source, scene_template, slot);
        });
    return true;
}

void GameServer::tick_scene_slots(float delta_seconds) {
    for (std::uint32_t index = 1; index < kSceneSlotCount; ++index) {
        SceneSlot& slot = scene_slots_[index];
        if (slot.state == SceneSlotState::kLoading && slot.pending.valid() &&
            slot.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            PreparedScene prepared = slot.pending.get();
            if (!prepared.ok || !commit_scene(index, std::move(prepared))) {
                if (slot.error.empty()) {
                    slot.error = prepared.error;
                }
                spdlog::error("scene slot {} load failed: {}", index, slot.error);
                slot.config.reset();
                slot.manager.reset();
                slot.state = SceneSlotState::kEmpty;
                ++slot.generation;
            }
        }
        if (slot.state == SceneSlotState::kReady && slot.manager != nullptr) {
            slot.manager->tick(delta_seconds);
        }
    }
}

bool GameServer::commit_scene(std::uint32_t index, PreparedScene prepared) {
    SceneSlot& slot = scene_slots_[index];
    if (kernel_ == nullptr) {
        slot.error = "no kernel";
        return false;
    }
    slot.recipe = std::move(prepared.recipe_bytes);
    slot.recipe_hash = slot.recipe.empty() ? 0u : prepared.terrain_recipe.heights_hash;
    if (!slot.recipe.empty()) {
        spdlog::info("scene slot {} generated seed={} heights_hash={:x} recipe_bytes={}",
            index, prepared.terrain_recipe.recipe.seed, slot.recipe_hash, slot.recipe.size());
    }
    slot.config = std::make_unique<GameServerGameplayConfig>(std::move(prepared.config));
    const GameServerGameplayConfig& config = *slot.config;
    KernelStaticSceneSlotConfig scene{};
    scene.struct_size = sizeof(scene);
    scene.slot = index;
    scene.artifact_bytes = prepared.collision_artifact.data();
    scene.artifact_size = static_cast<std::uint32_t>(prepared.collision_artifact.size());
    scene.scene_id = config.static_collision_scene.scene_id;
    // The camp's collider id plus the slot: unique per slot, and clear of
    // the camp's own.
    scene.collider_id = config.static_collision_scene.collider_id + index;
    scene.collision_layer = config.static_collision_scene.collision_layer;
    scene.offset = config.scene_offset;
    if (!Kernel_AddStaticSceneSlot(kernel_, &scene)) {
        slot.error = "kernel refused the slot's static scene";
        return false;
    }
    if (!register_player_barriers(kernel_, index, config)) {
        slot.error = "kernel refused the slot's player barriers";
        Kernel_RemoveStaticSceneSlot(kernel_, index);
        return false;
    }
    slot.manager = std::make_unique<AgentRuntimeManager>(kernel_, config);
    slot.manager->set_scene_bounds(scene_slot_bounds(index));
    slot.manager->set_shelter_director(&shelter_);
    if (!slot.manager->preload_directors()) {
        slot.error = "scene template's directors did not preload";
        Kernel_RemoveStaticSceneSlot(kernel_, index);
        return false;
    }
    if (!place_props_of(config)) {
        spdlog::warn("scene slot {} props not placed", index);
    }
    slot.state = SceneSlotState::kReady;
    spdlog::info(
        "scene slot {} ready template={} generation={} offset_x={}",
        index, slot.scene_template, slot.generation, config.scene_offset.x);
    return true;
}

std::uint32_t GameServer::clear_scene_bounds(const SceneBounds& bounds) const {
    std::uint32_t removed = 0;
    const KernelVec3 camp_spawn = config_.player.has_spawn_position
        ? config_.player.spawn_position
        : KernelVec3{0.0f, 0.0f, 0.0f};
    const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
    for (const std::uint16_t type : {static_cast<std::uint16_t>(KernelEntityType_Actor),
             static_cast<std::uint16_t>(KernelEntityType_Prop),
             static_cast<std::uint16_t>(KernelEntityType_Projectile)}) {
        std::vector<KernelServerEntityState> states(256);
        std::uint32_t count = 0;
        while (true) {
            for (KernelServerEntityState& state : states) state.struct_size = sizeof(state);
            count = Kernel_ServerQueryEntities(
                kernel_, type, states.data(), static_cast<std::uint32_t>(states.size()));
            if (count < states.size() || states.size() >= 65536u) break;
            states.resize(states.size() * 2);
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            const KernelServerEntityState& state = states[index];
            if (!bounds.contains(state.position.x)) continue;
            if (type == KernelEntityType_Actor && state.actor_type == KernelActorType_Player) {
                Kernel_ServerSetEntityTransform(kernel_, state.net_id, &camp_spawn, &facing);
                continue;
            }
            if (Kernel_ServerDestroyEntity(kernel_, state.net_id, KernelDespawnReason_Retired)) {
                ++removed;
            }
        }
    }
    return removed;
}

bool GameServer::unload_scene(std::uint32_t index) {
    if (index == 0u || index >= kSceneSlotCount ||
        scene_slots_[index].state != SceneSlotState::kReady || kernel_ == nullptr) {
        return false;
    }
    SceneSlot& slot = scene_slots_[index];
    const std::uint32_t removed = clear_scene_bounds(scene_slot_bounds(index));
    slot.manager.reset();
    slot.config.reset();
    Kernel_RemoveStaticSceneSlot(kernel_, index);
    ++slot.generation;
    slot.state = SceneSlotState::kEmpty;
    spdlog::info(
        "scene slot {} unloaded template={} removed={} generation={}",
        index, slot.scene_template, removed, slot.generation);
    return true;
}

SceneSlotState GameServer::scene_state(std::uint32_t slot) const {
    if (slot == 0u) return SceneSlotState::kReady;
    return slot < kSceneSlotCount ? scene_slots_[slot].state : SceneSlotState::kEmpty;
}

std::uint32_t GameServer::scene_generation(std::uint32_t slot) const {
    return slot < kSceneSlotCount ? scene_slots_[slot].generation : 0u;
}

const std::string& GameServer::scene_error(std::uint32_t slot) const {
    static const std::string kNone;
    return slot < kSceneSlotCount ? scene_slots_[slot].error : kNone;
}

const AgentRuntimeManager* GameServer::scene_manager(std::uint32_t slot) const {
    if (slot == 0u) return &agent_runtime_manager_;
    if (slot >= kSceneSlotCount || scene_slots_[slot].state != SceneSlotState::kReady) return nullptr;
    return scene_slots_[slot].manager.get();
}

const GameServerGameplayConfig* GameServer::scene_config(std::uint32_t slot) const {
    if (slot == 0u || slot >= kSceneSlotCount ||
        scene_slots_[slot].state != SceneSlotState::kReady) {
        return nullptr;
    }
    return scene_slots_[slot].config.get();
}

}  // namespace network_example::game_server
