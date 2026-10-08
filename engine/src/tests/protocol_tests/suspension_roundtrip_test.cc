// Snapshot schema 29: an own player's status suspension -- its velocity and
// end tick -- survives the wire, and costs its 16 bytes only while one
// stands. An agent carrying one is written as a full actor record, as one
// carrying a lockout is: the compact agent record has no room for it.

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

network_example::EntitySnapshot actor(network_example::ActorType type, bool suspended) {
    network_example::EntitySnapshot entity;
    entity.net_id = type == network_example::ActorType::kPlayer ? 7u : 9u;
    entity.type = network_example::EntityType::kActor;
    entity.actor_type = type;
    entity.owner_peer = type == network_example::ActorType::kPlayer ? 1u : 0u;
    entity.position = glm::vec3{1.0f, 2.5f, 2.0f};
    entity.has_suspension = suspended;
    entity.suspension_velocity = suspended ? glm::vec3{0.3f, 1.5f, -0.4f} : glm::vec3{0.0f};
    entity.suspension_until_tick = suspended ? 160u : 0u;
    return entity;
}

network_example::WorldSnapshot snapshot_of(const network_example::EntitySnapshot& entity) {
    network_example::WorldSnapshot snapshot;
    snapshot.header.server_tick = 120;
    snapshot.entities.push_back(entity);
    return snapshot;
}

network_example::EntitySnapshot round_trip(const network_example::EntitySnapshot& entity) {
    const std::vector<std::uint8_t> packet =
        network_example::encode_snapshot_packet(snapshot_of(entity), 1);
    network_example::WorldSnapshot decoded;
    require(network_example::decode_snapshot_packet(packet.data(), packet.size(), &decoded));
    require(decoded.entities.size() == 1u);
    return decoded.entities[0];
}

}  // namespace

int main() {
    for (const auto type :
         {network_example::ActorType::kPlayer, network_example::ActorType::kAgent}) {
        const network_example::EntitySnapshot held = round_trip(actor(type, true));
        require(held.has_suspension);
        require(held.suspension_velocity == glm::vec3(0.3f, 1.5f, -0.4f));
        require(held.suspension_until_tick == 160u);
        require(held.actor_type == type);

        const network_example::EntitySnapshot free = round_trip(actor(type, false));
        require(!free.has_suspension);
    }
    // Sixteen bytes while it stands, none otherwise, and the size estimate the
    // send budget uses agrees with the encoder.
    const auto player_held = actor(network_example::ActorType::kPlayer, true);
    const auto player_free = actor(network_example::ActorType::kPlayer, false);
    require(network_example::encode_snapshot_packet(snapshot_of(player_held), 1).size() ==
            network_example::encode_snapshot_packet(snapshot_of(player_free), 1).size() + 16u);
    require(network_example::estimate_snapshot_entity_size(player_held) ==
            network_example::estimate_snapshot_entity_size(player_free) + 16u);

    std::printf("suspension_roundtrip_test: PASS\n");
    return 0;
}
