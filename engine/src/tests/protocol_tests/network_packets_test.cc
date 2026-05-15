#include <cassert>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "protocol/public/network_packets.h"

namespace {

bool nearly_equal(float lhs, float rhs) {
    const float diff = lhs > rhs ? lhs - rhs : rhs - lhs;
    return diff < 0.0001f;
}

}  // namespace

int main() {
    PlayerInput input{};
    input.input_seq = 7;
    input.client_action_time_us = 11000;
    input.client_action_id = 1234;
    input.move = KernelVec2{0.5f, -0.25f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.buttons = InputButton_Fire | InputButton_Sprint | InputButton_Dodge |
                    InputButton_Parry;
    input.selected_weapon = 2;

    const std::vector<std::uint8_t> input_packet =
        network_example::encode_input_packet(3, input, 42);
    network_example::PeerId decoded_player = 0;
    PlayerInput decoded_input{};
    assert(network_example::decode_input_packet(
        input_packet.data(),
        input_packet.size(),
        &decoded_player,
        &decoded_input));
    assert(decoded_player == 3);
    assert(decoded_input.input_seq == input.input_seq);
    assert(decoded_input.client_action_time_us == input.client_action_time_us);
    assert(decoded_input.client_action_id == input.client_action_id);
    assert(nearly_equal(decoded_input.move.x, input.move.x));
    assert(nearly_equal(decoded_input.move.y, input.move.y));
    assert(nearly_equal(decoded_input.aim_dir.x, input.aim_dir.x));
    assert(decoded_input.buttons == input.buttons);
    assert(decoded_input.selected_weapon == input.selected_weapon);

    network_example::WorldSnapshot snapshot;
    snapshot.header.server_tick = 9;
    snapshot.header.server_time_ms = 300;
    snapshot.header.last_processed_input_seq = 7;
    network_example::EntitySnapshot entity;
    entity.net_id = 5;
    entity.type = network_example::EntityType::kProjectile;
    entity.owner_peer = 3;
    entity.position = glm::vec3{1.0f, 2.0f, 3.0f};
    entity.rotation = glm::quat{0.5f, 0.5f, 0.5f, 0.5f};
    entity.velocity = glm::vec3{4.0f, 5.0f, 6.0f};
    entity.hp = 88;
    entity.state = 513;
    entity.flags = 0x01020304u;
    entity.spawn_tick = 12;
    entity.client_action_id = 1234;
    snapshot.entities.push_back(entity);

    const std::vector<std::uint8_t> snapshot_packet =
        network_example::encode_snapshot_packet(snapshot, 43);
    network_example::WorldSnapshot decoded_snapshot;
    assert(network_example::decode_snapshot_packet(
        snapshot_packet.data(),
        snapshot_packet.size(),
        &decoded_snapshot));
    assert(decoded_snapshot.header.server_tick == 9);
    assert(decoded_snapshot.header.server_time_ms == 300);
    assert(decoded_snapshot.header.last_processed_input_seq == 7);
    assert(decoded_snapshot.entities.size() == 1);
    assert(decoded_snapshot.entities[0].net_id == 5);
    assert(decoded_snapshot.entities[0].type == network_example::EntityType::kProjectile);
    assert(decoded_snapshot.entities[0].owner_peer == 3);
    assert(nearly_equal(decoded_snapshot.entities[0].position.x, 1.0f));
    assert(nearly_equal(decoded_snapshot.entities[0].rotation.w, 0.5f));
    assert(nearly_equal(decoded_snapshot.entities[0].velocity.z, 6.0f));
    assert(decoded_snapshot.entities[0].hp == 88);
    assert(decoded_snapshot.entities[0].state == 513);
    assert(decoded_snapshot.entities[0].flags == 0x01020304u);
    assert(decoded_snapshot.entities[0].spawn_tick == 12);
    assert(decoded_snapshot.entities[0].client_action_id == 1234);

    KernelEvent reliable_event{};
    reliable_event.type = KernelEventType_PlayerLeft;
    reliable_event.tick = 19;
    reliable_event.net_id = 23;
    reliable_event.peer_id = 4;
    reliable_event.code = 99;
    const std::vector<std::uint8_t> reliable_event_packet =
        network_example::encode_reliable_event_packet(reliable_event, 44);
    KernelEvent decoded_event{};
    assert(network_example::decode_reliable_event_packet(
        reliable_event_packet.data(),
        reliable_event_packet.size(),
        &decoded_event));
    assert(decoded_event.type == KernelEventType_PlayerLeft);
    assert(decoded_event.tick == 19);
    assert(decoded_event.net_id == 23);
    assert(decoded_event.peer_id == 4);
    assert(decoded_event.code == 99);
    assert(!network_example::decode_reliable_event_packet(
        input_packet.data(),
        input_packet.size(),
        &decoded_event));

    network_example::EntitySpawnPacket spawn{};
    spawn.net_id = 41;
    spawn.entity_type = network_example::EntityType::kEnemy;
    spawn.owner_peer = 9;
    spawn.server_tick = 12;
    spawn.position = glm::vec3{3.0f, 4.0f, 5.0f};
    spawn.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    const std::vector<std::uint8_t> spawn_packet =
        network_example::encode_entity_spawn_packet(spawn, 45);
    network_example::EntitySpawnPacket decoded_spawn{};
    assert(network_example::decode_entity_spawn_packet(
        spawn_packet.data(),
        spawn_packet.size(),
        &decoded_spawn));
    assert(decoded_spawn.net_id == 41);
    assert(decoded_spawn.entity_type == network_example::EntityType::kEnemy);
    assert(decoded_spawn.owner_peer == 9);
    assert(decoded_spawn.server_tick == 12);
    assert(nearly_equal(decoded_spawn.position.y, 4.0f));
    assert(nearly_equal(decoded_spawn.rotation.w, 1.0f));
    assert(!network_example::decode_entity_spawn_packet(
        reliable_event_packet.data(),
        reliable_event_packet.size(),
        &decoded_spawn));

    network_example::EntityDespawnPacket despawn{};
    despawn.net_id = 41;
    despawn.server_tick = 18;
    despawn.reason = KernelDespawnReason_OutOfRange;
    const std::vector<std::uint8_t> despawn_packet =
        network_example::encode_entity_despawn_packet(despawn, 46);
    network_example::EntityDespawnPacket decoded_despawn{};
    assert(network_example::decode_entity_despawn_packet(
        despawn_packet.data(),
        despawn_packet.size(),
        &decoded_despawn));
    assert(decoded_despawn.net_id == 41);
    assert(decoded_despawn.server_tick == 18);
    assert(decoded_despawn.reason == KernelDespawnReason_OutOfRange);

    std::vector<std::uint8_t> bad_header = input_packet;
    bad_header[0] = 0;
    assert(!network_example::decode_input_packet(
        bad_header.data(),
        bad_header.size(),
        &decoded_player,
        &decoded_input));

    std::vector<std::uint8_t> bad_crc = input_packet;
    bad_crc.back() ^= 0xffu;
    assert(!network_example::decode_input_packet(
        bad_crc.data(),
        bad_crc.size(),
        &decoded_player,
        &decoded_input));
    std::vector<std::uint8_t> bad_reliable_crc = reliable_event_packet;
    bad_reliable_crc.back() ^= 0xffu;
    assert(!network_example::decode_reliable_event_packet(
        bad_reliable_crc.data(),
        bad_reliable_crc.size(),
        &decoded_event));

    std::vector<std::uint8_t> bad_size = input_packet;
    bad_size.pop_back();
    assert(!network_example::decode_input_packet(
        bad_size.data(),
        bad_size.size(),
        &decoded_player,
        &decoded_input));
    std::vector<std::uint8_t> bad_reliable_size = reliable_event_packet;
    bad_reliable_size.pop_back();
    assert(!network_example::decode_reliable_event_packet(
        bad_reliable_size.data(),
        bad_reliable_size.size(),
        &decoded_event));
    return 0;
}
