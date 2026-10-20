// P2 of docs/PLAYER_HOVER_PLAN.md: the authority's movement step for a status
// hover on the character controller, and the guarantee that without one
// nothing moves differently than before it existed (§9).
//
// Movement runs on a bare World with its own physics, as status_suspension_test
// does, so the fixture owns the tick count lockouts are measured against.
#include <algorithm>
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
#include "sync/public/snapshot.h"

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
        std::fprintf(stderr, "player_hover_movement_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr float kTick = 1.0f / 30.0f;
constexpr float kGravity = -9.81f;
constexpr float kHeight = 3.0f;
constexpr float kClimb = 4.0f;

bool nearly(float lhs, float rhs, float tolerance) {
    return std::fabs(lhs - rhs) < tolerance;
}

struct Fixture {
    explicit Fixture(glm::vec3 spawn = glm::vec3{0.0f}) : world(false) {
        world.set_collision_world(&physics);
        add_box(100, glm::vec3{0.0f, -0.5f, 0.0f}, glm::vec3{50.0f, 0.5f, 50.0f});
        actor = world.spawn_player(7, spawn);
        entity = *world.find_entity(actor);
        MovementState& movement = world.registry().get<MovementState>(entity);
        movement.speed_meters_per_second = 5.0f;
        movement.controller_type = MovementState::ControllerType::kCharacter;
        movement.movement_collider_template_id = 10;
        movement.gravity = glm::vec3{0.0f, kGravity, 0.0f};
        movement.max_slope_degrees = 50.0f;
        movement.step_height = 0.4f;
        movement.ground_probe_distance = 0.25f;
        movement.ground_snap_distance = 0.5f;
        movement.movement_collision_mask =
            physics::collision_layer_bit(CollisionLayer::kTerrain) |
            physics::collision_layer_bit(CollisionLayer::kStaticObstacle);

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

    // Settles the suspension bookkeeping first, as the kernel tick does.
    void tick(glm::vec2 move = glm::vec2{0.0f}) {
        settle_status_suspensions(world, tick_index, kTick);
        KernelPlayerInput input{};
        input.move = KernelVec2{move.x, move.y};
        const std::vector<QueuedInput> inputs{
            QueuedInput{7, input, tick_index, 0, false, 0},
        };
        simulate_actor_movement(world, inputs, kTick, tick_index, &events, &stats);
        ++tick_index;
    }

    StatusEffectState& statuses() {
        return world.registry().get_or_emplace<StatusEffectState>(entity);
    }

    // What the apply_hover_movement commit leaves on its instance (the
    // status path itself is covered below).
    void hover(std::uint32_t instance_id = 2u) {
        ActiveStatusEffect active;
        active.instance_id = instance_id;
        active.status_effect_id = 1006u;
        active.channel_id = 31u;
        active.expire_tick = tick_index + 100000u;
        active.hovers = true;
        active.hover_height_meters = kHeight;
        active.hover_vertical_speed = kClimb;
        statuses().active.push_back(active);
    }

    void end_hover() {
        auto& active = statuses().active;
        active.erase(
            std::remove_if(active.begin(), active.end(),
                           [](const ActiveStatusEffect& a) { return a.hovers; }),
            active.end());
    }

    // status_suspension_test's suspend(): a bubble, standing in for its status.
    void suspend(float rise) {
        ActiveStatusEffect active;
        active.instance_id = 1u;
        active.status_effect_id = 1005u;
        active.channel_id = 21u;
        active.expire_tick = tick_index + 100000u;
        active.suspends_movement = true;
        active.suspend_rise_speed = rise;
        statuses().active.push_back(active);
        world.registry().emplace_or_replace<HeldInSuspension>(entity);
        MovementState& movement = world.registry().get<MovementState>(entity);
        movement.ground_state = MovementState::GroundState::kAirborne;
        movement.has_controller_height = false;
    }

    void pop_bubble() {
        auto& active = statuses().active;
        active.erase(
            std::remove_if(active.begin(), active.end(),
                           [](const ActiveStatusEffect& a) { return a.suspends_movement; }),
            active.end());
    }

    void knock(glm::vec3 velocity, std::uint32_t ticks) {
        world.registry().get<Velocity>(entity).linear = velocity;
        world.registry().get<MovementState>(entity).ground_state =
            MovementState::GroundState::kAirborne;
        world.registry().emplace_or_replace<ImpulseLockout>(
            entity, ImpulseLockout{tick_index + ticks, tick_index});
    }

    glm::vec3 position() const { return world.registry().get<Transform>(entity).position; }
    glm::vec3 velocity() const { return world.registry().get<Velocity>(entity).linear; }
    MovementState::GroundState ground() const {
        return world.registry().get<MovementState>(entity).ground_state;
    }
    bool grounded() const { return ground() == MovementState::GroundState::kGrounded; }

    PhysicsWorld physics;
    World world;
    NetId actor = 0;
    entt::entity entity = entt::null;
    std::uint32_t tick_index = 1;
    std::vector<KernelEvent> events;
    MovementSimulationStats stats{};
};

// ---------------------------------------------------------------------------
// §9: without a hover status nothing changes
// ---------------------------------------------------------------------------

// Every tick's position, velocity and ground state folded into three numbers.
struct Trace {
    double position_sum = 0.0;
    double velocity_sum = 0.0;
    int grounded_ticks = 0;

    void add(const Fixture& f) {
        const glm::vec3 p = f.position();
        const glm::vec3 v = f.velocity();
        position_sum += static_cast<double>(p.x) + 3.0 * p.y + 7.0 * p.z;
        velocity_sum += static_cast<double>(v.x) + 3.0 * v.y + 7.0 * v.z;
        grounded_ticks += f.grounded() ? 1 : 0;
    }
};

// A walk, a knockback with a vertical part, a stagger, and a bubble's rise and
// drop, back to back on one character.
Trace no_hover_scenario(bool with_inert_status) {
    Fixture f;
    if (with_inert_status) {
        // A status with no hover on it, standing the whole time: the only
        // thing a catalog with the potion and no one drinking it can leave.
        ActiveStatusEffect active;
        active.instance_id = 9u;
        active.status_effect_id = 1003u;
        active.expire_tick = 100000u;
        f.statuses().active.push_back(active);
    }
    Trace trace;
    for (int i = 0; i < 10; ++i) { f.tick(); trace.add(f); }
    for (int i = 0; i < 40; ++i) { f.tick(glm::vec2{0.6f, -0.8f}); trace.add(f); }
    f.knock(glm::vec3{6.0f, 4.0f, -2.0f}, 40u);
    for (int i = 0; i < 50; ++i) { f.tick(glm::vec2{-1.0f, 0.0f}); trace.add(f); }
    StaggerState stagger;
    stagger.until_tick = f.tick_index + 10u;
    f.world.registry().emplace_or_replace<StaggerState>(f.entity, stagger);
    for (int i = 0; i < 20; ++i) { f.tick(glm::vec2{0.0f, 1.0f}); trace.add(f); }
    f.suspend(1.0f);
    for (int i = 0; i < 45; ++i) { f.tick(glm::vec2{1.0f, 0.0f}); trace.add(f); }
    f.pop_bubble();
    for (int i = 0; i < 60; ++i) { f.tick(glm::vec2{1.0f, 0.0f}); trace.add(f); }
    return trace;
}

// The golden numbers were recorded on c7e62c8f, before the movement step knew
// about hovers (the step is main's there; P1 touched no movement code).
void without_a_hover_nothing_moves_differently() {
    for (const bool inert : {false, true}) {
        const Trace trace = no_hover_scenario(inert);
        std::printf("[no-hover inert=%d] position %.9f velocity %.9f grounded %d\n",
                    inert ? 1 : 0, trace.position_sum, trace.velocity_sum,
                    trace.grounded_ticks);
        require(std::fabs(trace.position_sum - -6532.393735189) < 1e-6);
        require(std::fabs(trace.velocity_sum - -756.999998592) < 1e-6);
        require(trace.grounded_ticks == 141);
    }
}


// ---------------------------------------------------------------------------
// §2.1: the hover and what comes ahead of it
// ---------------------------------------------------------------------------

// On the ground, then hovering: a player as it is the tick it drinks.
Fixture hovering_at_height() {
    Fixture f;
    for (int i = 0; i < 10; ++i) f.tick();
    require(f.grounded());
    f.hover();
    for (int i = 0; i < 40; ++i) f.tick();
    require(nearly(f.position().y, kHeight, 0.05f));
    return f;
}

void it_rises_to_its_height_holds_it_and_walks() {
    Fixture f;
    for (int i = 0; i < 10; ++i) f.tick();
    require(f.grounded());
    f.hover();
    // Settling on the ground landed it once; only what follows counts.
    f.events.clear();
    int reached = -1;
    float worst = 0.0f;
    const float start_x = f.position().x;
    for (int tick = 0; tick < 90; ++tick) {
        f.tick(glm::vec2{1.0f, 0.0f});
        require(!f.grounded());
        if (reached < 0 && nearly(f.position().y, kHeight, 0.05f)) reached = tick;
        if (reached >= 0) worst = std::max(worst, std::fabs(f.position().y - kHeight));
    }
    const float walked = f.position().x - start_x;
    std::printf("[rise] reached %.1f m at tick %d, worst %.4f, walked %.3f m in 3 s\n",
                kHeight, reached, worst, walked);
    // 3 m at 4 m/s: 23 ticks.
    require(reached >= 20 && reached <= 26);
    require(worst < 0.05f);
    // Walking at its own speed the whole way: 5 m/s for 3 s.
    require(nearly(walked, 15.0f, 0.1f));
    // Never landed, so nothing landed.
    require(std::none_of(
        f.events.begin(), f.events.end(),
        [](const KernelEvent& e) { return e.type == KernelEventType_ActorLanded; }));
}

void when_it_ends_it_falls_and_lands_as_a_character() {
    Fixture f = hovering_at_height();
    f.end_hover();
    f.events.clear();
    int landed = -1;
    for (int tick = 0; tick < 60 && landed < 0; ++tick) {
        f.tick();
        if (f.grounded()) landed = tick;
    }
    std::printf("[end] landed after %d ticks at y=%.4f\n", landed, f.position().y);
    // The control for the check above: a real landing does raise the event.
    require(std::any_of(
        f.events.begin(), f.events.end(),
        [](const KernelEvent& e) { return e.type == KernelEventType_ActorLanded; }));
    require(landed >= 18 && landed <= 26);
    require(nearly(f.position().y, 0.0f, 0.05f));
}

void the_dead_fall() {
    Fixture f = hovering_at_height();
    f.world.registry().get_or_emplace<Health>(f.entity) = Health{0, 100};
    for (int i = 0; i < 40; ++i) f.tick();
    std::printf("[dead] y=%.4f grounded=%d\n", f.position().y, f.grounded() ? 1 : 0);
    require(f.grounded());
    require(nearly(f.position().y, 0.0f, 0.05f));
}

void a_sheltered_occupant_stays_put_and_rises_when_it_leaves() {
    Fixture f;
    for (int i = 0; i < 10; ++i) f.tick();
    f.world.registry().emplace<Sheltered>(f.entity);
    f.hover();
    for (int i = 0; i < 40; ++i) f.tick(glm::vec2{1.0f, 0.0f});
    std::printf("[shelter] inside y=%.4f x=%.4f\n", f.position().y, f.position().x);
    require(nearly(f.position().y, 0.0f, 0.05f));
    require(nearly(f.position().x, 0.0f, 0.01f));
    f.world.registry().remove<Sheltered>(f.entity);
    for (int i = 0; i < 40; ++i) f.tick();
    require(nearly(f.position().y, kHeight, 0.05f));
}

// The water bubble as it is today (§2.1 step 3 and 4): the rise, the drop to
// the ground, and then the hover lifts it again.
void a_bubble_lifts_it_drops_it_to_the_ground_then_it_hovers_again() {
    Fixture f = hovering_at_height();
    f.suspend(1.0f);
    for (int i = 0; i < 45; ++i) f.tick(glm::vec2{1.0f, 0.0f});
    const float lifted = f.position().y;
    require(nearly(lifted, kHeight + 1.5f, 0.05f));
    f.pop_bubble();
    int landed = -1;
    for (int tick = 0; tick < 60 && landed < 0; ++tick) {
        f.tick(glm::vec2{1.0f, 0.0f});
        if (f.grounded()) landed = tick;
    }
    require(landed > 0);
    const float floor = f.position().y;
    // The landing released the drop; the hover has it from the next tick.
    for (int i = 0; i < 40; ++i) f.tick();
    std::printf("[bubble] lifted to %.3f, landed after %d ticks at %.4f, back at %.4f\n",
                lifted, landed, floor, f.position().y);
    require(nearly(floor, 0.0f, 0.05f));
    require(!f.world.registry().all_of<ImpulseLockout>(f.entity));
    require(nearly(f.position().y, kHeight, 0.05f));
}

// A knockback carries its horizontal; its vertical does nothing to a hover.
void a_knockback_pushes_it_along_but_not_up() {
    Fixture f = hovering_at_height();
    const float start_x = f.position().x;
    f.knock(glm::vec3{6.0f, 8.0f, 0.0f}, 20u);
    float lowest = 1e9f;
    float highest = -1e9f;
    for (int i = 0; i < 20; ++i) {
        f.tick(glm::vec2{-1.0f, 0.0f});
        lowest = std::min(lowest, f.position().y);
        highest = std::max(highest, f.position().y);
    }
    const float pushed = f.position().x - start_x;
    std::printf("[knockback] pushed %.3f m, y %.4f..%.4f\n", pushed, lowest, highest);
    // 6 m/s for 20 ticks, steering against it all the while.
    require(nearly(pushed, 4.0f, 0.1f));
    require(highest - lowest < 0.05f);
    // And after it, the stick has it back.
    for (int i = 0; i < 10; ++i) f.tick(glm::vec2{-1.0f, 0.0f});
    require(f.position().x < start_x + pushed - 1.0f);
}

void a_stagger_stops_it_where_it_hovers() {
    Fixture f = hovering_at_height();
    StaggerState stagger;
    stagger.until_tick = f.tick_index + 15u;
    f.world.registry().emplace_or_replace<StaggerState>(f.entity, stagger);
    const glm::vec3 start = f.position();
    for (int i = 0; i < 14; ++i) f.tick(glm::vec2{1.0f, 0.0f});
    require(nearly(f.position().x, start.x, 0.01f));
    require(nearly(f.position().y, kHeight, 0.05f));
}

// Not a character: a hover status on anything else is inert.
void a_grounded_controller_ignores_a_hover() {
    Fixture f;
    f.world.registry().get<MovementState>(f.entity).controller_type =
        MovementState::ControllerType::kGrounded;
    for (int i = 0; i < 10; ++i) f.tick();
    const float floor = f.position().y;
    f.hover();
    for (int i = 0; i < 40; ++i) f.tick();
    require(nearly(f.position().y, floor, 0.001f));
}

// The P1 commit, through a real status: apply_status -> on_apply ->
// apply_hover_movement records the hover on the instance, and the step uses it.
void the_status_puts_the_hover_on_its_instance() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId player = world.spawn_player(1, glm::vec3{0.0f});
    const entt::entity entity = *world.find_entity(player);
    world.registry().emplace<Sheltered>(entity);

    KernelActionTriggerDefinition on_apply{};
    on_apply.struct_size = sizeof(on_apply);
    on_apply.action_count = 1u;
    on_apply.actions[0].action_type = KernelEntityTriggerActionType_ApplyHoverMovement;
    on_apply.actions[0].target_source = KernelEntityRefSource_EventSubject;
    on_apply.actions[0].hover_height_meters = kHeight;
    on_apply.actions[0].hover_vertical_speed = kClimb;
    RuntimeStatusEffectTemplate status;
    status.status_effect_id = 1006u;
    status.channel_id = 31u;
    status.duration_ticks = 900u;
    status.on_apply_binding =
        compile_action_trigger_definition(TriggerEventType::kStatusApplied, on_apply);
    require(status.on_apply_binding.has_value());
    world.set_status_effect_templates({status});
    // And the compile refuses it anywhere but a status's on_apply.
    require(!compile_action_trigger_definition(TriggerEventType::kCollision, on_apply)
                 .has_value());

    // The potion's on_item_used: apply_status onto the drinker.
    KernelActionTriggerDefinition drink{};
    drink.struct_size = sizeof(drink);
    drink.action_count = 1u;
    drink.actions[0].action_type = KernelEntityTriggerActionType_ApplyStatus;
    drink.actions[0].target_source = KernelEntityRefSource_EventTarget;
    drink.actions[0].status_effect_id = 1006u;
    const std::optional<CompiledActionGraphBinding> binding =
        compile_action_trigger_definition(TriggerEventType::kCollision, drink);
    require(binding.has_value());
    TriggerEvent event{
        TriggerEventType::kCollision, player, player, player,
        glm::vec3{0.0f}, glm::vec3{0.0f}};
    ActionExecutionProvenance provenance;
    provenance.request_id = 1;
    provenance.server_tick = engine.current_tick();
    provenance.instigator = player;
    std::vector<ActionGraphCommand> commands;
    std::string error;
    require(evaluate_action_graph(*binding, player, event, provenance, &commands, &error));
    require(execute_action_graph_command_batch(
        engine, ActionGraphCommandBatch{event, provenance, 1u, std::move(commands)}, 0u));

    const ActiveStatusEffect* hover = active_hover(world, entity);
    require(hover != nullptr);
    require(hover->hover_height_meters == kHeight);
    require(hover->hover_vertical_speed == kClimb);
    require(hover->expire_tick == engine.current_tick() + 900u);
    // Applied to a sheltered occupant too: the step is what holds it still.
    require(world.registry().all_of<Sheltered>(entity));
}

// ---------------------------------------------------------------------------
// P3: what the snapshot says
// ---------------------------------------------------------------------------

const EntitySnapshot& own_record(const Fixture& f, const WorldSnapshot& snapshot) {
    for (const EntitySnapshot& entity : snapshot.entities) {
        if (entity.net_id == f.actor) return entity;
    }
    require(false);
    return snapshot.entities.front();
}

void the_snapshot_says_hovering_not_falling_and_carries_the_status() {
    Fixture f = hovering_at_height();
    std::uint32_t expire = 0;
    for (const ActiveStatusEffect& active : f.statuses().active) {
        if (active.hovers) expire = active.expire_tick;
    }
    {
        const WorldSnapshot snapshot = build_world_snapshot(f.world, f.tick_index, 0, 0);
        const EntitySnapshot& own = own_record(f, snapshot);
        require((own.flags & kVisualFlagHovering) != 0u);
        require((own.flags & (kVisualFlagFalling | kVisualFlagGrounded)) == 0u);
        require(own.has_hover);
        require(own.hover_height_meters == kHeight);
        require(own.hover_vertical_speed == kClimb);
        require(own.hover_until_tick == expire);
    }
    // In a bubble the status still stands -- the owner's prediction needs it
    // for after the drop -- but the hover is not what moves it: no flag.
    f.suspend(1.0f);
    f.tick();
    {
        const WorldSnapshot snapshot = build_world_snapshot(f.world, f.tick_index, 0, 0);
        const EntitySnapshot& own = own_record(f, snapshot);
        require((own.flags & kVisualFlagHovering) == 0u);
        require((own.flags & kVisualFlagFalling) != 0u);
        require(own.has_hover);
    }
    // Gone with the status.
    f.pop_bubble();
    f.end_hover();
    for (int i = 0; i < 40; ++i) f.tick();
    {
        const WorldSnapshot snapshot = build_world_snapshot(f.world, f.tick_index, 0, 0);
        const EntitySnapshot& own = own_record(f, snapshot);
        require((own.flags & kVisualFlagHovering) == 0u);
        require((own.flags & kVisualFlagGrounded) != 0u);
        require(!own.has_hover);
    }
}

}  // namespace

int main() {
    without_a_hover_nothing_moves_differently();
    it_rises_to_its_height_holds_it_and_walks();
    when_it_ends_it_falls_and_lands_as_a_character();
    the_dead_fall();
    a_sheltered_occupant_stays_put_and_rises_when_it_leaves();
    a_bubble_lifts_it_drops_it_to_the_ground_then_it_hovers_again();
    a_knockback_pushes_it_along_but_not_up();
    a_stagger_stops_it_where_it_hovers();
    a_grounded_controller_ignores_a_hover();
    the_status_puts_the_hover_on_its_instance();
    the_snapshot_says_hovering_not_falling_and_carries_the_status();
    std::printf("player_hover_movement_test passed\n");
    return 0;
}
