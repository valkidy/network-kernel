#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <entt/entt.hpp>

#include "kernel/public/kernel_api.h"

#define private public
#include "kernel/src/kernel.h"
#include "simulation/src/systems.h"

namespace {

// assert() is compiled out under -c opt, which is the configuration this suite
// runs in, so every check in this file was previously not being run at all.
// This is the fourth file in this area found that way.
void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

}  // namespace

#undef private

namespace {

KernelColliderTemplateDefinition hit_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 20u;
    collider.shape_type = KernelColliderShapeType_Aabb;
    collider.center = KernelVec3{0.0f, 0.8f, 0.0f};
    collider.shape_params = KernelVec4{0.4f, 0.8f, 0.4f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Hit;
    collider.layer_mask = KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
    return collider;
}

KernelColliderTemplateDefinition movement_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 21u;
    collider.shape_type = KernelColliderShapeType_Capsule;
    collider.center = KernelVec3{0.0f, 0.9f, 0.0f};
    collider.shape_params = KernelVec4{0.55f, 0.35f, 0.0f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Movement;
    collider.layer_mask = KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
    return collider;
}

KernelActorTemplateDefinition agent_actor_template() {
    KernelActorTemplateDefinition actor{};
    actor.struct_size = sizeof(actor);
    actor.actor_template_id = 2u;
    actor.entity_type = KernelEntityType_Actor;
    actor.actor_type = KernelActorType_Agent;
    actor.collider_template_id = 20u;
    actor.vision.struct_size = sizeof(actor.vision);
    actor.vision.camp = KernelAgentCamp_EnemySide;
    return actor;
}

KernelEntityTemplateDefinition agent_entity_template() {
    KernelEntityTemplateDefinition entity{};
    entity.struct_size = sizeof(entity);
    entity.entity_template_id = 200u;
    entity.entity_type = KernelEntityType_Actor;
    entity.actor_type = KernelActorType_Agent;
    entity.actor_template_id = 2u;
    entity.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM | KERNEL_ENTITY_COMPONENT_HEALTH |
        KERNEL_ENTITY_COMPONENT_HITBOX | KERNEL_ENTITY_COMPONENT_AGENT_RUNTIME |
        KERNEL_ENTITY_COMPONENT_SENTRY_RUNTIME;
    entity.collider_template_id = 20u;
    entity.combat.struct_size = sizeof(entity.combat);
    entity.combat.hp = 10u;
    entity.combat.max_hp = 10u;
    entity.combat.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    entity.combat.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    entity.vision.struct_size = sizeof(entity.vision);
    entity.vision.camp = KernelAgentCamp_EnemySide;
    entity.ai.struct_size = sizeof(entity.ai);
    entity.ai.controller_type = KernelAiControllerType_Sentry;
    entity.ai.tick_interval = 1u;
    entity.movement.struct_size = sizeof(entity.movement);
    entity.movement.controller_type = KernelMovementControllerType_Grounded;
    entity.movement.movement_collider_template_id = 21u;
    return entity;
}

KernelEntityTemplateDefinition game_rule_director_template() {
    KernelEntityTemplateDefinition entity{};
    entity.struct_size = sizeof(entity);
    entity.entity_template_id = 100u;
    entity.entity_type = KernelEntityType_Director;
    entity.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM | KERNEL_ENTITY_COMPONENT_SERVER_ONLY |
        KERNEL_ENTITY_COMPONENT_AGENT_RUNTIME |
        KERNEL_ENTITY_COMPONENT_DIRECTOR_RUNTIME;
    entity.ai.struct_size = sizeof(entity.ai);
    entity.ai.controller_type = KernelAiControllerType_Director;
    entity.ai.tick_interval = 1u;
    entity.ai.director_kind = KernelDirectorKind_GameRule;
    entity.ai.game_rule_definition_id = 100u;
    entity.movement.struct_size = sizeof(entity.movement);
    return entity;
}

KernelEntityTemplateDefinition world_rule_director_template() {
    KernelEntityTemplateDefinition entity = game_rule_director_template();
    entity.ai.director_kind = KernelDirectorKind_WorldRule;
    entity.ai.game_rule_definition_id = 0u;
    entity.ai.spawn_target_count = 1u;
    entity.ai.spawn_entity_template_id = 200u;
    entity.ai.spawn_actor_template_id = 2u;
    entity.ai.spawn_position = KernelVec3{5.0f, 0.0f, 0.0f};
    entity.ai.spawn_radius = 0.0f;
    entity.ai.spawn_seed = 7u;
    return entity;
}

struct CatalogFixture {
    std::array<KernelColliderTemplateDefinition, 2> colliders = {
        hit_collider_template(),
        movement_collider_template(),
    };
    std::array<KernelActorTemplateDefinition, 1> actors = {
        agent_actor_template(),
    };
    std::array<KernelEntityTemplateDefinition, 2> entities = {
        game_rule_director_template(),
        agent_entity_template(),
    };
    std::array<KernelGameRuleNodeDefinition, 4> nodes = {{
        {sizeof(KernelGameRuleNodeDefinition), 1u,
         KernelGameRuleConditionType_GroupEliminated, 1u},
        {sizeof(KernelGameRuleNodeDefinition), 2u,
         KernelGameRuleConditionType_GroupEliminated, 2u},
        {sizeof(KernelGameRuleNodeDefinition), 3u,
         KernelGameRuleConditionType_GroupEliminated, 3u},
        {sizeof(KernelGameRuleNodeDefinition), 4u,
         KernelGameRuleConditionType_GroupEliminated, 4u},
    }};
    std::array<KernelGameRuleEdgeDefinition, 4> edges = {{
        {sizeof(KernelGameRuleEdgeDefinition), 1u, 2u},
        {sizeof(KernelGameRuleEdgeDefinition), 1u, 3u},
        {sizeof(KernelGameRuleEdgeDefinition), 2u, 4u},
        {sizeof(KernelGameRuleEdgeDefinition), 3u, 4u},
    }};
    std::array<KernelGameRuleSpawnGroupEffectDefinition, 4> effects = {{
        {sizeof(KernelGameRuleSpawnGroupEffectDefinition),
         KernelGameRuleEffectType_SpawnGroup, 1u, 1u, 1u, 200u,
         KernelVec3{1.0f, 0.0f, 0.0f}, 0.0f, 11u},
        {sizeof(KernelGameRuleSpawnGroupEffectDefinition),
         KernelGameRuleEffectType_SpawnGroup, 2u, 2u, 1u, 200u,
         KernelVec3{2.0f, 0.0f, 0.0f}, 0.0f, 22u},
        {sizeof(KernelGameRuleSpawnGroupEffectDefinition),
         KernelGameRuleEffectType_SpawnGroup, 3u, 3u, 1u, 200u,
         KernelVec3{3.0f, 0.0f, 0.0f}, 0.0f, 33u},
        {sizeof(KernelGameRuleSpawnGroupEffectDefinition),
         KernelGameRuleEffectType_SpawnGroup, 4u, 4u, 1u, 200u,
         KernelVec3{4.0f, 0.0f, 0.0f}, 0.0f, 44u},
    }};
    KernelGameRuleDefinition rule{
        sizeof(KernelGameRuleDefinition), 100u, 0u, 4u, 0u, 4u, 0u, 4u};

