// Packet schema 29: an inventory record carries the item's drop tag (K12) -- on
// an Add and a snapshot page, as part of the whole item, and on an Update that
// only retags it -- so the owner's client can mark a quest item or a map
// weapon. A tag past KERNEL_DROP_TAG_MAP_WEAPON is not a record.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "protocol/public/network_packets.h"

namespace {

namespace ne = network_example;

void require_impl(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}
#define require(condition) require_impl((condition), #condition, __LINE__)

ne::InventoryWireItem relic() {
    ne::InventoryWireItem item;
    item.item_instance_id = 77;
    item.item_template_id = 3030;
    item.quantity = 1;
    item.drop_tag = KERNEL_DROP_TAG_QUEST;
    return item;
}

}  // namespace

int main() {
    ne::InventoryDeltaBatchPacket batch;
    batch.inventory_container_id = 4;
    batch.first_revision = 9;
    ne::InventoryDeltaRecord add;
    add.type = KernelInventoryDeltaType_Add;
    add.slot = 2;
    add.previous_slot = 2;
    add.item = relic();
    batch.records.push_back(add);
    ne::InventoryDeltaRecord retag;
    retag.type = KernelInventoryDeltaType_Update;
    retag.slot = 2;
    retag.previous_slot = 2;
    retag.changed_fields = ne::kInventoryChangeDropTag;
    retag.item = relic();
    retag.item.drop_tag = KERNEL_DROP_TAG_MAP_WEAPON;
    batch.records.push_back(retag);
    const std::vector<std::uint8_t> encoded = ne::encode_inventory_delta_batch_packet(batch, 1);
    require(!encoded.empty());
    ne::InventoryDeltaBatchPacket decoded;
    require(ne::decode_inventory_delta_batch_packet(encoded.data(), encoded.size(), &decoded));
    require(decoded.records.size() == 2u);
    require(decoded.records[0].item.drop_tag == KERNEL_DROP_TAG_QUEST);
    require(decoded.records[1].changed_fields == ne::kInventoryChangeDropTag);
    require(decoded.records[1].item.drop_tag == KERNEL_DROP_TAG_MAP_WEAPON);

    ne::InventorySnapshotPagePacket page;
    page.inventory_container_id = 4;
    page.owner_entity_id = 7;
    page.revision = 11;
    page.slot_capacity = 8;
    page.page_index = 0;
    page.page_count = 1;
    page.entries.push_back(ne::InventorySnapshotEntry{2, relic()});
    const std::vector<std::uint8_t> page_bytes =
        ne::encode_inventory_snapshot_page_packet(page, 1);
    ne::InventorySnapshotPagePacket page_back;
    require(ne::decode_inventory_snapshot_page_packet(
        page_bytes.data(), page_bytes.size(), &page_back));
    require(page_back.entries.size() == 1u);
    require(page_back.entries[0].item.drop_tag == KERNEL_DROP_TAG_QUEST);

    // An unknown tag does not decode.
    ne::InventoryDeltaBatchPacket bad = batch;
    bad.records.resize(1);
    bad.records[0].item.drop_tag = 9;
    const std::vector<std::uint8_t> bad_bytes = ne::encode_inventory_delta_batch_packet(bad, 1);
    ne::InventoryDeltaBatchPacket ignored;
    require(bad_bytes.empty() ||
            !ne::decode_inventory_delta_batch_packet(bad_bytes.data(), bad_bytes.size(), &ignored));

    std::puts("inventory_drop_tag_roundtrip_test passed");
    return 0;
}
