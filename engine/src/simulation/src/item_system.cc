#include "simulation/public/item_system.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace network_example {
namespace {

bool action_allowed_in_inventory(std::uint8_t action) {
    return action == KernelDomainAction_None ||
        action == KernelDomainAction_Consume ||
        action == KernelDomainAction_Place ||
        action == KernelDomainAction_Throw;
}

bool action_allowed_in_world(std::uint8_t action) {
    return action == KernelDomainAction_None ||
        action == KernelDomainAction_Pickup ||
        action == KernelDomainAction_Carry ||
        action == KernelDomainAction_Activate;
}

std::uint32_t required_capability(std::uint8_t action) {
    switch (action) {
        case KernelDomainAction_Consume:
            return KernelItemCapability_Consumable;
        case KernelDomainAction_Pickup:
            return KernelItemCapability_Pickupable;
        case KernelDomainAction_Throw:
            return KernelItemCapability_Throwable;
        case KernelDomainAction_Place:
            return KernelItemCapability_Deployable;
        case KernelDomainAction_Carry:
            return KernelItemCapability_Carryable;
        case KernelDomainAction_Activate:
            return KernelItemCapability_Interactable;
        default:
            return 0;
    }
}

bool set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool compatible_runtime_state(
    const ItemInstanceRecord& lhs,
    const ItemInstanceRecord& rhs) {
    if (lhs.next_use_tick != rhs.next_use_tick ||
        lhs.portable_state.size() != rhs.portable_state.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.portable_state.size(); ++index) {
        const KernelPortableStateFieldDefinition& a = lhs.portable_state[index];
        const KernelPortableStateFieldDefinition& b = rhs.portable_state[index];
        if (a.field_id != b.field_id || a.type != b.type ||
            a.world_projection != b.world_projection ||
            a.uint32_default != b.uint32_default ||
            a.float_default != b.float_default ||
            a.bool_default != b.bool_default) {
            return false;
        }
    }
    return true;
}

bool valid_item_graph_action(const KernelActionDefinition& action) {
    if (action.action_type == KernelEntityTriggerActionType_ApplyDamage) {
        return action.damage_amount != 0u &&
            action.target_source <= KernelEntityRefSource_EventInstigator;
    }
    if (action.action_type == KernelEntityTriggerActionType_SpawnEntity) {
        return action.spawn_entity_template_id != 0u &&
            action.position_source == KernelEventVec3Source_Position &&
            action.owner_source <= KernelEntityRefSource_EventInstigator;
    }
    if (action.action_type == KernelEntityTriggerActionType_SpawnProjectile) {
        return action.spawn_projectile_template_id != 0u &&
            action.position_source <= KernelEventVec3Source_Direction &&
            action.direction_source <= KernelEventVec3Source_Direction;
    }
    return false;
}

bool valid_item_used_graph(
    const KernelActionTriggerDefinition& trigger,
    bool require_spawn) {
    if (trigger.struct_size < sizeof(KernelActionTriggerDefinition) ||
        trigger.action_count > KERNEL_MAX_ACTION_GRAPH_ACTIONS) {
        return false;
    }
    const std::uint32_t count = trigger.action_count == 0u
        ? (trigger.action_type == KernelEntityTriggerActionType_None ? 0u : 1u)
        : trigger.action_count;
    bool has_spawn = false;
    for (std::uint32_t index = 0; index < count; ++index) {
        KernelActionDefinition action{};
        if (trigger.action_count == 0u) {
            action.action_type = trigger.action_type;
            action.target_source = trigger.target_source;
            action.damage_amount = trigger.damage_amount;
            action.spawn_entity_template_id = trigger.spawn_entity_template_id;
            action.spawn_projectile_template_id =
                trigger.spawn_projectile_template_id;
            action.position_source = trigger.position_source;
            action.direction_source = trigger.direction_source;
            action.owner_source = trigger.owner_source;
        } else {
            action = trigger.actions[index];
        }
        if (!valid_item_graph_action(action)) {
            return false;
        }
        has_spawn = has_spawn ||
            action.action_type == KernelEntityTriggerActionType_SpawnEntity ||
            action.action_type == KernelEntityTriggerActionType_SpawnProjectile;
    }
    return !require_spawn || has_spawn;
}

}  // namespace

