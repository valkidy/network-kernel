// The sentry AI's knobs belong to the agent, not to the controller.
//
// They used to belong to the controller: AgentRuntimeManager built one
// AgentSentryConfig from the catalog's `enemy:` entry and ticked every agent
// through it. That silently broke any agent spawned from a different template.
// A game-rule wave spawning tripod_actor (passive_patrol: true) alongside a
// catalog whose `enemy:` named grenade_sentry (no passive_patrol) produced a
// tripod that never patrolled -- it ran the artillery sentry's config, and its
// own ai.sentry block was parsed, validated, and then never read. (The tripod
// is a patrol walking a behavior.route now, AI plan D19; the bug this
// guards against is the same.)
//
// Two layers, one test each:
//   1. the controller reads each agent's own config, so one instance drives a
//      mixed population;
//   2. the manager resolves that config from the template the entity was
//      actually spawned from, not from `enemy:`.
#include "game_server/src/agent_sentry_controller.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::uint32_t kSentryGruntTemplateId = 2;
constexpr std::uint32_t kTripodTemplateId = 22;

KernelConfig server_config() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;
    config.max_events = 64;
    config.max_render_states = 64;
    return config;
}

KernelConfig listen_server_config() {
    KernelConfig config = server_config();
    config.mode = KernelMode_ListenServer;
    return config;
}

std::uint32_t create_agent(KernelHandle* kernel, const KernelVec3& position) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = network_example::game_server::kEntityTypeActor;
    create_info.actor_type = network_example::game_server::kActorTypeAgent;
    create_info.position = position;
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    require(net_id != 0);
    return net_id;
}

// Two sentry configs that differ in what an idle tick visibly does: one looks
// around every tick, the other never does.
network_example::game_server::AgentSentryConfig sentry_config(
    std::uint32_t rotation_interval_ticks) {
    network_example::game_server::AgentSentryConfig config;
    config.patrol_rotation_interval_ticks = rotation_interval_ticks;
    return config;
}

// One controller, two agents, two configs. Before the fix both agents took
// whichever branch the single controller-wide config selected.
void one_controller_drives_a_mixed_population() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7788));

    const std::uint32_t walker_net_id =
        create_agent(kernel, KernelVec3{0.0f, 0.0f, 0.0f});
    const std::uint32_t artillery_net_id =
        create_agent(kernel, KernelVec3{0.0f, 0.0f, 8.0f});
    Kernel_Update(kernel, 1.0f / 30.0f);

    std::vector<network_example::game_server::AgentRuntimeState> agents(2);
    agents[0].net_id = walker_net_id;
    agents[0].sentry_config = sentry_config(1);
    agents[1].net_id = artillery_net_id;
    agents[1].sentry_config = sentry_config(0);

    const network_example::game_server::AgentSentryController controller;
    // AgentRuntimeManager takes the perception frame once per tick off the
    // actor snapshot it already has; a test driving the controller directly has
    // the frame take its own.
    network_example::game_server::PerceptionFrame frame;
    frame.refresh(kernel);
    controller.tick(
        kernel,
        frame,
        network_example::game_server::whole_batch(&agents),
        1.0f / 30.0f);

    // Each looks around on its own config's schedule, ticked by the same
    // controller in the same call; neither moves.
    require(agents[0].sentry.patrol_rotation_step == 1u);
    require(agents[1].sentry.patrol_rotation_step == 0u);
    for (const network_example::game_server::AgentRuntimeState& agent : agents) {
        require(agent.velocity.x == 0.0f && agent.velocity.z == 0.0f);
        require(
            agent.sentry.state ==
            network_example::game_server::AgentSentryState::kIdle);
    }

    Kernel_Destroy(kernel);
}

