#ifndef SIMULATION_PUBLIC_ITEM_SYSTEM_H_
#define SIMULATION_PUBLIC_ITEM_SYSTEM_H_

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "kernel/public/kernel_types.h"

namespace network_example {

struct ItemResidency {
    KernelItemResidencyKind kind = KernelItemResidency_None;
    KernelInventoryContainerId container_id = 0;
    std::uint16_t slot = 0;
    std::uint32_t prop_entity_id = 0;
    KernelWorldItemMode world_mode = KernelWorldItemMode_Placed;
    std::uint32_t carrier_entity_id = 0;
};

struct ItemInstanceRecord {
    KernelItemInstanceId item_instance_id = 0;
    std::uint32_t item_template_id = 0;
    std::uint32_t quantity = 0;
    std::vector<KernelPortableStateFieldDefinition> portable_state;
    ItemResidency residency;
    std::uint32_t next_use_tick = 0;
    bool terminal = false;
    // KERNEL_DROP_TAG_*.
    std::uint8_t drop_tag = 0;
};

struct InventoryContainerRecord {
    KernelInventoryContainerId inventory_container_id = 0;
    std::uint32_t owner_entity_id = 0;
    std::uint32_t slot_capacity = 0;
    std::vector<KernelItemInstanceId> slots;
    std::uint64_t revision = 0;
    // KernelInventoryContainerKind.
    std::uint8_t kind = KernelInventoryContainerKind_Items;
};

struct ItemConsumeResult {
    std::uint32_t committed_quantity = 0;
    bool terminal = false;
};

bool validate_item_template(
    const KernelItemTemplateDefinition& definition,
    std::string* error);

class ItemStore {
public:
    bool set_templates(
        std::span<const KernelItemTemplateDefinition> definitions,
        std::string* error);

    std::optional<KernelInventoryContainerId> create_container(
        std::uint32_t owner_entity_id,
        std::uint32_t slot_capacity,
        std::uint8_t kind = KernelInventoryContainerKind_Items);

    std::optional<KernelItemInstanceId> create_inventory_item(
        std::uint32_t item_template_id,
        std::uint32_t quantity,
        KernelInventoryContainerId container_id,
        std::optional<std::uint16_t> preferred_slot = std::nullopt);

    std::optional<KernelItemInstanceId> create_world_item(
        std::uint32_t item_template_id,
        std::uint32_t quantity,
        std::uint32_t prop_entity_id,
        KernelWorldItemMode world_mode = KernelWorldItemMode_Placed);

    const KernelItemTemplateDefinition* find_template(
        std::uint32_t item_template_id) const;
    const ItemInstanceRecord* find_item(KernelItemInstanceId id) const;
    ItemInstanceRecord* find_item(KernelItemInstanceId id);
    const InventoryContainerRecord* find_container(
        KernelInventoryContainerId id) const;
    // The owner's item container; never its weapon container.
    const InventoryContainerRecord* find_container_for_owner(
        std::uint32_t owner_entity_id) const;
    const InventoryContainerRecord* find_weapon_container_for_owner(
        std::uint32_t owner_entity_id) const;
    // Every weapon container, for keeping loadouts in step with them.
    std::vector<const InventoryContainerRecord*> weapon_containers() const;
    // Writes a uint32 portable state field of a live item, wherever it is,
    // publishing an Update delta when it sits in a container. False if the
    // item has no such field; true without a delta when the value is unchanged.
    bool set_portable_uint32(
        KernelItemInstanceId id,
        std::uint32_t field_id,
        std::uint32_t value);
    std::vector<KernelInventoryContainerId> containers_for_owner(
        std::uint32_t owner_entity_id) const;

    std::optional<KernelItemInstanceId> split_inventory_stack(
        KernelItemInstanceId source_id,
        std::uint32_t quantity,
        std::optional<std::uint16_t> destination_slot = std::nullopt);
    std::uint32_t merge_inventory_stacks(
        KernelItemInstanceId destination_id,
        KernelItemInstanceId source_id);
    std::optional<KernelItemInstanceId> split_to_world(
        KernelItemInstanceId source_id,
        std::uint32_t quantity,
        std::uint32_t prop_entity_id,
        KernelWorldItemMode world_mode);
    std::optional<KernelItemInstanceId> transfer_world_to_inventory(
        KernelItemInstanceId source_id,
        KernelInventoryContainerId container_id);
    // Moves `quantity` of an inventory item into another container (K9: from
    // a camp's stock onto a player). Fungible quantity tops up compatible
    // stacks there first and the rest takes one empty slot; a weapon takes its
    // category's slot. All or nothing: nullopt, and nothing moved, when it does
    // not fit. Returns the item now holding what moved -- the source itself
    // when all of it went to a slot of its own.
    std::optional<KernelItemInstanceId> transfer_to_container(
        KernelItemInstanceId source_id,
        std::uint32_t quantity,
        KernelInventoryContainerId container_id);
    std::optional<ItemConsumeResult> consume(
        KernelItemInstanceId id,
        std::uint32_t current_tick);
    std::optional<ItemConsumeResult> consume_quantity(
        KernelItemInstanceId id,
        std::uint32_t quantity,
        std::uint32_t current_tick);

