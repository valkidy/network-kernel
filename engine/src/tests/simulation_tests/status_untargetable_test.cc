// apply_untargetable: while the status instance stands, nothing can strike
// its subject. The water bubble's third part (docs/WATER_BUBBLE_PLAN.md, P3).
//
// Every case runs twice, on a target held and on one that is not, because a
// query that finds nothing is also what a target placed out of reach, a dead
// target (spawn_enemy starts at 0 hp) or a broken setup looks like. The kernel
// side -- its own physics world, a thrown prop's on_collision, agent vision --
// is //engine/src/tests/kernel_tests:status_untargetable_kernel_test.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/src/kernel.h"
#include "simulation/public/action_graph.h"
#include "simulation/public/simulation.h"
#include "simulation/src/systems.h"
#include "sync/public/history_buffer.h"

namespace {

using namespace network_example;

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "status_untargetable_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

NetId spawn_target(World& world, const glm::vec3& position) {
    const NetId net_id = world.spawn_enemy(position);
    const entt::entity entity = *world.find_entity(net_id);
    world.registry().get_or_emplace<Health>(entity) = Health{50, 50};
    world.registry().get<Hitbox>(entity) =
        Hitbox{{0.0f, 0.5f, 0.0f}, {0.25f, 0.5f, 0.25f}, 0};
    return net_id;
}

// What the apply_untargetable commit leaves on an actor; the status path
// itself is covered at the end.
void hold(World& world, NetId net_id, std::uint32_t instance_id = 1u) {
    StatusEffectState& state =
        world.registry().get_or_emplace<StatusEffectState>(*world.find_entity(net_id));
    ActiveStatusEffect active;
    active.instance_id = instance_id;
    active.status_effect_id = 1301u;
    active.channel_id = 31u;
    active.expire_tick = 100000u;
    active.untargetable = true;
    state.active.push_back(active);
}

void release(World& world, NetId net_id) {
    world.registry().get<StatusEffectState>(*world.find_entity(net_id)).active.clear();
}

std::uint16_t hp(const World& world, NetId net_id) {
    return world.registry().get<Health>(*world.find_entity(net_id)).hp;
}

// ---------------------------------------------------------------------------
// The hit volumes
// ---------------------------------------------------------------------------

NetId ray_hit(World& world, const glm::vec3& origin) {
    physics::RayCastRequest request{};
    request.origin = origin;
    request.direction = glm::vec3{1.0f, 0.0f, 0.0f};
    request.max_distance = 20.0f;
    physics::CollisionHit hit{};
    return world.collision_world()->ray_cast_closest(request, &hit)
        ? hit.identity.entity_net_id
        : 0u;
}

// The live world every hitscan, projectile, melee and beam query asks.
void the_live_world_leaves_its_hit_volume_out() {
    World world;
    const NetId target = spawn_target(world, glm::vec3{5.0f, 0.0f, 0.0f});
    require(ray_hit(world, glm::vec3{0.0f, 0.5f, 0.0f}) == target);
    hold(world, target);
    require(ray_hit(world, glm::vec3{0.0f, 0.5f, 0.0f}) == 0u);
    // The bubble burst: struck again, falling or not.
    release(world, target);
    require(ray_hit(world, glm::vec3{0.0f, 0.5f, 0.0f}) == target);
}

void a_rewound_shot_passes_through_it_too() {
    World world;
    const NetId target = spawn_target(world, glm::vec3{5.0f, 0.0f, 0.0f});
    HistoryBuffer history(8);
    history.write_frame(world, 1u);
    hold(world, target);
    history.write_frame(world, 2u);
    HistoricalHitResult hit;
    require(raycast_history_frame(
        *history.find_frame(1u), glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f}, 20.0f, 0u, &hit));
    require(hit.net_id == target);
    require(!raycast_history_frame(
        *history.find_frame(2u), glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f}, 20.0f, 0u, &hit));
}

NetId spawn_blast(World& world, const glm::vec3& position, float radius) {
    const NetId net_id = world.spawn_projectile(0, position, glm::vec3{0.0f});
    const entt::entity entity = *world.find_entity(net_id);
    ProjectileState& projectile = world.registry().get<ProjectileState>(entity);
    projectile.weapon_id = 7;
    projectile.damage = 20;
    projectile.collision_mask = kCollisionMaskDamageable;
    projectile.max_lifetime_ticks = 0;
    world.registry().replace<Hitbox>(
        entity, Hitbox{{0.0f, 0.0f, 0.0f}, {radius, radius, radius}, 0});
    world.registry().emplace<ProjectileAreaEffectRuntime>(
        entity,
        ProjectileAreaEffectRuntime{
            radius, 20, 10, 0, 7, kCollisionMaskDamageable,
            ProjectileDamageFalloff::kNone, {}});
    return net_id;
}