    bool load(network_example::KernelEngine& engine) const {
        KernelGameplayCatalogDefinition catalog{};
        catalog.struct_size = sizeof(catalog);
        catalog.catalog_version = 1u;
        catalog.catalog_hash = 1u;
        catalog.collider_templates = colliders.data();
        catalog.collider_template_count = colliders.size();
        catalog.actor_templates = actors.data();
        catalog.actor_template_count = actors.size();
        catalog.entity_templates = entities.data();
        catalog.entity_template_count = entities.size();
        catalog.game_rules = &rule;
        catalog.game_rule_count = 1u;
        catalog.game_rule_nodes = nodes.data();
        catalog.game_rule_node_count = nodes.size();
        catalog.game_rule_edges = edges.data();
        catalog.game_rule_edge_count = edges.size();
        catalog.game_rule_effects = effects.data();
        catalog.game_rule_effect_count = effects.size();
        return engine.load_gameplay_catalog(catalog);
    }
};

struct PlayerGateCatalogFixture {
    std::array<KernelColliderTemplateDefinition, 2> colliders = {
        hit_collider_template(),
        movement_collider_template(),
    };
    std::array<KernelActorTemplateDefinition, 1> actors = {
        agent_actor_template(),
    };
    std::array<KernelEntityTemplateDefinition, 2> entities = {
        game_rule_director_template(),
        agent_entity_template(),
    };
    std::array<KernelGameRuleNodeDefinition, 2> nodes = {{
        {sizeof(KernelGameRuleNodeDefinition), 1u,
         KernelGameRuleConditionType_PlayerCountAtLeast, 0u, 1u},
        {sizeof(KernelGameRuleNodeDefinition), 2u,
         KernelGameRuleConditionType_GroupEliminated, 2u, 0u},
    }};
    std::array<KernelGameRuleEdgeDefinition, 1> edges = {{
        {sizeof(KernelGameRuleEdgeDefinition), 1u, 2u},
    }};
    std::array<KernelGameRuleSpawnGroupEffectDefinition, 1> effects = {{
        {sizeof(KernelGameRuleSpawnGroupEffectDefinition),
         KernelGameRuleEffectType_SpawnGroup, 2u, 2u, 1u, 200u,
         KernelVec3{2.0f, 0.0f, 0.0f}, 0.0f, 22u},
    }};
    KernelGameRuleDefinition rule{
        sizeof(KernelGameRuleDefinition), 100u, 0u, 2u, 0u, 1u, 0u, 1u};

