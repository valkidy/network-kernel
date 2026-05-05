#include <array>
#include <cassert>

#include "protocol/public/packet_header.h"

int main() {
    const std::array<std::uint8_t, 3> payload = {1, 2, 3};
    network_example::PacketHeader header;
    header.message_type =
        static_cast<std::uint16_t>(network_example::MessageType::kInputPacket);
    header.sequence = 12;
    header.ack = 9;
    header.payload_size = payload.size();
    header.payload_crc = network_example::compute_payload_crc(payload.data(), payload.size());

    const network_example::EncodedPacketHeader encoded =
        network_example::encode_packet_header(header);
    network_example::PacketHeader decoded;
    assert(network_example::decode_packet_header(encoded.data(), encoded.size(), &decoded));
    assert(decoded.message_type == header.message_type);
    assert(decoded.sequence == 12);
    assert(decoded.ack == 9);
    assert(decoded.payload_size == payload.size());
    assert(decoded.payload_crc == header.payload_crc);

    auto corrupted = encoded;
    corrupted[0] = 0;
    assert(!network_example::decode_packet_header(corrupted.data(), corrupted.size(), &decoded));
    return 0;
}
