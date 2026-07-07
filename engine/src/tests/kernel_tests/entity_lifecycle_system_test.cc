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

KernelEntityTemplateDefinition director_template() {
    KernelEntityTemplateDefinition entity_template{};
    entity_template.struct_size = sizeof(entity_template);
    entity_template.entity_template_id = 100;
    entity_template.entity_type = KernelEntityType_Director;
    entity_template.component_flags =
        KERNEL_ENTITY_COMPONENT_SERVER_ONLY | KERNEL_ENTITY_COMPONENT_TRANSFORM |
        KERNEL_ENTITY_COMPONENT_AGENT_RUNTIME |
        KERNEL_ENTITY_COMPONENT_DIRECTOR_RUNTIME;
    entity_template.ai.struct_size = sizeof(entity_template.ai);
    entity_template.ai.controller_type = KernelAiControllerType_Director;
    entity_template.ai.tick_interval = 2;
    entity_template.ai.spawn_target_count = 3;
    entity_template.ai.spawn_entity_template_id = 200;
    entity_template.ai.spawn_actor_template_id = 2;
    entity_template.ai.spawn_position = KernelVec3{6.0f, 0.0f, 0.0f};
    entity_template.ai.spawn_radius = 1.0f;
    entity_template.ai.spawn_seed = 99;
    return entity_template;
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

void load_director_catalog(network_example::KernelEngine* engine) {
    const std::array<KernelColliderTemplateDefinition, 1> colliders = {
        hit_collider_template(),
    };
    const std::array<KernelActorTemplateDefinition, 1> actors = {
        agent_actor_template(),
    };
    const std::array<KernelEntityTemplateDefinition, 2> entity_templates = {
        director_template(),
        agent_entity_template(),
    };
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.catalog_hash = 1;
    catalog.collider_templates = colliders.data();
    catalog.collider_template_count = static_cast<std::uint32_t>(colliders.size());
    catalog.actor_templates = actors.data();
    catalog.actor_template_count = static_cast<std::uint32_t>(actors.size());
    catalog.entity_templates = entity_templates.data();
    catalog.entity_template_count =
        static_cast<std::uint32_t>(entity_templates.size());
    assert(engine->load_gameplay_catalog(catalog));
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

void lifecycle_system_materializes_server_only_director_template() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    load_director_catalog(&engine);

    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_template_id = 100;
    create_info.position = KernelVec3{1.0f, 0.0f, 2.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};

    std::uint32_t net_id = 0;
    assert(engine.server_create_entity(create_info, &net_id));
    const std::optional<entt::entity> entity = engine.world_.find_entity(net_id);
    assert(entity.has_value());
    assert(engine.world_.registry().all_of<network_example::ServerOnly>(*entity));
    assert(engine.world_.registry().all_of<network_example::AgentRuntime>(*entity));
    assert(engine.world_.registry().all_of<network_example::DirectorRuntime>(*entity));
    const network_example::EntityKind& kind =
        engine.world_.registry().get<network_example::EntityKind>(*entity);
    assert(kind.type == network_example::EntityType::kDirector);

    std::array<RenderEntityState, 4> render_states{};
    assert(engine.get_render_states(render_states.data(), render_states.size()) == 0);
    assert(engine.latest_snapshot_.entities.empty());
}

void director_ai_emits_spawn_intent_without_enqueueing_create_command() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    load_director_catalog(&engine);

    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_template_id = 100;
    create_info.position = KernelVec3{0.0f, 0.0f, 0.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};

    std::uint32_t director = 0;
    assert(engine.server_create_entity(create_info, &director));

    network_example::DirectorAISystem{}.update(engine);

    assert(engine.command_queue_.size() == 0);
    assert(engine.pending_director_intents_.size() == 1);
    const network_example::ai::ScopedIntent& intent =
        engine.pending_director_intents_[0];
    assert(intent.scope == network_example::ai::IntentScope::kDirector);
    assert(intent.type == "SpawnAgent");
    assert(intent.subject == director);
    assert(std::get<std::uint32_t>(intent.params.at("count")) == 3);
    assert(std::get<std::uint32_t>(
               intent.params.at("spawn_entity_template_id")) == 200);
    assert(std::get<std::uint32_t>(
               intent.params.at("spawn_actor_template_id")) == 2);
    assert(std::get<float>(intent.params.at("spawn_position_x")) == 6.0f);
    assert(std::get<float>(intent.params.at("spawn_radius")) == 1.0f);
    assert(std::get<std::uint32_t>(intent.params.at("base_spawn_cursor")) == 0);

    const std::optional<entt::entity> director_entity =
        engine.world_.find_entity(director);
    assert(director_entity.has_value());
    const network_example::DirectorRuntime& director_runtime =
        engine.world_.registry().get<network_example::DirectorRuntime>(
            *director_entity);
    assert(director_runtime.spawn_cursor == 0);
    assert(director_runtime.next_tick == 0);
}

