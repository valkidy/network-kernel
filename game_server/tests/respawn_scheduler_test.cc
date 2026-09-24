// The respawn policy on its own: when a dead player's revive falls due, how the
// team's shared pool is spent, and who joins dead. No kernel involved.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "game_server/src/respawn_scheduler.h"

namespace {

using network_example::game_server::PlayerRespawnConfig;
using network_example::game_server::RespawnScheduler;

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "respawn_scheduler_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr float kTick = 1.0f / 30.0f;

PlayerRespawnConfig config_with(std::int32_t team_revive_times) {
    PlayerRespawnConfig config;
    config.delay_seconds = 4.0f;
    config.team_revive_times = team_revive_times;
    return config;
}

// Ticks until something is due, returning what came due and how many ticks it
// took. Gives up after `limit`.
std::vector<std::uint32_t> run_until_due(
    RespawnScheduler* scheduler,
    std::uint32_t limit,
    std::uint32_t* ticks) {
    for (*ticks = 1; *ticks <= limit; ++*ticks) {
        std::vector<std::uint32_t> ready = scheduler->advance(kTick);
        if (!ready.empty()) {
            return ready;
        }
    }
    return {};
}

// Four seconds at 30 Hz is the 120th tick after the death, not before.
void a_revive_falls_due_after_the_delay() {
    RespawnScheduler scheduler(config_with(-1));
    scheduler.on_player_died(7u);
    std::uint32_t ticks = 0;
    const std::vector<std::uint32_t> ready = run_until_due(&scheduler, 200u, &ticks);
    require(ready == std::vector<std::uint32_t>{7u});
    require(ticks == 120u);
    require(!scheduler.is_waiting(7u));
    // Returned once only.
    require(run_until_due(&scheduler, 200u, &ticks).empty());
}

void unlimited_never_runs_out() {
    RespawnScheduler scheduler(config_with(-1));
    for (int life = 0; life < 50; ++life) {
        scheduler.on_player_died(7u);
        std::uint32_t ticks = 0;
        require(!run_until_due(&scheduler, 200u, &ticks).empty());
    }
    require(scheduler.remaining_revives() == -1);
    require(!scheduler.joins_dead());
}

// N revives are N: the (N+1)th death stays dead, and from then on newcomers
// join dead.
void a_limited_pool_runs_out() {
    RespawnScheduler scheduler(config_with(2));
    std::uint32_t ticks = 0;
    for (int life = 0; life < 2; ++life) {
        scheduler.on_player_died(7u);
        require(!run_until_due(&scheduler, 200u, &ticks).empty());
    }
    require(scheduler.remaining_revives() == 0);
    require(scheduler.joins_dead());
    scheduler.on_player_died(7u);
    require(run_until_due(&scheduler, 400u, &ticks).empty());
    require(!scheduler.is_waiting(7u));
}

// Two deaths in one tick with one revive left: the lower net id gets it,
// whichever order the deaths were reported in.
void the_last_revive_goes_to_the_lower_net_id() {
    RespawnScheduler scheduler(config_with(1));
    scheduler.on_player_died(9u);
    scheduler.on_player_died(4u);
    std::uint32_t ticks = 0;
    require(run_until_due(&scheduler, 200u, &ticks) == std::vector<std::uint32_t>{4u});
    require(scheduler.remaining_revives() == 0);
}

// Leaving while waiting cancels the revive, and the reservation is not given
// back: a reconnect is a way back in, not a way to save a life.
void leaving_forfeits_the_reserved_revive() {
    RespawnScheduler scheduler(config_with(1));
    scheduler.on_player_died(7u);
    std::uint32_t ticks = 0;
    require(run_until_due(&scheduler, 10u, &ticks).empty());
    require(scheduler.is_waiting(7u));
    scheduler.on_player_left(7u);
    require(!scheduler.is_waiting(7u));
    require(run_until_due(&scheduler, 200u, &ticks).empty());
    require(scheduler.remaining_revives() == 0);
}

// A pool authored as 0 means no revives, not no players: nobody joins dead
// until somebody has actually died.
void an_empty_pool_does_not_kill_arrivals_before_any_death() {
    RespawnScheduler scheduler(config_with(0));
    require(scheduler.remaining_revives() == 0);
    require(!scheduler.joins_dead());
    scheduler.on_player_died(7u);
    std::uint32_t ticks = 0;
    require(run_until_due(&scheduler, 200u, &ticks).empty());
    require(scheduler.joins_dead());
}

}  // namespace

int main() {
    a_revive_falls_due_after_the_delay();
    unlimited_never_runs_out();
    a_limited_pool_runs_out();
    the_last_revive_goes_to_the_lower_net_id();
    leaving_forfeits_the_reserved_revive();
    an_empty_pool_does_not_kill_arrivals_before_any_death();
    std::puts("respawn_scheduler_test: ok");
    return 0;
}