void an_area_effect_passes_it_by() {
    World world;
    const NetId held = spawn_target(world, glm::vec3{1.0f, 0.0f, 0.0f});
    const NetId control = spawn_target(world, glm::vec3{-1.0f, 0.0f, 0.0f});
    hold(world, held);
    spawn_blast(world, glm::vec3{0.0f, 0.5f, 0.0f}, 2.0f);
    DamagePipeline pipeline;
    std::vector<KernelEvent> events;
    simulate_area_effects(world, 0, &events, &pipeline);
    pipeline.confirm_ready(world, 0, 0, &events);
    require(hp(world, control) == 30u);
    require(hp(world, held) == 50u);
}

NetId spawn_beam(World& world, NetId shooter, const glm::vec3& origin) {
    const NetId net_id = world.spawn_projectile(1, origin, glm::vec3{0.0f});
    const entt::entity entity = *world.find_entity(net_id);
    ProjectileState& projectile = world.registry().get<ProjectileState>(entity);
    projectile.weapon_id = 5;
    projectile.damage = 10;
    projectile.shooter_net_id = shooter;
    projectile.collision_mask = kCollisionLayerHostileSide;
    projectile.max_lifetime_ticks = 0;
    world.registry().emplace<ProjectileBeamRuntime>(
        entity,
        ProjectileBeamRuntime{
            shooter, origin, glm::vec3{1.0f, 0.0f, 0.0f}, 10.0f, 0.25f, 10, 1000u,
            5, kCollisionLayerHostileSide, {}});
    return net_id;
}

void a_beam_passes_through_it() {
    for (const bool held : {false, true}) {
        World world;
        const NetId shooter = world.spawn_player(1, glm::vec3{0.0f});
        world.registry().get_or_emplace<Health>(*world.find_entity(shooter)) =
            Health{100, 100};
        const NetId target = spawn_target(world, glm::vec3{4.0f, 0.0f, 0.0f});
        if (held) {
            hold(world, target);
        }
        spawn_beam(world, shooter, glm::vec3{0.0f, 0.5f, 0.0f});
        DamagePipeline pipeline;
        std::vector<KernelEvent> events;
        for (std::uint32_t tick = 1; tick <= 30; ++tick) {
            simulate_beams(world, tick, 1.0f / 30.0f, tick * 33333ull, &events, &pipeline);
            apply_damage_applications(
                world, pipeline.drain_ready_damage(world, tick * 33333ull), tick,
                &events);
        }
        require(held ? hp(world, target) == 50u : hp(world, target) < 50u);
    }
}

// ---------------------------------------------------------------------------
// What still arrives
// ---------------------------------------------------------------------------

void damage_already_on_its_way_is_discarded() {
    World world;
    const NetId target = spawn_target(world, glm::vec3{0.0f});
    ConfirmedDamage hit;
    hit.target_net_id = target;
    hit.source_peer = 9u;
    hit.damage = 10;
    std::vector<KernelEvent> events;
    hold(world, target);
    apply_damage_applications(world, {hit}, 1u, &events);
    require(hp(world, target) == 50u);
    require(events.empty());
    // Once it ends -- the fall included -- damage lands again.
    release(world, target);
    apply_damage_applications(world, {hit}, 2u, &events);
    require(hp(world, target) == 40u);
}

bool run_batch(
    KernelEngine& engine,
    std::vector<ActionGraphCommand> commands,
    std::uint64_t request_id,
    NetId source,
    std::uint32_t status_instance_id = 0u) {
    const TriggerEvent event{TriggerEventType::kActivated, source, source, 0u};
    ActionExecutionProvenance provenance;
    provenance.request_id = request_id;
    provenance.server_tick = engine.current_tick();
    provenance.instigator = source;
    provenance.status_instance_id = status_instance_id;
    return execute_action_graph_command_batch(
        engine,
        ActionGraphCommandBatch{
            event, provenance, static_cast<std::uint32_t>(request_id),
            std::move(commands)},
        0u);
}

