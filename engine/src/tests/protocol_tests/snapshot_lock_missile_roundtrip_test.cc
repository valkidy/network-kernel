// Snapshot schema 30: lock-on missiles ride the snapshot, in a section written
// only when there is one -- like the beam section -- at 13 bytes a missile:
// shooter, target, commit, how many ticks before the snapshot it was launched,
// its flight and its flags. A snapshot with no missile costs not a byte more
// than one from before the section existed. docs/LOCK_ON_WEAPONS_PLAN.md, the
// traffic review after P4.
//
// Schema 31 (docs/LOCK_STRIKE_ACTION_PLAN.md P4): an action graph's missile
// carries the origin it leaves from and the shot it is (29 bytes) and may
// name no shooter; an action graph's instant strike rides a section of its
// own, naming its shot too.

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
           lhs.flight_ticks == rhs.flight_ticks &&
           lhs.flags == rhs.flags && lhs.origin == rhs.origin &&
           lhs.projectile_template_id == rhs.projectile_template_id;
}

ne::SnapshotLockMissile from_origin(
    std::uint32_t shooter,
    std::uint32_t target,
    std::uint16_t commit_index) {
    ne::SnapshotLockMissile record =
        missile(shooter, target, commit_index, 2, 9);
    record.flags = ne::kSnapshotLockRecordHasOrigin;
    record.origin = glm::vec3{3.5f, 1.0f, -7.25f};
    record.projectile_template_id = 36;
    return record;
}

ne::SnapshotLockStrike strike(std::vector<std::uint32_t> targets) {
    ne::SnapshotLockStrike record;
    record.origin = glm::vec3{-2.0f, 1.0f, 4.5f};
    record.direction = glm::normalize(glm::vec3{0.3f, 0.0f, 1.0f});
    record.collider_template_id = 54;
    record.projectile_template_id = 35;
    record.instigator_net_id = 0;
    record.strike_index = 3;
    record.launch_tick_delta = 1;
    record.arc_count = 4;
    record.target_net_ids = std::move(targets);
    return record;
}

