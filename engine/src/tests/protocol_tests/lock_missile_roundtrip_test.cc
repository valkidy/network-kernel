// Packet schema 32: a lock-on missile batch. A record names the shooter's
// action instance, commit and burst, the target, how many ticks before the
// batch it was launched, and its flight -- 17 bytes a missile. A record that
// names no shooter, no target or no flight is refused both ways.
// docs/LOCK_ON_WEAPONS_PLAN.md, P3.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "protocol/public/network_packets.h"
#include "protocol/public/packet_header.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}
#define require(condition) require_impl((condition), #condition, __LINE__)

namespace ne = network_example;

ne::LockMissileRecord missile(
    std::uint32_t shooter,
    std::uint16_t commit_index,
    std::uint32_t target,
    std::uint8_t launch_tick_delta,
    std::uint8_t flight_ticks) {
    ne::LockMissileRecord record;
    record.shooter_net_id = shooter;
    record.action_instance_id = 0xabcd1234u;
    record.commit_index = commit_index;
    record.burst_index = 2;
    record.target_net_id = target;
    record.launch_tick_delta = launch_tick_delta;
    record.flight_ticks = flight_ticks;
    return record;
}

bool same(const ne::LockMissileRecord& lhs, const ne::LockMissileRecord& rhs) {
    return lhs.shooter_net_id == rhs.shooter_net_id &&
           lhs.action_instance_id == rhs.action_instance_id &&
           lhs.commit_index == rhs.commit_index &&
           lhs.burst_index == rhs.burst_index &&
           lhs.target_net_id == rhs.target_net_id &&
           lhs.launch_tick_delta == rhs.launch_tick_delta &&
           lhs.flight_ticks == rhs.flight_ticks;
}

void a_batch_survives_the_wire() {
    ne::LockMissileBatchPacket packet;
    packet.server_tick = 9001;
    packet.records = {
        missile(7, 0, 40, 1, 12),
        // The widths' edges: the last commit index, a launch 255 ticks back,
        // the longest flight.
        missile(8, UINT16_MAX, 41, UINT8_MAX, UINT8_MAX),
    };
    const std::vector<std::uint8_t> one =
        ne::encode_lock_missile_batch_packet({9001, {packet.records[0]}});
    const std::vector<std::uint8_t> encoded =
        ne::encode_lock_missile_batch_packet(packet);
    require(!encoded.empty());
    // Every further missile costs exactly its record.
    require(encoded.size() - one.size() == 17u);

    ne::LockMissileBatchPacket decoded;
    require(ne::decode_lock_missile_batch_packet(
        encoded.data(), encoded.size(), &decoded));
    require(decoded.server_tick == 9001u);
    require(decoded.records.size() == 2u);
    require(same(decoded.records[0], packet.records[0]));
    require(same(decoded.records[1], packet.records[1]));
}

void a_record_without_a_shooter_a_target_or_a_flight_is_refused() {
    for (const ne::LockMissileRecord& bad : {
             missile(0, 0, 40, 0, 10),
             missile(7, 0, 0, 0, 10),
             missile(7, 0, 40, 0, 0),
         }) {
        require(ne::encode_lock_missile_batch_packet({1, {bad}}).empty());
    }
    require(ne::encode_lock_missile_batch_packet({1, {}}).empty());
}

// A truncated packet, and a packet of another type, do not decode as one.
void only_a_whole_missile_batch_decodes() {
    const std::vector<std::uint8_t> encoded =
        ne::encode_lock_missile_batch_packet({5, {missile(7, 3, 40, 0, 9)}});
    ne::LockMissileBatchPacket decoded;
    require(!ne::decode_lock_missile_batch_packet(
        encoded.data(), encoded.size() - 1u, &decoded));
    ne::ActorImpulseBatchPacket impulse;
    impulse.server_tick = 5;
    ne::ActorImpulseRecord record;
    record.net_id = 7;
    record.lockout_ticks = 3;
    impulse.records.push_back(record);
    const std::vector<std::uint8_t> other =
        ne::encode_actor_impulse_batch_packet(impulse);
    require(!other.empty());
    require(!ne::decode_lock_missile_batch_packet(
        other.data(), other.size(), &decoded));
}

}  // namespace

int main() {
    require(ne::kPacketSchemaVersion == 32u);
    a_batch_survives_the_wire();
    a_record_without_a_shooter_a_target_or_a_flight_is_refused();
    only_a_whole_missile_batch_decodes();
    std::puts("lock_missile_roundtrip_test passed");
    return 0;
}
