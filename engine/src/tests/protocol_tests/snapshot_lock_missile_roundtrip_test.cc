// Snapshot schema 30: lock-on missiles ride the snapshot, in a section written
// only when there is one -- like the beam section -- at 12 bytes a missile:
// shooter, target, commit, how many ticks before the snapshot it was launched,
// and its flight. A snapshot with no missile costs not a byte more than one
// from before the section existed. docs/LOCK_ON_WEAPONS_PLAN.md, the traffic
// review after P4.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "protocol/public/network_packets.h"
#include "protocol/public/packet_header.h"
#include "sync/public/snapshot.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}
#define require(condition) require_impl((condition), #condition, __LINE__)

namespace ne = network_example;

ne::SnapshotLockMissile missile(
    std::uint32_t shooter,
    std::uint32_t target,
    std::uint16_t commit_index,
    std::uint8_t launch_tick_delta,
    std::uint8_t flight_ticks) {
    return ne::SnapshotLockMissile{
        shooter, target, commit_index, launch_tick_delta, flight_ticks};
}

bool same(const ne::SnapshotLockMissile& lhs, const ne::SnapshotLockMissile& rhs) {
    return lhs.shooter_net_id == rhs.shooter_net_id &&
           lhs.target_net_id == rhs.target_net_id &&
           lhs.commit_index == rhs.commit_index &&
           lhs.launch_tick_delta == rhs.launch_tick_delta &&
           lhs.flight_ticks == rhs.flight_ticks;
}

ne::WorldSnapshot snapshot_with_an_actor() {
    ne::WorldSnapshot snapshot;
    snapshot.header = ne::SnapshotHeader{900, 30000, 12};
    ne::EntitySnapshot actor;
    actor.net_id = 5;
    actor.type = ne::EntityType::kActor;
    actor.actor_type = ne::ActorType::kPlayer;
    snapshot.entities.push_back(actor);
    return snapshot;
}

void missiles_survive_the_wire_beside_the_entities() {
    ne::WorldSnapshot snapshot = snapshot_with_an_actor();
    const std::size_t without = ne::encode_snapshot_packet(snapshot).size();
    snapshot.lock_missiles = {
        missile(5, 40, 0, 1, 12),
        // The widths' edges: the last commit, a launch 255 ticks back, the
        // longest flight.
        missile(5, 41, UINT16_MAX, UINT8_MAX, UINT8_MAX),
    };
    const std::vector<std::uint8_t> encoded = ne::encode_snapshot_packet(snapshot);
    // A section header, then 12 bytes a missile.
    require(encoded.size() == without + 4u + 2u * 12u);
    require(ne::estimate_snapshot_packet_size(snapshot) >= encoded.size());

    ne::WorldSnapshot decoded;
    require(ne::decode_snapshot_packet(encoded.data(), encoded.size(), &decoded));
    require(decoded.header.server_tick == 900u);
    require(decoded.entities.size() == 1u);
    require(decoded.entities[0].net_id == 5u);
    require(decoded.lock_missiles.size() == 2u);
    require(same(decoded.lock_missiles[0], snapshot.lock_missiles[0]));
    require(same(decoded.lock_missiles[1], snapshot.lock_missiles[1]));
}

// A snapshot that has a beam and missiles carries both optional sections.
void a_beam_and_missiles_ride_together() {
    ne::WorldSnapshot snapshot = snapshot_with_an_actor();
    ne::EntitySnapshot beam;
    beam.net_id = 77;
    beam.type = ne::EntityType::kProjectile;
    beam.state_flags |= ne::kSnapshotStateFlagProjectileBeam;
    beam.beam_effective_length = 13.75f;
    snapshot.entities.push_back(beam);
    snapshot.lock_missiles = {missile(5, 40, 3, 0, 9)};
    const std::vector<std::uint8_t> encoded = ne::encode_snapshot_packet(snapshot);
    ne::WorldSnapshot decoded;
    require(ne::decode_snapshot_packet(encoded.data(), encoded.size(), &decoded));
    require(decoded.entities.size() == 2u);
    require(decoded.lock_missiles.size() == 1u);
    require(same(decoded.lock_missiles[0], snapshot.lock_missiles[0]));
}

void no_missile_means_no_section() {
    const ne::WorldSnapshot snapshot = snapshot_with_an_actor();
    const std::vector<std::uint8_t> encoded = ne::encode_snapshot_packet(snapshot);
    ne::WorldSnapshot decoded;
    require(ne::decode_snapshot_packet(encoded.data(), encoded.size(), &decoded));
    require(decoded.lock_missiles.empty());
}

// A missile naming no shooter, no target or no flight, and a snapshot cut
// short inside the section, do not decode.
void a_broken_missile_section_is_refused() {
    for (const ne::SnapshotLockMissile& bad : {
             missile(0, 40, 0, 0, 10),
             missile(5, 0, 0, 0, 10),
             missile(5, 40, 0, 0, 0),
         }) {
        ne::WorldSnapshot snapshot = snapshot_with_an_actor();
        snapshot.lock_missiles = {bad};
        const std::vector<std::uint8_t> encoded = ne::encode_snapshot_packet(snapshot);
        ne::WorldSnapshot decoded;
        require(!ne::decode_snapshot_packet(encoded.data(), encoded.size(), &decoded));
    }
    ne::WorldSnapshot snapshot = snapshot_with_an_actor();
    snapshot.lock_missiles = {missile(5, 40, 0, 0, 10)};
    const std::vector<std::uint8_t> encoded = ne::encode_snapshot_packet(snapshot);
    ne::WorldSnapshot decoded;
    require(!ne::decode_snapshot_packet(encoded.data(), encoded.size() - 1u, &decoded));
}

}  // namespace

int main() {
    require(ne::kSnapshotSchemaVersion == 30u);
    require(ne::kPacketSchemaVersion == 32u);
    missiles_survive_the_wire_beside_the_entities();
    a_beam_and_missiles_ride_together();
    no_missile_means_no_section();
    a_broken_missile_section_is_refused();
    std::puts("snapshot_lock_missile_roundtrip_test passed");
    return 0;
}