// An event queued before the bubble, or a graph that names the target
// outright, never reaches it either -- except its own statuses running on.
void a_command_naming_it_is_dropped_unless_it_is_its_own_status() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1, glm::vec3{0.0f, 0.0f, -10.0f});
    const NetId held = spawn_target(world, glm::vec3{2.0f, 0.0f, 0.0f});
    const NetId control = spawn_target(world, glm::vec3{-2.0f, 0.0f, 0.0f});
    hold(world, held);
    RuntimeStatusEffectTemplate status;
    status.status_effect_id = 1302u;
    status.channel_id = 32u;
    status.duration_ticks = 30u;
    world.set_status_effect_templates({status});

    // Wounded, so a heal has somewhere to go. (A negative health change is
    // damage, and goes the way damage goes: discarded on arrival, above.)
    for (const NetId target : {control, held}) {
        world.registry().get<Health>(*world.find_entity(target)).hp = 40;
    }
    std::uint64_t request = 1u;
    for (const NetId target : {control, held}) {
        const entt::entity entity = *world.find_entity(target);
        ActionApplyImpulseCommand impulse;
        impulse.source = source;
        impulse.target = target;
        impulse.strength = 5.0f;
        impulse.direction = glm::vec3{0.0f, 1.0f, 0.0f};
        ActionApplyHealthChangeCommand health{source, target, 7};
        ActionApplyStatusCommand apply{source, target, 1302u};
        require(run_batch(engine, {impulse, health, apply}, request++, source));
        const bool struck = target == control;
        require(world.registry().get<Velocity>(entity).linear.y == (struck ? 5.0f : 0.0f));
        require(hp(world, target) == (struck ? 47u : 40u));
        const StatusEffectState* state = world.registry().try_get<StatusEffectState>(entity);
        const bool has_status = state != nullptr &&
            std::any_of(state->active.begin(), state->active.end(),
                        [](const ActiveStatusEffect& active) {
                            return active.status_effect_id == 1302u;
                        });
        require(has_status == struck);
    }
    // Its own status's lifecycle -- a regeneration tick, say -- still runs.
    ActionApplyHealthChangeCommand regen{held, held, 5};
    require(run_batch(engine, {regen}, request++, held, 1u));
    require(hp(world, held) == 45u);
}

// ---------------------------------------------------------------------------
// The action, through the status
// ---------------------------------------------------------------------------

KernelActionTriggerDefinition untargetable_trigger(std::uint8_t target_source) {
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(trigger);
    trigger.action_count = 1u;
    trigger.actions[0].action_type = KernelEntityTriggerActionType_ApplyUntargetable;
    trigger.actions[0].target_source = target_source;
    return trigger;
}

void only_a_status_on_apply_onto_its_subject_compiles() {
    require(compile_action_trigger_definition(
                TriggerEventType::kStatusApplied,
                untargetable_trigger(KernelEntityRefSource_EventSubject))
                .has_value());
    require(!compile_action_trigger_definition(
                 TriggerEventType::kCollision,
                 untargetable_trigger(KernelEntityRefSource_EventSubject))
                 .has_value());
    require(!compile_action_trigger_definition(
                 TriggerEventType::kStatusApplied,
                 untargetable_trigger(KernelEntityRefSource_EventTarget))
                 .has_value());
}

void the_status_makes_it_untargetable_for_exactly_its_lifetime() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1, glm::vec3{0.0f});
    const NetId target = spawn_target(world, glm::vec3{5.0f, 0.0f, 0.0f});
    const entt::entity entity = *world.find_entity(target);
    RuntimeStatusEffectTemplate status;
    status.status_effect_id = 1303u;
    status.channel_id = 33u;
    status.duration_ticks = 30u;
    status.on_apply_binding = compile_action_trigger_definition(
        TriggerEventType::kStatusApplied,
        untargetable_trigger(KernelEntityRefSource_EventSubject));
    world.set_status_effect_templates({status});

    require(!status_untargetable(world, entity));
    ActionApplyStatusCommand apply{source, target, 1303u};
    require(run_batch(engine, {apply}, 1u, source));
    require(status_untargetable(world, entity));
    world.registry().get<StatusEffectState>(entity).active[0].expire_tick = 0u;
    simulate_status_effects(engine, 0u);
    require(!status_untargetable(world, entity));
}

}  // namespace

int main() {
    the_live_world_leaves_its_hit_volume_out();
    a_rewound_shot_passes_through_it_too();
    an_area_effect_passes_it_by();
    a_beam_passes_through_it();
    damage_already_on_its_way_is_discarded();
    a_command_naming_it_is_dropped_unless_it_is_its_own_status();
    only_a_status_on_apply_onto_its_subject_compiles();
    the_status_makes_it_untargetable_for_exactly_its_lifetime();
    std::puts("status_untargetable_test passed");
    return 0;
}
