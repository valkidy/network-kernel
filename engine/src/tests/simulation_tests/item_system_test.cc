#include <cstdlib>
#include <string>
#include <vector>

#include "simulation/public/item_system.h"

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

KernelItemTemplateDefinition fungible_template() {
    KernelItemTemplateDefinition item{};
    item.struct_size = sizeof(item);
    item.item_template_id = 10;
    item.item_mode = KernelItemMode_Fungible;
    item.max_stack = 10;
    item.capability_flags =
        KernelItemCapability_Pickupable |
        KernelItemCapability_Deployable |
        KernelItemCapability_Throwable |
        KernelItemCapability_Consumable;
    item.entity_template_id = 100;
    item.interaction_range = 3.0f;
    item.throw_policy.struct_size = sizeof(item.throw_policy);
    item.throw_policy.mode = KernelItemThrowMode_IdentityPreserving;
    item.throw_policy.speed = 12.0f;
    item.use_policy.struct_size = sizeof(item.use_policy);
    item.use_policy.quantity_cost = 1;
    item.item_used_trigger.struct_size = sizeof(item.item_used_trigger);
    return item;
}

KernelItemTemplateDefinition stateful_template() {
    KernelItemTemplateDefinition item{};
    item.struct_size = sizeof(item);
    item.item_template_id = 20;
    item.item_mode = KernelItemMode_Stateful;
    item.max_stack = 1;
    item.capability_flags = KernelItemCapability_Consumable;
    item.throw_policy.struct_size = sizeof(item.throw_policy);
    item.use_policy.struct_size = sizeof(item.use_policy);
    item.use_policy.charge_field_id = 7;
    item.use_policy.destroy_when_empty = 1;
    item.use_policy.cooldown_ticks = 2;
    item.portable_state_field_count = 1;
    item.portable_state_fields[0].field_id = 7;
    item.portable_state_fields[0].type = KernelPortableStateType_Uint32;
    item.portable_state_fields[0].uint32_default = 3;
    item.item_used_trigger.struct_size = sizeof(item.item_used_trigger);
    return item;
}

void validates_templates() {
    std::string error;
    require(network_example::validate_item_template(fungible_template(), &error));
    require(network_example::validate_item_template(stateful_template(), &error));

    KernelItemTemplateDefinition invalid = stateful_template();
    invalid.max_stack = 2;
    require(!network_example::validate_item_template(invalid, &error));

    invalid = fungible_template();
    invalid.capability_flags &= ~KernelItemCapability_Throwable;
    require(!network_example::validate_item_template(invalid, &error));

    invalid = fungible_template();
    invalid.interaction_range = 0.0f;
    require(!network_example::validate_item_template(invalid, &error));

    invalid = fungible_template();
    invalid.throw_policy.mode = KernelItemThrowMode_ConsumeAndSpawn;
    require(!network_example::validate_item_template(invalid, &error));

    invalid = fungible_template();
    invalid.portable_state_field_count = 1;
    invalid.portable_state_fields[0].field_id = 9;
    invalid.portable_state_fields[0].type = KernelPortableStateType_Float;
    invalid.portable_state_fields[0].world_projection =
        KernelPortableStateProjection_HealthCurrent;
    require(!network_example::validate_item_template(invalid, &error));

    invalid = stateful_template();
    invalid.use_policy.charge_field_id = 99;
    require(!network_example::validate_item_template(invalid, &error));

    invalid = stateful_template();
    invalid.capability_flags |= KernelItemCapability_Throwable;
    require(!network_example::validate_item_template(invalid, &error));

    invalid = fungible_template();
    invalid.portable_state_field_count = 2;
    invalid.portable_state_fields[0].field_id = 9;
    invalid.portable_state_fields[0].type = KernelPortableStateType_Bool;
    invalid.portable_state_fields[1] = invalid.portable_state_fields[0];
    require(!network_example::validate_item_template(invalid, &error));
}