void director_ai_ignores_non_director_entity_sources() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    load_director_catalog(&engine);

    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_template_id = 200;
    create_info.position = KernelVec3{0.0f, 0.0f, 0.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};

    std::uint32_t agent = 0;
    assert(engine.server_create_entity(create_info, &agent));
    const std::optional<entt::entity> agent_entity =
        engine.world_.find_entity(agent);
    assert(agent_entity.has_value());
    auto& registry = engine.world_.registry();
    network_example::AgentRuntime& runtime =
        registry.get<network_example::AgentRuntime>(*agent_entity);
    runtime.controller_type = network_example::AiControllerType::kDirector;
    registry.emplace_or_replace<network_example::DirectorRuntime>(
        *agent_entity,
        network_example::DirectorRuntime{
            1u,
            0u,
            3u,
            200u,
            2u,
            glm::vec3{6.0f, 0.0f, 0.0f},
            1.0f,
            99u,
            0u,
        });
    registry.emplace_or_replace<network_example::ServerOnly>(*agent_entity);

    network_example::DirectorAISystem{}.update(engine);

    assert(engine.pending_director_intents_.empty());
}

void director_intent_executor_reports_unsupported_intent() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    network_example::ai::ScopedIntent intent{};
    intent.scope = network_example::ai::IntentScope::kDirector;
    intent.type = "UnknownDirectorIntent";
    intent.subject = 777;

    const network_example::DirectorIntentExecutionResult result =
        network_example::DirectorIntentExecutor{}.execute(engine, intent);

    assert(result.status == network_example::ai::IntentStatus::kFailed);
    assert(result.unsupported);
    assert(result.created_count == 0);
    assert(engine.command_queue_.size() == 0);
}

void director_intent_executor_rejects_invalid_source_director() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    load_director_catalog(&engine);

    network_example::ai::ScopedIntent intent{};
    intent.scope = network_example::ai::IntentScope::kDirector;
    intent.type = "SpawnAgent";
    intent.subject = 777;
    intent.params["count"] = std::uint32_t{1};
    intent.params["spawn_target_count"] = std::uint32_t{3};
    intent.params["spawn_entity_template_id"] = std::uint32_t{200};
    intent.params["spawn_actor_template_id"] = std::uint32_t{2};
    intent.params["spawn_position_x"] = 6.0f;
    intent.params["spawn_position_y"] = 0.0f;
    intent.params["spawn_position_z"] = 0.0f;
    intent.params["spawn_radius"] = 1.0f;
    intent.params["base_spawn_cursor"] = std::uint32_t{0};

    const network_example::DirectorIntentExecutionResult result =
        network_example::DirectorIntentExecutor{}.execute(engine, intent);

    assert(result.status == network_example::ai::IntentStatus::kFailed);
    assert(!result.unsupported);
    assert(result.created_count == 0);
    assert(engine.command_queue_.size() == 0);
}

