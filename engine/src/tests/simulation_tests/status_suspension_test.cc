// apply_suspend_movement: a status on_apply action that holds its subject in
// the air -- rising, drifting, out of its own control -- for as long as that
// status instance stands, and drops it straight down when it ends. The water
// bubble's second part (docs/WATER_BUBBLE_PLAN.md, P2).
//
// Movement runs on a bare World with its own physics, as apply_pull_test does,
// so the fixture owns the tick count the lockouts are measured against.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/src/kernel.h"
#include "physics/public/physics_world.h"
#include "simulation/public/action_graph.h"
#include "simulation/public/simulation.h"
#include "simulation/src/systems.h"

namespace {

using namespace network_example;

using network_example::physics::CollisionLayer;
using network_example::physics::CollisionObjectDescriptor;
using network_example::physics::CollisionObjectIdentity;
using network_example::physics::CollisionObjectKind;
using network_example::physics::CollisionShapeType;
using network_example::physics::PhysicsWorld;

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "status_suspension_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr float kTick = 1.0f / 30.0f;
constexpr float kGravity = -9.81f;
// The capsule: feet at the transform, 1.8 m tall.
constexpr float kBodyHeight = 1.8f;

bool nearly(float lhs, float rhs, float tolerance = 0.001f) {
    return std::fabs(lhs - rhs) < tolerance;
}

struct Fixture {
    Fixture(MovementState::ControllerType controller, glm::vec3 spawn, bool ground = true)
        : world(false) {
        world.set_collision_world(&physics);
        if (ground) {
            add_box(100, glm::vec3{0.0f, -0.5f, 0.0f}, glm::vec3{50.0f, 0.5f, 50.0f});
        }
        actor = world.spawn_player(7, spawn);
        entity = *world.find_entity(actor);
        MovementState& movement = world.registry().get<MovementState>(entity);
        movement.speed_meters_per_second = 5.0f;
        movement.controller_type = controller;
        movement.movement_collider_template_id = 10;
        movement.gravity = glm::vec3{0.0f, kGravity, 0.0f};
        movement.max_slope_degrees = 50.0f;
        movement.step_height = 0.4f;
        movement.ground_probe_distance = 0.25f;
        movement.ground_snap_distance = 0.5f;
        movement.movement_collision_mask =
            physics::collision_layer_bit(CollisionLayer::kTerrain) |
            physics::collision_layer_bit(CollisionLayer::kStaticObstacle);
        if (controller == MovementState::ControllerType::kHover) {
            movement.hover_height_meters = 9.0f;
            movement.hover_vertical_speed_meters_per_second = 3.0f;
        }

        ColliderInstance collider{};
        collider.collider_template_id = 10;
        collider.owner_net_id = actor;
        collider.entity_net_id = actor;
        collider.entity_type = EntityType::kActor;
        collider.actor_type = ActorType::kPlayer;
        collider.shape_type = ColliderShapeType::kCapsule;
        collider.purpose_flags = KernelColliderPurpose_Movement;
        collider.local_center = glm::vec3{0.0f, 0.9f, 0.0f};
        collider.world_center = spawn + collider.local_center;
        collider.radius = 0.35f;
        collider.capsule_half_height = 0.55f;
        const ColliderInstance& stored =
            world.collider_registry().upsert_entity_collider(actor, 10, collider);
        movement.movement_collider_id = stored.collider_id;
    }

    void add_box(std::uint32_t id, glm::vec3 center, glm::vec3 half_extents) {
        CollisionObjectDescriptor object{};
        object.identity = CollisionObjectIdentity{
            0, id, 0,
            CollisionObjectKind::kStaticObstacle,
            CollisionLayer::kStaticObstacle,
        };
        object.shape.type = CollisionShapeType::kBox;
        object.shape.half_extents = half_extents;
        object.position = center;
        std::string error;
        require(physics.upsert_object(object, &error));
    }

    // A slab whose underside is at `height`, over the whole test area.
    void add_roof(float height) {
        add_box(101, glm::vec3{0.0f, height + 0.2f, 0.0f}, glm::vec3{50.0f, 0.2f, 50.0f});
    }