void enforces_slots_split_merge_and_tombstones() {
    network_example::ItemStore store;
    const std::vector<KernelItemTemplateDefinition> templates = {
        fungible_template(),
        stateful_template(),
    };
    std::string error;
    require(store.set_templates(templates, &error));
    const auto container = store.create_container(42, 2);
    require(container.has_value());

    const auto first = store.create_inventory_item(10, 8, *container);
    require(first.has_value());
    const auto split = store.split_inventory_stack(*first, 3);
    require(split.has_value());
    require(store.find_item(*first)->quantity == 5);
    require(store.find_item(*split)->quantity == 3);
    require(!store.create_inventory_item(10, 1, *container).has_value());

    require(store.merge_inventory_stacks(*first, *split) == 3);
    require(store.find_item(*first)->quantity == 8);
    require(store.find_item(*split)->terminal);
    require(store.find_item(*split)->residency.kind ==
        KernelItemResidency_Terminal);

    const auto next = store.create_inventory_item(20, 1, *container);
    require(next.has_value());
    require(*next > *split);
    require(store.container_view(*container).occupied_slot_count == 2);
    require(store.move_inventory_slot(*next, 0) == false);

    const std::vector<KernelInventoryDelta> first_delta_page =
        store.take_inventory_deltas(*container, 1);
    require(first_delta_page.size() == 1);
    std::vector<KernelInventoryDelta> deltas =
        store.take_inventory_deltas(*container);
    require(!deltas.empty());
    require(deltas.front().revision > first_delta_page.front().revision);
    for (std::size_t index = 1; index < deltas.size(); ++index) {
        require(deltas[index].revision > deltas[index - 1].revision);
    }

    require(store.consume(*next, 5).has_value());
    require(store.item_view(*next).portable_state_fields[0].uint32_default == 2);
    const std::vector<KernelInventoryDelta> charge_delta =
        store.take_inventory_deltas(*container);
    require(charge_delta.size() == 1u);
    require(charge_delta[0].changed_fields ==
        (KernelInventoryChange_PortableState |
         KernelInventoryChange_Cooldown));
    require(!store.consume(*next, 6).has_value());
    require(store.consume(*next, 7).has_value());
    const auto last_charge = store.consume(*next, 9);
    require(last_charge.has_value());
    require(last_charge->terminal);
    require(store.find_item(*next)->terminal);

    const std::vector<KernelInventoryDelta> history =
        store.inventory_deltas_since(*container, 0);
    require(!history.empty());
    require(history.back().revision == store.container_view(*container).revision);
}

void preserves_identity_across_residency_and_world_modes() {
    network_example::ItemStore store;
    const std::vector<KernelItemTemplateDefinition> templates = {
        fungible_template(),
    };
    std::string error;
    require(store.set_templates(templates, &error));
    const auto container = store.create_container(42, 1);
    const auto item = store.create_inventory_item(10, 4, *container);
    require(item.has_value());

    require(store.move_to_world(
        *item,
        900,
        KernelWorldItemMode_InFlight));
    require(store.find_item(*item)->residency.prop_entity_id == 900);
    require(store.set_world_mode(
        *item,
        KernelWorldItemMode_Carrying,
        42));
    require(store.find_item(*item)->residency.carrier_entity_id == 42);
    require(store.set_world_mode(*item, KernelWorldItemMode_Placed));
    require(store.move_to_inventory(*item, *container));
    require(store.find_item(*item)->item_instance_id == *item);
    require(store.find_item(*item)->residency.kind ==
        KernelItemResidency_Inventory);
}

void applies_replica_snapshot_and_contiguous_deltas() {
    network_example::ItemStore source;
    network_example::ItemStore replica;
    const std::vector<KernelItemTemplateDefinition> templates = {
        fungible_template(),
    };
    std::string error;
    require(source.set_templates(templates, &error));
    require(replica.set_templates(templates, &error));
    const auto container = source.create_container(42, 3);
    const auto item = source.create_inventory_item(10, 2, *container);
    KernelInventoryContainerView container_view = source.container_view(*container);
    KernelItemInstanceView item_view = source.item_view(*item);
    require(replica.apply_replica_snapshot(container_view, {&item_view, 1}));
    require(replica.find_item(*item)->quantity == 2);

    source.take_inventory_deltas(*container);
    require(source.move_inventory_slot(*item, 2));
    const auto deltas = source.inventory_deltas_since(
        *container, container_view.revision);
    require(replica.apply_replica_deltas(*container, deltas));
    require(replica.find_item(*item)->residency.slot == 2);
    require(replica.container_view(*container).revision ==
        source.container_view(*container).revision);

    KernelInventoryDelta gap = deltas.back();
    gap.revision += 2;
    require(!replica.apply_replica_deltas(*container, {&gap, 1}));
}

}  // namespace

int main() {
    validates_templates();
    enforces_slots_split_merge_and_tombstones();
    preserves_identity_across_residency_and_world_modes();
    applies_replica_snapshot_and_contiguous_deltas();
    return 0;
}
