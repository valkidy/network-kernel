#include "game_server/src/spawner_director.h"

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
constexpr std::uint32_t kNestTemplateId = 300;

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

KernelEntityTemplateDefinition nest_entity_template() {
    KernelEntityTemplateDefinition entity_template{};
    entity_template.struct_size = sizeof(entity_template);
    entity_template.entity_template_id = kNestTemplateId;
    entity_template.entity_type = KernelEntityType_Prop;
    // A nest is an ordinary destructible object: health, a hit collider, and
    // nothing that says "director" anywhere.
    // A nest is an ordinary destructible object: health, a hit collider, and
    // nothing anywhere that says "director".
    entity_template.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM | KERNEL_ENTITY_COMPONENT_HEALTH |
        KERNEL_ENTITY_COMPONENT_HITBOX;
    entity_template.collider_template_id = 1u;
    entity_template.combat.struct_size = sizeof(entity_template.combat);
    entity_template.combat.hp = 500;
    entity_template.combat.max_hp = 500;
    entity_template.combat.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    entity_template.combat.hitbox_half_extents = KernelVec3{0.8f, 0.8f, 0.8f};
    // Both struct sizes are required of every entity template, prop or not:
    // the loader checks them before it looks at what the template is for.
    entity_template.ai.struct_size = sizeof(KernelEntityAiDefinition);
    entity_template.movement.struct_size = sizeof(KernelMovementDefinition);
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
    const std::array<KernelEntityTemplateDefinition, 2> entity_templates_all = {
        agent_entity_template(),
        nest_entity_template(),
    };
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.catalog_hash = 1;
    catalog.collider_templates = colliders.data();
    catalog.collider_template_count =
        static_cast<std::uint32_t>(colliders.size());
    catalog.actor_templates = &actor_template;
    catalog.actor_template_count = 1;
    catalog.entity_templates = entity_templates_all.data();
    catalog.entity_template_count =
        static_cast<std::uint32_t>(entity_templates_all.size());
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


std::uint32_t create_nest(KernelHandle* kernel, const KernelVec3& position) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_template_id = kNestTemplateId;
    create_info.position = position;
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    require(net_id != 0);
    return net_id;
}

