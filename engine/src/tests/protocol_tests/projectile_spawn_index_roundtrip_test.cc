// Packet schema 31: a projectile spawn record carries which commit of its
// action fired it and which of that commit's pellets it is, after the action
// instance id, in four bytes (two of them the commit, one the pellet, one
// reserved). docs/HELD_FIRE_PROJECTILE_PREDICTION_PLAN.md, P1.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

#include "protocol/public/network_packets.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}
#define require(condition) require_impl((condition), #condition, __LINE__)

network_example::ProjectileSpawnRecord record(
    std::uint32_t net_id,
    std::uint16_t commit_index,
    std::uint8_t burst_index) {
    network_example::ProjectileSpawnRecord spawn{
        net_id,
        11,
        7,
        1234,
        glm::vec3{1.0f, 2.0f, 3.0f},
        glm::vec3{4.0f, 5.0f, 6.0f},
    };
    spawn.commit_index = commit_index;
    spawn.burst_index = burst_index;
    return spawn;
}

std::vector<std::uint8_t> encode(const std::vector<network_example::ProjectileSpawnRecord>& records) {
    network_example::ProjectileSpawnBatchPacket batch{};
    batch.server_tick = 77;
    batch.server_time_us = 77000;
    batch.catalog_hash = 0x8877665544332211ull;
    network_example::ProjectileSpawnGroup group{};
    group.projectile_template_id = 3;
    group.records = records;
    batch.groups.push_back(group);
    return network_example::encode_projectile_spawn_batch_packet(batch, 47);
}

}  // namespace

int main() {
    // The largest of each, so a narrowed field shows.
    const std::vector<network_example::ProjectileSpawnRecord> sent{
        record(101, 0u, 0u),
        record(102, 65535u, 2u),
        record(103, 7u, 255u),
    };
    const std::vector<std::uint8_t> packet = encode(sent);
    network_example::ProjectileSpawnBatchPacket decoded{};
    require(network_example::decode_projectile_spawn_batch_packet(
        packet.data(), packet.size(), &decoded));
    require(decoded.groups.size() == 1u);
    const std::vector<network_example::ProjectileSpawnRecord>& got = decoded.groups[0].records;
    require(got.size() == sent.size());
    for (std::size_t index = 0; index < sent.size(); ++index) {
        require(got[index].projectile_net_id == sent[index].projectile_net_id);
        require(got[index].action_instance_id == 1234u);
        require(got[index].commit_index == sent[index].commit_index);
        require(got[index].burst_index == sent[index].burst_index);
        // The fields after the new ones still land where they should.
        require(got[index].spawn_position == glm::vec3(1.0f, 2.0f, 3.0f));
        require(got[index].initial_velocity == glm::vec3(4.0f, 5.0f, 6.0f));
    }

    // Each record costs 44 bytes: one more record is exactly 44 more.
    const std::size_t two = encode({sent[0], sent[1]}).size();
    const std::size_t three = packet.size();
    require(three - two == 44u);

    std::printf("projectile_spawn_index_roundtrip_test: PASS\n");
    return 0;
}