    bool load(network_example::KernelEngine& engine) const {
        KernelGameplayCatalogDefinition catalog{};
        catalog.struct_size = sizeof(catalog);
        catalog.catalog_version = 1u;
        catalog.catalog_hash = 1u;
        catalog.collider_templates = colliders.data();
        catalog.collider_template_count = colliders.size();
        catalog.actor_templates = actors.data();
        catalog.actor_template_count = actors.size();
        catalog.entity_templates = entities.data();
        catalog.entity_template_count = entities.size();
        catalog.game_rules = &rule;
        catalog.game_rule_count = 1u;
        catalog.game_rule_nodes = nodes.data();
        catalog.game_rule_node_count = nodes.size();
        catalog.game_rule_edges = edges.data();
        catalog.game_rule_edge_count = edges.size();
        catalog.game_rule_effects = effects.data();
        catalog.game_rule_effect_count = effects.size();
        return engine.load_gameplay_catalog(catalog);
    }
};

KernelConfig dedicated_config() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30u;
    config.tick.snapshot_rate = 15u;
    return config;
}

void initialize_engine(network_example::KernelEngine& engine) {
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    const CatalogFixture fixture;
    require(fixture.load(engine));
}

void initialize_player_gate_engine(network_example::KernelEngine& engine) {
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    const PlayerGateCatalogFixture fixture;
    require(fixture.load(engine));
}

void initialize_world_rule_engine(network_example::KernelEngine& engine) {
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    const std::array<KernelColliderTemplateDefinition, 2> colliders = {
        hit_collider_template(),
        movement_collider_template(),
    };
    const std::array<KernelActorTemplateDefinition, 1> actors = {
        agent_actor_template(),
    };
    const std::array<KernelEntityTemplateDefinition, 2> entities = {
        world_rule_director_template(),
        agent_entity_template(),
    };
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1u;
    catalog.catalog_hash = 1u;
    catalog.collider_templates = colliders.data();
    catalog.collider_template_count = colliders.size();
    catalog.actor_templates = actors.data();
    catalog.actor_template_count = actors.size();
    catalog.entity_templates = entities.data();
    catalog.entity_template_count = entities.size();
    require(engine.load_gameplay_catalog(catalog));
}

network_example::NetId create_director(network_example::KernelEngine& engine) {
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_template_id = 100u;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    network_example::NetId director = 0u;
    require(engine.server_create_entity(create, &director));
    return director;
}

network_example::GameRuleRuntime& runtime_for(
    network_example::KernelEngine& engine,
    network_example::NetId director) {
    const std::optional<entt::entity> entity = engine.world_.find_entity(director);
    require(entity.has_value());
    return engine.world_.registry().get<network_example::GameRuleRuntime>(*entity);
}

network_example::NetId entity_in_group(
    network_example::KernelEngine& engine,
    network_example::NetId director,
    std::uint32_t group_id) {
    auto view = engine.world_.registry().view<
        const network_example::NetworkIdentity,
        const network_example::GameplayGroupMembership>();
    for (const entt::entity entity : view) {
        const network_example::GameplayGroupMembership& membership =
            view.get<const network_example::GameplayGroupMembership>(entity);
        if (membership.director_net_id == director &&
            membership.group_id == group_id) {
            return view.get<const network_example::NetworkIdentity>(entity).net_id;
        }
    }
    return 0u;
}

