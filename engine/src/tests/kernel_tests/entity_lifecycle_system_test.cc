#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>
#include <variant>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/public/kernel_api.h"

#define private public
#include "kernel/src/kernel.h"
#include "simulation/src/systems.h"

namespace {

// assert() is compiled out under -c opt, which is the configuration this suite
// runs in, so every check in this file used to be skipped. This was the last of
// five files in this area found that way.
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
    require(lifecycle.create_entity(engine, player_create_info(), &net_id));
    require(net_id != 0);

    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(engine.server_get_entity_state(net_id, &state));
    require(state.valid != 0u);
    require(state.net_id == net_id);
    require(state.entity_type == static_cast<std::uint16_t>(network_example::EntityType::kActor));
    require(state.actor_type == KernelActorType_Player);
    require(state.owner_peer == 42);
    require(state.animation_state == 7);
    // The authored flags survive creation. Not an equality check: the reported
    // word is the authored bits OR'd with flags derived from the entity's own
    // state, and this entity has a MovementState, so it also reports FALLING.
    // Pinning the whole word made this assertion a statement about
    // derived_visual_flags rather than about create_entity, and it went stale
    // the moment that gained the grounded/falling bits -- which is exactly what
    // it had been failing on, unnoticed, while assert() was compiled out.
    require((state.visual_flags & 0x44u) == 0x44u);
    require(state.position.x == 2.0f);
    require(state.position.z == 3.0f);
    require(engine.events_.size() == 1);
    require(engine.events_[0].type == KernelEventType_EntitySpawned);
    // No snapshot assertion. Creation used to build one; snapshots are built in
    // their own pass now, which this test never runs -- and a freshly created
    // actor is deliberately held out of the first one anyway until physics
    // finalises it. Asserting an empty snapshot here would state nothing, and
    // asserting a populated one would be testing the snapshot pass rather than
    // create_entity.
}

void lifecycle_system_destroy_matches_legacy_side_effects() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    std::uint32_t net_id = 0;
    require(engine.server_create_entity(player_create_info(), &net_id));
    engine.events_.clear();
    engine.lifecycle_events_.clear();
    engine.vision_configs_[net_id] = KernelAgentVisionConfig{};
    engine.vision_states_[net_id] = network_example::KernelEngine::VisionRuntimeState{};

    network_example::EntityLifecycleSystem lifecycle;
    require(lifecycle.destroy_entity(engine, net_id, KernelDespawnReason_Destroyed));

    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(!engine.server_get_entity_state(net_id, &state));
    require(engine.vision_configs_.find(net_id) == engine.vision_configs_.end());
    require(engine.vision_states_.find(net_id) == engine.vision_states_.end());
    require(engine.events_.size() == 1);
    require(engine.events_[0].type == KernelEventType_EntityDestroyed);
    require(engine.lifecycle_events_.size() == 1);
    require(engine.lifecycle_events_[0].net_id == net_id);
    require(engine.lifecycle_events_[0].reason == KernelDespawnReason_Destroyed);
    require(engine.latest_snapshot_.entities.empty());
}

// Four deaths in one tick, one per cell of {player, agent} x {default, explicit
// policy}: the policy alone decides who stays, every one of them reports
// EntityDied, and the corpse that stays keeps neither its knockback nor its
// stagger.
void damage_death_follows_each_entity_policy() {
    using network_example::ConfirmedDamage;
    using network_example::DeathBehavior;
    using network_example::DeathPolicy;
    using network_example::Health;
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    entt::registry& registry = engine.world_.registry();

    const auto with_health = [&](std::uint32_t net_id) {
        const auto entity = engine.world_.find_entity(net_id);
        require(entity.has_value());
        registry.emplace_or_replace<Health>(*entity, Health{100, 100});
        return *entity;
    };
    std::uint32_t player = 0;
    require(engine.server_create_entity(player_create_info(), &player));
    std::uint32_t destroyed_player = 0;
    require(engine.server_create_entity(player_create_info(), &destroyed_player));
    const std::uint32_t agent =
        engine.world_.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    const std::uint32_t dormant_agent =
        engine.world_.spawn_enemy(glm::vec3{6.0f, 0.0f, 0.0f});
    const entt::entity player_entity = with_health(player);
    registry.emplace_or_replace<DeathBehavior>(
        with_health(destroyed_player), DeathBehavior{DeathPolicy::kDestroy});
    with_health(agent);
    registry.emplace_or_replace<DeathBehavior>(
        with_health(dormant_agent), DeathBehavior{DeathPolicy::kDormant});

    // What the killing blow leaves on the player.
    registry.emplace_or_replace<network_example::ImpulseLockout>(
        player_entity, network_example::ImpulseLockout{1000u, 0u});
    network_example::StaggerState stagger;
    stagger.until_tick = 1000u;
    registry.emplace_or_replace<network_example::StaggerState>(
        player_entity, stagger);
    registry.get_or_emplace<network_example::Velocity>(player_entity).linear =
        glm::vec3{3.0f, -2.0f, 4.0f};

    std::vector<ConfirmedDamage> damage;
    for (const std::uint32_t target : {player, destroyed_player, agent, dormant_agent}) {
        ConfirmedDamage hit;
        hit.source_net_id = 99u;
        hit.target_net_id = target;
        hit.source_peer = 5u;
        hit.damage = 1000u;
        damage.push_back(hit);
    }
    engine.events_.clear();
    const std::vector<ConfirmedDamage> depleted =
        network_example::apply_damage_applications(
            engine.world_, damage, engine.current_tick(), &engine.events_);
    require(depleted.size() == 4u);
    network_example::EntityLifecycleSystem lifecycle;
    lifecycle.enter_death_state(engine, depleted);
    lifecycle.destroy_dead_entities(engine, depleted);

    require(engine.world_.find_entity(player).has_value());
    require(!engine.world_.find_entity(destroyed_player).has_value());
    require(!engine.world_.find_entity(agent).has_value());
    require(engine.world_.find_entity(dormant_agent).has_value());

    std::size_t died = 0;
    for (const KernelEvent& event : engine.events_) {
        if (event.type != KernelEventType_EntityDied) {
            continue;
        }
        ++died;
        require(event.peer_id == 5u);
        require(event.code == 99u);
    }
    require(died == 4u);

    require(!registry.all_of<network_example::ImpulseLockout>(player_entity));
    require(!registry.all_of<network_example::StaggerState>(player_entity));
    const glm::vec3 velocity =
        registry.get<network_example::Velocity>(player_entity).linear;
    require(velocity.x == 0.0f);
    require(velocity.z == 0.0f);
    // Gravity's axis is left alone: a body killed in the air still falls.
    require(velocity.y == -2.0f);
}