    bool move_to_world(
        KernelItemInstanceId id,
        std::uint32_t prop_entity_id,
        KernelWorldItemMode world_mode);
    bool move_to_inventory(
        KernelItemInstanceId id,
        KernelInventoryContainerId container_id,
        std::optional<std::uint16_t> preferred_slot = std::nullopt);
    bool move_inventory_slot(
        KernelItemInstanceId id,
        std::uint16_t destination_slot);
    bool set_world_mode(
        KernelItemInstanceId id,
        KernelWorldItemMode world_mode,
        std::uint32_t carrier_entity_id = 0);
    bool terminate(KernelItemInstanceId id);
    // Sets a live item's drop tag, publishing an Update when it sits in a
    // container. False for a terminal or unknown item, or a tag past
    // KERNEL_DROP_TAG_MAP_WEAPON.
    bool set_drop_tag(KernelItemInstanceId id, std::uint8_t drop_tag);
    // Terminates every item in the container whose drop tag is NONE, the
    // tagged ones staying where they are. False if there is no such container.
    bool clear_untagged(KernelInventoryContainerId id);
    // Terminates every item in the container, publishing a Remove delta per
    // slot. The container itself stays, empty. False if there is no such
    // container.
    bool clear_container(KernelInventoryContainerId id);
    // Terminates every item in the container and then removes the container
    // itself, with its delta history: a camp's stock when the camp goes, or a
    // client's copy of a container it no longer sees. False if there is no
    // such container.
    bool destroy_container(KernelInventoryContainerId id);

    std::vector<KernelInventoryDelta> take_inventory_deltas(
        KernelInventoryContainerId container_id,
        std::size_t max_deltas = std::numeric_limits<std::size_t>::max());
    std::vector<KernelInventoryDelta> inventory_deltas_since(
        KernelInventoryContainerId container_id,
        std::uint64_t after_revision,
        std::size_t max_deltas = std::numeric_limits<std::size_t>::max()) const;
    bool apply_replica_snapshot(
        const KernelInventoryContainerView& container,
        std::span<const KernelItemInstanceView> items);
    bool apply_replica_deltas(
        KernelInventoryContainerId container_id,
        std::span<const KernelInventoryDelta> deltas);
    KernelItemInstanceView item_view(KernelItemInstanceId id) const;
    KernelInventoryContainerView container_view(
        KernelInventoryContainerId id) const;

private:
    std::optional<std::uint16_t> find_empty_slot(
        const InventoryContainerRecord& container,
        const KernelItemTemplateDefinition& definition,
        std::optional<std::uint16_t> preferred_slot) const;
    void remove_from_inventory(ItemInstanceRecord* item);
    void publish_delta(
        InventoryContainerRecord* container,
        KernelInventoryDeltaType type,
        std::uint16_t slot,
        std::uint16_t previous_slot,
        const ItemInstanceRecord* item,
        std::uint16_t changed_fields = KernelInventoryChange_All);

    KernelItemInstanceId next_item_instance_id_ = 1;
    KernelInventoryContainerId next_container_id_ = 1;
    std::unordered_map<std::uint32_t, KernelItemTemplateDefinition> templates_;
    std::unordered_map<KernelItemInstanceId, ItemInstanceRecord> items_;
    std::unordered_map<KernelInventoryContainerId, InventoryContainerRecord>
        containers_;
    std::unordered_map<KernelInventoryContainerId, std::vector<KernelInventoryDelta>>
        pending_deltas_;
    std::unordered_map<KernelInventoryContainerId, std::vector<KernelInventoryDelta>>
        delta_history_;
};

}  // namespace network_example

#endif  // SIMULATION_PUBLIC_ITEM_SYSTEM_H_
