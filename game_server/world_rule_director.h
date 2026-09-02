#ifndef GAME_SERVER_WORLD_RULE_DIRECTOR_H_
#define GAME_SERVER_WORLD_RULE_DIRECTOR_H_

#include <cstdint>
#include <string>
#include <vector>

#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

// One authored world rule: keep this many agents alive, spawning them on a ring
// around a point.
//
// This used to be a director entity in the world, with a WorldRuleRuntime
// component, emitting a SpawnAgent intent that a kernel executor validated back
// against that component. None of that was doing anything the kernel needed: a
// population ceiling is a gameplay rule, and the kernel's job is to store and
// mutate entities, not to decide how many of them there should be.
struct WorldRuleSpawnConfig {
    // The director entity template this came from. Identity only; nothing is
    // spawned from it any more.
    std::uint32_t director_template_id = 0;
    std::string name;

    std::uint32_t spawn_entity_template_id = 0;
    std::uint32_t spawn_actor_template_id = 0;
    std::uint32_t target_count = 0;
    KernelVec3 position{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    std::uint32_t tick_interval = 1;
};

// Tops the world's agent population back up to each rule's target.
//
// The count it works against is every agent alive, not just the ones this rule
// spawned -- which is what the kernel version counted, and is kept deliberately.
// It means a patrol squad in the world reduces how many of these spawn, and that
// is a real interaction rather than an accident: both are the world's own
// population.
class WorldRuleDirector {
public:
    explicit WorldRuleDirector(std::vector<WorldRuleSpawnConfig> rules = {});

    // `live_agent_count` is every agent in the world, counted by the caller
    // because it already has the list.
    void tick(KernelHandle* kernel, std::uint32_t live_agent_count);

    const std::vector<WorldRuleSpawnConfig>& rules() const;
    std::uint32_t spawned_agent_count() const;

private:
    struct RuleRuntime {
        std::uint32_t ticks_until_spawn = 0;
        // Where the ring is picked up from, so a rule that tops up twice does
        // not stack the second batch on top of the first.
        //
        // It starts at zero rather than at the authored `seed`. That is what the
        // kernel did: the seed was stored on the component, authored in the
        // catalog and included in the catalog hash, and never reached the
        // placement maths, which read the cursor. Preserved rather than fixed --
        // making the seed matter is a behaviour change, and this is a move.
        std::uint32_t spawn_cursor = 0;
    };

    std::vector<WorldRuleSpawnConfig> rules_;
    std::vector<RuleRuntime> runtimes_;
    std::uint32_t spawned_agent_count_ = 0;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_WORLD_RULE_DIRECTOR_H_