// A revive is for the dead only, brings the body back whole and lifted, drops
// what the old life left on it, and holds off damage for exactly as long as
// asked.
void revive_restores_only_the_dead() {
    using network_example::ConfirmedDamage;
    using network_example::Health;
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    entt::registry& registry = engine.world_.registry();

    std::uint32_t player = 0;
    require(engine.server_create_entity(player_create_info(), &player));
    const entt::entity entity = *engine.world_.find_entity(player);
    registry.emplace_or_replace<Health>(entity, Health{100, 100});
    const float ground_y =
        registry.get<network_example::Transform>(entity).position.y;

    // The living are not revived, and nothing about them changes.
    require(!engine.server_revive_entity(player, 5.0f, 60u));
    require(!registry.all_of<network_example::DamageImmunity>(entity));

    const auto hit = [&](std::uint16_t amount, std::uint32_t tick) {
        ConfirmedDamage damage;
        damage.source_net_id = 99u;
        damage.target_net_id = player;
        damage.damage = amount;
        engine.events_.clear();
        return network_example::apply_damage_applications(
            engine.world_, {damage}, tick, &engine.events_);
    };
    const std::vector<ConfirmedDamage> depleted = hit(1000u, engine.current_tick());
    require(depleted.size() == 1u);
    network_example::EntityLifecycleSystem lifecycle;
    lifecycle.enter_death_state(engine, depleted);
    lifecycle.destroy_dead_entities(engine, depleted);
    require(engine.world_.find_entity(player).has_value());

    // What a body can pick up while it lies there.
    network_example::StatusEffectState status;
    network_example::ActiveStatusEffect burning;
    burning.instance_id = 9u;
    burning.status_effect_id = 3u;
    status.active.push_back(burning);
    status.speed_modifiers.push_back(network_example::SpeedModifier{9u, 0.0f, 0.5f});
    registry.emplace_or_replace<network_example::StatusEffectState>(entity, status);
    registry.get_or_emplace<network_example::Velocity>(entity).linear =
        glm::vec3{0.0f, -4.0f, 0.0f};

    const std::uint32_t revive_tick = engine.current_tick();
    require(engine.server_revive_entity(player, 5.0f, 60u));
    require(registry.get<Health>(entity).hp == 100u);
    // No movement capsule here, so nothing to clamp against: the whole lift.
    require(registry.get<network_example::Transform>(entity).position.y ==
            ground_y + 5.0f);
    require(registry.get<network_example::Velocity>(entity).linear ==
            glm::vec3{0.0f});
    const auto& cleared = registry.get<network_example::StatusEffectState>(entity);
    require(cleared.active.empty());
    require(cleared.speed_modifiers.empty());
    require(registry.get<network_example::DamageImmunity>(entity).until_tick ==
            revive_tick + 60u);

    // Alive again, so a second revive is refused.
    require(!engine.server_revive_entity(player, 5.0f, 60u));

    // Immune through the last protected tick: no health lost, no hit reported.
    require(hit(10u, revive_tick + 59u).empty());
    require(registry.get<Health>(entity).hp == 100u);
    require(engine.events_.empty());
    // And not a tick longer.
    hit(10u, revive_tick + 60u);
    require(registry.get<Health>(entity).hp == 90u);
}

}  // namespace

// The world-rule director cases that used to live here are gone with the
// mechanism: a world rule is game_server's WorldRuleDirector now, and its
// behaviour -- filling a target, the wall-clock interval, the golden-angle ring
// and its cursor -- is covered by //game_server:world_rule_director_test with
// assertions that are not compiled out.
//
// NOTE: the assertions in this file are require(), and this suite runs under
// -c opt, where they are compiled to nothing. It is red for a reason that is
// therefore not an assertion, and that predates this change. Converting it is
// its own task.
int main() {
    lifecycle_system_create_matches_legacy_path();
    lifecycle_system_destroy_matches_legacy_side_effects();
    damage_death_follows_each_entity_policy();
    revive_restores_only_the_dead();
    return 0;
}
