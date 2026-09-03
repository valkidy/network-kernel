#include "game_server/game_rule_director.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "game_server/agent_runtime.h"
#include "kernel/public/kernel_api.h"
#include "kernel/src/kernel_api_internal.h"

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr std::uint32_t kAgentTemplateId = 102;

KernelConfig server_config() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;
    config.max_events = 64;
    config.max_render_states = 64;
    return config;
}

KernelColliderTemplateDefinition hit_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 1;
    collider.shape_type = KernelColliderShapeType_Aabb;
    collider.center = KernelVec3{0.0f, 0.8f, 0.0f};
    collider.shape_params = KernelVec4{0.4f, 0.8f, 0.4f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Hit;
    collider.layer_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    return collider;
}

KernelColliderTemplateDefinition vision_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 2;
    collider.shape_type = KernelColliderShapeType_Cone;
    collider.shape_params = KernelVec4{20.0f, 90.0f, 0.0f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Vision;
    collider.layer_mask = KERNEL_COLLISION_LAYER_AGENT_VISION;
    return collider;
}

KernelColliderTemplateDefinition movement_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 11;
    collider.shape_type = KernelColliderShapeType_Capsule;
    collider.center = KernelVec3{0.0f, 0.9f, 0.0f};
    collider.shape_params = KernelVec4{0.55f, 0.35f, 0.0f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Movement;
    collider.layer_mask =
        KERNEL_COLLISION_LAYER_PLAYER_SIDE | KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
    return collider;
}

KernelEntityTemplateDefinition agent_entity_template() {
    KernelEntityTemplateDefinition entity_template{};
    entity_template.struct_size = sizeof(entity_template);
    entity_template.entity_template_id = kAgentTemplateId;
    entity_template.entity_type = KernelEntityType_Actor;
    entity_template.actor_type = KernelActorType_Agent;
    entity_template.actor_template_id = 2u;
    entity_template.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM | KERNEL_ENTITY_COMPONENT_VELOCITY |
        KERNEL_ENTITY_COMPONENT_HEALTH | KERNEL_ENTITY_COMPONENT_HITBOX |
        KERNEL_ENTITY_COMPONENT_AGENT_RUNTIME |
        KERNEL_ENTITY_COMPONENT_SENTRY_RUNTIME;
    entity_template.collider_template_id = 1u;
    entity_template.combat.struct_size = sizeof(entity_template.combat);
    entity_template.combat.hp = 100;
    entity_template.combat.max_hp = 100;
    entity_template.combat.move_speed_meters_per_second = 2.5f;
    entity_template.combat.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    entity_template.combat.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    entity_template.movement.struct_size = sizeof(KernelMovementDefinition);
    entity_template.movement.controller_type =
        KernelMovementControllerType_Grounded;
    entity_template.movement.movement_collider_template_id = 11u;
    entity_template.movement.max_slope_degrees = 50.0f;
    entity_template.movement.ground_probe_distance = 0.25f;
    entity_template.movement.ground_snap_distance = 0.5f;
    entity_template.ai.struct_size = sizeof(KernelEntityAiDefinition);
    entity_template.ai.controller_type = KernelAiControllerType_Sentry;
    entity_template.ai.tick_interval = 1u;
    entity_template.vision.struct_size = sizeof(KernelAgentVisionConfig);
    entity_template.vision.camp = KernelAgentCamp_EnemySide;
    entity_template.vision.vision_collider_template_id = 2u;
    entity_template.vision.max_visible_hostiles = KERNEL_MAX_VISIBLE_HOSTILES;
    entity_template.vision.max_visible_allies = KERNEL_MAX_VISIBLE_ALLIES;
    return entity_template;
}

KernelActorTemplateDefinition agent_actor_template() {
    KernelActorTemplateDefinition actor_template{};
    actor_template.struct_size = sizeof(actor_template);
    actor_template.actor_template_id = 2u;
    actor_template.entity_type = KernelEntityType_Actor;
    actor_template.actor_type = KernelActorType_Agent;
    actor_template.collider_template_id = 1u;
    actor_template.vision.struct_size = sizeof(KernelAgentVisionConfig);
    actor_template.vision.camp = KernelAgentCamp_EnemySide;
    actor_template.vision.vision_collider_template_id = 2u;
    actor_template.vision.max_visible_hostiles = KERNEL_MAX_VISIBLE_HOSTILES;
    actor_template.vision.max_visible_allies = KERNEL_MAX_VISIBLE_ALLIES;
    return actor_template;
}