bool validate_item_template(
    const KernelItemTemplateDefinition& definition,
    std::string* error) {
    if (definition.struct_size < sizeof(KernelItemTemplateDefinition) ||
        definition.item_template_id == 0 || definition.max_stack == 0) {
        return set_error(error, "invalid item template header");
    }
    if (definition.item_mode > KernelItemMode_Stateful ||
        (definition.item_mode == KernelItemMode_Stateful &&
         definition.max_stack != 1)) {
        return set_error(error, "stateful item requires max_stack == 1");
    }
    if (definition.portable_state_field_count >
        KERNEL_MAX_PORTABLE_STATE_FIELDS) {
        return set_error(error, "too many portable state fields");
    }
    std::unordered_set<std::uint32_t> field_ids;
    bool has_health_projection = false;
    for (std::uint32_t index = 0;
         index < definition.portable_state_field_count;
         ++index) {
        const KernelPortableStateFieldDefinition& field =
            definition.portable_state_fields[index];
        if (field.field_id == 0 || !field_ids.insert(field.field_id).second ||
            field.type > KernelPortableStateType_Bool ||
            field.world_projection >
                KernelPortableStateProjection_HealthCurrent) {
            return set_error(error, "invalid portable state field");
        }
        if (field.world_projection ==
            KernelPortableStateProjection_HealthCurrent) {
            if (has_health_projection ||
                field.type != KernelPortableStateType_Uint32) {
                return set_error(error, "invalid health projection");
            }
            has_health_projection = true;
        }
    }
    const KernelItemInputMappingDefinition& mapping = definition.input_mapping;
    if (!action_allowed_in_inventory(mapping.inventory_use) ||
        !action_allowed_in_inventory(mapping.inventory_fire) ||
        !action_allowed_in_world(mapping.world_interact_tap) ||
        !action_allowed_in_world(mapping.world_interact_hold)) {
        return set_error(error, "domain action is invalid for mapping context");
    }
    const std::uint8_t actions[] = {
        mapping.inventory_use,
        mapping.inventory_fire,
        mapping.world_interact_tap,
        mapping.world_interact_hold,
    };
    for (const std::uint8_t action : actions) {
        const std::uint32_t capability = required_capability(action);
        if (capability != 0 &&
            (definition.capability_flags & capability) == 0) {
            return set_error(error, "domain action is missing required capability");
        }
    }
    if ((mapping.world_interact_tap != KernelDomainAction_None ||
         mapping.world_interact_hold != KernelDomainAction_None) &&
        definition.interaction_range <= 0.0f) {
        return set_error(error, "world mapping requires interaction range");
    }
    if ((definition.capability_flags & KernelItemCapability_Deployable) != 0 &&
        definition.entity_template_id == 0) {
        return set_error(error, "deployable item requires entity template");
    }
    if (definition.throw_policy.struct_size <
            sizeof(KernelItemThrowDefinition) ||
        definition.throw_policy.mode >
            KernelItemThrowMode_ConsumeAndSpawn ||
        (definition.throw_policy.mode != KernelItemThrowMode_None &&
         definition.throw_policy.speed <= 0.0f)) {
        return set_error(error, "invalid throw policy");
    }
    if (definition.throw_policy.mode ==
            KernelItemThrowMode_IdentityPreserving &&
        definition.entity_template_id == 0) {
        return set_error(
            error,
            "identity-preserving throw requires entity template");
    }
    if (definition.use_policy.struct_size < sizeof(KernelItemUseDefinition)) {
        return set_error(error, "invalid use policy");
    }
    const bool consumes =
        mapping.inventory_use == KernelDomainAction_Consume ||
        mapping.inventory_fire == KernelDomainAction_Consume ||
        definition.throw_policy.mode == KernelItemThrowMode_ConsumeAndSpawn;
    if (consumes && !valid_item_used_graph(
            definition.item_used_trigger,
            definition.throw_policy.mode ==
                KernelItemThrowMode_ConsumeAndSpawn)) {
        return set_error(error, "invalid on_item_used graph");
    }
    if (definition.use_policy.charge_field_id != 0) {
        const auto found = std::find_if(
            definition.portable_state_fields,
            definition.portable_state_fields +
                definition.portable_state_field_count,
            [&](const KernelPortableStateFieldDefinition& field) {
                return field.field_id == definition.use_policy.charge_field_id &&
                    field.type == KernelPortableStateType_Uint32;
            });
        if (found == definition.portable_state_fields +
                definition.portable_state_field_count) {
            return set_error(error, "charge field must be uint32 portable state");
        }
    }
    return true;
}