void director_intent_executor_does_not_commit_when_enqueue_fails() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    load_director_catalog(&engine);

    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_template_id = 100;
    create_info.position = KernelVec3{0.0f, 0.0f, 0.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};

    std::uint32_t director = 0;
    assert(engine.server_create_entity(create_info, &director));
    network_example::DirectorAISystem{}.update(engine);
    assert(engine.pending_director_intents_.size() == 1);

    network_example::simulation::Command filler{};
    filler.id = network_example::simulation::CommandId::kUnknown;
    for (std::size_t index = 0;
         index < network_example::simulation::CommandQueue::kDefaultCapacity;
         ++index) {
        assert(engine.command_queue_.enqueue(filler));
    }

    const network_example::DirectorIntentExecutionResult result =
        network_example::DirectorIntentExecutor{}.execute(
            engine,
            engine.pending_director_intents_[0]);

    assert(result.status == network_example::ai::IntentStatus::kFailed);
    assert(result.created_count == 0);
    const std::optional<entt::entity> director_entity =
        engine.world_.find_entity(director);
    assert(director_entity.has_value());
    const network_example::DirectorRuntime& director_runtime =
        engine.world_.registry().get<network_example::DirectorRuntime>(
            *director_entity);
    assert(director_runtime.spawn_cursor == 0);
    assert(director_runtime.next_tick == 0);
}

void director_runtime_spawns_agents_until_target_count() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    load_director_catalog(&engine);

    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_template_id = 100;
    create_info.position = KernelVec3{0.0f, 0.0f, 0.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};

    std::uint32_t director = 0;
    assert(engine.server_create_entity(create_info, &director));

    engine.update(1.0f / 30.0f);
    assert(engine.command_queue_.size() == 3);
    assert(engine.pending_director_intents_.empty());
    assert(engine.last_director_intent_processed_count_ == 1);
    assert(engine.last_director_intent_created_count_ == 3);
    assert(engine.last_director_intent_failed_count_ == 0);
    assert(engine.last_director_intent_unsupported_count_ == 0);
    const std::optional<entt::entity> director_entity =
        engine.world_.find_entity(director);
    assert(director_entity.has_value());
    const network_example::DirectorRuntime& director_runtime =
        engine.world_.registry().get<network_example::DirectorRuntime>(
            *director_entity);
    assert(director_runtime.spawn_cursor == 3);
    assert(director_runtime.next_tick == 2);
    engine.update(1.0f / 30.0f);
    assert(engine.last_simulation_command_processed_count_ == 3);
    assert(engine.failed_simulation_command_count_ == 0);
    for (std::uint32_t tick = 0; tick < 4; ++tick) {
        engine.update(1.0f / 30.0f);
    }

    std::array<KernelServerEntityState, 8> states{};
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(state);
    }
    const std::uint32_t count = engine.server_query_entities(
        network_example::EntityType::kActor,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    std::uint32_t agent_count = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].actor_type == KernelActorType_Agent) {
            ++agent_count;
            assert(states[index].actor_template_id == 2);
            const std::optional<entt::entity> agent_entity =
                engine.world_.find_entity(states[index].net_id);
            assert(agent_entity.has_value());
            assert(engine.world_.registry().all_of<network_example::AgentRuntime>(
                *agent_entity));
            assert(engine.world_.registry().all_of<network_example::AgentSentryRuntime>(
                *agent_entity));
            assert(engine.vision_configs_.find(states[index].net_id) !=
                   engine.vision_configs_.end());
        }
    }
    assert(agent_count == 3);
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

int main() {
    lifecycle_system_create_matches_legacy_path();
    lifecycle_system_materializes_server_only_director_template();
    director_ai_emits_spawn_intent_without_enqueueing_create_command();
    director_ai_ignores_non_director_entity_sources();
    director_intent_executor_reports_unsupported_intent();
    director_intent_executor_rejects_invalid_source_director();
    director_intent_executor_does_not_commit_when_enqueue_fails();
    director_runtime_spawns_agents_until_target_count();
    lifecycle_system_destroy_matches_legacy_side_effects();
    return 0;
}
