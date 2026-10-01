// Snapshot schema 26: an own player's shelter -- the building it is inside --
// survives the wire, because the client's prediction stands still in the
// building only if it arrives. Four bytes, and only while the player is
// inside: outside, the record is exactly what it was.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

#include "protocol/public/network_packets.h"
#include "sync/public/snapshot.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}
#define require(condition) require_impl((condition), #condition, __LINE__)

network_example::WorldSnapshot player_in(network_example::NetId shelter) {
    network_example::WorldSnapshot snapshot;
    snapshot.header.server_tick = 120;
    network_example::EntitySnapshot player;
    player.net_id = 7;
    player.type = network_example::EntityType::kActor;
    player.actor_type = network_example::ActorType::kPlayer;
    player.owner_peer = 1;
    player.position = glm::vec3{1.0f, 0.0f, 2.0f};
    player.shelter_net_id = shelter;
    snapshot.entities.push_back(player);
    return snapshot;
}

network_example::WorldSnapshot round_trip(const network_example::WorldSnapshot& snapshot) {
    const std::vector<std::uint8_t> packet =
        network_example::encode_snapshot_packet(snapshot, 1);
    network_example::WorldSnapshot decoded;
    require(network_example::decode_snapshot_packet(packet.data(), packet.size(), &decoded));
    require(decoded.entities.size() == 1u);
    return decoded;
}

}  // namespace

int main() {
    require(round_trip(player_in(42)).entities[0].shelter_net_id == 42u);
    require(round_trip(player_in(0)).entities[0].shelter_net_id == 0u);

    // Four bytes inside, none outside, and the send budget agrees.
    const std::size_t inside =
        network_example::encode_snapshot_packet(player_in(42), 1).size();
    const std::size_t outside =
        network_example::encode_snapshot_packet(player_in(0), 1).size();
    require(inside == outside + 4u);
    require(network_example::estimate_snapshot_entity_size(player_in(42).entities[0]) ==
            network_example::estimate_snapshot_entity_size(player_in(0).entities[0]) + 4u);

    // An agent carrying one is never squeezed into the compact agent record,
    // which has nowhere to put it.
    network_example::WorldSnapshot agent = player_in(42);
    agent.entities[0].actor_type = network_example::ActorType::kAgent;
    require(round_trip(agent).entities[0].shelter_net_id == 42u);

    std::printf("shelter_roundtrip_test: PASS\n");
    return 0;
}
