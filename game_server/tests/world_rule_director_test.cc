#include "game_server/src/world_rule_director.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "game_server/src/agent_runtime.h"
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
constexpr float kGoldenAngleRadians = 2.39996323f;

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

network_example::game_server::WorldRuleSpawnConfig rule_of(
    std::uint32_t target,
    float radius,
    std::uint32_t tick_interval) {
    network_example::game_server::WorldRuleSpawnConfig rule;
    rule.director_template_id = 100;
    rule.name = "test_rule";
    rule.spawn_entity_template_id = kAgentTemplateId;
    rule.target_count = target;
    rule.position = KernelVec3{6.0f, 0.0f, 0.0f};
    rule.radius = radius;
    rule.tick_interval = tick_interval;
    return rule;
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

bool almost_equal(float lhs, float rhs, float tolerance = 0.001f) {
    return std::fabs(lhs - rhs) <= tolerance;
}

// Fills the shortfall in one go and then stops. This is the behaviour that used
// to be a director entity, a WorldRuleRuntime component and a SpawnAgent intent
// the kernel validated back against that component; it is game_server's now,
// and these are the claims that moved with it.
void a_world_rule_fills_its_target_and_stops() {
    using network_example::game_server::WorldRuleDirector;

    KernelHandle* kernel = start_server(7841);
    WorldRuleDirector director({rule_of(3, 0.0f, 1)});

    director.tick(kernel, agent_count(kernel));
    require(agent_count(kernel) == 3u);
    require(director.spawned_agent_count() == 3u);

    // Satisfied, so nothing more however long it runs.
    for (int tick = 0; tick < 20; ++tick) {
        director.tick(kernel, agent_count(kernel));
    }
    require(agent_count(kernel) == 3u);
    require(director.spawned_agent_count() == 3u);

    Kernel_Destroy(kernel);
}

// The interval is wall clock, not time spent short. A rule that has been at
// full strength for longer than its interval replaces a casualty on the tick it
// appears; an interval that only ran while the rule was short would leave the
// world a body down for up to a full interval every time.
void a_rule_at_strength_replaces_a_casualty_immediately() {
    using network_example::game_server::WorldRuleDirector;

    KernelHandle* kernel = start_server(7842);
    WorldRuleDirector director({rule_of(2, 0.0f, 10)});

    director.tick(kernel, agent_count(kernel));
    require(agent_count(kernel) == 2u);

    // Fifteen ticks at full strength, which is longer than the interval.
    for (int tick = 0; tick < 15; ++tick) {
        director.tick(kernel, agent_count(kernel));
    }
    require(agent_count(kernel) == 2u);
    require(director.spawned_agent_count() == 2u);

    // One dies, and the replacement is on this tick rather than an interval
    // later.
    director.tick(kernel, 1u);
    require(director.spawned_agent_count() == 3u);

    Kernel_Destroy(kernel);
}

// The interval is what separates two top-ups, so a rule that is short by one
// for many ticks does not spawn one per tick.
void an_interval_separates_two_top_ups() {
    using network_example::game_server::WorldRuleDirector;

    KernelHandle* kernel = start_server(7843);
    WorldRuleDirector director({rule_of(4, 0.0f, 10)});

    // Told it is empty every tick, so the shortfall never closes.
    for (int tick = 0; tick < 9; ++tick) {
        director.tick(kernel, 0u);
    }
    require(director.spawned_agent_count() == 4u);
    for (int tick = 0; tick < 2; ++tick) {
        director.tick(kernel, 0u);
    }
    require(director.spawned_agent_count() == 8u);

    Kernel_Destroy(kernel);
}

// Placement is the golden-angle ring the kernel used, and the cursor carries
// across top-ups so a second batch does not land on the first.
void placement_is_a_golden_angle_ring_that_advances() {
    using network_example::game_server::WorldRuleDirector;

    KernelHandle* kernel = start_server(7844);
    const network_example::game_server::WorldRuleSpawnConfig rule =
        rule_of(2, 5.0f, 1);
    WorldRuleDirector director({rule});

    director.tick(kernel, 0u);
    std::vector<KernelVec3> positions = agent_positions(kernel);
    require(positions.size() == 2u);
    for (std::uint32_t index = 0; index < 2u; ++index) {
        const float angle = static_cast<float>(index) * kGoldenAngleRadians;
        bool matched = false;
        for (const KernelVec3& position : positions) {
            matched = matched ||
                (almost_equal(
                     position.x, rule.position.x + std::cos(angle) * rule.radius,
                     0.01f) &&
                 almost_equal(
                     position.z, rule.position.z + std::sin(angle) * rule.radius,
                     0.01f));
        }
        require(matched);
    }

    // A second top-up picks the ring up where the first left it. Without the
    // cursor both batches would sit on the same two points.
    director.tick(kernel, 0u);
    positions = agent_positions(kernel);
    require(positions.size() == 4u);
    const float third_angle = 2.0f * kGoldenAngleRadians;
    bool matched_third = false;
    for (const KernelVec3& position : positions) {
        matched_third = matched_third ||
            almost_equal(
                position.x, rule.position.x + std::cos(third_angle) * rule.radius,
                0.01f);
    }
    require(matched_third);

    Kernel_Destroy(kernel);
}

// A rule with nothing to spawn, or nothing to spawn it from, does nothing.
void an_empty_rule_spawns_nothing() {
    using network_example::game_server::WorldRuleDirector;

    KernelHandle* kernel = start_server(7845);
    network_example::game_server::WorldRuleSpawnConfig no_target =
        rule_of(0, 0.0f, 1);
    network_example::game_server::WorldRuleSpawnConfig no_template =
        rule_of(3, 0.0f, 1);
    no_template.spawn_entity_template_id = 0;
    no_template.spawn_actor_template_id = 0;
    WorldRuleDirector director({no_target, no_template});

    for (int tick = 0; tick < 10; ++tick) {
        director.tick(kernel, 0u);
    }
    require(agent_count(kernel) == 0u);
    require(director.spawned_agent_count() == 0u);

    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    a_world_rule_fills_its_target_and_stops();
    a_rule_at_strength_replaces_a_casualty_immediately();
    an_interval_separates_two_top_ups();
    placement_is_a_golden_angle_ring_that_advances();
    an_empty_rule_spawns_nothing();
    return 0;
}
