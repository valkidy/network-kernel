// Snapshot schema 23: an own player's impulse lockout says whether it is the
// knockdown recovery, and that survives the wire -- the client's prediction
// roots the player through it only if it arrives. It costs no bytes: it is a
// record flag, and only ever sent with a lockout.

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

network_example::WorldSnapshot thrown(bool recovering) {
    network_example::WorldSnapshot snapshot;
    snapshot.header.server_tick = 120;
    network_example::EntitySnapshot player;
    player.net_id = 7;
    player.type = network_example::EntityType::kActor;
    player.actor_type = network_example::ActorType::kPlayer;
    player.owner_peer = 1;
    player.position = glm::vec3{1.0f, 0.0f, 2.0f};
    player.has_impulse_lockout = true;
    player.impulse_lockout_armed_tick = 100;
    player.impulse_lockout_until_tick = 141;
    player.impulse_lockout_recovering = recovering;
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
    const network_example::WorldSnapshot down = round_trip(thrown(true));
    require(down.entities[0].has_impulse_lockout);
    require(down.entities[0].impulse_lockout_recovering);
    require(down.entities[0].impulse_lockout_until_tick == 141u);

    const network_example::WorldSnapshot flying = round_trip(thrown(false));
    require(flying.entities[0].has_impulse_lockout);
    require(!flying.entities[0].impulse_lockout_recovering);

    require(network_example::estimate_snapshot_entity_size(thrown(true).entities[0]) ==
            network_example::estimate_snapshot_entity_size(thrown(false).entities[0]));
    require(network_example::encode_snapshot_packet(thrown(true), 1).size() ==
            network_example::encode_snapshot_packet(thrown(false), 1).size());

    // Without a lockout the flag means nothing: the encoder drops it rather
    // than send a bit the decoder refuses.
    network_example::WorldSnapshot lying = thrown(true);
    lying.entities[0].has_impulse_lockout = false;
    const network_example::WorldSnapshot decoded = round_trip(lying);
    require(!decoded.entities[0].has_impulse_lockout);
    require(!decoded.entities[0].impulse_lockout_recovering);

    std::printf("knockdown_lockout_roundtrip_test: PASS\n");
    return 0;
}