KernelHandle* start_server(std::uint16_t port) {
    static KernelConfig config;
    config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, port));
    const std::array<KernelColliderTemplateDefinition, 3> colliders = {
        hit_collider_template(),
        vision_collider_template(),
        movement_collider_template(),
    };
    const KernelEntityTemplateDefinition entity_template = agent_entity_template();
    const KernelActorTemplateDefinition actor_template = agent_actor_template();
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.catalog_hash = 1;
    catalog.collider_templates = colliders.data();
    catalog.collider_template_count =
        static_cast<std::uint32_t>(colliders.size());
    catalog.actor_templates = &actor_template;
    catalog.actor_template_count = 1;
    catalog.entity_templates = &entity_template;
    catalog.entity_template_count = 1;
    require(Kernel_LoadGameplayCatalog(kernel, &catalog, nullptr));
    return kernel;
}

std::uint32_t agent_count(KernelHandle* kernel) {
    std::array<KernelServerEntityState, 64> states{};
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    const std::uint32_t count = Kernel_ServerQueryEntities(
        kernel,
        network_example::game_server::kEntityTypeActor,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    std::uint32_t agents = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].actor_type ==
            network_example::game_server::kActorTypeAgent) {
            ++agents;
        }
    }
    return agents;
}

std::vector<KernelVec3> agent_positions(KernelHandle* kernel) {
    std::array<KernelServerEntityState, 64> states{};
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    const std::uint32_t count = Kernel_ServerQueryEntities(
        kernel,
        network_example::game_server::kEntityTypeActor,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    std::vector<KernelVec3> positions;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].actor_type ==
            network_example::game_server::kActorTypeAgent) {
            positions.push_back(states[index].position);
        }
    }
    return positions;
}


std::uint32_t create_player(KernelHandle* kernel) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = network_example::game_server::kEntityTypeActor;
    create_info.actor_type = network_example::game_server::kActorTypePlayer;
    create_info.position = KernelVec3{0.0f, 0.0f, 0.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    return net_id;
}

void destroy_all_agents(KernelHandle* kernel) {
    std::array<KernelServerEntityState, 64> states{};
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    const std::uint32_t count = Kernel_ServerQueryEntities(
        kernel,
        network_example::game_server::kEntityTypeActor,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].actor_type !=
            network_example::game_server::kActorTypeAgent) {
            continue;
        }
        KernelEntityLifecycleCommand command{};
        command.struct_size = sizeof(command);
        command.command_type = KernelEntityLifecycleCommandType_Destroy;
        command.net_id = states[index].net_id;
        command.reason = 0;
        Kernel_ServerEnqueueEntityLifecycle(
            kernel, KernelCommandSource_Internal, &command);
    }
    Kernel_Update(kernel, 1.0f / 30.0f);
}

using network_example::game_server::GameRuleConditionType;
using network_example::game_server::GameRuleConfig;
using network_example::game_server::GameRuleDirector;
using network_example::game_server::GameRuleNodeConfig;
using network_example::game_server::GameRuleStatus;

GameRuleNodeConfig gate_node(std::uint32_t node_id, std::uint32_t players) {
    GameRuleNodeConfig node;
    node.node_id = node_id;
    node.condition_type = GameRuleConditionType::kPlayerCountAtLeast;
    node.condition_count = players;
    return node;
}

GameRuleNodeConfig wave_node(
    std::uint32_t node_id,
    std::uint32_t group_id,
    std::uint32_t count) {
    GameRuleNodeConfig node;
    node.node_id = node_id;
    node.condition_type = GameRuleConditionType::kGroupEliminated;
    node.condition_group_id = group_id;
    node.has_spawn_effect = true;
    node.spawn.group_id = group_id;
    node.spawn.count = count;
    node.spawn.entity_template_id = kAgentTemplateId;
    node.spawn.radius = 3.0f;
    node.spawn.seed = 1;
    return node;
}

GameRuleConfig rule_of(std::vector<GameRuleNodeConfig> nodes) {
    GameRuleConfig rule;
    rule.director_template_id = 101;
    rule.name = "test_rule";
    rule.tick_interval = 1;
    rule.nodes = std::move(nodes);
    return rule;
}

