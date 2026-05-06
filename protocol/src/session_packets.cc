#include "protocol/public/session_packets.h"

#include <cstddef>

namespace network_example {
namespace {

constexpr std::size_t kHandshakePayloadSize = 4;
constexpr std::size_t kWelcomePayloadSize = 12;
constexpr std::size_t kDisconnectPayloadSize = 4;

void write_u32(std::vector<std::uint8_t>* buffer, std::uint32_t value) {
    buffer->push_back(static_cast<std::uint8_t>(value & 0xffu));
    buffer->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    buffer->push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
    buffer->push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
}

bool read_u32(
    const std::uint8_t* data,
    std::size_t size,
    std::size_t* offset,
    std::uint32_t* out_value) {
    if (*offset + 4 > size) {
        return false;
    }
    *out_value = static_cast<std::uint32_t>(data[*offset]) |
                 (static_cast<std::uint32_t>(data[*offset + 1]) << 8u) |
                 (static_cast<std::uint32_t>(data[*offset + 2]) << 16u) |
                 (static_cast<std::uint32_t>(data[*offset + 3]) << 24u);
    *offset += 4;
    return true;
}

std::vector<std::uint8_t> wrap_packet(
    MessageType message_type,
    const std::vector<std::uint8_t>& payload,
    std::uint32_t sequence) {
    PacketHeader header;
    header.message_type = static_cast<std::uint16_t>(message_type);
    header.sequence = sequence;
    header.payload_size = static_cast<std::uint32_t>(payload.size());
    header.payload_crc = compute_payload_crc(payload.data(), payload.size());

    const EncodedPacketHeader encoded_header = encode_packet_header(header);
    std::vector<std::uint8_t> packet;
    packet.reserve(kPacketHeaderSize + payload.size());
    packet.insert(packet.end(), encoded_header.begin(), encoded_header.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

bool unwrap_packet(
    const std::uint8_t* data,
    std::size_t size,
    MessageType expected_type,
    const std::uint8_t** out_payload,
    std::size_t* out_payload_size) {
    if (data == nullptr || out_payload == nullptr || out_payload_size == nullptr ||
        size < kPacketHeaderSize) {
        return false;
    }

    PacketHeader header;
    if (!decode_packet_header(data, size, &header) ||
        header.message_type != static_cast<std::uint16_t>(expected_type)) {
        return false;
    }
    if (header.payload_size != size - kPacketHeaderSize) {
        return false;
    }

    const std::uint8_t* payload = data + kPacketHeaderSize;
    if (compute_payload_crc(payload, header.payload_size) != header.payload_crc) {
        return false;
    }

    *out_payload = payload;
    *out_payload_size = header.payload_size;
    return true;
}

}  // namespace

std::vector<std::uint8_t> encode_handshake_packet(
    const HandshakePacket& packet,
    std::uint32_t sequence) {
    std::vector<std::uint8_t> payload;
    payload.reserve(kHandshakePayloadSize);
    write_u32(&payload, packet.client_nonce);
    return wrap_packet(MessageType::kHandshake, payload, sequence);
}

bool decode_handshake_packet(
    const std::uint8_t* data,
    std::size_t size,
    HandshakePacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !unwrap_packet(data, size, MessageType::kHandshake, &payload, &payload_size) ||
        payload_size != kHandshakePayloadSize) {
        return false;
    }

    HandshakePacket packet;
    std::size_t offset = 0;
    if (!read_u32(payload, payload_size, &offset, &packet.client_nonce) ||
        offset != payload_size) {
        return false;
    }

    *out_packet = packet;
    return true;
}

std::vector<std::uint8_t> encode_welcome_packet(
    const WelcomePacket& packet,
    std::uint32_t sequence) {
    std::vector<std::uint8_t> payload;
    payload.reserve(kWelcomePayloadSize);
    write_u32(&payload, packet.assigned_peer_id);
    write_u32(&payload, packet.server_tick_rate);
    write_u32(&payload, packet.snapshot_rate);
    return wrap_packet(MessageType::kWelcome, payload, sequence);
}

bool decode_welcome_packet(
    const std::uint8_t* data,
    std::size_t size,
    WelcomePacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !unwrap_packet(
            data,
            size,
            MessageType::kWelcome,
            &payload,
            &payload_size) ||
        payload_size != kWelcomePayloadSize) {
        return false;
    }

    WelcomePacket packet;
    std::size_t offset = 0;
    if (!read_u32(payload, payload_size, &offset, &packet.assigned_peer_id) ||
        !read_u32(payload, payload_size, &offset, &packet.server_tick_rate) ||
        !read_u32(payload, payload_size, &offset, &packet.snapshot_rate) ||
        offset != payload_size) {
        return false;
    }

    *out_packet = packet;
    return true;
}

std::vector<std::uint8_t> encode_disconnect_packet(
    const DisconnectPacket& packet,
    std::uint32_t sequence) {
    std::vector<std::uint8_t> payload;
    payload.reserve(kDisconnectPayloadSize);
    write_u32(&payload, packet.reason_code);
    return wrap_packet(MessageType::kDisconnect, payload, sequence);
}

bool decode_disconnect_packet(
    const std::uint8_t* data,
    std::size_t size,
    DisconnectPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !unwrap_packet(data, size, MessageType::kDisconnect, &payload, &payload_size) ||
        payload_size != kDisconnectPayloadSize) {
        return false;
    }

    DisconnectPacket packet;
    std::size_t offset = 0;
    if (!read_u32(payload, payload_size, &offset, &packet.reason_code) ||
        offset != payload_size) {
        return false;
    }

    *out_packet = packet;
    return true;
}

}  // namespace network_example
