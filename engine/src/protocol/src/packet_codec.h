#ifndef PROTOCOL_SRC_PACKET_CODEC_H_
#define PROTOCOL_SRC_PACKET_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "protocol/public/packet_header.h"

namespace network_example::protocol_internal {

class PacketWriter {
public:
    void reserve(std::size_t size) { buffer_.reserve(size); }

    void write_u8(std::uint8_t value) { buffer_.push_back(value); }
    void write_u16(std::uint16_t value) {
        buffer_.push_back(static_cast<std::uint8_t>(value & 0xffu));
        buffer_.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    }
    void write_u32(std::uint32_t value) {
        buffer_.push_back(static_cast<std::uint8_t>(value & 0xffu));
        buffer_.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
        buffer_.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
        buffer_.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
    }
    void write_u64(std::uint64_t value) {
        write_u32(static_cast<std::uint32_t>(value & 0xffffffffu));
        write_u32(static_cast<std::uint32_t>((value >> 32u) & 0xffffffffu));
    }
    void write_float(float value) {
        static_assert(sizeof(float) == sizeof(std::uint32_t));
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(float));
        write_u32(bits);
    }
    void write_vec3(const glm::vec3& value) {
        write_float(value.x);
        write_float(value.y);
        write_float(value.z);
    }
    void write_quat(const glm::quat& value) {
        write_float(value.x);
        write_float(value.y);
        write_float(value.z);
        write_float(value.w);
    }
    void write_bytes(const void* data, std::size_t size) {
        if (size == 0) {
            return;
        }
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + size);
    }

    const std::vector<std::uint8_t>& bytes() const { return buffer_; }

private:
    std::vector<std::uint8_t> buffer_;
};

class PacketReader {
public:
    PacketReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    bool read_u8(std::uint8_t* out_value) {
        if (!can_read(1) || out_value == nullptr) {
            return false;
        }
        *out_value = data_[offset_];
        offset_ += 1;
        return true;
    }
    bool read_u16(std::uint16_t* out_value) {
        if (!can_read(2) || out_value == nullptr) {
            return false;
        }
        *out_value = static_cast<std::uint16_t>(data_[offset_]) |
                     static_cast<std::uint16_t>(data_[offset_ + 1] << 8u);
        offset_ += 2;
        return true;
    }
    bool read_u32(std::uint32_t* out_value) {
        if (!can_read(4) || out_value == nullptr) {
            return false;
        }
        *out_value = static_cast<std::uint32_t>(data_[offset_]) |
                     (static_cast<std::uint32_t>(data_[offset_ + 1]) << 8u) |
                     (static_cast<std::uint32_t>(data_[offset_ + 2]) << 16u) |
                     (static_cast<std::uint32_t>(data_[offset_ + 3]) << 24u);
        offset_ += 4;
        return true;
    }
    bool read_u64(std::uint64_t* out_value) {
        std::uint32_t low = 0;
        std::uint32_t high = 0;
        if (!read_u32(&low) || !read_u32(&high) || out_value == nullptr) {
            return false;
        }
        *out_value = static_cast<std::uint64_t>(low) |
                     (static_cast<std::uint64_t>(high) << 32u);
        return true;
    }
    bool read_float(float* out_value) {
        std::uint32_t bits = 0;
        if (!read_u32(&bits) || out_value == nullptr) {
            return false;
        }
        std::memcpy(out_value, &bits, sizeof(float));
        return true;
    }
    bool read_vec3(glm::vec3* out_value) {
        return out_value != nullptr && read_float(&out_value->x) &&
               read_float(&out_value->y) && read_float(&out_value->z);
    }
    bool read_quat(glm::quat* out_value) {
        return out_value != nullptr && read_float(&out_value->x) &&
               read_float(&out_value->y) && read_float(&out_value->z) &&
               read_float(&out_value->w);
    }
    bool read_bytes(void* out_data, std::size_t size) {
        if (size == 0) {
            return true;
        }
        if (!can_read(size) || out_data == nullptr) {
            return false;
        }
        std::memcpy(out_data, data_ + offset_, size);
        offset_ += size;
        return true;
    }

    bool done() const { return offset_ == size_; }

private:
    bool can_read(std::size_t bytes) const {
        return data_ != nullptr && offset_ + bytes <= size_;
    }

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
};

inline std::vector<std::uint8_t> wrap_packet(
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

inline bool unwrap_packet(
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

}  // namespace network_example::protocol_internal

#endif  // PROTOCOL_SRC_PACKET_CODEC_H_
