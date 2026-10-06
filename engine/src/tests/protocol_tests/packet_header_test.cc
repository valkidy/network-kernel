#include <array>
#include <cstdio>
#include <cstdlib>

#include "protocol/public/packet_header.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

}  // namespace

#define require(condition) \
    require_impl(static_cast<bool>(condition), #condition, __LINE__)

int main() {
    const std::array<std::uint8_t, 3> payload = {1, 2, 3};
    network_example::PacketHeader header;
    header.message_type =
        static_cast<std::uint16_t>(network_example::MessageType::kPlayerInputPacket);
    header.sequence = 12;
    header.ack = 9;
    header.payload_size = payload.size();
    header.payload_crc = network_example::compute_payload_crc(payload.data(), payload.size());

    const network_example::EncodedPacketHeader encoded =
        network_example::encode_packet_header(header);
    network_example::PacketHeader decoded;
    require(network_example::decode_packet_header(encoded.data(), encoded.size(), &decoded));
    require(decoded.message_type == header.message_type);
    require(decoded.sequence == 12);
    require(decoded.ack == 9);
    require(decoded.payload_size == payload.size());
    require(decoded.payload_crc == header.payload_crc);

    auto corrupted = encoded;
    corrupted[0] = 0;
    require(!network_example::decode_packet_header(corrupted.data(), corrupted.size(), &decoded));
    return 0;
}
