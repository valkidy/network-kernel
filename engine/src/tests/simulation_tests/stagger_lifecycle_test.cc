// Hit stagger across the paths a real actor takes, not ones a test emplaces.
//
// A player is spawned bare by World::spawn_player and only ever gets its
// template from set_actor_template, so a StaggerProfile applied only in the
// entity-create path never reached a player at all. And death and revive
// removed StaggerState without the replicated flag, which only the action pass
// clears -- by walking StaggerState -- so dying mid-stagger left the actor
// flagged as staggered for the rest of the session.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

#include "simulation/public/simulation.h"
#include "simulation/src/systems.h"

// reset_runtime_state and the template tables are private; see
// impulse_lockout_test.cc for why this is how tests reach a server engine.
#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

using namespace network_example;

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "stagger_lifecycle_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::uint32_t kStaggeringTemplateId = 901u;
constexpr std::uint32_t kSteadyTemplateId = 902u;
constexpr float kThreshold = 120.0f;

KernelEntityTemplateDefinition actor_template(std::uint32_t id, float threshold) {
    KernelEntityTemplateDefinition entity_template{};
    entity_template.struct_size = sizeof(entity_template);
    entity_template.entity_template_id = id;
    entity_template.entity_type = KernelEntityType_Actor;
    entity_template.actor_template_id = id;
    entity_template.stagger_threshold = threshold;
    entity_template.stagger_per_damage = 1.0f;
    entity_template.stagger_ticks = 12u;
    entity_template.stagger_immunity_ticks = 60u;
    return entity_template;
}

struct Fixture {
    Fixture() : engine(server_config()) {
        engine.reset_runtime_state(KernelMode_DedicatedServer);
        for (const std::uint32_t id : {kStaggeringTemplateId, kSteadyTemplateId}) {
            KernelActorTemplateDefinition actor{};
            actor.actor_template_id = id;
            engine.actor_templates_.push_back(actor);
        }
        engine.entity_templates_.push_back(
            actor_template(kStaggeringTemplateId, kThreshold));
        engine.entity_templates_.push_back(actor_template(kSteadyTemplateId, 0.0f));
        // Exactly how a joining peer's actor comes into being.
        player = engine.world_.spawn_player(1u, glm::vec3{0.0f});
        entity = *engine.world_.find_entity(player);
        engine.world_.registry().get_or_emplace<Health>(entity) = Health{200, 200};
    }

    static KernelConfig server_config() {
        KernelConfig config{};
        config.mode = KernelMode_DedicatedServer;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        return config;
    }

    bool set_template(std::uint32_t id) {
        return EntityStateSystem{}.set_actor_template(engine, player, id);
    }

    std::vector<ConfirmedDamage> hit(std::uint16_t damage) {
        ConfirmedDamage confirmed;
        confirmed.target_net_id = player;
        confirmed.damage = damage;
        return apply_damage_applications(
            engine.world_, {confirmed}, engine.current_tick(), nullptr);
    }

    bool flagged() const {
        const ReplicationState* replication =
            engine.world_.registry().try_get<ReplicationState>(entity);
        return replication != nullptr &&
            (replication->visual_flags & kVisualFlagStaggered) != 0u;
    }

    bool staggered() const {
        return is_staggered(engine.world_, entity, engine.current_tick() + 1u);
    }

    KernelEngine engine;
    NetId player = 0;
    entt::entity entity = entt::null;
};

void a_player_gets_its_profile_from_set_actor_template() {
    Fixture f;
    // Spawned bare: no template has touched it yet.
    require(!f.engine.world_.registry().all_of<StaggerProfile>(f.entity));
    require(f.set_template(kStaggeringTemplateId));
    const StaggerProfile* profile =
        f.engine.world_.registry().try_get<StaggerProfile>(f.entity);
    require(profile != nullptr);
    require(profile->threshold == kThreshold);
    require(profile->stagger_ticks == 12u);

    // And it is live: a hit over the threshold staggers the player.
    f.hit(150u);
    require(f.staggered());
    require(f.flagged());
}

// The control for the one above: without the template the same hit does not
// stagger, and swapping to a template with no profile takes it away again.
void a_template_without_stagger_leaves_the_player_steady() {
    Fixture bare;
    bare.hit(150u);
    require(!bare.staggered());

    Fixture swapped;
    require(swapped.set_template(kStaggeringTemplateId));
    require(swapped.set_template(kSteadyTemplateId));
    require(!swapped.engine.world_.registry().all_of<StaggerProfile>(swapped.entity));
    swapped.hit(150u);
    require(!swapped.staggered());
}

void dying_mid_stagger_clears_the_flag() {
    Fixture f;
    require(f.set_template(kStaggeringTemplateId));
    f.hit(150u);
    require(f.flagged());

    const std::vector<ConfirmedDamage> depleted = f.hit(100u);
    require(depleted.size() == 1u);
    EntityLifecycleSystem{}.enter_death_state(f.engine, depleted);
    require(!f.engine.world_.registry().all_of<StaggerState>(f.entity));
    require(!f.flagged());
}

void a_revive_clears_the_flag() {
    Fixture f;
    require(f.set_template(kStaggeringTemplateId));
    f.hit(150u);
    require(f.flagged());
    // Dead without passing through enter_death_state, so only the revive's
    // own cleanup stands between the flag and the next life.
    f.engine.world_.registry().get<Health>(f.entity).hp = 0u;
    require(EntityStateSystem{}.revive(f.engine, f.player, 0.0f, 0u));
    require(!f.engine.world_.registry().all_of<StaggerState>(f.entity));
    require(!f.flagged());
    // The profile survives: the next life can be staggered again.
    require(f.engine.world_.registry().all_of<StaggerProfile>(f.entity));
}

}  // namespace

int main() {
    a_player_gets_its_profile_from_set_actor_template();
    a_template_without_stagger_leaves_the_player_steady();
    dying_mid_stagger_clears_the_flag();
    a_revive_clears_the_flag();
    std::puts("stagger_lifecycle_test: ok");
    return 0;
}
