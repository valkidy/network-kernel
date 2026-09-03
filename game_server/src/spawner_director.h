#ifndef GAME_SERVER_SPAWNER_DIRECTOR_H_
#define GAME_SERVER_SPAWNER_DIRECTOR_H_

#include <cstdint>
#include <string>
#include <vector>

#include "game_server/src/spawn_sampling.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

// A rule carried by a thing in the world: every so often, put this many units
// out around me.
//
// It is authored on an entity template rather than at catalog top level, and
// that is the whole point. A rule that lives on the carrier lives exactly as
// long as the carrier: destroy the nest and its rule goes with it, with no
// "anchor died, stop spawning" bookkeeping to get wrong. Three nests are three
// instances of one template, each with its own cadence and its own ceiling,
// rather than three near-identical authored rules.
//
// It never reaches the kernel, the same way `ai.sentry` and `ai.patrol` do not:
// game_server parses it off the template and runs it.
struct SpawnerConfig {
    bool authored = false;
    // Mixed with the carrier's net id, so two nests of one template do not put
    // out identical waves. That makes replays depend on entity creation order
    // being deterministic rather than on this value alone -- which holds, since
    // net ids are handed out in order, but it is a weaker promise than the one
    // a top-level rule can make.
    std::uint32_t seed = 1;
    std::uint32_t interval_ticks = 300;
    // Alive at once, from this carrier. Zero is unbounded, which for a periodic
    // emitter means it floods; authoring one is strongly advised.
    //
    // Waves are whole: a nest with room for two and a wave of three puts out
    // nothing and waits, rather than a wave of two. A wave cannot be trimmed,
    // because the composition's minimums are assigned before anything is drawn
    // -- asking for fewer than they sum to returns them anyway. So a ceiling
    // that is not a comfortable multiple of the wave size leaves headroom
    // unused, and validate_spawner_config refuses one below a whole wave.
    std::uint32_t max_live_agents = 0;
    // Where they come out: a disc centred on the carrier, wherever it is now.
    float radius = 0.0f;
    std::uint32_t count_min = 1;
    std::uint32_t count_max = 1;
    std::vector<SpawnCompositionEntry> composition;
};

// A template that carries a spawner, and what kind of entity it is -- the kind
// is what the director queries for, so a catalog with no spawners costs no
// queries at all.
struct SpawnerCarrierConfig {
    std::uint32_t entity_template_id = 0;
    std::uint16_t entity_type = 0;
    std::string name;
    SpawnerConfig spawner;
};

std::string validate_spawner_config(const SpawnerConfig& spawner);

// Runs one rule instance per live carrier.
class SpawnerDirector {
public:
    explicit SpawnerDirector(std::vector<SpawnerCarrierConfig> carriers = {});

    void tick(KernelHandle* kernel);

    struct Instance {
        std::uint32_t carrier_net_id = 0;
        std::uint32_t entity_template_id = 0;
        std::uint32_t ticks_until_spawn = 0;
        std::uint32_t spawn_ordinal = 0;
        // What this carrier has put out and not yet lost, which is what the
        // ceiling counts. Units outlive their carrier on purpose: despawning
        // what a player has just fought their way through, at the moment they
        // destroy the nest, is worse than any population it would save.
        std::vector<std::uint32_t> spawned_net_ids;
    };

    const std::vector<SpawnerCarrierConfig>& carriers() const;
    const std::vector<Instance>& instances() const;
    std::uint32_t spawned_unit_count() const;

private:
    const SpawnerCarrierConfig* carrier_for(std::uint32_t entity_template_id) const;

    std::vector<SpawnerCarrierConfig> carriers_;
    std::vector<std::uint16_t> queried_entity_types_;
    std::vector<Instance> instances_;
    std::vector<KernelServerEntityState> query_buffer_;
    std::uint32_t spawned_unit_count_ = 0;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_SPAWNER_DIRECTOR_H_
