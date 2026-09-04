#ifndef GAME_SERVER_GAME_RULE_DIRECTOR_H_
#define GAME_SERVER_GAME_RULE_DIRECTOR_H_

#include <cstdint>
#include <string>
#include <vector>

#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

enum class GameRuleConditionType : std::uint8_t {
    kGroupEliminated = 0,
    kPlayerCountAtLeast = 1,
};

enum class GameRuleNodeState : std::uint8_t {
    kInactive = 0,
    kActive = 1,
    kCompleted = 2,
};

enum class GameRuleStatus : std::uint8_t {
    kRunning = 0,
    kCompleted = 1,
    kFailed = 2,
};

// What activating a node spawns. One group per node at most, which is what the
// authoring allows.
struct GameRuleSpawnEffectConfig {
    std::uint32_t group_id = 0;
    std::uint32_t count = 0;
    std::uint32_t entity_template_id = 0;
    KernelVec3 position{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    std::uint32_t seed = 1;
};

struct GameRuleNodeConfig {
    std::uint32_t node_id = 0;
    GameRuleConditionType condition_type =
        GameRuleConditionType::kGroupEliminated;
    std::uint32_t condition_group_id = 0;
    std::uint32_t condition_count = 0;
    std::vector<std::uint32_t> next_node_ids;
    bool has_spawn_effect = false;
    GameRuleSpawnEffectConfig spawn;
};

// One authored mission flow: an acyclic graph of nodes, each waiting on a
// condition and each optionally spawning a group when it opens.
struct GameRuleConfig {
    std::uint32_t director_template_id = 0;
    std::string name;
    std::uint32_t tick_interval = 1;
    std::vector<GameRuleNodeConfig> nodes;
};

// Runs the authored mission flows.
//
// This used to be a director entity carrying GameRuleRuntime, ticked by the
// kernel, emitting SpawnGroup intents that a kernel executor validated back
// against catalog structs in the kernel ABI, with group bookkeeping wired into
// the entity-creation dispatcher. None of that is something the kernel needs:
// "this wave is cleared, open the next one" is a mission rule.
//
// Spawning here is synchronous -- Kernel_ServerCreateEntity returns the net id
// -- which removes the whole pending/sealed dance the kernel version needed.
// That existed because creation went through a command queue, so a group could
// not know when it was fully populated. It knows immediately.
class GameRuleDirector {
public:
    explicit GameRuleDirector(std::vector<GameRuleConfig> rules = {});

    void tick(KernelHandle* kernel);

    // No `sealed` flag. The kernel version needed one -- it dispatched creates
    // through a command queue, so a group could not know when it was fully
    // populated, and an empty-but-unfilled group would have read as cleared.
    // Here a node is activated, its group filled and its members recorded
    // inside one call, so there is no tick on which an active node can see a
    // group that has not been filled yet. A flag nothing can observe is a flag
    // that will eventually be believed; this was checked by mutation, and
    // removing the check changed no behaviour.
    struct GroupRuntime {
        std::uint32_t group_id = 0;
        std::vector<std::uint32_t> member_net_ids;
        bool failed = false;
    };

    struct RuleRuntime {
        bool initialized = false;
        GameRuleStatus status = GameRuleStatus::kRunning;
        std::uint32_t ticks_until_update = 0;
        std::vector<GameRuleNodeState> node_states;
        std::vector<GroupRuntime> groups;
    };

    const std::vector<GameRuleConfig>& rules() const;
    const std::vector<RuleRuntime>& runtimes() const;

private:
    void activate_node(
        KernelHandle* kernel,
        const GameRuleConfig& rule,
        RuleRuntime* runtime,
        std::size_t node_offset);

    std::vector<GameRuleConfig> rules_;
    std::vector<RuleRuntime> runtimes_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_GAME_RULE_DIRECTOR_H_
