#include "sync/public/history_buffer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace network_example {
namespace {

bool ray_intersects_aabb(
    const glm::vec3& ray_origin,
    const glm::vec3& ray_direction,
    const glm::vec3& box_center,
    const glm::vec3& box_half_extents,
    float* out_distance) {
    float t_min = 0.0f;
    float t_max = std::numeric_limits<float>::max();
    const glm::vec3 min_corner = box_center - box_half_extents;
    const glm::vec3 max_corner = box_center + box_half_extents;

    for (int axis = 0; axis < 3; ++axis) {
        const float origin = ray_origin[axis];
        const float direction = ray_direction[axis];
        if (std::abs(direction) < 0.000001f) {
            if (origin < min_corner[axis] || origin > max_corner[axis]) {
                return false;
            }
            continue;
        }

        float t1 = (min_corner[axis] - origin) / direction;
        float t2 = (max_corner[axis] - origin) / direction;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        t_min = std::max(t_min, t1);
        t_max = std::min(t_max, t2);
        if (t_min > t_max) {
            return false;
        }
    }

    if (out_distance != nullptr) {
        *out_distance = t_min;
    }
    return true;
}

}  // namespace

HistoryBuffer::HistoryBuffer(std::uint32_t max_frames) : frames_(std::max(1u, max_frames)) {}

void HistoryBuffer::write_frame(const World& world, std::uint32_t server_tick) {
    HistoryFrame& frame = frames_[write_index_];
    frame.server_tick = server_tick;
    frame.valid = true;
    frame.volumes.clear();

    auto view = world.registry().view<
        const NetworkIdentity,
        const Transform,
        const Hitbox>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<const NetworkIdentity>(entity);
        const Transform& transform = view.get<const Transform>(entity);
        const Hitbox& hitbox = view.get<const Hitbox>(entity);
        const bool alive =
            !world.registry().all_of<Health>(entity) ||
            world.registry().get<Health>(entity).hp > 0;
        frame.volumes.push_back(HitVolumeSnapshot{
            identity.net_id,
            transform.position + hitbox.center,
            hitbox.half_extents,
            transform.rotation,
            hitbox.hit_zone,
            0,
            static_cast<std::uint8_t>(alive ? 1 : 0),
        });
    }

    write_index_ = (write_index_ + 1) % frames_.size();
}

const HistoryFrame* HistoryBuffer::find_frame(std::uint32_t server_tick) const {
    for (const HistoryFrame& frame : frames_) {
        if (frame.valid && frame.server_tick == server_tick) {
            return &frame;
        }
    }
    return nullptr;
}

const HistoryFrame* HistoryBuffer::find_frame_clamped(std::uint32_t server_tick) const {
    if (empty()) {
        return nullptr;
    }

    const std::uint32_t clamped_tick =
        std::clamp(server_tick, oldest_tick(), newest_tick());
    if (const HistoryFrame* exact = find_frame(clamped_tick)) {
        return exact;
    }

    const HistoryFrame* nearest = nullptr;
    std::uint32_t nearest_distance = std::numeric_limits<std::uint32_t>::max();
    for (const HistoryFrame& frame : frames_) {
        if (!frame.valid) {
            continue;
        }
        const std::uint32_t distance = frame.server_tick > clamped_tick
                                           ? frame.server_tick - clamped_tick
                                           : clamped_tick - frame.server_tick;
        if (distance < nearest_distance) {
            nearest = &frame;
            nearest_distance = distance;
        }
    }
    return nearest;
}

bool HistoryBuffer::empty() const {
    return std::none_of(frames_.begin(), frames_.end(), [](const HistoryFrame& frame) {
        return frame.valid;
    });
}

std::uint32_t HistoryBuffer::oldest_tick() const {
    std::uint32_t oldest = std::numeric_limits<std::uint32_t>::max();
    for (const HistoryFrame& frame : frames_) {
        if (frame.valid) {
            oldest = std::min(oldest, frame.server_tick);
        }
    }
    return oldest == std::numeric_limits<std::uint32_t>::max() ? 0 : oldest;
}

std::uint32_t HistoryBuffer::newest_tick() const {
    std::uint32_t newest = 0;
    for (const HistoryFrame& frame : frames_) {
        if (frame.valid) {
            newest = std::max(newest, frame.server_tick);
        }
    }
    return newest;
}

std::uint32_t HistoryBuffer::size() const {
    return static_cast<std::uint32_t>(frames_.size());
}

bool raycast_history_frame(
    const HistoryFrame& frame,
    const glm::vec3& ray_origin,
    const glm::vec3& ray_direction,
    float max_range,
    NetId ignored_net_id,
    HistoricalHitResult* out_hit) {
    if (!frame.valid) {
        return false;
    }

    float best_distance = max_range;
    HistoricalHitResult best_hit;
    bool has_hit = false;
    for (const HitVolumeSnapshot& volume : frame.volumes) {
        if (volume.net_id == ignored_net_id || volume.alive == 0) {
            continue;
        }

        float distance = 0.0f;
        if (ray_intersects_aabb(
                ray_origin,
                ray_direction,
                volume.center,
                volume.half_extents,
                &distance) &&
            distance <= best_distance) {
            best_distance = distance;
            best_hit = HistoricalHitResult{
                volume.net_id,
                distance,
                ray_origin + ray_direction * distance,
                volume};
            has_hit = true;
        }
    }

    if (has_hit && out_hit != nullptr) {
        *out_hit = best_hit;
    }
    return has_hit;
}

bool sweep_history_frame(
    const HistoryFrame& frame,
    const glm::vec3& segment_start,
    const glm::vec3& segment_end,
    NetId ignored_net_id,
    HistoricalHitResult* out_hit) {
    const glm::vec3 displacement = segment_end - segment_start;
    const float length = glm::length(displacement);
    if (length <= 0.0001f) {
        return false;
    }
    return raycast_history_frame(
        frame,
        segment_start,
        displacement / length,
        length,
        ignored_net_id,
        out_hit);
}

}  // namespace network_example
