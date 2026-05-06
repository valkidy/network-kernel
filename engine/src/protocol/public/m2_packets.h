#ifndef PROTOCOL_PUBLIC_M2_PACKETS_H_
#define PROTOCOL_PUBLIC_M2_PACKETS_H_

#include <cstdint>
#include <vector>

#include "kernel/public/kernel_types.h"
#include "protocol/public/packet_header.h"
#include "sync/public/snapshot.h"
#include "world/public/components.h"

namespace network_example {

std::vector<std::uint8_t> encode_input_packet(
    PeerId player_id,
    const PlayerInput& input,
    std::uint32_t sequence = 0);

bool decode_input_packet(
    const std::uint8_t* data,
    std::size_t size,
    PeerId* out_player_id,
    PlayerInput* out_input);

std::vector<std::uint8_t> encode_snapshot_packet(
    const WorldSnapshot& snapshot,
    std::uint32_t sequence = 0);

bool decode_snapshot_packet(
    const std::uint8_t* data,
    std::size_t size,
    WorldSnapshot* out_snapshot);

}  // namespace network_example

#endif  // PROTOCOL_PUBLIC_M2_PACKETS_H_