bool ItemStore::set_templates(
    std::span<const KernelItemTemplateDefinition> definitions,
    std::string* error) {
    std::unordered_map<std::uint32_t, KernelItemTemplateDefinition> validated;
    for (const KernelItemTemplateDefinition& definition : definitions) {
        if (!validate_item_template(definition, error) ||
            !validated.emplace(definition.item_template_id, definition).second) {
            if (error != nullptr && error->empty()) {
                *error = "duplicate item template id";
            }
            return false;
        }
    }
    templates_ = std::move(validated);
    return true;
}

std::optional<KernelInventoryContainerId> ItemStore::create_container(
    std::uint32_t owner_entity_id,
    std::uint32_t slot_capacity) {
    if (owner_entity_id == 0 || slot_capacity == 0 ||
        slot_capacity > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }
    const KernelInventoryContainerId id = next_container_id_++;
    containers_.emplace(
        id,
        InventoryContainerRecord{
            id,
            owner_entity_id,
            slot_capacity,
            std::vector<KernelItemInstanceId>(slot_capacity, 0),
            0,
        });
    return id;
}

std::optional<std::uint16_t> ItemStore::find_empty_slot(
    const InventoryContainerRecord& container,
    std::optional<std::uint16_t> preferred_slot) const {
    if (preferred_slot.has_value()) {
        if (*preferred_slot >= container.slots.size() ||
            container.slots[*preferred_slot] != 0) {
            return std::nullopt;
        }
        return preferred_slot;
    }
    const auto found = std::find(container.slots.begin(), container.slots.end(), 0);
    if (found == container.slots.end()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(
        std::distance(container.slots.begin(), found));
}

std::optional<KernelItemInstanceId> ItemStore::create_inventory_item(
    std::uint32_t item_template_id,
    std::uint32_t quantity,
    KernelInventoryContainerId container_id,
    std::optional<std::uint16_t> preferred_slot) {
    const KernelItemTemplateDefinition* definition =
        find_template(item_template_id);
    auto container = containers_.find(container_id);
    if (definition == nullptr || container == containers_.end() || quantity == 0 ||
        quantity > definition->max_stack ||
        (definition->item_mode == KernelItemMode_Stateful && quantity != 1)) {
        return std::nullopt;
    }
    const std::optional<std::uint16_t> slot =
        find_empty_slot(container->second, preferred_slot);
    if (!slot.has_value()) {
        return std::nullopt;
    }
    const KernelItemInstanceId id = next_item_instance_id_++;
    ItemInstanceRecord item;
    item.item_instance_id = id;
    item.item_template_id = item_template_id;
    item.quantity = quantity;
    item.portable_state.assign(
        definition->portable_state_fields,
        definition->portable_state_fields +
            definition->portable_state_field_count);
    item.residency = ItemResidency{
        KernelItemResidency_Inventory,
        container_id,
        *slot,
        0,
        KernelWorldItemMode_Placed,
        0,
    };
    auto inserted = items_.emplace(id, std::move(item));
    container->second.slots[*slot] = id;
    publish_delta(
        &container->second,
        KernelInventoryDeltaType_Add,
        *slot,
        *slot,
        &inserted.first->second);
    return id;
}

std::optional<KernelItemInstanceId> ItemStore::create_world_item(
    std::uint32_t item_template_id,
    std::uint32_t quantity,
    std::uint32_t prop_entity_id,
    KernelWorldItemMode world_mode) {
    const KernelItemTemplateDefinition* definition =
        find_template(item_template_id);
    if (definition == nullptr || definition->entity_template_id == 0 ||
        quantity == 0 || quantity > definition->max_stack ||
        prop_entity_id == 0 || world_mode > KernelWorldItemMode_InFlight ||
        (definition->item_mode == KernelItemMode_Stateful && quantity != 1)) {
        return std::nullopt;
    }
    const auto prop_claimed = std::find_if(
        items_.begin(),
        items_.end(),
        [prop_entity_id](const auto& entry) {
            return !entry.second.terminal &&
                entry.second.residency.kind == KernelItemResidency_World &&
                entry.second.residency.prop_entity_id == prop_entity_id;
        });
    if (prop_claimed != items_.end()) {
        return std::nullopt;
    }
    const KernelItemInstanceId id = next_item_instance_id_++;
    ItemInstanceRecord item;
    item.item_instance_id = id;
    item.item_template_id = item_template_id;
    item.quantity = quantity;
    item.portable_state.assign(
        definition->portable_state_fields,
        definition->portable_state_fields +
            definition->portable_state_field_count);
    item.residency.kind = KernelItemResidency_World;
    item.residency.prop_entity_id = prop_entity_id;
    item.residency.world_mode = world_mode;
    items_.emplace(id, std::move(item));
    return id;
}

const KernelItemTemplateDefinition* ItemStore::find_template(
    std::uint32_t item_template_id) const {
    const auto found = templates_.find(item_template_id);
    return found == templates_.end() ? nullptr : &found->second;
}

const ItemInstanceRecord* ItemStore::find_item(KernelItemInstanceId id) const {
    const auto found = items_.find(id);
    return found == items_.end() ? nullptr : &found->second;
}

ItemInstanceRecord* ItemStore::find_item(KernelItemInstanceId id) {
    const auto found = items_.find(id);
    return found == items_.end() ? nullptr : &found->second;
}

const InventoryContainerRecord* ItemStore::find_container(
    KernelInventoryContainerId id) const {
    const auto found = containers_.find(id);
    return found == containers_.end() ? nullptr : &found->second;
}

const InventoryContainerRecord* ItemStore::find_container_for_owner(
    std::uint32_t owner_entity_id) const {
    const auto found = std::find_if(
        containers_.begin(),
        containers_.end(),
        [owner_entity_id](const auto& entry) {
            return entry.second.owner_entity_id == owner_entity_id;
        });
    return found == containers_.end() ? nullptr : &found->second;
}

std::optional<KernelItemInstanceId> ItemStore::split_inventory_stack(
    KernelItemInstanceId source_id,
    std::uint32_t quantity,
    std::optional<std::uint16_t> destination_slot) {
    ItemInstanceRecord* source = find_item(source_id);
    if (source == nullptr || source->terminal ||
        source->residency.kind != KernelItemResidency_Inventory ||
        quantity == 0 || quantity >= source->quantity) {
        return std::nullopt;
    }
    const KernelItemTemplateDefinition* definition =
        find_template(source->item_template_id);
    if (definition == nullptr || definition->item_mode != KernelItemMode_Fungible) {
        return std::nullopt;
    }
    const auto created = create_inventory_item(
        source->item_template_id,
        quantity,
        source->residency.container_id,
        destination_slot);
    if (!created.has_value()) {
        return std::nullopt;
    }
    source = find_item(source_id);
    source->quantity -= quantity;
    auto container = containers_.find(source->residency.container_id);
    publish_delta(
        &container->second,
        KernelInventoryDeltaType_Update,
        source->residency.slot,
        source->residency.slot,
        source);
    return created;
}

std::uint32_t ItemStore::merge_inventory_stacks(
    KernelItemInstanceId destination_id,
    KernelItemInstanceId source_id) {
    ItemInstanceRecord* destination = find_item(destination_id);
    ItemInstanceRecord* source = find_item(source_id);
    if (destination == nullptr || source == nullptr || destination == source ||
        destination->terminal || source->terminal ||
        destination->item_template_id != source->item_template_id ||
        !compatible_runtime_state(*destination, *source) ||
        destination->residency.kind != KernelItemResidency_Inventory ||
        source->residency.kind != KernelItemResidency_Inventory ||
        destination->residency.container_id != source->residency.container_id) {
        return 0;
    }
    const KernelItemTemplateDefinition* definition =
        find_template(destination->item_template_id);
    if (definition == nullptr || definition->item_mode != KernelItemMode_Fungible ||
        destination->quantity >= definition->max_stack) {
        return 0;
    }
    const std::uint32_t moved = std::min<std::uint32_t>(
        source->quantity,
        definition->max_stack - destination->quantity);
    destination->quantity += moved;
    source->quantity -= moved;
    auto container = containers_.find(destination->residency.container_id);
    publish_delta(
        &container->second,
        KernelInventoryDeltaType_Update,
        destination->residency.slot,
        destination->residency.slot,
        destination);
    if (source->quantity == 0) {
        terminate(source_id);
    } else {
        publish_delta(
            &container->second,
            KernelInventoryDeltaType_Update,
            source->residency.slot,
            source->residency.slot,
            source);
    }
    return moved;
}

std::optional<KernelItemInstanceId> ItemStore::split_to_world(
    KernelItemInstanceId source_id,
    std::uint32_t quantity,
    std::uint32_t prop_entity_id,
    KernelWorldItemMode world_mode) {
    ItemInstanceRecord* source = find_item(source_id);
    if (source == nullptr || source->terminal ||
        source->residency.kind != KernelItemResidency_Inventory ||
        quantity == 0 || quantity >= source->quantity || prop_entity_id == 0) {
        return std::nullopt;
    }
    const KernelItemTemplateDefinition* definition =
        find_template(source->item_template_id);
    if (definition == nullptr || definition->item_mode != KernelItemMode_Fungible) {
        return std::nullopt;
    }
    const KernelItemInstanceId id = next_item_instance_id_++;
    ItemInstanceRecord split;
    split.item_instance_id = id;
    split.item_template_id = source->item_template_id;
    split.quantity = quantity;
    split.portable_state = source->portable_state;
    split.residency.kind = KernelItemResidency_World;
    split.residency.prop_entity_id = prop_entity_id;
    split.residency.world_mode = world_mode;
    items_.emplace(id, std::move(split));
    source = find_item(source_id);
    source->quantity -= quantity;
    auto container = containers_.find(source->residency.container_id);
    publish_delta(
        &container->second,
        KernelInventoryDeltaType_Update,
        source->residency.slot,
        source->residency.slot,
        source);
    return id;
}

std::optional<KernelItemInstanceId> ItemStore::transfer_world_to_inventory(
    KernelItemInstanceId source_id,
    KernelInventoryContainerId container_id) {
    ItemInstanceRecord* source = find_item(source_id);
    auto container = containers_.find(container_id);
    if (source == nullptr || source->terminal ||
        source->residency.kind != KernelItemResidency_World ||
        container == containers_.end()) {
        return std::nullopt;
    }
    const KernelItemTemplateDefinition* definition =
        find_template(source->item_template_id);
    if (definition == nullptr) {
        return std::nullopt;
    }
    std::vector<KernelItemInstanceId> destinations;
    std::uint64_t available = 0;
    if (definition->item_mode == KernelItemMode_Fungible) {
        for (const KernelItemInstanceId candidate_id : container->second.slots) {
            ItemInstanceRecord* candidate = find_item(candidate_id);
            if (candidate != nullptr && !candidate->terminal &&
                candidate->item_template_id == source->item_template_id &&
                compatible_runtime_state(*candidate, *source) &&
                candidate->quantity < definition->max_stack) {
                destinations.push_back(candidate_id);
                available += definition->max_stack - candidate->quantity;
            }
        }
    }
    const bool needs_slot = available < source->quantity;
    const std::optional<std::uint16_t> empty_slot =
        needs_slot ? find_empty_slot(container->second, std::nullopt)
                   : std::optional<std::uint16_t>{};
    if (needs_slot && !empty_slot.has_value()) {
        return std::nullopt;
    }
    for (const KernelItemInstanceId destination_id : destinations) {
        source = find_item(source_id);
        if (source->quantity == 0) break;
        ItemInstanceRecord* destination = find_item(destination_id);
        const std::uint32_t moved = std::min<std::uint32_t>(
            source->quantity,
            definition->max_stack - destination->quantity);
        destination->quantity += moved;
        source->quantity -= moved;
        publish_delta(
            &container->second,
            KernelInventoryDeltaType_Update,
            destination->residency.slot,
            destination->residency.slot,
            destination);
    }
    source = find_item(source_id);
    if (source->quantity == 0) {
        terminate(source_id);
        return destinations.empty()
            ? std::optional<KernelItemInstanceId>{}
            : std::optional<KernelItemInstanceId>{destinations.front()};
    }
    source->residency = ItemResidency{
        KernelItemResidency_Inventory,
        container_id,
        *empty_slot,
        0,
        KernelWorldItemMode_Placed,
        0,
    };
    container->second.slots[*empty_slot] = source_id;
    publish_delta(
        &container->second,
        KernelInventoryDeltaType_Add,
        *empty_slot,
        *empty_slot,
        source);
    return source_id;
}

std::optional<ItemConsumeResult> ItemStore::consume(
    KernelItemInstanceId id,
    std::uint32_t current_tick) {
    ItemInstanceRecord* item = find_item(id);
    if (item == nullptr || item->terminal ||
        item->residency.kind != KernelItemResidency_Inventory ||
        current_tick < item->next_use_tick) {
        return std::nullopt;
    }
    const KernelItemTemplateDefinition* definition =
        find_template(item->item_template_id);
    if (definition == nullptr ||
        (definition->capability_flags & KernelItemCapability_Consumable) == 0) {
        return std::nullopt;
    }
    ItemConsumeResult result;
    if (definition->use_policy.charge_field_id != 0) {
        const auto field = std::find_if(
            item->portable_state.begin(),
            item->portable_state.end(),
            [&](const KernelPortableStateFieldDefinition& candidate) {
                return candidate.field_id ==
                    definition->use_policy.charge_field_id;
            });
        if (field == item->portable_state.end() || field->uint32_default == 0) {
            return std::nullopt;
        }
        --field->uint32_default;
        result.committed_quantity = 1;
        if (field->uint32_default == 0 &&
            definition->use_policy.destroy_when_empty != 0) {
            result.terminal = terminate(id);
        }
    } else {
        const std::uint32_t cost = definition->use_policy.quantity_cost;
        if (cost == 0 || item->quantity < cost) {
            return std::nullopt;
        }
        item->quantity -= cost;
        result.committed_quantity = cost;
        if (item->quantity == 0) {
            result.terminal = terminate(id);
        }
    }
    item = find_item(id);
    if (!item->terminal) {
        item->next_use_tick = current_tick +
            definition->use_policy.cooldown_ticks;
        auto container = containers_.find(item->residency.container_id);
        publish_delta(
            &container->second,
            KernelInventoryDeltaType_Update,
            item->residency.slot,
            item->residency.slot,
            item);
    }
    return result;
}

std::optional<ItemConsumeResult> ItemStore::consume_quantity(
    KernelItemInstanceId id,
    std::uint32_t quantity,
    std::uint32_t current_tick) {
    ItemInstanceRecord* item = find_item(id);
    if (item == nullptr || item->terminal || quantity == 0 ||
        item->residency.kind != KernelItemResidency_Inventory ||
        item->quantity < quantity || current_tick < item->next_use_tick) {
        return std::nullopt;
    }
    const KernelItemTemplateDefinition* definition =
        find_template(item->item_template_id);
    if (definition == nullptr ||
        (definition->item_mode == KernelItemMode_Stateful && quantity != 1)) {
        return std::nullopt;
    }
    item->quantity -= quantity;
    ItemConsumeResult result{quantity, false};
    if (item->quantity == 0) {
        result.terminal = terminate(id);
    } else {
        item->next_use_tick = current_tick + definition->use_policy.cooldown_ticks;
        auto container = containers_.find(item->residency.container_id);
        publish_delta(
            &container->second,
            KernelInventoryDeltaType_Update,
            item->residency.slot,
            item->residency.slot,
            item);
    }
    return result;
}

void ItemStore::remove_from_inventory(ItemInstanceRecord* item) {
    if (item == nullptr ||
        item->residency.kind != KernelItemResidency_Inventory) {
        return;
    }
    auto container = containers_.find(item->residency.container_id);
    if (container == containers_.end() ||
        item->residency.slot >= container->second.slots.size() ||
        container->second.slots[item->residency.slot] !=
            item->item_instance_id) {
        return;
    }
    const std::uint16_t slot = item->residency.slot;
    container->second.slots[slot] = 0;
    publish_delta(
        &container->second,
        KernelInventoryDeltaType_Remove,
        slot,
        slot,
        item);
}

bool ItemStore::move_to_world(
    KernelItemInstanceId id,
    std::uint32_t prop_entity_id,
    KernelWorldItemMode world_mode) {
    ItemInstanceRecord* item = find_item(id);
    if (item == nullptr || item->terminal || prop_entity_id == 0 ||
        world_mode > KernelWorldItemMode_InFlight) {
        return false;
    }
    const auto claimed = std::find_if(
        items_.begin(),
        items_.end(),
        [id, prop_entity_id](const auto& entry) {
            return entry.first != id && !entry.second.terminal &&
                entry.second.residency.kind == KernelItemResidency_World &&
                entry.second.residency.prop_entity_id == prop_entity_id;
        });
    if (claimed != items_.end()) {
        return false;
    }
    remove_from_inventory(item);
    item->residency = ItemResidency{
        KernelItemResidency_World,
        0,
        0,
        prop_entity_id,
        world_mode,
        0,
    };
    return true;
}

bool ItemStore::move_to_inventory(
    KernelItemInstanceId id,
    KernelInventoryContainerId container_id,
    std::optional<std::uint16_t> preferred_slot) {
    ItemInstanceRecord* item = find_item(id);
    auto container = containers_.find(container_id);
    if (item == nullptr || item->terminal ||
        container == containers_.end()) {
        return false;
    }
    const std::optional<std::uint16_t> slot =
        find_empty_slot(container->second, preferred_slot);
    if (!slot.has_value()) {
        return false;
    }
    remove_from_inventory(item);
    item->residency = ItemResidency{
        KernelItemResidency_Inventory,
        container_id,
        *slot,
        0,
        KernelWorldItemMode_Placed,
        0,
    };
    container->second.slots[*slot] = id;
    publish_delta(
        &container->second,
        KernelInventoryDeltaType_Add,
        *slot,
        *slot,
        item);
    return true;
}

bool ItemStore::set_world_mode(
    KernelItemInstanceId id,
    KernelWorldItemMode world_mode,
    std::uint32_t carrier_entity_id) {
    ItemInstanceRecord* item = find_item(id);
    if (item == nullptr || item->terminal ||
        item->residency.kind != KernelItemResidency_World ||
        world_mode > KernelWorldItemMode_InFlight ||
        (world_mode == KernelWorldItemMode_Carrying && carrier_entity_id == 0)) {
        return false;
    }
    item->residency.world_mode = world_mode;
    item->residency.carrier_entity_id =
        world_mode == KernelWorldItemMode_Carrying ? carrier_entity_id : 0;
    return true;
}

bool ItemStore::terminate(KernelItemInstanceId id) {
    ItemInstanceRecord* item = find_item(id);
    if (item == nullptr || item->terminal) {
        return false;
    }
    remove_from_inventory(item);
    item->quantity = 0;
    item->terminal = true;
    item->residency = ItemResidency{};
    item->residency.kind = KernelItemResidency_Terminal;
    return true;
}

void ItemStore::publish_delta(
    InventoryContainerRecord* container,
    KernelInventoryDeltaType type,
    std::uint16_t slot,
    std::uint16_t previous_slot,
    const ItemInstanceRecord* item) {
    if (container == nullptr) {
        return;
    }
    ++container->revision;
    KernelInventoryDelta delta{};
    delta.struct_size = sizeof(delta);
    delta.inventory_container_id = container->inventory_container_id;
    delta.revision = container->revision;
    delta.type = static_cast<std::uint8_t>(type);
    delta.slot = slot;
    delta.previous_slot = previous_slot;
    if (item != nullptr) {
        delta.item = item_view(item->item_instance_id);
    }
    pending_deltas_[container->inventory_container_id].push_back(delta);
}

std::vector<KernelInventoryDelta> ItemStore::take_inventory_deltas(
    KernelInventoryContainerId container_id,
    std::size_t max_deltas) {
    auto found = pending_deltas_.find(container_id);
    if (found == pending_deltas_.end() || max_deltas == 0u) {
        return {};
    }
    const std::size_t count = std::min(max_deltas, found->second.size());
    std::vector<KernelInventoryDelta> result(
        found->second.begin(), found->second.begin() + count);
    found->second.erase(found->second.begin(), found->second.begin() + count);
    if (found->second.empty()) {
        pending_deltas_.erase(found);
    }
    return result;
}

KernelItemInstanceView ItemStore::item_view(KernelItemInstanceId id) const {
    KernelItemInstanceView view{};
    view.struct_size = sizeof(view);
    const ItemInstanceRecord* item = find_item(id);
    if (item == nullptr) {
        return view;
    }
    view.item_instance_id = item->item_instance_id;
    view.item_template_id = item->item_template_id;
    view.quantity = item->quantity;
    view.residency = static_cast<std::uint8_t>(item->residency.kind);
    view.world_mode = static_cast<std::uint8_t>(item->residency.world_mode);
    view.slot = item->residency.slot;
    view.inventory_container_id = item->residency.container_id;
    view.prop_entity_id = item->residency.prop_entity_id;
    view.carrier_entity_id = item->residency.carrier_entity_id;
    view.terminal = item->terminal ? 1u : 0u;
    view.next_use_tick = item->next_use_tick;
    view.portable_state_field_count = static_cast<std::uint32_t>(
        std::min<std::size_t>(
            item->portable_state.size(), KERNEL_MAX_PORTABLE_STATE_FIELDS));
    std::copy_n(
        item->portable_state.begin(),
        view.portable_state_field_count,
        view.portable_state_fields);
    return view;
}

KernelInventoryContainerView ItemStore::container_view(
    KernelInventoryContainerId id) const {
    KernelInventoryContainerView view{};
    view.struct_size = sizeof(view);
    const InventoryContainerRecord* container = find_container(id);
    if (container == nullptr) {
        return view;
    }
    view.inventory_container_id = container->inventory_container_id;
    view.owner_entity_id = container->owner_entity_id;
    view.slot_capacity = container->slot_capacity;
    view.occupied_slot_count = static_cast<std::uint32_t>(std::count_if(
        container->slots.begin(),
        container->slots.end(),
        [](KernelItemInstanceId item) { return item != 0; }));
    view.revision = container->revision;
    return view;
}

}  // namespace network_example
