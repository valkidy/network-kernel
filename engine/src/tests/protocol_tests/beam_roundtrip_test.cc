// A snapshot carrying a beam must survive the wire. The beam section is written
// only when a beam exists, so a snapshot has four sections or five; a decoder
// that insists on four rejects the whole packet the moment anything fires,
// dropping every entity in it rather than just the beam.
#include <cassert>
#include <cstdio>
#include <vector>
#include "protocol/public/network_packets.h"
#include "sync/public/snapshot.h"

namespace ne = network_example;

int main() {
    ne::WorldSnapshot snapshot;
    snapshot.header = ne::SnapshotHeader{42, 1000, 7};

    ne::EntitySnapshot beam;
    beam.net_id = 77;
    beam.type = ne::EntityType::kProjectile;
    beam.position = glm::vec3{1.0f, 2.0f, 3.0f};
    beam.state_flags |= ne::kSnapshotStateFlagProjectileBeam;
    beam.beam_end = glm::vec3{15.0f, 2.0f, 3.0f};
    snapshot.entities.push_back(beam);

    std::vector<std::uint8_t> bytes = ne::encode_snapshot_packet(snapshot);
    std::printf("encoded %zu bytes\n", bytes.size());

    ne::WorldSnapshot decoded;
    const bool ok = ne::decode_snapshot_packet(bytes.data(), bytes.size(), &decoded);
    std::printf("decode ok=%d entities=%zu\n", ok ? 1 : 0, decoded.entities.size());
    assert(ok);
    assert(decoded.entities.size() == 1);
    const ne::EntitySnapshot& out = decoded.entities[0];
    std::printf("net_id=%u position=(%.2f,%.2f,%.2f) beam_end=(%.2f,%.2f,%.2f) beam_flag=%d\n",
                out.net_id,
                out.position.x, out.position.y, out.position.z,
                out.beam_end.x, out.beam_end.y, out.beam_end.z,
                (out.state_flags & ne::kSnapshotStateFlagProjectileBeam) != 0 ? 1 : 0);
    assert(out.beam_end.x == 15.0f);
    assert(out.beam_end.y == 2.0f);
    assert(out.beam_end.z == 3.0f);
    assert((out.state_flags & ne::kSnapshotStateFlagProjectileBeam) != 0);

    // A snapshot with no beam still carries exactly the four standard sections.
    ne::WorldSnapshot plain;
    plain.header = snapshot.header;
    ne::EntitySnapshot rocket;
    rocket.net_id = 78;
    rocket.type = ne::EntityType::kProjectile;
    rocket.position = glm::vec3{4.0f, 5.0f, 6.0f};
    plain.entities.push_back(rocket);
    const std::vector<std::uint8_t> plain_bytes = ne::encode_snapshot_packet(plain);
    ne::WorldSnapshot plain_decoded;
    assert(ne::decode_snapshot_packet(
        plain_bytes.data(), plain_bytes.size(), &plain_decoded));
    assert(plain_decoded.entities.size() == 1);
    // Braces would read as extra macro arguments here, so compare components.
    assert(plain_decoded.entities[0].beam_end.x == 0.0f);
    assert(plain_decoded.entities[0].beam_end.y == 0.0f);
    assert(plain_decoded.entities[0].beam_end.z == 0.0f);
    std::printf("OK\n");
    return 0;
}
