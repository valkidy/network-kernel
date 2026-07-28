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
};

struct InventoryContainerRecord {
    KernelInventoryContainerId inventory_container_id = 0;
    std::uint32_t owner_entity_id = 0;
    std::uint32_t slot_capacity = 0;
    std::vector<KernelItemInstanceId> slots;
    std::uint64_t revision = 0;
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
        std::uint32_t slot_capacity);

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
    const InventoryContainerRecord* find_container_for_owner(
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
    bool set_world_mode(
        KernelItemInstanceId id,
        KernelWorldItemMode world_mode,
        std::uint32_t carrier_entity_id = 0);
    bool terminate(KernelItemInstanceId id);

    std::vector<KernelInventoryDelta> take_inventory_deltas(
        KernelInventoryContainerId container_id,
        std::size_t max_deltas = std::numeric_limits<std::size_t>::max());
    KernelItemInstanceView item_view(KernelItemInstanceId id) const;
    KernelInventoryContainerView container_view(
        KernelInventoryContainerId id) const;

private:
    std::optional<std::uint16_t> find_empty_slot(
        const InventoryContainerRecord& container,
        std::optional<std::uint16_t> preferred_slot) const;
    void remove_from_inventory(ItemInstanceRecord* item);
    void publish_delta(
        InventoryContainerRecord* container,
        KernelInventoryDeltaType type,
        std::uint16_t slot,
        std::uint16_t previous_slot,
        const ItemInstanceRecord* item);

    KernelItemInstanceId next_item_instance_id_ = 1;
    KernelInventoryContainerId next_container_id_ = 1;
    std::unordered_map<std::uint32_t, KernelItemTemplateDefinition> templates_;
    std::unordered_map<KernelItemInstanceId, ItemInstanceRecord> items_;
    std::unordered_map<KernelInventoryContainerId, InventoryContainerRecord>
        containers_;
    std::unordered_map<KernelInventoryContainerId, std::vector<KernelInventoryDelta>>
        pending_deltas_;
};

}  // namespace network_example

#endif  // SIMULATION_PUBLIC_ITEM_SYSTEM_H_
