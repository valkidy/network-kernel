#include <cstring>

#include <cassert>
#include <vector>

#include "protocol/public/session_packets.h"

int main() {
    network_example::HandshakePacket handshake;
    handshake.client_nonce = 1234;
    handshake.protocol_version = network_example::kProtocolVersion;
    handshake.snapshot_schema_version = network_example::kSnapshotSchemaVersion;
    handshake.packet_schema_version = network_example::kPacketSchemaVersion;
    handshake.catalog_version = 3;
    handshake.catalog_hash = 0x1122334455667788ull;
    std::strncpy(
        handshake.module_version,
        "0.6.4-test",
        sizeof(handshake.module_version) - 1);
    std::strncpy(
        handshake.git_commit,
        "abcdef123456",
        sizeof(handshake.git_commit) - 1);
    const std::vector<std::uint8_t> handshake_packet =
        network_example::encode_handshake_packet(handshake, 1);
    assert(handshake_packet.size() == 178);
    network_example::HandshakePacket decoded_handshake;
    assert(network_example::decode_handshake_packet(
        handshake_packet.data(),
        handshake_packet.size(),
        &decoded_handshake));
    assert(decoded_handshake.client_nonce == handshake.client_nonce);
    assert(decoded_handshake.protocol_version == network_example::kProtocolVersion);
    assert(decoded_handshake.snapshot_schema_version ==
           network_example::kSnapshotSchemaVersion);
    assert(decoded_handshake.packet_schema_version ==
           network_example::kPacketSchemaVersion);
    assert(decoded_handshake.catalog_version == 3);
    assert(decoded_handshake.catalog_hash == 0x1122334455667788ull);
    assert(std::strcmp(decoded_handshake.module_version, "0.6.4-test") == 0);
    assert(std::strcmp(decoded_handshake.git_commit, "abcdef123456") == 0);

    network_example::GameplayCatalogManifestRequestPacket manifest_request;
    manifest_request.protocol_version = network_example::kProtocolVersion;
    manifest_request.snapshot_schema_version = network_example::kSnapshotSchemaVersion;
    manifest_request.packet_schema_version = network_example::kPacketSchemaVersion;
    const std::vector<std::uint8_t> manifest_request_packet =
        network_example::encode_gameplay_catalog_manifest_request_packet(
            manifest_request,
            2);
    assert(manifest_request_packet.size() == 34);
    network_example::GameplayCatalogManifestRequestPacket decoded_manifest_request;
    assert(network_example::decode_gameplay_catalog_manifest_request_packet(
        manifest_request_packet.data(),
        manifest_request_packet.size(),
        &decoded_manifest_request));
    assert(decoded_manifest_request.protocol_version == network_example::kProtocolVersion);
    assert(
        decoded_manifest_request.snapshot_schema_version ==
        network_example::kSnapshotSchemaVersion);
    assert(
        decoded_manifest_request.packet_schema_version ==
        network_example::kPacketSchemaVersion);

    network_example::GameplayCatalogManifestPacket manifest;
    manifest.catalog_version = 7;
    manifest.catalog_hash = 0x1020304050607080ull;
    manifest.bundle_size = 123456;
    std::strncpy(
        manifest.entry_path,
        "gameplay_catalog.yaml",
        sizeof(manifest.entry_path) - 1);
    std::strncpy(
        manifest.content_namespace,
        "production",
        sizeof(manifest.content_namespace) - 1);
    for (std::size_t index = 0; index < manifest.bundle_sha256.size(); ++index) {
        manifest.bundle_sha256[index] = static_cast<std::uint8_t>(index);
    }
    const std::vector<std::uint8_t> manifest_packet =
        network_example::encode_gameplay_catalog_manifest_packet(manifest, 3);
    assert(manifest_packet.size() == 268);
    network_example::GameplayCatalogManifestPacket decoded_manifest;
    assert(network_example::decode_gameplay_catalog_manifest_packet(
        manifest_packet.data(),
        manifest_packet.size(),
        &decoded_manifest));
    assert(decoded_manifest.catalog_version == 7);
    assert(decoded_manifest.catalog_hash == 0x1020304050607080ull);
    assert(decoded_manifest.bundle_size == 123456);
    assert(std::strcmp(decoded_manifest.entry_path, "gameplay_catalog.yaml") == 0);
    assert(std::strcmp(decoded_manifest.content_namespace, "production") == 0);
    assert(decoded_manifest.bundle_sha256 == manifest.bundle_sha256);

    network_example::GameplayCatalogBundleRequestPacket bundle_request;
    bundle_request.bundle_sha256 = manifest.bundle_sha256;
    const std::vector<std::uint8_t> bundle_request_packet =
        network_example::encode_gameplay_catalog_bundle_request_packet(
            bundle_request,
            4);
    assert(bundle_request_packet.size() == 60);
    network_example::GameplayCatalogBundleRequestPacket decoded_bundle_request;
    assert(network_example::decode_gameplay_catalog_bundle_request_packet(
        bundle_request_packet.data(),
        bundle_request_packet.size(),
        &decoded_bundle_request));
    assert(decoded_bundle_request.bundle_sha256 == manifest.bundle_sha256);

    network_example::GameplayCatalogBundleChunkPacket chunk;
    chunk.bundle_sha256 = manifest.bundle_sha256;
    chunk.offset = 32768;
    chunk.total_size = 65539;
    chunk.bytes = {1, 2, 3};
    const std::vector<std::uint8_t> chunk_packet =
        network_example::encode_gameplay_catalog_bundle_chunk_packet(chunk, 5);
    assert(chunk_packet.size() == 75);
    network_example::GameplayCatalogBundleChunkPacket maximum_chunk = chunk;
    maximum_chunk.bytes.resize(network_example::kGameplayCatalogBundleChunkBytes);
    assert(network_example::encode_gameplay_catalog_bundle_chunk_packet(
               maximum_chunk,
               5)
               .size() == 32840);
    network_example::GameplayCatalogBundleChunkPacket decoded_chunk;
    assert(network_example::decode_gameplay_catalog_bundle_chunk_packet(
        chunk_packet.data(),
        chunk_packet.size(),
        &decoded_chunk));
    assert(decoded_chunk.bundle_sha256 == manifest.bundle_sha256);
    assert(decoded_chunk.offset == 32768);
    assert(decoded_chunk.total_size == 65539);
    assert(decoded_chunk.bytes == std::vector<std::uint8_t>({1, 2, 3}));

    network_example::GameplayCatalogSyncErrorPacket sync_error;
    sync_error.error_code =
        network_example::GameplayCatalogSyncErrorCode::kBundleUnavailable;
    const std::vector<std::uint8_t> sync_error_packet =
        network_example::encode_gameplay_catalog_sync_error_packet(sync_error, 6);
    assert(sync_error_packet.size() == 32);
    network_example::GameplayCatalogSyncErrorPacket decoded_sync_error;
    assert(network_example::decode_gameplay_catalog_sync_error_packet(
        sync_error_packet.data(),
        sync_error_packet.size(),
        &decoded_sync_error));
    assert(
        decoded_sync_error.error_code ==
        network_example::GameplayCatalogSyncErrorCode::kBundleUnavailable);

    network_example::WelcomePacket welcome;
    welcome.assigned_peer_id = 7;
    welcome.assigned_player_net_id = 11;
    welcome.server_tick = 44;
    welcome.server_tick_rate = 30;
    welcome.snapshot_rate = 15;
    welcome.catalog_version = 3;
    welcome.catalog_hash = 0x1122334455667788ull;
    welcome.actor_blocking_mode = KernelActorBlockingMode_Predicted;
    const std::vector<std::uint8_t> welcome_packet =
        network_example::encode_welcome_packet(welcome, 2);
    assert(welcome_packet.size() == 60);
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
    assert(decoded_welcome.catalog_version == 3);
    assert(decoded_welcome.catalog_hash == 0x1122334455667788ull);
    assert(
        decoded_welcome.actor_blocking_mode ==
        KernelActorBlockingMode_Predicted);
    std::vector<std::uint8_t> invalid_welcome_packet = welcome_packet;
    invalid_welcome_packet[56] = 2u;
    invalid_welcome_packet[57] = 0u;
    invalid_welcome_packet[58] = 0u;
    invalid_welcome_packet[59] = 0u;
    assert(!network_example::decode_welcome_packet(
        invalid_welcome_packet.data(),
        invalid_welcome_packet.size(),
        &decoded_welcome));

    network_example::PingPongPacket ping_pong;
    ping_pong.nonce = 88;
    ping_pong.server_send_time_us = 100000;
    ping_pong.client_receive_time_us = 141000;
    ping_pong.client_send_time_us = 142000;
    ping_pong.server_rtt_us = 43000;
    ping_pong.server_jitter_us = 2000;
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
    assert(decoded_ping_pong.server_rtt_us == 43000);
    assert(decoded_ping_pong.server_jitter_us == 2000);

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

    std::vector<std::uint8_t> oversized_chunk = chunk_packet;
    oversized_chunk.resize(
        network_example::kGameplayCatalogBundleChunkBytes +
        network_example::kPacketHeaderSize +
        network_example::kGameplayCatalogBundleChunkHeaderSize +
        1);
    assert(!network_example::decode_gameplay_catalog_bundle_chunk_packet(
        oversized_chunk.data(),
        oversized_chunk.size(),
        &decoded_chunk));

    return 0;
}
