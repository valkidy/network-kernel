// apply_impulse's input lockout.
//
// A horizontal knockback used to be invisible on anything that steers itself.
// The impulse landed in Velocity correctly, and then the very next tick threw
// the horizontal half away again: player_movement rebuilds desired_horizontal
// from input for any actor that sent one, and EntityStateSystem::set_velocity
// overwrites all three axes for any AI controller that pushes a velocity every
// tick. Only the Y component survived, which is why knockback read as "pop up
// in place". lockout_ticks holds both of those off for a while.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/action_graph.h"
#include "simulation/src/systems.h"

// reset_runtime_state is the only way to reach a running server engine without
// binding a socket, and it is private. command_queue_test.cc reaches it the
// same way; a port bind in a unit test is what made the sentry test flaky.
#define private public
#include "kernel/src/kernel.h"
#undef private

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
        std::fprintf(stderr, "impulse_lockout_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr float kSpeed = 5.0f;
constexpr float kKnockbackX = 8.0f;

// ---------------------------------------------------------------------------
// The authoring path: a graph that authors lockout_ticks arms the component.
// ---------------------------------------------------------------------------

CompiledActionGraphBinding impulse_binding(std::uint32_t lockout_ticks) {
    CompiledActionGraphBinding binding;
    binding.event_type = TriggerEventType::kActivated;
    binding.graph.id = "apply_impulse";
    binding.graph.parameters = {
        {"target", std::monostate{}},
        {"strength", kKnockbackX},
        // A level blast: the radial direction out of an explosion at the
        // target's own height has essentially no Y, which is exactly the case
        // the lockout exists for -- the surviving axis used to be the ~zero one.
        {"direction", glm::vec3{1.0f, 0.0f, 0.0f}},
    };
    ActionApplyImpulseDefinition impulse;
    impulse.target_parameter = "target";
    impulse.strength_parameter = "strength";
    impulse.direction_parameter = "direction";
    impulse.collision_mask = KERNEL_COLLISION_MASK_ACTOR;
    impulse.lockout_ticks = lockout_ticks;
    binding.graph.actions = {impulse};
    binding.parameters = {
        {"target", EntityRefExpression{EntityRefSource::kEventTarget}},
    };
    return binding;
}

bool fire_impulse(
    KernelEngine& engine,
    NetId source,
    NetId target,
    std::uint32_t lockout_ticks,
    std::uint32_t request_id) {
    const TriggerEvent event{
        TriggerEventType::kActivated,
        source,
        source,
        target,
    };
    ActionExecutionProvenance provenance;
    provenance.request_id = request_id;
    provenance.server_tick = engine.current_tick();
    provenance.instigator = source;
    provenance.owner_peer = 1u;
    std::vector<ActionGraphCommand> commands;
    if (!evaluate_action_graph(
            impulse_binding(lockout_ticks),
            source,
            event,
            provenance,
            &commands,
            nullptr)) {
        return false;
    }
    require(commands.size() == 1u);
    return execute_action_graph_command_batch(
        engine,
        ActionGraphCommandBatch{
            event, provenance, request_id, std::move(commands)},
        0u);
}

void authored_lockout_ticks_arm_the_component() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    const entt::entity target_entity = *world.find_entity(target);

    require(fire_impulse(engine, source, target, 12u, 1u));

    // The impulse itself still lands exactly as before.
    const Velocity& velocity = world.registry().get<Velocity>(target_entity);
    require(velocity.linear.x > kKnockbackX - 0.001f);
    require(velocity.linear.x < kKnockbackX + 0.001f);

    require(world.registry().all_of<ImpulseLockout>(target_entity));
    require(
        world.registry().get<ImpulseLockout>(target_entity).until_tick ==
        engine.current_tick() + 12u);
}

// The whole point of defaulting to zero: every template authored before this
// existed keeps behaving exactly as it did.
void an_impulse_without_lockout_ticks_arms_nothing() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    const entt::entity target_entity = *world.find_entity(target);

    require(fire_impulse(engine, source, target, 0u, 1u));

    const Velocity& velocity = world.registry().get<Velocity>(target_entity);
    require(velocity.linear.x > kKnockbackX - 0.001f);
    require(!world.registry().all_of<ImpulseLockout>(target_entity));
}

// A second impulse mid-lockout re-arms from zero rather than extending the
// remainder, so a heavy hit is never shortened by a light one that preceded it.
void a_second_impulse_re_arms_the_full_duration() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    const entt::entity target_entity = *world.find_entity(target);

    require(fire_impulse(engine, source, target, 4u, 1u));
    require(fire_impulse(engine, source, target, 30u, 2u));
    require(
        world.registry().get<ImpulseLockout>(target_entity).until_tick ==
        engine.current_tick() + 30u);
}

