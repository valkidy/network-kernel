#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "protocol/public/network_packets.h"

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
            static_cast<std::uint16_t>(1u + index),
        });
    }
    const std::vector<std::uint8_t> encoded =
        encode_status_effect_state_packet(packet, 11u);
    require(!encoded.empty());
    StatusEffectStatePacket decoded;
    require(decode_status_effect_state_packet(
        encoded.data(), encoded.size(), &decoded));
    require(decoded.server_tick == packet.server_tick);
    require(decoded.target_net_id == packet.target_net_id);
    require(decoded.revision == packet.revision);
    require(decoded.records.size() == kMaxActiveStatusEffects);
    require(decoded.records.back().status_instance_id == 2031u);
    require(decoded.records.back().stack_count == 32u);

    packet.records.push_back(StatusEffectStateRecord{
        2000u, 3000u, 3u, 80u, 140u});
    require(encode_status_effect_state_packet(packet).empty());

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
    require(!duplicate_bytes.empty());
    require(!decode_status_effect_state_packet(
        duplicate_bytes.data(), duplicate_bytes.size(), &decoded));

    StatusEffectStatePacket empty;
    empty.server_tick = 12u;
    empty.target_net_id = 7u;
    empty.revision = 2u;
    const std::vector<std::uint8_t> empty_bytes =
        encode_status_effect_state_packet(empty);
    require(!empty_bytes.empty());
    require(decode_status_effect_state_packet(
        empty_bytes.data(), empty_bytes.size(), &decoded));
    require(decoded.records.empty());

    StatusEffectStatePacket invalid_stack;
    invalid_stack.server_tick = 12u;
    invalid_stack.target_net_id = 7u;
    invalid_stack.revision = 3u;
    invalid_stack.records = {
        StatusEffectStateRecord{1u, 6u, 3u, 1u, 2u, 0u},
    };
    require(encode_status_effect_state_packet(invalid_stack).empty());
    invalid_stack.records[0].stack_count = 33u;
    require(encode_status_effect_state_packet(invalid_stack).empty());
    return 0;
}
