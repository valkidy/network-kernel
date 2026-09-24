#ifndef GAME_SERVER_SRC_RESPAWN_SCHEDULER_H_
#define GAME_SERVER_SRC_RESPAWN_SCHEDULER_H_

#include <cstdint>
#include <map>
#include <vector>

#include "game_server/src/gameplay_config.h"

namespace network_example::game_server {

// When dead players come back, and whether they may. Pure bookkeeping: it
// neither reads nor writes the kernel, so the policy is testable on its own and
// GameServer does the reviving.
//
// The team shares one pool of revives. A death reserves one the moment it is
// resolved, not when the revive lands, so two deaths racing for the last one
// are settled here, in net id order, rather than by whose timer ends first.
// A player who leaves while waiting loses the revive it reserved.
class RespawnScheduler {
public:
    explicit RespawnScheduler(PlayerRespawnConfig config);

    // Queued, not decided: advance() resolves every death of a tick together.
    void on_player_died(std::uint32_t net_id);
    void on_player_left(std::uint32_t net_id);

    // Resolves the deaths queued since the last call, moves the clock on by
    // delta_seconds, and returns the players whose revive is now due, in net
    // id order. Each is returned once.
    std::vector<std::uint32_t> advance(float delta_seconds);

    // -1 while unlimited.
    std::int32_t remaining_revives() const { return remaining_; }
    // A newcomer joins dead once the team is out of revives -- but only after
    // someone has actually died, or a pool authored as 0 would leave every
    // player dead on arrival.
    bool joins_dead() const { return remaining_ == 0 && deaths_ != 0u; }
    bool is_waiting(std::uint32_t net_id) const {
        return due_.find(net_id) != due_.end();
    }

private:
    PlayerRespawnConfig config_;
    std::int32_t remaining_;
    std::uint32_t deaths_ = 0;
    double now_seconds_ = 0.0;
    std::vector<std::uint32_t> died_;
    std::map<std::uint32_t, double> due_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_SRC_RESPAWN_SCHEDULER_H_
