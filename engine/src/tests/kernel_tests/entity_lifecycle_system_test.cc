#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/public/kernel_api.h"

#define private public
#include "kernel/src/kernel.h"
#include "simulation/src/systems.h"
#undef private

namespace {

static_assert(
    std::is_empty_v<network_example::AgentSentryRuntime>,
    "Kernel AgentSentryRuntime must stay an empty marker; game_server owns sentry AI state.");

KernelServerEntityCreateInfo player_create_info() {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = static_cast<std::uint16_t>(network_example::EntityType::kActor);
    create_info.actor_type = KernelActorType_Player;
    create_info.owner_peer = 42;
    create_info.position = KernelVec3{2.0f, 0.0f, 3.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    create_info.animation_state = 7;
    create_info.visual_flags = 0x44u;
    return create_info;
}

KernelColliderTemplateDefinition hit_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 20;
    collider.shape_type = KernelColliderShapeType_Aabb;
    collider.center = KernelVec3{0.0f, 0.8f, 0.0f};
    collider.shape_params = KernelVec4{0.4f, 0.8f, 0.4f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Hit;
    collider.layer_mask = KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
    return collider;
}

KernelEntityTemplateDefinition agent_entity_template() {
    KernelEntityTemplateDefinition entity_template{};
    entity_template.struct_size = sizeof(entity_template);
    entity_template.entity_template_id = 200;
    entity_template.entity_type = KernelEntityType_Actor;
    entity_template.actor_type = KernelActorType_Agent;
    entity_template.actor_template_id = 2;
    entity_template.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM | KERNEL_ENTITY_COMPONENT_VELOCITY |
        KERNEL_ENTITY_COMPONENT_HEALTH | KERNEL_ENTITY_COMPONENT_HITBOX |
        KERNEL_ENTITY_COMPONENT_AGENT_RUNTIME |
        KERNEL_ENTITY_COMPONENT_SENTRY_RUNTIME;
    entity_template.collider_template_id = 20;
    entity_template.combat.struct_size = sizeof(entity_template.combat);
    entity_template.combat.hp = 100;
    entity_template.combat.max_hp = 100;
    entity_template.combat.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    entity_template.combat.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    entity_template.vision.struct_size = sizeof(entity_template.vision);
    entity_template.vision.camp = KernelAgentCamp_EnemySide;
    entity_template.ai.struct_size = sizeof(entity_template.ai);
    entity_template.ai.controller_type = KernelAiControllerType_Sentry;
    entity_template.ai.tick_interval = 1;
    return entity_template;
}

KernelActorTemplateDefinition agent_actor_template() {
    KernelActorTemplateDefinition actor_template{};
    actor_template.struct_size = sizeof(actor_template);
    actor_template.actor_template_id = 2;
    actor_template.entity_type = KernelEntityType_Actor;
    actor_template.actor_type = KernelActorType_Agent;
    actor_template.collider_template_id = 20;
    actor_template.vision.struct_size = sizeof(actor_template.vision);
    actor_template.vision.camp = KernelAgentCamp_EnemySide;
    return actor_template;
}

void lifecycle_system_create_matches_legacy_path() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    network_example::EntityLifecycleSystem lifecycle;
    std::uint32_t net_id = 0;
    assert(lifecycle.create_entity(engine, player_create_info(), &net_id));
    assert(net_id != 0);

    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    assert(engine.server_get_entity_state(net_id, &state));
    assert(state.valid != 0u);
    assert(state.net_id == net_id);
    assert(state.entity_type == static_cast<std::uint16_t>(network_example::EntityType::kActor));
    assert(state.actor_type == KernelActorType_Player);
    assert(state.owner_peer == 42);
    assert(state.animation_state == 7);
    assert(state.visual_flags == 0x44u);
    assert(state.position.x == 2.0f);
    assert(state.position.z == 3.0f);
    assert(engine.events_.size() == 1);
    assert(engine.events_[0].type == KernelEventType_EntitySpawned);
    assert(engine.latest_snapshot_.entities.size() == 1);
}

void lifecycle_system_destroy_matches_legacy_side_effects() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    std::uint32_t net_id = 0;
    assert(engine.server_create_entity(player_create_info(), &net_id));
    engine.events_.clear();
    engine.lifecycle_events_.clear();
    engine.vision_configs_[net_id] = KernelAgentVisionConfig{};
    engine.vision_states_[net_id] = network_example::KernelEngine::VisionRuntimeState{};

    network_example::EntityLifecycleSystem lifecycle;
    assert(lifecycle.destroy_entity(engine, net_id, KernelDespawnReason_Destroyed));

    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    assert(!engine.server_get_entity_state(net_id, &state));
    assert(engine.vision_configs_.find(net_id) == engine.vision_configs_.end());
    assert(engine.vision_states_.find(net_id) == engine.vision_states_.end());
    assert(engine.events_.size() == 1);
    assert(engine.events_[0].type == KernelEventType_EntityDestroyed);
    assert(engine.lifecycle_events_.size() == 1);
    assert(engine.lifecycle_events_[0].net_id == net_id);
    assert(engine.lifecycle_events_[0].reason == KernelDespawnReason_Destroyed);
    assert(engine.latest_snapshot_.entities.empty());
}

}  // namespace

// The world-rule director cases that used to live here are gone with the
// mechanism: a world rule is game_server's WorldRuleDirector now, and its
// behaviour -- filling a target, the wall-clock interval, the golden-angle ring
// and its cursor -- is covered by //game_server:world_rule_director_test with
// assertions that are not compiled out.
//
// NOTE: the assertions in this file are assert(), and this suite runs under
// -c opt, where they are compiled to nothing. It is red for a reason that is
// therefore not an assertion, and that predates this change. Converting it is
// its own task.
int main() {
    lifecycle_system_create_matches_legacy_path();
    lifecycle_system_destroy_matches_legacy_side_effects();
    return 0;
}