    // Steering hard the whole way, as an AI controller would: a suspension is
    // what must keep it from steering.
    void tick(glm::vec2 move = glm::vec2{0.0f}) {
        KernelPlayerInput input{};
        input.move = KernelVec2{move.x, move.y};
        const std::vector<QueuedInput> inputs{
            QueuedInput{7, input, tick_index, 0, false, 0},
        };
        simulate_actor_movement(world, inputs, kTick, tick_index, &events, &stats);
        ++tick_index;
    }

    // What the apply_suspend_movement commit does, on an instance standing in
    // for the status (the status path itself is covered below).
    void suspend(float rise, glm::vec3 drift) {
        StatusEffectState& state =
            world.registry().get_or_emplace<StatusEffectState>(entity);
        ActiveStatusEffect active;
        active.instance_id = 1u;
        active.status_effect_id = 1201u;
        active.channel_id = 21u;
        active.expire_tick = tick_index + 10000u;
        active.suspends_movement = true;
        active.suspend_rise_speed = rise;
        active.suspend_drift_velocity = drift;
        state.active.push_back(active);
        world.registry().emplace_or_replace<HeldInSuspension>(entity);
        MovementState& movement = world.registry().get<MovementState>(entity);
        movement.ground_state = MovementState::GroundState::kAirborne;
        movement.has_controller_height = false;
    }

    // The status ends, and the next tick's settle arms the drop.
    std::vector<std::pair<NetId, float>> pop() {
        world.registry().get<StatusEffectState>(entity).active.clear();
        return settle_status_suspensions(world, tick_index, kTick);
    }

    glm::vec3 position() const { return world.registry().get<Transform>(entity).position; }
    glm::vec3 velocity() const { return world.registry().get<Velocity>(entity).linear; }
    bool grounded() const {
        return world.registry().get<MovementState>(entity).ground_state ==
            MovementState::GroundState::kGrounded;
    }
    const ImpulseLockout* lockout() const {
        return world.registry().try_get<ImpulseLockout>(entity);
    }

