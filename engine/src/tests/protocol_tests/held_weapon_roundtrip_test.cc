// Snapshot schema 28: every player record says which weapon the player holds
// (design D27), one byte, so every client can draw it -- an unarmed player
// says KERNEL_HELD_WEAPON_NONE. Agents never carry it. Packet schema 28: an
// inventory snapshot page says which kind of container it is, so the owner can
// tell its weapon container from its items even when the former is empty.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

#include "protocol/public/network_packets.h"
#include "sync/public/snapshot.h"

namespace {

namespace ne = network_example;

void require_impl(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}
#define require(condition) require_impl((condition), #condition, __LINE__)

ne::EntitySnapshot player(ne::NetId net_id, bool says, std::uint8_t held) {
    ne::EntitySnapshot entity;
    entity.net_id = net_id;
    entity.type = ne::EntityType::kActor;
    entity.actor_type = ne::ActorType::kPlayer;
    entity.owner_peer = net_id;
    entity.position = glm::vec3{1.0f, 0.0f, 2.0f};
    entity.has_held_weapon = says;
    entity.held_weapon_id = held;
    return entity;
}

}  // namespace

int main() {
    ne::WorldSnapshot snapshot;
    snapshot.header.server_tick = 120;
    snapshot.entities.push_back(player(7, true, 5));
    snapshot.entities.push_back(player(8, true, KERNEL_HELD_WEAPON_NONE));
    snapshot.entities.push_back(player(9, false, KERNEL_HELD_WEAPON_NONE));
    const std::vector<std::uint8_t> packet = ne::encode_snapshot_packet(snapshot, 1);
    ne::WorldSnapshot decoded;
    require(ne::decode_snapshot_packet(packet.data(), packet.size(), &decoded));
    require(decoded.entities.size() == 3u);
    const auto find = [&](ne::NetId net_id) -> const ne::EntitySnapshot& {
        for (const ne::EntitySnapshot& entity : decoded.entities) {
            if (entity.net_id == net_id) return entity;
        }
        require(false);
        return decoded.entities.front();
    };
    require(find(7).has_held_weapon);
    require(find(7).held_weapon_id == 5u);
    require(find(8).has_held_weapon);
    require(find(8).held_weapon_id == KERNEL_HELD_WEAPON_NONE);
    require(!find(9).has_held_weapon);

    // One byte, and only when it is said.
    require(ne::estimate_snapshot_entity_size(player(7, true, 5)) ==
            ne::estimate_snapshot_entity_size(player(9, false, 0)) + 1u);

    // Agents never carry it, whatever the field holds.
    ne::EntitySnapshot agent = player(10, true, 3);
    agent.actor_type = ne::ActorType::kAgent;
    ne::WorldSnapshot agents;
    agents.header.server_tick = 120;
    agents.entities.push_back(agent);
    const std::vector<std::uint8_t> agent_packet = ne::encode_snapshot_packet(agents, 1);
    ne::WorldSnapshot agent_decoded;
    require(ne::decode_snapshot_packet(agent_packet.data(), agent_packet.size(), &agent_decoded));
    require(agent_decoded.entities.size() == 1u);
    require(!agent_decoded.entities[0].has_held_weapon);

    // The container kind rides the inventory page.
    for (const std::uint8_t kind : {std::uint8_t{KernelInventoryContainerKind_Items},
                                    std::uint8_t{KernelInventoryContainerKind_Weapons}}) {
        ne::InventorySnapshotPagePacket page;
        page.inventory_container_id = 4;
        page.owner_entity_id = 7;
        page.revision = 3;
        page.slot_capacity = 4;
        page.page_index = 0;
        page.page_count = 1;
        page.container_kind = kind;
        const std::vector<std::uint8_t> encoded =
            ne::encode_inventory_snapshot_page_packet(page, 1);
        require(!encoded.empty());
        ne::InventorySnapshotPagePacket back;
        require(ne::decode_inventory_snapshot_page_packet(encoded.data(), encoded.size(), &back));
        require(back.container_kind == kind);
    }
    std::puts("held_weapon_roundtrip_test passed");
    return 0;
}