// ---------------------------------------------------------------------------
// The AI-controller seam: one gate for every controller, present and future.
// ---------------------------------------------------------------------------

void set_velocity_is_refused_while_the_lockout_stands() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    World& world = engine.simulation_world();
    const NetId agent = world.spawn_enemy(glm::vec3{0.0f});
    const entt::entity entity = *world.find_entity(agent);
    world.registry().get_or_emplace<Velocity>(entity).linear =
        glm::vec3{kKnockbackX, 3.0f, 0.0f};

    const EntityStateSystem entity_state;
    const KernelVec3 zero{0.0f, 0.0f, 0.0f};

    // This is what a sentry controller does every single tick.
    require(entity_state.set_velocity(engine, agent, zero));
    require(world.registry().get<Velocity>(entity).linear.x == 0.0f);

    // Armed: the same unconditional write is now refused outright, so the
    // controller does not have to know the rule.
    world.registry().get<Velocity>(entity).linear =
        glm::vec3{kKnockbackX, 3.0f, 0.0f};
    world.registry().emplace<ImpulseLockout>(
        entity, ImpulseLockout{engine.current_tick() + 5u});
    require(!entity_state.set_velocity(engine, agent, zero));
    require(world.registry().get<Velocity>(entity).linear.x == kKnockbackX);
    require(world.registry().get<Velocity>(entity).linear.y == 3.0f);

    // Expired: control goes straight back to the controller.
    world.registry().get<ImpulseLockout>(entity).until_tick =
        engine.current_tick();
    require(entity_state.set_velocity(engine, agent, zero));
    require(world.registry().get<Velocity>(entity).linear.x == 0.0f);
}

// ---------------------------------------------------------------------------
// The movement seam: input does not get to redefine the horizontal velocity.
// ---------------------------------------------------------------------------

struct MovementFixture {
    explicit MovementFixture(glm::vec3 spawn) : world(false) {
        world.set_collision_world(&physics);
        add_ground();
        actor = world.spawn_player(7, spawn);
        entity = *world.find_entity(actor);
        MovementState& movement = world.registry().get<MovementState>(entity);
        movement.speed_meters_per_second = kSpeed;
        movement.controller_type = MovementState::ControllerType::kCharacter;
        movement.movement_collider_template_id = 10;
        movement.gravity = glm::vec3{0.0f, -9.81f, 0.0f};
        movement.max_slope_degrees = 50.0f;
        movement.step_height = 0.4f;
        movement.ground_probe_distance = 0.25f;
        movement.ground_snap_distance = 0.5f;

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
        movement_collider_id = stored.collider_id;
        sync_body();
    }

    void add_ground() {
        CollisionObjectDescriptor object{};
        object.identity = CollisionObjectIdentity{
            0, 100, 0,
            CollisionObjectKind::kStaticObstacle,
            CollisionLayer::kStaticObstacle,
        };
        object.shape.type = CollisionShapeType::kBox;
        object.shape.half_extents = glm::vec3{50.0f, 0.5f, 50.0f};
        object.position = glm::vec3{0.0f, -0.5f, 0.0f};
        std::string error;
        require(physics.upsert_object(object, &error));
    }

    void sync_body() {
        const Transform& transform = world.registry().get<Transform>(entity);
        CollisionObjectDescriptor object{};
        object.identity = CollisionObjectIdentity{
            actor, movement_collider_id, 0,
            CollisionObjectKind::kActorMovement,
            CollisionLayer::kActorMovement,
        };
        object.shape.type = CollisionShapeType::kCapsule;
        object.shape.radius = 0.35f;
        object.shape.capsule_half_height = 0.55f;
        object.position = transform.position + glm::vec3{0.0f, 0.9f, 0.0f};
        std::string error;
        require(physics.upsert_object(object, &error));
    }

    // Every tick carries an input, which is what makes this the failing case:
    // a player holding a key, and equally every AI controller in the repo,
    // which all send exactly one input per tick on purpose.
    void tick_pushing_back() {
        KernelPlayerInput input{};
        input.move = KernelVec2{-1.0f, 0.0f};
        const std::vector<QueuedInput> inputs{
            QueuedInput{7, input, tick_index, 0, false, 0},
        };
        simulate_actor_movement(
            world, inputs, 1.0f / 30.0f, tick_index, &events, &stats);
        sync_body();
        ++tick_index;
    }

