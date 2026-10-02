// Packet schema 26: an entity spawn carries the tick a prop with a lifecycle was
// spawned on, separately from the tick the packet was sent on. The two differ
// whenever a client hears of the prop late -- joining after it landed, or the
// prop coming into range -- and only the first says when it expires.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

#include "protocol/public/network_packets.h"
#include "protocol/public/packet_header.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}
#define require(condition) require_impl((condition), #condition, __LINE__)

}  // namespace

int main() {
    namespace ne = network_example;
    ne::EntitySpawnPacket spawn{};
    spawn.net_id = 91;
    spawn.entity_type = ne::EntityType::kProp;
    spawn.server_tick = 4000;
    spawn.entity_template_id = 216;
    spawn.position = glm::vec3{1.0f, 0.0f, -2.0f};
    spawn.spawn_tick = 1234;

    const std::vector<std::uint8_t> packet = ne::encode_entity_spawn_packet(spawn, 7);
    // Header plus 77: the 73 the record was, and the u32 spawn tick.
    require(packet.size() == ne::kPacketHeaderSize + 77u);

    ne::EntitySpawnPacket decoded{};
    require(ne::decode_entity_spawn_packet(packet.data(), packet.size(), &decoded));
    require(decoded.net_id == 91u);
    require(decoded.server_tick == 4000u);
    require(decoded.spawn_tick == 1234u);
    require(decoded.entity_template_id == 216u);

    // A record short of the spawn tick is refused rather than read with a
    // garbage tick. The header is rebuilt to describe the shorter payload --
    // size and checksum both -- so it is the record length that turns it
    // away, not a header that disagrees with the data.
    std::vector<std::uint8_t> truncated = packet;
    truncated.pop_back();
    ne::PacketHeader header{};
    require(ne::decode_packet_header(truncated.data(), truncated.size(), &header));
    require(header.payload_size == 77u);
    header.payload_size = 76u;
    header.payload_crc = ne::compute_payload_crc(
        truncated.data() + ne::kPacketHeaderSize,
        header.payload_size);
    const ne::EncodedPacketHeader rewritten = ne::encode_packet_header(header);
    for (std::size_t index = 0; index < rewritten.size(); ++index) {
        truncated[index] = rewritten[index];
    }
    require(!ne::decode_entity_spawn_packet(truncated.data(), truncated.size(), &decoded));

    std::printf("entity_spawn_tick_roundtrip_test: PASS\n");
    return 0;
}
