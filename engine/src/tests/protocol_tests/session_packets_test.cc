#include <cassert>
#include <vector>

#include "protocol/public/session_packets.h"

int main() {
    network_example::HandshakePacket handshake;
    handshake.client_nonce = 1234;
    const std::vector<std::uint8_t> handshake_packet =
        network_example::encode_handshake_packet(handshake, 1);
    network_example::HandshakePacket decoded_handshake;
    assert(network_example::decode_handshake_packet(
        handshake_packet.data(),
        handshake_packet.size(),
        &decoded_handshake));
    assert(decoded_handshake.client_nonce == handshake.client_nonce);

    network_example::WelcomePacket welcome;
    welcome.assigned_peer_id = 7;
    welcome.assigned_player_net_id = 11;
    welcome.server_tick = 44;
    welcome.server_tick_rate = 30;
    welcome.snapshot_rate = 15;
    const std::vector<std::uint8_t> welcome_packet =
        network_example::encode_welcome_packet(welcome, 2);
    network_example::WelcomePacket decoded_welcome;
    assert(network_example::decode_welcome_packet(
        welcome_packet.data(),
        welcome_packet.size(),
        &decoded_welcome));
    assert(decoded_welcome.assigned_peer_id == 7);
    assert(decoded_welcome.assigned_player_net_id == 11);
    assert(decoded_welcome.server_tick == 44);
    assert(decoded_welcome.server_tick_rate == 30);
    assert(decoded_welcome.snapshot_rate == 15);

    network_example::PingPongPacket ping_pong;
    ping_pong.nonce = 88;
    ping_pong.server_send_time_us = 100000;
    ping_pong.client_receive_time_us = 141000;
    ping_pong.client_send_time_us = 142000;
    const std::vector<std::uint8_t> ping_pong_packet =
        network_example::encode_ping_pong_packet(ping_pong, 3);
    network_example::PingPongPacket decoded_ping_pong;
    assert(network_example::decode_ping_pong_packet(
        ping_pong_packet.data(),
        ping_pong_packet.size(),
        &decoded_ping_pong));
    assert(decoded_ping_pong.nonce == 88);
    assert(decoded_ping_pong.server_send_time_us == 100000);
    assert(decoded_ping_pong.client_receive_time_us == 141000);
    assert(decoded_ping_pong.client_send_time_us == 142000);

    network_example::DisconnectPacket disconnect;
    disconnect.reason_code = 99;
    const std::vector<std::uint8_t> disconnect_packet =
        network_example::encode_disconnect_packet(disconnect, 4);
    network_example::DisconnectPacket decoded_disconnect;
    assert(network_example::decode_disconnect_packet(
        disconnect_packet.data(),
        disconnect_packet.size(),
        &decoded_disconnect));
    assert(decoded_disconnect.reason_code == 99);

    std::vector<std::uint8_t> truncated = welcome_packet;
    truncated.pop_back();
    assert(!network_example::decode_welcome_packet(
        truncated.data(),
        truncated.size(),
        &decoded_welcome));

    std::vector<std::uint8_t> bad_crc = welcome_packet;
    bad_crc.back() ^= 0xffu;
    assert(!network_example::decode_welcome_packet(
        bad_crc.data(),
        bad_crc.size(),
        &decoded_welcome));

    std::vector<std::uint8_t> bad_version = welcome_packet;
    bad_version[4] ^= 0xffu;
    assert(!network_example::decode_welcome_packet(
        bad_version.data(),
        bad_version.size(),
        &decoded_welcome));

    std::vector<std::uint8_t> bad_ping_pong_crc = ping_pong_packet;
    bad_ping_pong_crc.back() ^= 0xffu;
    assert(!network_example::decode_ping_pong_packet(
        bad_ping_pong_crc.data(),
        bad_ping_pong_crc.size(),
        &decoded_ping_pong));

    std::vector<std::uint8_t> truncated_ping_pong = ping_pong_packet;
    truncated_ping_pong.pop_back();
    assert(!network_example::decode_ping_pong_packet(
        truncated_ping_pong.data(),
        truncated_ping_pong.size(),
        &decoded_ping_pong));

    return 0;
}