    PhysicsWorld physics;
    World world;
    NetId actor = 0;
    entt::entity entity = entt::null;
    std::uint32_t tick_index = 1;
    std::vector<KernelEvent> events;
    MovementSimulationStats stats{};
};

// ---------------------------------------------------------------------------
// The rise
// ---------------------------------------------------------------------------

void the_rise_and_drift_are_exact_and_input_cannot_steer_them() {
    constexpr std::uint32_t kTicks = 30u;
    constexpr float kRise = 1.0f;
    const glm::vec3 drift{0.3f, 0.0f, -0.2f};
    // Control: the same steering, unheld, moves the character.
    {
        Fixture f(MovementState::ControllerType::kCharacter, glm::vec3{0.0f});
        for (int i = 0; i < 5; ++i) f.tick();
        require(f.grounded());
        const glm::vec3 start = f.position();
        for (std::uint32_t i = 0; i < kTicks; ++i) f.tick(glm::vec2{-1.0f, 0.0f});
        require(f.position().x < start.x - 1.0f);
    }
    for (const auto controller :
         {MovementState::ControllerType::kCharacter,
          MovementState::ControllerType::kGrounded}) {
        Fixture f(controller, glm::vec3{0.0f});
        for (int i = 0; i < 5; ++i) f.tick();
        require(f.grounded());
        const glm::vec3 start = f.position();
        f.suspend(kRise, drift);
        for (std::uint32_t i = 0; i < kTicks; ++i) {
            f.tick(glm::vec2{-1.0f, 0.0f});
            require(!f.grounded());
        }
        const glm::vec3 moved = f.position() - start;
        const float seconds = static_cast<float>(kTicks) * kTick;
        if (!nearly(moved.y, kRise * seconds, 0.01f) ||
            !nearly(moved.x, drift.x * seconds, 0.01f) ||
            !nearly(moved.z, drift.z * seconds, 0.01f)) {
            std::fprintf(
                stderr, "controller %d moved (%.4f, %.4f, %.4f)\n",
                static_cast<int>(controller), moved.x, moved.y, moved.z);
        }
        require(nearly(moved.y, kRise * seconds, 0.01f));
        require(nearly(moved.x, drift.x * seconds, 0.01f));
        require(nearly(moved.z, drift.z * seconds, 0.01f));
    }
}

void a_roof_stops_the_rise_on_every_controller() {
    constexpr float kRoof = 3.0f;
    for (const auto controller :
         {MovementState::ControllerType::kCharacter,
          MovementState::ControllerType::kGrounded}) {
        Fixture f(controller, glm::vec3{0.0f});
        for (int i = 0; i < 5; ++i) f.tick();
        require(f.grounded());
        // After it is seated: a grounded actor's first placement looks down
        // from far above, and would stand it on the roof.
        f.add_roof(kRoof);
        f.suspend(2.0f, glm::vec3{0.0f});
        for (int i = 0; i < 60; ++i) f.tick();
        // It rose, and its head is under the roof, not through it: 2 m/s for
        // two seconds would have put its feet at 4 m.
        const float feet = f.position().y;
        if (!(feet > 0.5f && feet + kBodyHeight <= kRoof + 0.01f)) {
            std::fprintf(stderr, "controller %d feet at %.4f\n",
                         static_cast<int>(controller), feet);
        }
        require(feet > 0.5f);
        require(feet + kBodyHeight <= kRoof + 0.01f);
    }
    // The hover, the same, from its 9 m under a roof at 11 m.
    Fixture f(MovementState::ControllerType::kHover, glm::vec3{0.0f, 9.0f, 0.0f});
    f.add_roof(11.5f);
    for (int i = 0; i < 10; ++i) f.tick();
    require(nearly(f.position().y, 9.0f, 0.01f));
    f.suspend(2.0f, glm::vec3{0.0f});
    for (int i = 0; i < 60; ++i) f.tick();
    require(f.position().y > 9.3f);
    require(f.position().y + kBodyHeight <= 11.5f + 0.01f);
}

// ---------------------------------------------------------------------------
// The drop
// ---------------------------------------------------------------------------

void the_drop_is_straight_down_locked_and_lands_on_the_predicted_tick() {
    for (const auto controller :
         {MovementState::ControllerType::kCharacter,
          MovementState::ControllerType::kGrounded}) {
        Fixture f(controller, glm::vec3{0.0f});
        for (int i = 0; i < 5; ++i) f.tick();
        f.suspend(1.5f, glm::vec3{0.4f, 0.0f, 0.0f});
        for (int i = 0; i < 40; ++i) f.tick();
        const glm::vec3 top = f.position();
        require(top.y > 1.5f);

        const auto drops = f.pop();
        require(drops.size() == 1u);
        require(drops[0].first == f.actor);
        // The anchor's floor is the ground under it.
        require(nearly(drops[0].second, 0.0f, 0.02f));
        require(f.velocity() == glm::vec3{0.0f});
        require(f.lockout() != nullptr && f.lockout()->free_fall);
        require(!f.world.registry().all_of<HeldInSuspension>(f.entity));

        const std::uint32_t predicted = knockback_flight_ticks(
            top, glm::vec3{0.0f}, kGravity, drops[0].second, kTick,
            KERNEL_MAX_IMPULSE_LOCKOUT_TICKS);
        std::uint32_t landed = 0;
        for (std::uint32_t step = 1; step <= predicted + 10u; ++step) {
            // Locked out of its own actions the whole way down.
            require(action_block_reason(f.world, f.entity, f.tick_index) ==
                    KernelLocalActionResultReason_KnockedBack);
            f.tick(glm::vec2{1.0f, 0.0f});
            // Straight down, whatever it steers.
            require(nearly(f.position().x, top.x, 0.0005f));
            if (f.grounded()) {
                landed = step;
                break;
            }
        }
        if (landed != predicted) {
            std::fprintf(stderr, "controller %d landed on %u, predicted %u\n",
                         static_cast<int>(controller), landed, predicted);
        }
        require(landed == predicted);
        require(f.lockout() == nullptr);
    }
}

void a_drone_drops_to_the_ground_and_climbs_back() {
    Fixture f(MovementState::ControllerType::kHover, glm::vec3{0.0f, 9.0f, 0.0f});
    for (int i = 0; i < 10; ++i) f.tick();
    require(nearly(f.position().y, 9.0f, 0.01f));
    f.suspend(1.0f, glm::vec3{0.0f});
    for (int i = 0; i < 30; ++i) f.tick();
    const glm::vec3 top = f.position();
    require(nearly(top.y, 10.0f, 0.01f));

    const auto drops = f.pop();
    require(drops.size() == 1u);
    // The ground, not the 9 m it hovered at: a client floors the drawn drop here.
    require(nearly(drops[0].second, 0.0f, 0.02f));
    const std::uint32_t predicted = knockback_flight_ticks(
        top, glm::vec3{0.0f}, kGravity, drops[0].second, kTick,
        KERNEL_MAX_IMPULSE_LOCKOUT_TICKS);
    std::uint32_t landed = 0;
    for (std::uint32_t step = 1; step <= predicted + 10u; ++step) {
        f.tick();
        if (f.lockout() == nullptr) {
            landed = step;
            break;
        }
    }
    if (landed != predicted) {
        std::fprintf(stderr, "drone landed on %u, predicted %u, at %.3f\n",
                     landed, predicted, f.position().y);
    }
    require(landed == predicted);
    require(f.position().y < 0.05f);
    // Then it is a hover again: back to 9 m at 3 m/s, three seconds.
    for (int i = 0; i < 100; ++i) f.tick();
    require(nearly(f.position().y, 9.0f, 0.01f));
}

void a_drone_with_no_floor_holds_where_its_drop_runs_out() {
    Fixture f(MovementState::ControllerType::kHover, glm::vec3{0.0f, 9.0f, 0.0f}, false);
    for (int i = 0; i < 5; ++i) f.tick();
    // Nothing below: it holds the height it has.
    require(nearly(f.position().y, 9.0f, 0.0001f));
    f.suspend(1.0f, glm::vec3{0.0f});
    for (int i = 0; i < 30; ++i) f.tick();
    const auto drops = f.pop();
    require(drops.size() == 1u);
    require(nearly(drops[0].second, f.position().y - 50.0f, 0.001f));
    const std::uint32_t until = f.lockout()->until_tick;
    while (f.tick_index < until + 1u) f.tick();
    require(f.lockout() == nullptr);
    const float stopped = f.position().y;
    require(stopped < -30.0f);
    for (int i = 0; i < 30; ++i) f.tick();
    require(nearly(f.position().y, stopped, 0.0001f));
}

// ---------------------------------------------------------------------------
// The action, through the status
// ---------------------------------------------------------------------------

constexpr std::uint32_t kStatusId = 1201u;

KernelActionTriggerDefinition suspend_trigger(float rise, float drift) {
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(trigger);
    trigger.action_count = 1u;
    KernelActionDefinition& action = trigger.actions[0];
    action.action_type = KernelEntityTriggerActionType_ApplySuspendMovement;
    action.target_source = KernelEntityRefSource_EventSubject;
    action.suspend_rise_speed = rise;
    action.suspend_drift_speed = drift;
    return trigger;
}

void only_a_status_on_apply_with_sane_speeds_compiles() {
    require(compile_action_trigger_definition(
                TriggerEventType::kStatusApplied, suspend_trigger(1.0f, 0.5f))
                .has_value());
    require(!compile_action_trigger_definition(
                 TriggerEventType::kCollision, suspend_trigger(1.0f, 0.5f))
                 .has_value());
    require(!compile_action_trigger_definition(
                 TriggerEventType::kStatusApplied, suspend_trigger(25.0f, 0.5f))
                 .has_value());
    require(!compile_action_trigger_definition(
                 TriggerEventType::kStatusApplied, suspend_trigger(1.0f, -1.0f))
                 .has_value());
    KernelActionTriggerDefinition onto_target = suspend_trigger(1.0f, 0.5f);
    onto_target.actions[0].target_source = KernelEntityRefSource_EventTarget;
    require(!compile_action_trigger_definition(
                 TriggerEventType::kStatusApplied, onto_target)
                 .has_value());
}

void applying_the_status_carries_its_direction_into_the_drift() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{4.0f, 0.0f, 0.0f});
    const entt::entity target_entity = *world.find_entity(target);
    world.registry().get_or_emplace<Health>(target_entity) = Health{50, 50};
    MovementState& movement =
        world.registry().get_or_emplace<MovementState>(target_entity);
    movement.gravity = glm::vec3{0.0f, kGravity, 0.0f};
    movement.ground_state = MovementState::GroundState::kGrounded;
    // Mid-knockback: the bubble replaces it.
    world.registry().emplace_or_replace<ImpulseLockout>(
        target_entity, ImpulseLockout{engine.current_tick() + 50u, engine.current_tick()});

    RuntimeStatusEffectTemplate status;
    status.status_effect_id = kStatusId;
    status.channel_id = 21u;
    status.duration_ticks = 60u;
    status.on_apply_binding = compile_action_trigger_definition(
        TriggerEventType::kStatusApplied, suspend_trigger(1.5f, 0.5f));
    require(status.on_apply_binding.has_value());
    world.set_status_effect_templates({status});

    // A hit with the status, carrying the hit's direction.
    KernelActionTriggerDefinition hit{};
    hit.struct_size = sizeof(hit);
    hit.action_count = 1u;
    hit.actions[0].action_type = KernelEntityTriggerActionType_ApplyStatus;
    hit.actions[0].target_source = KernelEntityRefSource_EventTarget;
    hit.actions[0].status_effect_id = kStatusId;
    hit.actions[0].status_direction_authored = 1u;
    hit.actions[0].direction_source = KernelEventVec3Source_Direction;
    const std::optional<CompiledActionGraphBinding> binding =
        compile_action_trigger_definition(TriggerEventType::kCollision, hit);
    require(binding.has_value());
    TriggerEvent event{
        TriggerEventType::kCollision, source, source, target,
        glm::vec3{4.0f, 1.0f, 0.0f}, glm::vec3{3.0f, -2.0f, 4.0f}};
    ActionExecutionProvenance provenance;
    provenance.request_id = 1;
    provenance.server_tick = engine.current_tick();
    provenance.instigator = source;
    std::vector<ActionGraphCommand> commands;
    std::string error;
    require(evaluate_action_graph(*binding, source, event, provenance, &commands, &error));
    require(execute_action_graph_command_batch(
        engine, ActionGraphCommandBatch{event, provenance, 1u, std::move(commands)}, 0u));

    const ActiveStatusEffect* held = active_suspension(world, target_entity);
    require(held != nullptr);
    require(held->suspend_rise_speed == 1.5f);
    // The horizontal of (3, -2, 4) is (3, 4) / 5, at 0.5 m/s.
    require(nearly(held->suspend_drift_velocity.x, 0.3f, 0.0001f));
    require(held->suspend_drift_velocity.y == 0.0f);
    require(nearly(held->suspend_drift_velocity.z, 0.4f, 0.0001f));
    require(!world.registry().all_of<ImpulseLockout>(target_entity));
    require(world.registry().all_of<HeldInSuspension>(target_entity));
    require(movement.ground_state == MovementState::GroundState::kAirborne);

    // Without a direction the same status drifts nowhere: the control for the
    // direction being what drives it.
    hit.actions[0].status_direction_authored = 0u;
    world.registry().get<StatusEffectState>(target_entity).active.clear();
    const std::optional<CompiledActionGraphBinding> undirected =
        compile_action_trigger_definition(TriggerEventType::kCollision, hit);
    commands.clear();
    provenance.request_id = 2;
    require(evaluate_action_graph(*undirected, source, event, provenance, &commands, &error));
    require(execute_action_graph_command_batch(
        engine, ActionGraphCommandBatch{event, provenance, 2u, std::move(commands)}, 0u));
    held = active_suspension(world, target_entity);
    require(held != nullptr);
    require(held->suspend_drift_velocity == glm::vec3{0.0f});
}

}  // namespace

int main() {
    the_rise_and_drift_are_exact_and_input_cannot_steer_them();
    a_roof_stops_the_rise_on_every_controller();
    the_drop_is_straight_down_locked_and_lands_on_the_predicted_tick();
    a_drone_drops_to_the_ground_and_climbs_back();
    a_drone_with_no_floor_holds_where_its_drop_runs_out();
    only_a_status_on_apply_with_sane_speeds_compiles();
    applying_the_status_carries_its_direction_into_the_drift();
    std::puts("status_suspension_test passed");
    return 0;
}