// Nothing spawns until somebody is there to fight it, and then the wave opens.
void a_wave_waits_for_its_player_gate() {
    KernelHandle* kernel = start_server(7851);
    GameRuleNodeConfig gate = gate_node(1, 1);
    gate.next_node_ids = {2};
    GameRuleDirector director({rule_of({gate, wave_node(2, 10, 2)})});

    for (int tick = 0; tick < 10; ++tick) {
        director.tick(kernel);
    }
    require(agent_count(kernel) == 0u);
    require(director.runtimes()[0].status == GameRuleStatus::kRunning);

    create_player(kernel);
    Kernel_Update(kernel, 1.0f / 30.0f);
    for (int tick = 0; tick < 3; ++tick) {
        director.tick(kernel);
    }
    require(agent_count(kernel) == 2u);

    Kernel_Destroy(kernel);
}

// Clearing a wave opens the next one, and only clearing it does. A group that
// has not been filled yet must not read as cleared, which is what `sealed` is
// for.
void a_cleared_wave_opens_the_next() {
    KernelHandle* kernel = start_server(7852);
    GameRuleNodeConfig first = wave_node(1, 10, 2);
    first.next_node_ids = {2};
    GameRuleDirector director({rule_of({first, wave_node(2, 20, 3)})});

    director.tick(kernel);
    require(agent_count(kernel) == 2u);
    for (int tick = 0; tick < 5; ++tick) {
        director.tick(kernel);
    }
    // Still alive, so the second wave has not opened.
    require(agent_count(kernel) == 2u);

    destroy_all_agents(kernel);
    for (int tick = 0; tick < 3; ++tick) {
        director.tick(kernel);
    }
    require(agent_count(kernel) == 3u);

    destroy_all_agents(kernel);
    for (int tick = 0; tick < 3; ++tick) {
        director.tick(kernel);
    }
    require(director.runtimes()[0].status == GameRuleStatus::kCompleted);

    Kernel_Destroy(kernel);
}

// A join waits for every branch, not for the first one home.
void a_join_waits_for_every_branch() {
    KernelHandle* kernel = start_server(7853);
    GameRuleNodeConfig root = wave_node(1, 10, 1);
    root.next_node_ids = {2, 3};
    GameRuleNodeConfig left = wave_node(2, 20, 1);
    left.next_node_ids = {4};
    GameRuleNodeConfig right = wave_node(3, 30, 1);
    right.next_node_ids = {4};
    GameRuleDirector director(
        {rule_of({root, left, right, wave_node(4, 40, 2)})});

    director.tick(kernel);
    require(agent_count(kernel) == 1u);
    destroy_all_agents(kernel);
    for (int tick = 0; tick < 3; ++tick) {
        director.tick(kernel);
    }
    // Both branches opened together.
    require(agent_count(kernel) == 2u);

    // Clearing them opens the join, and not before: with only one branch's
    // worth of members gone the join must stay shut, which is what a join that
    // fired on the first completed predecessor would get wrong.
    const std::vector<KernelVec3> positions = agent_positions(kernel);
    require(positions.size() == 2u);
    destroy_all_agents(kernel);
    for (int tick = 0; tick < 3; ++tick) {
        director.tick(kernel);
    }
    require(agent_count(kernel) == 2u);
    require(director.runtimes()[0].node_states.size() == 4u);

    Kernel_Destroy(kernel);
}

// A wave that cannot be spawned fails the rule rather than leaving it waiting
// forever on a group that was never filled.
void a_spawn_that_cannot_happen_fails_the_rule() {
    KernelHandle* kernel = start_server(7854);
    GameRuleNodeConfig broken = wave_node(1, 10, 2);
    broken.spawn.entity_template_id = 9999;
    GameRuleDirector director({rule_of({broken})});

    director.tick(kernel);
    director.tick(kernel);
    require(director.runtimes()[0].status == GameRuleStatus::kFailed);
    require(agent_count(kernel) == 0u);

    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    a_wave_waits_for_its_player_gate();
    a_cleared_wave_opens_the_next();
    a_join_waits_for_every_branch();
    a_spawn_that_cannot_happen_fails_the_rule();
    return 0;
}
