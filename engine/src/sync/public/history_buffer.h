#ifndef SYNC_PUBLIC_HISTORY_BUFFER_H_
#define SYNC_PUBLIC_HISTORY_BUFFER_H_

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "world/public/world.h"

namespace network_example {

struct HitVolumeSnapshot {
    NetId net_id = 0;
    glm::vec3 center{0.0f, 0.0f, 0.0f};
    glm::vec3 half_extents{0.0f, 0.0f, 0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    std::uint8_t hit_zone = 0;
    std::uint8_t team = 0;
    std::uint8_t alive = 0;
};

struct HistoryFrame {
    std::uint32_t server_tick = 0;
    bool valid = false;
    std::vector<HitVolumeSnapshot> volumes;
};

struct HistoricalHitResult {
    NetId net_id = 0;
    float distance = 0.0f;
    glm::vec3 impact_position{0.0f, 0.0f, 0.0f};
    HitVolumeSnapshot volume;
};

class HistoryBuffer {
public:
    explicit HistoryBuffer(std::uint32_t max_frames);

    void write_frame(const World& world, std::uint32_t server_tick);
    const HistoryFrame* find_frame(std::uint32_t server_tick) const;
    const HistoryFrame* find_frame_clamped(std::uint32_t server_tick) const;
    bool empty() const;
    std::uint32_t oldest_tick() const;
    std::uint32_t newest_tick() const;
    std::uint32_t size() const;

private:
    std::vector<HistoryFrame> frames_;
    std::uint32_t write_index_ = 0;
};

bool raycast_history_frame(
    const HistoryFrame& frame,
    const glm::vec3& ray_origin,
    const glm::vec3& ray_direction,
    float max_range,
    NetId ignored_net_id,
    HistoricalHitResult* out_hit);

bool sweep_history_frame(
    const HistoryFrame& frame,
    const glm::vec3& segment_start,
    const glm::vec3& segment_end,
    NetId ignored_net_id,
    HistoricalHitResult* out_hit);

}  // namespace network_example

#endif  // SYNC_PUBLIC_HISTORY_BUFFER_H_
