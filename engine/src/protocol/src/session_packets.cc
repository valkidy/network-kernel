#include "protocol/public/session_packets.h"

#include <cstddef>

#include "protocol/src/packet_codec.h"

namespace network_example {
namespace {

constexpr std::size_t kHandshakePayloadSize = 4 + 2 + 2 + 2 +
                                              kHandshakeTextSize +
                                              kHandshakeTextSize;
constexpr std::size_t kWelcomePayloadSize = 20;
constexpr std::size_t kPingPongPayloadSize = 28;
constexpr std::size_t kDisconnectPayloadSize = 4;

}  // namespace

std::vector<std::uint8_t> encode_handshake_packet(
    const HandshakePacket& packet,
    std::uint32_t sequence) {
    protocol_internal::PacketWriter payload;
    payload.reserve(kHandshakePayloadSize);
    payload.write_u32(packet.client_nonce);
    payload.write_u16(packet.protocol_version);
    payload.write_u16(packet.snapshot_schema_version);
    payload.write_u16(packet.packet_schema_version);
    payload.write_bytes(packet.module_version, sizeof(packet.module_version));
    payload.write_bytes(packet.git_commit, sizeof(packet.git_commit));
    return protocol_internal::wrap_packet(
        MessageType::kHandshake,
        payload.bytes(),
        sequence);
}

bool decode_handshake_packet(
    const std::uint8_t* data,
    std::size_t size,
    HandshakePacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kHandshake,
            &payload,
            &payload_size) ||
        payload_size != kHandshakePayloadSize) {
        return false;
    }

    HandshakePacket packet;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.client_nonce) ||
        !reader.read_u16(&packet.protocol_version) ||
        !reader.read_u16(&packet.snapshot_schema_version) ||
        !reader.read_u16(&packet.packet_schema_version) ||
        !reader.read_bytes(packet.module_version, sizeof(packet.module_version)) ||
        !reader.read_bytes(packet.git_commit, sizeof(packet.git_commit)) ||
        !reader.done()) {
        return false;
    }
    packet.module_version[sizeof(packet.module_version) - 1] = '\0';
    packet.git_commit[sizeof(packet.git_commit) - 1] = '\0';

    *out_packet = packet;
    return true;
}

std::vector<std::uint8_t> encode_welcome_packet(
    const WelcomePacket& packet,
    std::uint32_t sequence) {
    protocol_internal::PacketWriter payload;
    payload.reserve(kWelcomePayloadSize);
    payload.write_u32(packet.assigned_peer_id);
    payload.write_u32(packet.assigned_player_net_id);
    payload.write_u32(packet.server_tick);
    payload.write_u32(packet.server_tick_rate);
    payload.write_u32(packet.snapshot_rate);
    return protocol_internal::wrap_packet(
        MessageType::kWelcome,
        payload.bytes(),
        sequence);
}

bool decode_welcome_packet(
    const std::uint8_t* data,
    std::size_t size,
    WelcomePacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kWelcome,
            &payload,
            &payload_size) ||
        payload_size != kWelcomePayloadSize) {
        return false;
    }

    WelcomePacket packet;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.assigned_peer_id) ||
        !reader.read_u32(&packet.assigned_player_net_id) ||
        !reader.read_u32(&packet.server_tick) ||
        !reader.read_u32(&packet.server_tick_rate) ||
        !reader.read_u32(&packet.snapshot_rate) ||
        !reader.done()) {
        return false;
    }

    *out_packet = packet;
    return true;
}

std::vector<std::uint8_t> encode_ping_pong_packet(
    const PingPongPacket& packet,
    std::uint32_t sequence) {
    protocol_internal::PacketWriter payload;
    payload.reserve(kPingPongPayloadSize);
    payload.write_u32(packet.nonce);
    payload.write_u64(packet.server_send_time_us);
    payload.write_u64(packet.client_receive_time_us);
    payload.write_u64(packet.client_send_time_us);
    return protocol_internal::wrap_packet(
        MessageType::kPingPong,
        payload.bytes(),
        sequence);
}

bool decode_ping_pong_packet(
    const std::uint8_t* data,
    std::size_t size,
    PingPongPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kPingPong,
            &payload,
            &payload_size) ||
        payload_size != kPingPongPayloadSize) {
        return false;
    }

    PingPongPacket packet;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.nonce) ||
        !reader.read_u64(&packet.server_send_time_us) ||
        !reader.read_u64(&packet.client_receive_time_us) ||
        !reader.read_u64(&packet.client_send_time_us) ||
        !reader.done()) {
        return false;
    }

    *out_packet = packet;
    return true;
}

std::vector<std::uint8_t> encode_disconnect_packet(
    const DisconnectPacket& packet,
    std::uint32_t sequence) {
    protocol_internal::PacketWriter payload;
    payload.reserve(kDisconnectPayloadSize);
    payload.write_u32(packet.reason_code);
    return protocol_internal::wrap_packet(
        MessageType::kDisconnect,
        payload.bytes(),
        sequence);
}

bool decode_disconnect_packet(
    const std::uint8_t* data,
    std::size_t size,
    DisconnectPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kDisconnect,
            &payload,
            &payload_size) ||
        payload_size != kDisconnectPayloadSize) {
        return false;
    }

    DisconnectPacket packet;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.reason_code) || !reader.done()) {
        return false;
    }

    *out_packet = packet;
    return true;
}

}  // namespace network_example