bool same(const ne::SnapshotLockStrike& lhs, const ne::SnapshotLockStrike& rhs) {
    // The direction travels at 16 bits an axis.
    return lhs.origin == rhs.origin &&
           glm::length(lhs.direction - rhs.direction) < 0.0002f &&
           lhs.collider_template_id == rhs.collider_template_id &&
           lhs.projectile_template_id == rhs.projectile_template_id &&
           lhs.instigator_net_id == rhs.instigator_net_id &&
           lhs.strike_index == rhs.strike_index &&
           lhs.launch_tick_delta == rhs.launch_tick_delta &&
           lhs.arc_count == rhs.arc_count &&
           lhs.target_net_ids == rhs.target_net_ids;
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
    // A section header, then 13 bytes a missile.
    require(encoded.size() == without + 4u + 2u * 13u);
    require(ne::snapshot_lock_record_bytes(snapshot) == 4u + 2u * 13u);
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
// Schema 31: a graph's missile names where it leaves from, and needs no
// shooter for it; a graph's strike rides its own section, its unstruck arcs
// left to the client. All the optional sections together, and the size
// helper agreeing with the encoder to the byte.
void a_graph_missile_and_strike_survive_the_wire() {
    ne::WorldSnapshot snapshot = snapshot_with_an_actor();
    const std::size_t without = ne::encode_snapshot_packet(snapshot).size();
    snapshot.lock_missiles = {missile(5, 40, 0, 1, 12), from_origin(0, 41, 2)};
    snapshot.lock_strikes = {strike({40, 41}), strike({})};
    snapshot.lock_strikes[1].direction = glm::vec3{0.0f};
    snapshot.lock_strikes[1].strike_index = 4;
    const std::vector<std::uint8_t> encoded = ne::encode_snapshot_packet(snapshot);
    const std::size_t records = (4u + 13u + 29u) + (4u + (35u + 8u) + 35u);
    require(ne::snapshot_lock_record_bytes(snapshot) == records);
    require(encoded.size() == without + records);
    require(ne::estimate_snapshot_packet_size(snapshot) >= encoded.size());

    ne::WorldSnapshot decoded;
    require(ne::decode_snapshot_packet(encoded.data(), encoded.size(), &decoded));
    require(decoded.entities.size() == 1u);
    require(decoded.lock_missiles.size() == 2u);
    require(same(decoded.lock_missiles[0], snapshot.lock_missiles[0]));
    require(same(decoded.lock_missiles[1], snapshot.lock_missiles[1]));
    require(decoded.lock_strikes.size() == 2u);
    require(same(decoded.lock_strikes[0], snapshot.lock_strikes[0]));
    require(same(decoded.lock_strikes[1], snapshot.lock_strikes[1]));

    // With a beam as well: every optional section at once is still accepted.
    ne::EntitySnapshot beam;
    beam.net_id = 77;
    beam.type = ne::EntityType::kProjectile;
    beam.state_flags |= ne::kSnapshotStateFlagProjectileBeam;
    beam.beam_effective_length = 13.75f;
    snapshot.entities.push_back(beam);
    const std::vector<std::uint8_t> with_beam = ne::encode_snapshot_packet(snapshot);
    require(ne::decode_snapshot_packet(with_beam.data(), with_beam.size(), &decoded));
    require(decoded.lock_strikes.size() == 2u);
}

void a_broken_strike_section_is_refused() {
    using Break = void (*)(ne::SnapshotLockStrike*);
    for (const Break breaks : {
             +[](ne::SnapshotLockStrike* record) { record->arc_count = 0; },
             +[](ne::SnapshotLockStrike* record) { record->arc_count = 17; },
             +[](ne::SnapshotLockStrike* record) { record->arc_count = 1; },
             +[](ne::SnapshotLockStrike* record) { record->collider_template_id = 0; },
             +[](ne::SnapshotLockStrike* record) { record->projectile_template_id = 0; },
             +[](ne::SnapshotLockStrike* record) { record->target_net_ids[0] = 0; },
         }) {
        ne::WorldSnapshot snapshot = snapshot_with_an_actor();
        snapshot.lock_strikes = {strike({40, 41})};
        breaks(&snapshot.lock_strikes[0]);
        const std::vector<std::uint8_t> encoded = ne::encode_snapshot_packet(snapshot);
        ne::WorldSnapshot decoded;
        require(!ne::decode_snapshot_packet(encoded.data(), encoded.size(), &decoded));
    }
    ne::WorldSnapshot snapshot = snapshot_with_an_actor();
    snapshot.lock_strikes = {strike({40})};
    const std::vector<std::uint8_t> encoded = ne::encode_snapshot_packet(snapshot);
    ne::WorldSnapshot decoded;
    require(!ne::decode_snapshot_packet(encoded.data(), encoded.size() - 1u, &decoded));
}

void a_broken_missile_section_is_refused() {
    ne::SnapshotLockMissile unknown_flag = missile(5, 40, 0, 0, 10);
    unknown_flag.flags = 0x80u;
    // A graph's missile must say what it is.
    ne::SnapshotLockMissile no_shot = from_origin(0, 40, 0);
    no_shot.projectile_template_id = 0;
    for (const ne::SnapshotLockMissile& bad : {
             // No shooter and no origin: nothing to draw it from.
             missile(0, 40, 0, 0, 10),
             missile(5, 0, 0, 0, 10),
             missile(5, 40, 0, 0, 0),
             unknown_flag,
             no_shot,
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
    require(ne::kSnapshotSchemaVersion == 32u);
    require(ne::kPacketSchemaVersion == 33u);
    missiles_survive_the_wire_beside_the_entities();
    a_beam_and_missiles_ride_together();
    no_missile_means_no_section();
    a_broken_missile_section_is_refused();
    a_graph_missile_and_strike_survive_the_wire();
    a_broken_strike_section_is_refused();
    std::puts("snapshot_lock_missile_roundtrip_test passed");
    return 0;
}