// The manager resolves each agent's config from the template the entity was
// spawned from. The catalog's `enemy:` entry names a different template on
// purpose: that mismatch is exactly the shipping situation (a wave spawns
// tripod_actor while `enemy:` names grenade_sentry) and it is what used to
// decide the AI.
void manager_resolves_config_from_the_spawned_template() {
    network_example::game_server::GameServerGameplayConfig gameplay_config =
        network_example::game_server::default_game_server_gameplay_config();

    const network_example::game_server::ActorTemplateConfig* tripod =
        find_actor_template(gameplay_config, kTripodTemplateId);
    require(tripod != nullptr);
    require(tripod->default_controller_role ==
            network_example::game_server::AgentControllerRole::kGuard);

    // The divergence this test is built on: the grunt is a stationary sentry
    // and the tripod a guard, so an agent reading anyone's config but its
    // own is visible in whether it walks the route below.
    const network_example::game_server::ActorTemplateConfig* grunt =
        find_actor_template(gameplay_config, kSentryGruntTemplateId);
    require(grunt != nullptr);
    require(grunt->default_controller_role ==
            network_example::game_server::AgentControllerRole::kSentry);

    // Spawn the tripod from a world rule stated here. There used to be an
    // `enemy:` entry to defeat first: it rewrote every world rule's spawn to
    // one catalog-wide template, which collapsed the divergence above. That
    // mechanism is gone, so the rule is simply the one this test writes.
    gameplay_config.preload_director_template_ids.clear();
    // The rules the catalog extracted go too. Directors are game_server config
    // now, so editing the entity templates below no longer reaches them --
    // this test states the rule it wants directly.
    gameplay_config.world_rule_spawns.clear();
    gameplay_config.game_rules.clear();
    for (network_example::game_server::EntityTemplateConfig& entity_template :
         gameplay_config.entity_templates) {
        if (entity_template.entity_type != KernelEntityType_Director ||
            entity_template.director_kind != network_example::game_server::AuthoredDirectorKind::kWorldRule) {
            continue;
        }
        entity_template.director_spawn_target_count = 1;
        entity_template.director_spawn_radius = 0.0f;
        entity_template.director_spawn_position = KernelVec3{6.0f, 0.0f, 0.0f};
        entity_template.director_spawn_entity_template_id = kTripodTemplateId;
        entity_template.director_spawn_entity_template_ref = "tripod_actor";
        gameplay_config.preload_director_template_ids.push_back(
            entity_template.actor_template_id);
        // A world rule is game_server's own now rather than a director entity,
        // so a config assembled in code has to carry the rule as well as the
        // template it came from. The catalog path does this in
        // apply_catalog_world_rule_config.
        network_example::game_server::WorldRuleSpawnConfig rule;
        rule.director_template_id = entity_template.actor_template_id;
        rule.name = entity_template.name;
        rule.target_count = entity_template.director_spawn_target_count;
        rule.spawn_entity_template_id =
            entity_template.director_spawn_entity_template_id;
        rule.position = entity_template.director_spawn_position;
        rule.radius = entity_template.director_spawn_radius;
        // Offsets from the spawn point, as the shipped world rules author it.
        rule.controller.route = {
            KernelVec3{-30.0f, 0.0f, 0.0f},
            KernelVec3{30.0f, 0.0f, 0.0f},
        };
        gameplay_config.world_rule_spawns.push_back(rule);
    }
    require(!gameplay_config.preload_director_template_ids.empty());
    require(!gameplay_config.world_rule_spawns.empty());

    KernelConfig config = listen_server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartListenServer(kernel, 7790));

    network_example::game_server::GameServer game_server(kernel, gameplay_config);
    require(game_server.preload_directors());
    for (int frame = 0; frame < 3; ++frame) {
        game_server.tick(1.0f / 30.0f);
        Kernel_Update(kernel, 1.0f / 30.0f);
    }

    const std::vector<network_example::game_server::AgentRuntimeState>& agents =
        game_server.agent_runtime_manager().agents();
    require(agents.size() == 1);
    // The tripod's own template, not the grunt's: a patrol, by the route its
    // world rule gave it.
    require(agents[0].controller_role ==
            network_example::game_server::AgentControllerRole::kPatrol);
    require(!agents[0].route_patrol.path.empty());
    require(
        agents[0].sentry_config.move_speed_meters_per_second ==
        tripod->move_speed_meters_per_second);
    require(agents[0].sentry_config.weapon_id == tripod->sentry.weapon_id);

    // Config resolution is only half the story -- the point of the fix is that
    // the thing actually walks. It joins the route 30 m from where it spawned,
    // so a run of frames has to move it along x, one way or the other.
    const std::uint32_t agent_net_id = agents[0].net_id;
    KernelServerEntityState before{};
    before.struct_size = sizeof(before);
    require(Kernel_ServerGetEntityState(kernel, agent_net_id, &before));
    // There is no ground here, so it walks while it falls. A second is enough
    // to cover the metre (about 2.8 m) and keeps it above the kill floor,
    // which it crosses at about 1.3 s and is removed for.
    for (int frame = 0; frame < 30; ++frame) {
        game_server.tick(1.0f / 30.0f);
        Kernel_Update(kernel, 1.0f / 30.0f);
    }
    KernelServerEntityState after{};
    after.struct_size = sizeof(after);
    require(Kernel_ServerGetEntityState(kernel, agent_net_id, &after));
    require(after.valid != 0u);
    require(std::fabs(after.position.x - before.position.x) > 1.0f);

    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    one_controller_drives_a_mixed_population();
    manager_resolves_config_from_the_spawned_template();
    std::printf("agent_sentry_per_agent_config_test: PASS\n");
    return 0;
}
