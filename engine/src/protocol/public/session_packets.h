#ifndef PROTOCOL_PUBLIC_SESSION_PACKETS_H_
#define PROTOCOL_PUBLIC_SESSION_PACKETS_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "protocol/public/packet_header.h"
#include "world/public/components.h"

namespace network_example {

constexpr std::size_t kHandshakeTextSize = 64;

struct HandshakePacket {
    std::uint32_t client_nonce = 0;
    std::uint16_t protocol_version = kProtocolVersion;
    std::uint16_t snapshot_schema_version = kSnapshotSchemaVersion;
    std::uint16_t packet_schema_version = kPacketSchemaVersion;
    char module_version[kHandshakeTextSize] = {};
    char git_commit[kHandshakeTextSize] = {};
};

enum SessionDisconnectReason : std::uint32_t {
    kDisconnectReasonProtocolVersionMismatch = 1001,
    kDisconnectReasonSnapshotSchemaMismatch = 1002,
    kDisconnectReasonPacketSchemaMismatch = 1003,
};

struct WelcomePacket {
    PeerId assigned_peer_id = 0;
    NetId assigned_player_net_id = 0;
    std::uint32_t server_tick = 0;
    std::uint32_t server_tick_rate = 0;
    std::uint32_t snapshot_rate = 0;
};

struct PingPongPacket {
    std::uint32_t nonce = 0;
    std::uint64_t server_send_time_us = 0;
    std::uint64_t client_receive_time_us = 0;
    std::uint64_t client_send_time_us = 0;
};

struct DisconnectPacket {
    std::uint32_t reason_code = 0;
};

std::vector<std::uint8_t> encode_handshake_packet(
    const HandshakePacket& packet,
    std::uint32_t sequence = 0);
bool decode_handshake_packet(
    const std::uint8_t* data,
    std::size_t size,
    HandshakePacket* out_packet);

std::vector<std::uint8_t> encode_welcome_packet(
    const WelcomePacket& packet,
    std::uint32_t sequence = 0);
bool decode_welcome_packet(
    const std::uint8_t* data,
    std::size_t size,
    WelcomePacket* out_packet);

std::vector<std::uint8_t> encode_ping_pong_packet(
    const PingPongPacket& packet,
    std::uint32_t sequence = 0);
bool decode_ping_pong_packet(
    const std::uint8_t* data,
    std::size_t size,
    PingPongPacket* out_packet);

std::vector<std::uint8_t> encode_disconnect_packet(
    const DisconnectPacket& packet,
    std::uint32_t sequence = 0);
bool decode_disconnect_packet(
    const std::uint8_t* data,
    std::size_t size,
    DisconnectPacket* out_packet);

}  // namespace network_example

#endif  // PROTOCOL_PUBLIC_SESSION_PACKETS_H_
