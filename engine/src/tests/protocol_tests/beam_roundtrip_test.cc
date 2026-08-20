// A snapshot carrying a beam must survive the wire. The beam section is written
// only when a beam exists, so a snapshot has four sections or five; a decoder
// that insists on four rejects the whole packet the moment anything fires,
// dropping every entity in it rather than just the beam.
//
// The section itself carries only the beam's reach. Origin and aim are the
// shooter's and are rebuilt by the kernel, so the decoder is expected to leave
// position and rotation alone -- what it must not do is lose the reach or the
// flag that says this record is a beam at all.
#include <cassert>
#include <cmath>
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
    beam.state_flags |= ne::kSnapshotStateFlagProjectileBeam;
    beam.beam_effective_length = 13.75f;
    snapshot.entities.push_back(beam);

    std::vector<std::uint8_t> bytes = ne::encode_snapshot_packet(snapshot);
    std::printf("encoded %zu bytes\n", bytes.size());

    ne::WorldSnapshot decoded;
    const bool ok = ne::decode_snapshot_packet(bytes.data(), bytes.size(), &decoded);
    std::printf("decode ok=%d entities=%zu\n", ok ? 1 : 0, decoded.entities.size());
    assert(ok);
    assert(decoded.entities.size() == 1);
    const ne::EntitySnapshot& out = decoded.entities[0];
    std::printf("net_id=%u beam_effective_length=%.4f beam_flag=%d\n",
                out.net_id,
                out.beam_effective_length,
                (out.state_flags & ne::kSnapshotStateFlagProjectileBeam) != 0 ? 1 : 0);
    assert(out.net_id == 77);
    assert((out.state_flags & ne::kSnapshotStateFlagProjectileBeam) != 0);
    // Centimetre quantisation: 13.75 m survives exactly, and nothing may drift
    // by more than half a centimetre.
    assert(std::fabs(out.beam_effective_length - 13.75f) < 0.005f);

    // The whole point of the 6-byte record. net_id 4 + reach 2, and nothing
    // else: no position, rotation, velocity, state or flags.
    ne::WorldSnapshot empty;
    empty.header = snapshot.header;
    const std::vector<std::uint8_t> empty_bytes = ne::encode_snapshot_packet(empty);
    // One beam costs its own record plus the optional section header the beam
    // is what brings into existence.
    const std::size_t beam_cost = bytes.size() - empty_bytes.size();
    std::printf("beam costs %zu bytes over an empty snapshot\n", beam_cost);
    assert(beam_cost == 6 + 4);

    // A reach past what the u16 can hold clamps instead of wrapping to nothing.
    ne::WorldSnapshot huge;
    huge.header = snapshot.header;
    ne::EntitySnapshot long_beam = beam;
    long_beam.beam_effective_length = 900.0f;
    huge.entities.push_back(long_beam);
    const std::vector<std::uint8_t> huge_bytes = ne::encode_snapshot_packet(huge);
    ne::WorldSnapshot huge_decoded;
    assert(ne::decode_snapshot_packet(
        huge_bytes.data(), huge_bytes.size(), &huge_decoded));
    assert(huge_decoded.entities.size() == 1);
    std::printf("clamped reach=%.2f\n", huge_decoded.entities[0].beam_effective_length);
    assert(huge_decoded.entities[0].beam_effective_length > 655.0f);

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
    assert(plain_decoded.entities[0].beam_effective_length == 0.0f);
    assert((plain_decoded.entities[0].state_flags &
            ne::kSnapshotStateFlagProjectileBeam) == 0);
    std::printf("OK\n");
    return 0;
}