    // Exactly what the impulse handler does: velocity, forced airborne, and a
    // lockout armed on the current tick.
    void arm_lockout(std::uint32_t ticks, float launch_y = 0.0f) {
        world.registry().emplace_or_replace<ImpulseLockout>(
            entity, ImpulseLockout{tick_index + ticks, tick_index});
        world.registry().get<Velocity>(entity).linear =
            glm::vec3{kKnockbackX, launch_y, 0.0f};
        MovementState& movement = world.registry().get<MovementState>(entity);
        movement.ground_state = MovementState::GroundState::kAirborne;
        movement.has_controller_height = false;
    }

    bool grounded() const {
        return world.registry().get<MovementState>(entity).ground_state ==
            MovementState::GroundState::kGrounded;
    }

    float velocity_x() const {
        return world.registry().get<Velocity>(entity).linear.x;
    }

    bool locked() const {
        return world.registry().all_of<ImpulseLockout>(entity);
    }

    PhysicsWorld physics;
    World world;
    NetId actor = 0;
    entt::entity entity = entt::null;
    std::uint32_t movement_collider_id = 0;
    std::uint32_t tick_index = 1;
    std::vector<KernelEvent> events;
    MovementSimulationStats stats{};
};

void input_does_not_redefine_horizontal_velocity_during_lockout() {
    // High enough that the fall never reaches the ground inside the window --
    // landing is a separate release condition, tested below.
    MovementFixture fixture({0.0f, 40.0f, 0.0f});
    constexpr std::uint32_t kLockoutTicks = 6;
    fixture.arm_lockout(kLockoutTicks);

    // Without the lockout this is the bug: one tick of input and x is -5.
    for (std::uint32_t tick = 0; tick < kLockoutTicks; ++tick) {
        fixture.tick_pushing_back();
        require(fixture.velocity_x() > kKnockbackX - 0.001f);
        require(fixture.velocity_x() < kKnockbackX + 0.001f);
    }

    // The tick after the window belongs to input again, and the component is
    // reaped on the same tick it stops applying.
    fixture.tick_pushing_back();
    require(!fixture.locked());
    require(fixture.velocity_x() < -kSpeed + 0.001f);
    require(fixture.velocity_x() > -kSpeed - 0.001f);
}

void landing_releases_the_lockout_early() {
    // A real knockback arc off the ground, with a lockout ceiling far longer
    // than the flight, so only the landing can end it.
    MovementFixture fixture({0.0f, 0.05f, 0.0f});
    fixture.arm_lockout(300u, 6.0f);

    bool released = false;
    std::uint32_t ticks = 0;
    for (; ticks < 90 && !released; ++ticks) {
        fixture.tick_pushing_back();
        released = !fixture.locked();
    }
    require(released);
    require(fixture.grounded());
    // It flew first -- a release on the arming tick would be the bug below.
    require(ticks > 1);

    // And control is immediately back with whoever is driving.
    fixture.tick_pushing_back();
    require(fixture.velocity_x() < 0.0f);
}

// The regression this pairs with: applying an impulse forces the actor
// airborne, so a flat knockback on someone already standing re-lands during
// the very same movement step. If the landing release fired on the arming
// tick, the long horizontal knockback -- the case the split strength form
// exists to author -- would be released before it held anything off, and the
// next tick of input would erase it exactly as before the lockout existed.
void a_flat_knockback_on_a_grounded_actor_is_not_released_by_relanding() {
    MovementFixture fixture({0.0f, 0.0f, 0.0f});
    // Settle on the ground first, so the actor really is standing.
    fixture.tick_pushing_back();
    require(fixture.grounded());

    constexpr std::uint32_t kLockoutTicks = 5;
    fixture.arm_lockout(kLockoutTicks);

    for (std::uint32_t tick = 0; tick < kLockoutTicks; ++tick) {
        fixture.tick_pushing_back();
        require(fixture.velocity_x() > 0.0f);
    }
    fixture.tick_pushing_back();
    require(!fixture.locked());
    require(fixture.velocity_x() < 0.0f);
}

}  // namespace

int main() {
    authored_lockout_ticks_arm_the_component();
    an_impulse_without_lockout_ticks_arms_nothing();
    a_second_impulse_re_arms_the_full_duration();
    set_velocity_is_refused_while_the_lockout_stands();
    input_does_not_redefine_horizontal_velocity_during_lockout();
    landing_releases_the_lockout_early();
    a_flat_knockback_on_a_grounded_actor_is_not_released_by_relanding();
    return 0;
}