void update(network_example::KernelEngine& engine) {
    engine.update(1.0f / 30.0f);
}

std::vector<network_example::NetId> live_agents(
    network_example::KernelEngine& engine) {
    std::vector<network_example::NetId> agents;
    auto view = engine.world_.registry().view<
        const network_example::EntityKind,
        const network_example::NetworkIdentity>();
    for (const entt::entity entity : view) {
        const network_example::EntityKind& kind =
            view.get<const network_example::EntityKind>(entity);
        if (kind.type == network_example::EntityType::kActor &&
            kind.actor_type == network_example::ActorType::kAgent) {
            agents.push_back(
                view.get<const network_example::NetworkIdentity>(entity).net_id);
        }
    }
    return agents;
}

void world_rule_still_maintains_target_count() {
    const KernelConfig config = dedicated_config();
    network_example::KernelEngine engine(config);
    initialize_world_rule_engine(engine);
    const network_example::NetId director = create_director(engine);
    const std::optional<entt::entity> director_entity =
        engine.world_.find_entity(director);
    require(director_entity.has_value());
    require((engine.world_.registry().all_of<
        network_example::DirectorRuntime,
        network_example::WorldRuleRuntime>(*director_entity)));
    require(!engine.world_.registry().all_of<network_example::GameRuleRuntime>(
        *director_entity));
    update(engine);
    update(engine);
    std::vector<network_example::NetId> agents = live_agents(engine);
    require(agents.size() == 1u);
    require(network_example::EntityLifecycleSystem{}.destroy_entity(
        engine, agents[0], KernelDespawnReason_Destroyed));
    update(engine);
    update(engine);
    agents = live_agents(engine);
    require(agents.size() == 1u);
}

void game_rule_waits_for_player_before_spawning_wave() {
    const KernelConfig config = dedicated_config();
    network_example::KernelEngine engine(config);
    initialize_player_gate_engine(engine);
    const network_example::NetId director = create_director(engine);

    update(engine);
    network_example::GameRuleRuntime& runtime = runtime_for(engine, director);
    require(runtime.node_states[0] == network_example::GameRuleNodeState::kActive);
    require(runtime.node_states[1] == network_example::GameRuleNodeState::kInactive);
    require(runtime.groups.size() == 1u);
    require(runtime.groups[0].group_id == 2u);
    require(engine.command_queue_.size() == 0u);

    update(engine);
    require(runtime.node_states[0] == network_example::GameRuleNodeState::kActive);
    require(runtime.node_states[1] == network_example::GameRuleNodeState::kInactive);

    KernelServerEntityCreateInfo player_create{};
    player_create.struct_size = sizeof(player_create);
    player_create.entity_type = KernelEntityType_Actor;
    player_create.actor_type = KernelActorType_Player;
    player_create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    network_example::NetId player = 0u;
    require(engine.server_create_entity(player_create, &player));
    require(player != 0u);

    update(engine);
    require(runtime.node_states[0] == network_example::GameRuleNodeState::kCompleted);
    require(runtime.node_states[1] == network_example::GameRuleNodeState::kActive);
    require(engine.command_queue_.size() == 1u);
    update(engine);
    require(runtime.groups[0].sealed);
    require(runtime.groups[0].alive_count == 1u);
}

