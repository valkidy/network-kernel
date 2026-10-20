// Snapshot schema 32: an own player's status hover -- its clearance, vertical
// speed and end tick -- survives the wire, and costs its 12 bytes only while
// it stands. Numbers no authority could have sent are refused. An agent
// carrying one is written as a full actor record, as one carrying a
// suspension is (docs/PLAYER_HOVER_PLAN.md P3).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
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

ne::EntitySnapshot actor(ne::ActorType type, bool hovering) {
    ne::EntitySnapshot entity;
    entity.net_id = type == ne::ActorType::kPlayer ? 7u : 9u;
    entity.type = ne::EntityType::kActor;
    entity.actor_type = type;
    entity.owner_peer = type == ne::ActorType::kPlayer ? 1u : 0u;
    entity.position = glm::vec3{1.0f, 3.0f, 2.0f};
    entity.has_hover = hovering;
    entity.hover_height_meters = hovering ? 3.0f : 0.0f;
    entity.hover_vertical_speed = hovering ? 4.0f : 0.0f;
    entity.hover_until_tick = hovering ? 1020u : 0u;
    return entity;
}

ne::WorldSnapshot snapshot_of(const ne::EntitySnapshot& entity) {
    ne::WorldSnapshot snapshot;
    snapshot.header.server_tick = 120;
    snapshot.entities.push_back(entity);
    return snapshot;
}

bool decodes(const ne::EntitySnapshot& entity, ne::EntitySnapshot* out) {
    const std::vector<std::uint8_t> packet =
        ne::encode_snapshot_packet(snapshot_of(entity), 1);
    ne::WorldSnapshot decoded;
    if (!ne::decode_snapshot_packet(packet.data(), packet.size(), &decoded) ||
        decoded.entities.size() != 1u) {
        return false;
    }
    *out = decoded.entities[0];
    return true;
}

}  // namespace

int main() {
    for (const auto type : {ne::ActorType::kPlayer, ne::ActorType::kAgent}) {
        ne::EntitySnapshot held;
        require(decodes(actor(type, true), &held));
        require(held.has_hover);
        require(held.hover_height_meters == 3.0f);
        require(held.hover_vertical_speed == 4.0f);
        require(held.hover_until_tick == 1020u);
        require(held.actor_type == type);

        ne::EntitySnapshot free;
        require(decodes(actor(type, false), &free));
        require(!free.has_hover);
    }
    // Together with a suspension -- a hovering player in a bubble -- both
    // blocks come through.
    ne::EntitySnapshot both = actor(ne::ActorType::kPlayer, true);
    both.has_suspension = true;
    both.suspension_velocity = glm::vec3{0.0f, 1.0f, 0.0f};
    both.suspension_until_tick = 200u;
    ne::EntitySnapshot decoded_both;
    require(decodes(both, &decoded_both));
    require(decoded_both.has_hover && decoded_both.has_suspension);
    require(decoded_both.hover_until_tick == 1020u);
    require(decoded_both.suspension_until_tick == 200u);

    // Twelve bytes while it stands, none otherwise, and the send budget's
    // estimate agrees with the encoder.
    const auto player_held = actor(ne::ActorType::kPlayer, true);
    const auto player_free = actor(ne::ActorType::kPlayer, false);
    require(ne::encode_snapshot_packet(snapshot_of(player_held), 1).size() ==
            ne::encode_snapshot_packet(snapshot_of(player_free), 1).size() + 12u);
    require(ne::estimate_snapshot_entity_size(player_held) ==
            ne::estimate_snapshot_entity_size(player_free) + 12u);

    // What the authority's own checks would never have let out.
    for (const auto& [height, speed] : std::vector<std::pair<float, float>>{
             {0.0f, 4.0f}, {25.0f, 4.0f}, {3.0f, 0.0f}, {3.0f, 25.0f},
             {std::nanf(""), 4.0f}, {3.0f, -1.0f}}) {
        ne::EntitySnapshot bad = player_held;
        bad.hover_height_meters = height;
        bad.hover_vertical_speed = speed;
        ne::EntitySnapshot ignored;
        require(!decodes(bad, &ignored));
    }

    std::printf("hover_roundtrip_test: PASS\n");
    return 0;
}
