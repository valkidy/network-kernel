#ifndef GAME_SERVER_AGENT_BUDGET_H_
#define GAME_SERVER_AGENT_BUDGET_H_

#include <cstdint>
#include <limits>

namespace network_example::game_server {

// The catalog's agent_budget, as one tick sees it: the ceiling, and how many
// agents are alive or already promised this tick.
//
// Built by the agent runtime at the top of a tick from the actor snapshot,
// then handed to each director that fills room when there is room -- patrols,
// then spawners -- which take only whole groups or waves that fit and spend
// what they create, so the next one in line sees what is left. Mission and
// world rules never ask; what they create is counted, not refused. A game
// rule's wave or an action graph's spawn is counted from the next tick's
// snapshot, which is what makes the ceiling soft by that much.
struct AgentBudget {
    // Zero is unbounded.
    std::uint32_t ceiling = 0;
    std::uint32_t live = 0;

    std::uint32_t room() const {
        if (ceiling == 0u) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        return live >= ceiling ? 0u : ceiling - live;
    }

    void spend(std::uint32_t agents) { live += agents; }
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_BUDGET_H_
