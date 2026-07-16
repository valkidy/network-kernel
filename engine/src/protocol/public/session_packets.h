#ifndef PROTOCOL_PUBLIC_SESSION_PACKETS_H_
#define PROTOCOL_PUBLIC_SESSION_PACKETS_H_

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

#include "kernel/public/kernel_types.h"
#include "protocol/public/packet_header.h"
#include "world/public/components.h"

namespace network_example {

constexpr std::size_t kHandshakeTextSize = 64;
constexpr std::size_t kGameplayCatalogEntryPathSize = 128;
constexpr std::size_t kGameplayCatalogContentNamespaceSize = 64;
constexpr std::size_t kGameplayCatalogSha256Size = 32;
constexpr std::size_t kGameplayCatalogBundleChunkBytes = 32 * 1024;
constexpr std::size_t kGameplayCatalogBundleChunkHeaderSize =
    kGameplayCatalogSha256Size + 4 + 4 + 4;

struct HandshakePacket {
    std::uint32_t client_nonce = 0;
    std::uint16_t protocol_version = kProtocolVersion;
    std::uint16_t snapshot_schema_version = kSnapshotSchemaVersion;
    std::uint16_t packet_schema_version = kPacketSchemaVersion;
    std::uint32_t catalog_version = 0;
    std::uint64_t catalog_hash = 0;
    char module_version[kHandshakeTextSize] = {};
    char git_commit[kHandshakeTextSize] = {};
};

enum SessionDisconnectReason : std::uint32_t {
    kDisconnectReasonProtocolVersionMismatch = 1001,
    kDisconnectReasonSnapshotSchemaMismatch = 1002,
    kDisconnectReasonPacketSchemaMismatch = 1003,
    kDisconnectReasonCatalogMismatch = 1004,
};

struct WelcomePacket {
    PeerId assigned_peer_id = 0;
    NetId assigned_player_net_id = 0;
    std::uint32_t server_tick = 0;
    std::uint32_t server_tick_rate = 0;
    std::uint32_t snapshot_rate = 0;
    std::uint32_t catalog_version = 0;
    std::uint64_t catalog_hash = 0;
    std::uint32_t actor_blocking_mode = KernelActorBlockingMode_Disabled;
};

struct PingPongPacket {
    std::uint32_t nonce = 0;
    std::uint64_t server_send_time_us = 0;
    std::uint64_t client_receive_time_us = 0;
    std::uint64_t client_send_time_us = 0;
    std::uint64_t server_rtt_us = 0;
    std::uint64_t server_jitter_us = 0;
};

struct DisconnectPacket {
    std::uint32_t reason_code = 0;
};

struct GameplayCatalogManifestRequestPacket {
    std::uint16_t protocol_version = kProtocolVersion;
    std::uint16_t snapshot_schema_version = kSnapshotSchemaVersion;
    std::uint16_t packet_schema_version = kPacketSchemaVersion;
};

struct GameplayCatalogManifestPacket {
    std::uint32_t catalog_version = 0;
    std::uint64_t catalog_hash = 0;
    std::uint32_t bundle_size = 0;
    std::array<std::uint8_t, kGameplayCatalogSha256Size> bundle_sha256{};
    char entry_path[kGameplayCatalogEntryPathSize] = {};
    char content_namespace[kGameplayCatalogContentNamespaceSize] = {};
};

struct GameplayCatalogBundleRequestPacket {
    std::array<std::uint8_t, kGameplayCatalogSha256Size> bundle_sha256{};
};

struct GameplayCatalogBundleChunkPacket {
    std::array<std::uint8_t, kGameplayCatalogSha256Size> bundle_sha256{};
    std::uint32_t offset = 0;
    std::uint32_t total_size = 0;
    std::vector<std::uint8_t> bytes;
};

enum class GameplayCatalogSyncErrorCode : std::uint32_t {
    kUnsupported = 1,
    kBundleUnavailable = 2,
    kVersionMismatch = 3,
    kInvalidRequest = 4,
};

struct GameplayCatalogSyncErrorPacket {
    GameplayCatalogSyncErrorCode error_code =
        GameplayCatalogSyncErrorCode::kInvalidRequest;
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

std::vector<std::uint8_t> encode_gameplay_catalog_manifest_request_packet(
    const GameplayCatalogManifestRequestPacket& packet,
    std::uint32_t sequence = 0);
bool decode_gameplay_catalog_manifest_request_packet(
    const std::uint8_t* data,
    std::size_t size,
    GameplayCatalogManifestRequestPacket* out_packet);

std::vector<std::uint8_t> encode_gameplay_catalog_manifest_packet(
    const GameplayCatalogManifestPacket& packet,
    std::uint32_t sequence = 0);
bool decode_gameplay_catalog_manifest_packet(
    const std::uint8_t* data,
    std::size_t size,
    GameplayCatalogManifestPacket* out_packet);

std::vector<std::uint8_t> encode_gameplay_catalog_bundle_request_packet(
    const GameplayCatalogBundleRequestPacket& packet,
    std::uint32_t sequence = 0);
bool decode_gameplay_catalog_bundle_request_packet(
    const std::uint8_t* data,
    std::size_t size,
    GameplayCatalogBundleRequestPacket* out_packet);

std::vector<std::uint8_t> encode_gameplay_catalog_bundle_chunk_packet(
    const GameplayCatalogBundleChunkPacket& packet,
    std::uint32_t sequence = 0);
bool decode_gameplay_catalog_bundle_chunk_packet(
    const std::uint8_t* data,
    std::size_t size,
    GameplayCatalogBundleChunkPacket* out_packet);

std::vector<std::uint8_t> encode_gameplay_catalog_sync_error_packet(
    const GameplayCatalogSyncErrorPacket& packet,
    std::uint32_t sequence = 0);
bool decode_gameplay_catalog_sync_error_packet(
    const std::uint8_t* data,
    std::size_t size,
    GameplayCatalogSyncErrorPacket* out_packet);

}  // namespace network_example

#endif  // PROTOCOL_PUBLIC_SESSION_PACKETS_H_