void game_rule_advances_branch_and_join_deterministically() {
    const KernelConfig config = dedicated_config();
    network_example::KernelEngine engine(config);
    initialize_engine(engine);
    const network_example::NetId director = create_director(engine);

    update(engine);
    network_example::GameRuleRuntime& runtime = runtime_for(engine, director);
    require(runtime.node_states[0] == network_example::GameRuleNodeState::kActive);
    require(runtime.groups[0].pending_spawn_count == 1u);
    require(!runtime.groups[0].sealed);
    require(engine.command_queue_.size() == 1u);

    update(engine);
    require(runtime.groups[0].pending_spawn_count == 0u);
    require(runtime.groups[0].alive_count == 1u);
    require(runtime.groups[0].sealed);
    require(runtime.node_states[0] == network_example::GameRuleNodeState::kActive);

    KernelServerEntityCreateInfo unrelated_create{};
    unrelated_create.struct_size = sizeof(unrelated_create);
    unrelated_create.entity_template_id = 200u;
    unrelated_create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    network_example::NetId unrelated = 0u;
    require(engine.server_create_entity(unrelated_create, &unrelated));
    require(network_example::EntityLifecycleSystem{}.destroy_entity(
        engine, unrelated, KernelDespawnReason_Destroyed));
    require(runtime.groups[0].alive_count == 1u);

    const network_example::NetId group_one = entity_in_group(engine, director, 1u);
    require(group_one != 0u);
    require(network_example::EntityLifecycleSystem{}.destroy_entity(
        engine, group_one, KernelDespawnReason_Destroyed));
    update(engine);
    require(runtime.node_states[0] == network_example::GameRuleNodeState::kCompleted);
    require(runtime.node_states[1] == network_example::GameRuleNodeState::kActive);
    require(runtime.node_states[2] == network_example::GameRuleNodeState::kActive);
    require(runtime.node_states[3] == network_example::GameRuleNodeState::kInactive);
    require(engine.command_queue_.size() == 2u);
    const auto commands = engine.command_queue_.commands();
    require(commands[0].create_entity.gameplay_group_id == 2u);
    require(commands[1].create_entity.gameplay_group_id == 3u);

    update(engine);
    const network_example::NetId group_two = entity_in_group(engine, director, 2u);
    const network_example::NetId group_three = entity_in_group(engine, director, 3u);
    require(group_two != 0u && group_three != 0u);
    require(network_example::EntityLifecycleSystem{}.destroy_entity(
        engine, group_two, KernelDespawnReason_OutOfRange));
    update(engine);
    require(runtime.node_states[1] == network_example::GameRuleNodeState::kCompleted);
    require(runtime.node_states[3] == network_example::GameRuleNodeState::kInactive);

    require(network_example::EntityLifecycleSystem{}.destroy_entity(
        engine, group_three, KernelDespawnReason_Disconnected));
    update(engine);
    require(runtime.node_states[2] == network_example::GameRuleNodeState::kCompleted);
    require(runtime.node_states[3] == network_example::GameRuleNodeState::kActive);
    require(engine.command_queue_.size() == 1u);

    update(engine);
    const network_example::NetId group_four = entity_in_group(engine, director, 4u);
    require(group_four != 0u);
    require(network_example::EntityLifecycleSystem{}.destroy_entity(
        engine, group_four, KernelDespawnReason_Destroyed));
    update(engine);
    require(runtime.status == network_example::GameRuleStatus::kCompleted);
}

void spawn_enqueue_failure_fails_game_rule() {
    const KernelConfig config = dedicated_config();
    network_example::KernelEngine engine(config);
    initialize_engine(engine);
    const network_example::NetId director = create_director(engine);
    network_example::DirectorAISystem{}.update(engine);
    require(engine.pending_director_intents_.size() == 1u);
    network_example::simulation::Command filler{};
    while (engine.command_queue_.enqueue(filler)) {
    }
    network_example::DirectorIntentExecutor{}.update(engine);
    const network_example::GameRuleRuntime& runtime = runtime_for(engine, director);
    require(runtime.status == network_example::GameRuleStatus::kFailed);
    require(runtime.groups[0].failed);
}

void spawn_execution_failure_fails_game_rule() {
    const KernelConfig config = dedicated_config();
    network_example::KernelEngine engine(config);
    initialize_engine(engine);
    const network_example::NetId director = create_director(engine);
    network_example::DirectorAISystem{}.update(engine);
    network_example::DirectorIntentExecutor{}.update(engine);
    network_example::GameRuleRuntime& runtime = runtime_for(engine, director);
    require(runtime.groups[0].pending_spawn_count == 1u);
    engine.entity_templates_.erase(
        std::remove_if(
            engine.entity_templates_.begin(),
            engine.entity_templates_.end(),
            [](const KernelEntityTemplateDefinition& entity) {
                return entity.entity_template_id == 200u;
            }),
        engine.entity_templates_.end());
    update(engine);
    require(runtime.status == network_example::GameRuleStatus::kFailed);
    require(runtime.groups[0].failed);
    require(runtime.groups[0].pending_spawn_count == 0u);
    require(runtime.groups[0].sealed);
}

}  // namespace

int main() {
    world_rule_still_maintains_target_count();
    game_rule_waits_for_player_before_spawning_wave();
    game_rule_advances_branch_and_join_deterministically();
    spawn_enqueue_failure_fails_game_rule();
    spawn_execution_failure_fails_game_rule();
    return 0;
}
