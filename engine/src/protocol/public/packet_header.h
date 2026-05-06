#ifndef PROTOCOL_PUBLIC_PACKET_HEADER_H_
#define PROTOCOL_PUBLIC_PACKET_HEADER_H_

#include <array>
#include <cstddef>
#include <cstdint>

namespace network_example {

constexpr std::uint32_t kPacketMagic = 0x4e584b31u;
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::uint16_t kSchemaVersion = 1;
constexpr std::size_t kPacketHeaderSize = 28;

enum class MessageType : std::uint16_t {
    kHandshake = 1,
    kInputPacket = 2,
    kSnapshotPacket = 3,
    kReliableEventPacket = 4,
    kFireCommand = 5,
    kHitResult = 6,
    kExplosionResult = 7,
    kDisconnect = 8,
    kPingPong = 9,
    kWelcome = 10,
};

struct PacketHeader {
    std::uint32_t magic = kPacketMagic;
    std::uint16_t protocol_version = kProtocolVersion;
    std::uint16_t schema_version = kSchemaVersion;
    std::uint16_t message_type = 0;
    std::uint16_t flags = 0;
    std::uint32_t sequence = 0;
    std::uint32_t ack = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t payload_crc = 0;
};

using EncodedPacketHeader = std::array<std::uint8_t, kPacketHeaderSize>;

std::uint32_t compute_payload_crc(const std::uint8_t* data, std::size_t size);
EncodedPacketHeader encode_packet_header(const PacketHeader& header);
bool decode_packet_header(
    const std::uint8_t* data,
    std::size_t size,
    PacketHeader* out_header);

}  // namespace network_example

#endif  // PROTOCOL_PUBLIC_PACKET_HEADER_H_
