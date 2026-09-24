#include "game_server/src/respawn_scheduler.h"

#include <algorithm>

namespace network_example::game_server {

RespawnScheduler::RespawnScheduler(PlayerRespawnConfig config)
    : config_(config), remaining_(config.team_revive_times) {}

void RespawnScheduler::on_player_died(std::uint32_t net_id) {
    if (net_id != 0u) {
        died_.push_back(net_id);
    }
}

void RespawnScheduler::on_player_left(std::uint32_t net_id) {
    due_.erase(net_id);
    died_.erase(std::remove(died_.begin(), died_.end(), net_id), died_.end());
}

std::vector<std::uint32_t> RespawnScheduler::advance(float delta_seconds) {
    std::sort(died_.begin(), died_.end());
    died_.erase(std::unique(died_.begin(), died_.end()), died_.end());
    for (const std::uint32_t net_id : died_) {
        ++deaths_;
        if (remaining_ == 0) {
            continue;
        }
        if (remaining_ > 0) {
            --remaining_;
        }
        due_[net_id] = now_seconds_ + config_.delay_seconds;
    }
    died_.clear();

    now_seconds_ += delta_seconds;
    std::vector<std::uint32_t> ready;
    for (auto it = due_.begin(); it != due_.end();) {
        if (it->second <= now_seconds_) {
            ready.push_back(it->first);
            it = due_.erase(it);
        } else {
            ++it;
        }
    }
    return ready;
}

}  // namespace network_example::game_server
