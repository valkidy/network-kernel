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
    assert(decoded_welcome.server_tick_rate == 30);
    assert(decoded_welcome.snapshot_rate == 15);

    network_example::DisconnectPacket disconnect;
    disconnect.reason_code = 99;
    const std::vector<std::uint8_t> disconnect_packet =
        network_example::encode_disconnect_packet(disconnect, 3);
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

    return 0;
}
