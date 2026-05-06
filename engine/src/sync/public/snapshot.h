#ifndef SYNC_PUBLIC_SNAPSHOT_H_
#define SYNC_PUBLIC_SNAPSHOT_H_

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "world/public/world.h"

namespace network_example {

struct SnapshotHeader {
    std::uint32_t server_tick = 0;
    std::uint32_t server_time_ms = 0;
    std::uint32_t last_processed_input_seq = 0;
};

struct EntitySnapshot {
    NetId net_id = 0;
    EntityType type = EntityType::kUnknown;
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::vec3 velocity{0.0f, 0.0f, 0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    std::uint16_t hp = 0;
    std::uint8_t state = 0;
    std::uint8_t flags = 0;
};

struct WorldSnapshot {
    SnapshotHeader header;
    std::vector<EntitySnapshot> entities;
};

WorldSnapshot build_world_snapshot(
    const World& world,
    std::uint32_t server_tick,
    std::uint32_t server_time_ms,
    std::uint32_t last_processed_input_seq);

}  // namespace network_example

#endif  // SYNC_PUBLIC_SNAPSHOT_H_
