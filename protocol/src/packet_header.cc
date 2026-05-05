#include "protocol/public/packet_header.h"

namespace network_example {
namespace {

void write_u16(EncodedPacketHeader* buffer, std::size_t offset, std::uint16_t value) {
    (*buffer)[offset] = static_cast<std::uint8_t>(value & 0xffu);
    (*buffer)[offset + 1] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
}

void write_u32(EncodedPacketHeader* buffer, std::size_t offset, std::uint32_t value) {
    (*buffer)[offset] = static_cast<std::uint8_t>(value & 0xffu);
    (*buffer)[offset + 1] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
    (*buffer)[offset + 2] = static_cast<std::uint8_t>((value >> 16u) & 0xffu);
    (*buffer)[offset + 3] = static_cast<std::uint8_t>((value >> 24u) & 0xffu);
}

std::uint16_t read_u16(const std::uint8_t* data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) |
           static_cast<std::uint16_t>(data[offset + 1] << 8u);
}

std::uint32_t read_u32(const std::uint8_t* data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16u) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24u);
}

}  // namespace

std::uint32_t compute_payload_crc(const std::uint8_t* data, std::size_t size) {
    constexpr std::uint32_t kFnvOffset = 2166136261u;
    constexpr std::uint32_t kFnvPrime = 16777619u;
    std::uint32_t hash = kFnvOffset;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= kFnvPrime;
    }
    return hash;
}

EncodedPacketHeader encode_packet_header(const PacketHeader& header) {
    EncodedPacketHeader encoded{};
    write_u32(&encoded, 0, header.magic);
    write_u16(&encoded, 4, header.protocol_version);
    write_u16(&encoded, 6, header.schema_version);
    write_u16(&encoded, 8, header.message_type);
    write_u16(&encoded, 10, header.flags);
    write_u32(&encoded, 12, header.sequence);
    write_u32(&encoded, 16, header.ack);
    write_u32(&encoded, 20, header.payload_size);
    write_u32(&encoded, 24, header.payload_crc);
    return encoded;
}

bool decode_packet_header(
    const std::uint8_t* data,
    std::size_t size,
    PacketHeader* out_header) {
    if (data == nullptr || out_header == nullptr || size < kPacketHeaderSize) {
        return false;
    }

    PacketHeader header;
    header.magic = read_u32(data, 0);
    header.protocol_version = read_u16(data, 4);
    header.schema_version = read_u16(data, 6);
    header.message_type = read_u16(data, 8);
    header.flags = read_u16(data, 10);
    header.sequence = read_u32(data, 12);
    header.ack = read_u32(data, 16);
    header.payload_size = read_u32(data, 20);
    header.payload_crc = read_u32(data, 24);

    if (header.magic != kPacketMagic ||
        header.protocol_version != kProtocolVersion ||
        header.schema_version != kSchemaVersion) {
        return false;
    }

    *out_header = header;
    return true;
}

}  // namespace network_example
