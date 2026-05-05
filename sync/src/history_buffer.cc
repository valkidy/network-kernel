#include "sync/public/history_buffer.h"

#include <algorithm>

namespace network_example {

HistoryBuffer::HistoryBuffer(std::uint32_t max_frames) : frames_(std::max(1u, max_frames)) {}

void HistoryBuffer::write_frame(const World& world, std::uint32_t server_tick) {
    HistoryFrame& frame = frames_[write_index_];
    frame.server_tick = server_tick;
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
        if (frame.server_tick == server_tick) {
            return &frame;
        }
    }
    return nullptr;
}

std::uint32_t HistoryBuffer::size() const {
    return static_cast<std::uint32_t>(frames_.size());
}

}  // namespace network_example
