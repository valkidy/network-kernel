#include <cassert>
#include <vector>

#include <glm/glm.hpp>

#include "protocol/public/m2_packets.h"

namespace {

bool nearly_equal(float lhs, float rhs) {
    const float diff = lhs > rhs ? lhs - rhs : rhs - lhs;
    return diff < 0.0001f;
}

}  // namespace

int main() {
    PlayerInput input{};
    input.input_seq = 7;
    input.client_tick = 11;
    input.move = KernelVec2{0.5f, -0.25f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.buttons = InputButton_Fire | InputButton_Sprint;
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
    assert(decoded_input.client_tick == input.client_tick);
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
    entity.type = network_example::EntityType::kPlayer;
    entity.position = glm::vec3{1.0f, 2.0f, 3.0f};
    entity.velocity = glm::vec3{4.0f, 5.0f, 6.0f};
    entity.hp = 88;
    entity.state = 2;
    entity.flags = 3;
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
    assert(decoded_snapshot.entities[0].type == network_example::EntityType::kPlayer);
    assert(nearly_equal(decoded_snapshot.entities[0].position.x, 1.0f));
    assert(nearly_equal(decoded_snapshot.entities[0].velocity.z, 6.0f));
    assert(decoded_snapshot.entities[0].hp == 88);
    assert(decoded_snapshot.entities[0].state == 2);
    assert(decoded_snapshot.entities[0].flags == 3);

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

    std::vector<std::uint8_t> bad_size = input_packet;
    bad_size.pop_back();
    assert(!network_example::decode_input_packet(
        bad_size.data(),
        bad_size.size(),
        &decoded_player,
        &decoded_input));
    return 0;
}