void destroy_entity(KernelHandle* kernel, std::uint32_t net_id) {
    KernelEntityLifecycleCommand command{};
    command.struct_size = sizeof(command);
    command.command_type = KernelEntityLifecycleCommandType_Destroy;
    command.net_id = net_id;
    command.reason = 0;
    Kernel_ServerEnqueueEntityLifecycle(
        kernel, KernelCommandSource_Internal, &command);
    Kernel_Update(kernel, 1.0f / 30.0f);
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

using network_example::game_server::SpawnCompositionEntry;
using network_example::game_server::SpawnerCarrierConfig;
using network_example::game_server::SpawnerConfig;
using network_example::game_server::SpawnerDirector;

SpawnerCarrierConfig nest_carrier(
    std::uint32_t interval,
    std::uint32_t count_min,
    std::uint32_t count_max,
    std::uint32_t max_live) {
    SpawnerCarrierConfig carrier;
    carrier.entity_template_id = kNestTemplateId;
    carrier.entity_type = KernelEntityType_Prop;
    carrier.name = "nest";
    carrier.spawner.authored = true;
    carrier.spawner.seed = 99;
    carrier.spawner.interval_ticks = interval;
    carrier.spawner.max_live_agents = max_live;
    carrier.spawner.radius = 4.0f;
    carrier.spawner.count_min = count_min;
    carrier.spawner.count_max = count_max;
    carrier.spawner.composition = {
        SpawnCompositionEntry{"unit", kAgentTemplateId, count_min, count_max},
    };
    return carrier;
}

// The rule fires on its carrier's cadence, and staggers by one interval rather
// than emptying itself the tick the nest appears.
void a_nest_emits_on_its_interval() {
    KernelHandle* kernel = start_server(7861);
    SpawnerDirector director({nest_carrier(5, 2, 2, 0)});
    create_nest(kernel, KernelVec3{10.0f, 0.0f, 0.0f});

    // The countdown is decremented before it is tested, so a rule with an
    // interval of five fires on the fifth tick, not the sixth.
    for (std::uint32_t tick = 0; tick < 4; ++tick) {
        director.tick(kernel);
        require(agent_count(kernel) == 0u);
    }
    director.tick(kernel);
    require(agent_count(kernel) == 2u);
    require(director.instances().size() == 1u);

    for (std::uint32_t tick = 0; tick < 5; ++tick) {
        director.tick(kernel);
    }
    require(agent_count(kernel) == 4u);

    // Around the nest, not at the origin.
    for (const KernelVec3& position : agent_positions(kernel)) {
        const float dx = position.x - 10.0f;
        const float dz = position.z;
        require(std::sqrt(dx * dx + dz * dz) <= 4.5f);
    }

    Kernel_Destroy(kernel);
}

// Two nests of one template are two rules. They run independently, and they do
// not put out identical waves -- which they would if the draw came from the
// template's seed alone.
void two_nests_of_one_template_run_independently() {
    KernelHandle* kernel = start_server(7862);
    SpawnerDirector director({nest_carrier(3, 1, 6, 0)});
    create_nest(kernel, KernelVec3{10.0f, 0.0f, 0.0f});
    create_nest(kernel, KernelVec3{-40.0f, 0.0f, 0.0f});

    for (int tick = 0; tick < 30; ++tick) {
        director.tick(kernel);
    }
    require(director.instances().size() == 2u);
    const std::size_t first = director.instances()[0].spawned_net_ids.size();
    const std::size_t second = director.instances()[1].spawned_net_ids.size();
    require(first > 0u && second > 0u);
    require(first != second);

    Kernel_Destroy(kernel);
}

// Destroying the nest stops it, and leaves what it has already put out. That is
// the point of the rule living on the carrier: its lifetime is the carrier's,
// with no "anchor died" bookkeeping, and despawning what a player has just
// fought through would be worse than any population it saved.
void destroying_a_nest_stops_it_and_keeps_its_units() {
    KernelHandle* kernel = start_server(7863);
    SpawnerDirector director({nest_carrier(3, 2, 2, 0)});
    const std::uint32_t nest = create_nest(kernel, KernelVec3{10.0f, 0.0f, 0.0f});

    for (int tick = 0; tick < 10; ++tick) {
        director.tick(kernel);
    }
    const std::uint32_t before = agent_count(kernel);
    require(before > 0u);

    destroy_entity(kernel, nest);
    director.tick(kernel);
    require(director.instances().empty());
    require(agent_count(kernel) == before);

    for (int tick = 0; tick < 30; ++tick) {
        director.tick(kernel);
    }
    require(agent_count(kernel) == before);

    Kernel_Destroy(kernel);
}

// The ceiling bounds a periodic emitter, and waves stay whole: with room for
// two and a wave of three, the nest waits rather than putting out a short wave.
// It cannot put out a short one -- the composition's minimums are assigned
// before anything is drawn, so a trimmed count comes back the same size and
// walks straight through the ceiling.
void a_ceiling_bounds_the_nest_and_waves_stay_whole() {
    KernelHandle* kernel = start_server(7864);
    SpawnerDirector director({nest_carrier(2, 3, 3, 5)});
    create_nest(kernel, KernelVec3{10.0f, 0.0f, 0.0f});

    for (int tick = 0; tick < 40; ++tick) {
        director.tick(kernel);
        require(agent_count(kernel) <= 5u);
    }
    // Three, and then no room for a second whole wave, so it holds at three
    // rather than creeping to five.
    require(agent_count(kernel) == 3u);

    // Room reappears and the nest fills it.
    const std::vector<KernelVec3> before = agent_positions(kernel);
    require(before.size() == 3u);
    destroy_all_agents(kernel);
    for (int tick = 0; tick < 10; ++tick) {
        director.tick(kernel);
    }
    require(agent_count(kernel) > 0u);

    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    a_nest_emits_on_its_interval();
    two_nests_of_one_template_run_independently();
    destroying_a_nest_stops_it_and_keeps_its_units();
    a_ceiling_bounds_the_nest_and_waves_stay_whole();
    return 0;
}
