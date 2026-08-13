#include <cassert>
#include <cstdint>
#include <vector>

#include "protocol/public/network_packets.h"

int main() {
    using namespace network_example;

    StatusEffectStatePacket packet;
    packet.server_tick = 100u;
    packet.target_net_id = 7u;
    packet.revision = 9u;
    for (std::uint32_t index = 0u; index < kMaxActiveStatusEffects; ++index) {
        packet.records.push_back(StatusEffectStateRecord{
            1000u + index,
            2000u + index,
            3u,
            80u,
            120u + index,
        });
    }
    const std::vector<std::uint8_t> encoded =
        encode_status_effect_state_packet(packet, 11u);
    assert(!encoded.empty());
    StatusEffectStatePacket decoded;
    assert(decode_status_effect_state_packet(
        encoded.data(), encoded.size(), &decoded));
    assert(decoded.server_tick == packet.server_tick);
    assert(decoded.target_net_id == packet.target_net_id);
    assert(decoded.revision == packet.revision);
    assert(decoded.records.size() == kMaxActiveStatusEffects);
    assert(decoded.records.back().status_instance_id == 2031u);

    packet.records.push_back(StatusEffectStateRecord{
        2000u, 3000u, 3u, 80u, 140u});
    assert(encode_status_effect_state_packet(packet).empty());

    StatusEffectStatePacket duplicate;
    duplicate.server_tick = 10u;
    duplicate.target_net_id = 7u;
    duplicate.revision = 1u;
    duplicate.records = {
        StatusEffectStateRecord{1u, 5u, 3u, 1u, 2u},
        StatusEffectStateRecord{2u, 5u, 3u, 1u, 2u},
    };
    const std::vector<std::uint8_t> duplicate_bytes =
        encode_status_effect_state_packet(duplicate);
    assert(!duplicate_bytes.empty());
    assert(!decode_status_effect_state_packet(
        duplicate_bytes.data(), duplicate_bytes.size(), &decoded));

    StatusEffectStatePacket empty;
    empty.server_tick = 12u;
    empty.target_net_id = 7u;
    empty.revision = 2u;
    const std::vector<std::uint8_t> empty_bytes =
        encode_status_effect_state_packet(empty);
    assert(!empty_bytes.empty());
    assert(decode_status_effect_state_packet(
        empty_bytes.data(), empty_bytes.size(), &decoded));
    assert(decoded.records.empty());
    return 0;
}
