// Kernel_ServerEnqueueEntityLeap: an agent's own leap, held by a LeapLockout.
//
// The claims: the flight lands on the solved spot on the solved tick, on the
// level, up a ledge and down a drop, whatever the controller steers meanwhile;
// landing hands it straight back, with no knockdown recovery; nothing is
// refused in the air; anything else that moves it ends the leap; and the
// anchor a remote client gets replays to the same landing.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>
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
        std::fprintf(stderr, "entity_leap_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr float kGravity = -9.81f;

bool nearly(float lhs, float rhs, float tolerance) {
    return std::fabs(lhs - rhs) < tolerance;
}

KernelVec3 to_kernel(const glm::vec3& value) {
    return KernelVec3{value.x, value.y, value.z};
}

// An agent on a 100 m slab whose top is y = 0, driven by the real movement
// solver. Ticks are counted from the engine's current tick, which nothing here
// advances: the arming tick is `tick`, and its movement step is the flight's
// first, as in simulate_tick, where commands drain ahead of movement.
struct Box {
    glm::vec3 center;
    glm::vec3 half_extents;
};

struct Fixture {
    explicit Fixture(
        glm::vec3 spawn, bool player = false, std::vector<Box> boxes = {})
        : engine(KernelConfig{}), world(engine.simulation_world()) {
        world.set_collision_world(&physics);
        add_box(100, glm::vec3{0.0f, -0.5f, 0.0f}, glm::vec3{50.0f, 0.5f, 50.0f});
        std::uint32_t id = 101;
        for (const Box& box : boxes) add_box(id++, box.center, box.half_extents);
        actor = player ? world.spawn_player(7, spawn) : world.spawn_enemy(spawn);
        entity = *world.find_entity(actor);
        world.registry().get_or_emplace<Health>(entity) = Health{50, 50};
        MovementState& movement = world.registry().get<MovementState>(entity);
        movement.speed_meters_per_second = 5.0f;
        movement.controller_type = MovementState::ControllerType::kCharacter;
        movement.movement_collider_template_id = 10;
        movement.gravity = glm::vec3{0.0f, kGravity, 0.0f};
        movement.max_slope_degrees = 50.0f;
        movement.step_height = 0.4f;
        movement.ground_probe_distance = 0.25f;
        movement.ground_snap_distance = 0.5f;

        ColliderInstance collider{};
        collider.collider_template_id = 10;
        collider.owner_net_id = actor;
        collider.entity_net_id = actor;
        collider.entity_type = EntityType::kActor;
        collider.actor_type = player ? ActorType::kPlayer : ActorType::kAgent;
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
        // Settled on the ticks just before the engine's own, so a leap armed
        // now is armed on the tick the next step simulates.
        tick = engine.current_tick() - 8u;
        for (int i = 0; i < 8; ++i) step(glm::vec2{0.0f});
        require(tick == engine.current_tick());
        require(grounded());
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

    // One movement step on `tick`, with the controller holding `move`, as a
    // chaser does every tick it is in the air.
    void step(const glm::vec2& move) {
        KernelPlayerInput input{};
        input.move = KernelVec2{move.x, move.y};
        const std::vector<QueuedInput> inputs{
            QueuedInput{7, input, tick, 0, false, actor},
        };
        simulate_actor_movement(world, inputs, kTick, tick, &events, &stats);
        sync_body();
        ++tick;
    }

    // Arming on the current tick, the way the command does.
    bool leap(const glm::vec3& destination, std::uint32_t airtime) {
        return EntityStateSystem{}.start_leap(
            engine, actor, to_kernel(destination), airtime);
    }

    // Steps from the arming tick until it is down; the tick it landed on, or
    // 0. The arming tick's own step is the flight's first.
    std::uint32_t fly_from(std::uint32_t armed, std::uint32_t limit, glm::vec2 steer) {
        tick = armed;
        for (std::uint32_t n = 0; n < limit; ++n) {
            const std::uint32_t this_tick = tick;
            step(steer);
            if (this_tick > armed && grounded()) return this_tick;
        }
        return 0u;
    }

    glm::vec3 position() const {
        return world.registry().get<Transform>(entity).position;
    }

    bool grounded() const {
        return world.registry().get<MovementState>(entity).ground_state ==
            MovementState::GroundState::kGrounded;
    }

    static constexpr float kTick = 1.0f / 30.0f;
    PhysicsWorld physics;
    KernelEngine engine;
    World& world;
    NetId actor = 0;
    entt::entity entity = entt::null;
    std::uint32_t movement_collider_id = 0;
    std::uint32_t tick = 0;
    std::vector<KernelEvent> events;
    MovementSimulationStats stats{};
};

constexpr float kTick = Fixture::kTick;

// ---------------------------------------------------------------------------

void lands_on_the_solved_spot_on_the_solved_tick() {
    struct Case {
        float dy;
        float distance;
        float apex;
    };
    for (const Case& c : {Case{0.0f, 3.0f, 0.2f}, Case{0.0f, 6.4f, 0.5f},
                          Case{0.0f, 9.0f, 1.5f}, Case{0.4f, 6.0f, 0.5f},
                          Case{1.0f, 6.0f, 0.5f}, Case{-1.0f, 6.0f, 0.5f},
                          Case{-2.0f, 6.0f, 0.5f}, Case{-2.0f, 6.0f, 0.1f}}) {
        const float launch_y = c.dy < 0.0f ? -c.dy : 0.0f;
        const float land_y = c.dy < 0.0f ? 0.0f : c.dy;
        std::vector<Box> boxes;
        if (c.dy > 0.0f) {
            // A ledge from 1.5 m short of the destination on, so the capsule
            // clears its edge on the way down.
            boxes.push_back(Box{glm::vec3{c.distance + 3.0f, c.dy * 0.5f, 0.0f},
                                glm::vec3{4.5f, c.dy * 0.5f, 5.0f}});
        } else if (c.dy < 0.0f) {
            // The platform it starts on, ending a metre ahead of it.
            boxes.push_back(Box{glm::vec3{-4.0f, launch_y * 0.5f, 0.0f},
                                glm::vec3{5.0f, launch_y * 0.5f, 5.0f}});
        }
        Fixture fixture(glm::vec3{0.0f, launch_y, 0.0f}, false, boxes);
        require(nearly(fixture.position().y, launch_y, 0.05f));

        // destination.y is only where to look: the ground under it decides.
        const glm::vec3 destination{c.distance, land_y + 1.5f, 0.0f};
        const std::uint32_t airtime = leap_airtime_ticks(
            c.distance, c.dy, c.apex, 0.0f, kGravity, kTick);
        require(fixture.leap(destination, airtime));
        const LeapLockout lockout =
            fixture.world.registry().get<LeapLockout>(fixture.entity);
        require(lockout.armed_tick == fixture.engine.current_tick());
        require(nearly(lockout.landing_y, land_y, 0.01f));

        // Steering straight back the whole way.
        const std::uint32_t landed =
            fixture.fly_from(lockout.armed_tick, airtime + 10u, glm::vec2{-1.0f, 0.0f});
        const glm::vec3 at = fixture.position();
        std::fprintf(stderr,
                     "dy %+4.1f d %4.1f apex %.1f N %2u: landed on step %u "
                     "(solved %u) at (%.4f, %.4f, %.4f)\n",
                     c.dy, c.distance, c.apex, airtime,
                     landed - lockout.armed_tick + 1u,
                     lockout.landing_tick - lockout.armed_tick + 1u, at.x, at.y, at.z);
        require(landed == lockout.landing_tick);
        require(lockout.landing_tick == lockout.armed_tick + airtime - 1u);
        require(nearly(at.x, c.distance, 0.02f));
        require(nearly(at.z, 0.0f, 0.02f));
        require(nearly(at.y, land_y, 0.05f));
        require(!fixture.world.registry().all_of<LeapLockout>(fixture.entity));
    }
}

// The rule that made a separate lockout worth having: a gingerbread authors a
// knockdown, and a leap must not land into it.
void landing_is_not_a_knockdown_and_nothing_is_refused_in_the_air() {
    Fixture fixture(glm::vec3{0.0f});
    fixture.world.registry().emplace<KnockdownProfile>(
        fixture.entity, KnockdownProfile{21u});
    const std::uint32_t armed = fixture.tick;
    require(fixture.leap(glm::vec3{5.0f, 0.0f, 0.0f}, 20u));

    // In the air: no action refusal, flagged as a leap and not a knockback,
    // and an AI controller's velocity write is refused.
    fixture.tick = armed;
    fixture.step(glm::vec2{0.0f});
    require(action_block_reason(fixture.world, fixture.entity, fixture.tick) ==
            KernelLocalActionResultReason_None);
    const std::uint32_t flags = derived_visual_flags(fixture.world, fixture.entity);
    require((flags & kVisualFlagLeaping) != 0u);
    require((flags & kVisualFlagKnockedBack) == 0u);
    const glm::vec3 flying = fixture.world.registry().get<Velocity>(fixture.entity).linear;
    require(!EntityStateSystem{}.set_velocity(
        fixture.engine, fixture.actor, KernelVec3{0.0f, 0.0f, 0.0f}));
    require(fixture.world.registry().get<Velocity>(fixture.entity).linear == flying);

    require(fixture.fly_from(fixture.tick, 30u, glm::vec2{0.0f}) != 0u);
    require(!fixture.world.registry().all_of<LeapLockout>(fixture.entity));
    require(!fixture.world.registry().all_of<ImpulseLockout>(fixture.entity));
    require(action_block_reason(fixture.world, fixture.entity, fixture.tick) ==
            KernelLocalActionResultReason_None);
    require((derived_visual_flags(fixture.world, fixture.entity) &
             kVisualFlagLeaping) == 0u);

    // And the controller has it back on the very next step.
    const float x = fixture.position().x;
    fixture.step(glm::vec2{1.0f, 0.0f});
    require(fixture.position().x > x + 0.1f);
}

void refused_unless_a_grounded_free_agent_with_ground_to_land_on() {
    {
        Fixture player(glm::vec3{0.0f}, true);
        require(!player.leap(glm::vec3{5.0f, 0.0f, 0.0f}, 20u));
        require(!player.world.registry().all_of<LeapLockout>(player.entity));
    }
    Fixture fixture(glm::vec3{0.0f});
    // One step is its own arming tick.
    require(!fixture.leap(glm::vec3{5.0f, 0.0f, 0.0f}, 1u));
    require(!fixture.leap(glm::vec3{5.0f, 0.0f, 0.0f}, KERNEL_MAX_IMPULSE_LOCKOUT_TICKS + 1u));
    // A drop given so little airtime that it would not rise off its ledge.
    {
        Fixture ledge(glm::vec3{0.0f, 2.0f, 0.0f}, false,
                      {Box{glm::vec3{-4.0f, 1.0f, 0.0f}, glm::vec3{5.0f, 1.0f, 5.0f}}});
        require(!ledge.leap(glm::vec3{6.0f, 0.0f, 0.0f}, 20u));
        require(ledge.leap(glm::vec3{6.0f, 0.0f, 0.0f},
                           leap_airtime_ticks(6.0f, -2.0f, 0.1f, 0.0f, kGravity, kTick)));
    }
    // Off the edge of the slab: no ground under it.
    require(!fixture.leap(glm::vec3{80.0f, 0.0f, 0.0f}, 20u));
    // In a knockback.
    fixture.world.registry().emplace<ImpulseLockout>(
        fixture.entity, ImpulseLockout{fixture.engine.current_tick() + 10u,
                                       fixture.engine.current_tick()});
    require(!fixture.leap(glm::vec3{5.0f, 0.0f, 0.0f}, 20u));
    fixture.world.registry().remove<ImpulseLockout>(fixture.entity);
    // Dead.
    fixture.world.registry().get<Health>(fixture.entity).hp = 0u;
    require(!fixture.leap(glm::vec3{5.0f, 0.0f, 0.0f}, 20u));
    fixture.world.registry().get<Health>(fixture.entity).hp = 50u;
    require(!fixture.world.registry().all_of<LeapLockout>(fixture.entity));

    // Already leaping, and so airborne.
    require(fixture.leap(glm::vec3{5.0f, 0.0f, 0.0f}, 20u));
    require(!fixture.leap(glm::vec3{-5.0f, 0.0f, 0.0f}, 20u));
    fixture.world.registry().remove<LeapLockout>(fixture.entity);
    require(!fixture.leap(glm::vec3{-5.0f, 0.0f, 0.0f}, 20u));
}

// What apply_impulse does to a target, through the command batch.
void knock_back(Fixture& fixture, std::uint32_t lockout_ticks) {
    ActionApplyImpulseCommand impulse{};
    impulse.source = fixture.actor;
    impulse.target = fixture.actor;
    impulse.strength = 8.0f;
    impulse.direction = glm::vec3{0.0f, 0.0f, 1.0f};
    impulse.lockout_ticks = lockout_ticks;
    const TriggerEvent event{
        TriggerEventType::kActivated, fixture.actor, fixture.actor, fixture.actor};
    ActionExecutionProvenance provenance;
    provenance.request_id = 1u;
    provenance.server_tick = fixture.engine.current_tick();
    provenance.instigator = fixture.actor;
    std::vector<ActionGraphCommand> commands{impulse};
    require(execute_action_graph_command_batch(
        fixture.engine,
        ActionGraphCommandBatch{event, provenance, 1u, std::move(commands)},
        0u));
}

void a_knockback_ends_the_leap_and_owns_the_flight() {
    Fixture fixture(glm::vec3{0.0f});
    fixture.world.registry().emplace<KnockdownProfile>(
        fixture.entity, KnockdownProfile{21u});
    require(fixture.leap(glm::vec3{6.0f, 0.0f, 0.0f}, 20u));
    knock_back(fixture, 30u);
    require(!fixture.world.registry().all_of<LeapLockout>(fixture.entity));
    require(fixture.world.registry().all_of<ImpulseLockout>(fixture.entity));
    const std::uint32_t flags = derived_visual_flags(fixture.world, fixture.entity);
    require((flags & kVisualFlagLeaping) == 0u);
    require((flags & kVisualFlagKnockedBack) != 0u);
    // The knockback's own landing is the one that knocks it down.
    require(fixture.fly_from(fixture.tick, 60u, glm::vec2{0.0f}) != 0u);
    require(fixture.world.registry().get<ImpulseLockout>(fixture.entity).recovering);
}

void death_ends_the_leap() {
    {
        Fixture fixture(glm::vec3{0.0f});
        require(fixture.leap(glm::vec3{6.0f, 0.0f, 0.0f}, 20u));
        fixture.world.registry().get<Health>(fixture.entity).hp = 0u;
        ConfirmedDamage killing{};
        killing.target_net_id = fixture.actor;
        killing.damage = 50u;
        EntityLifecycleSystem{}.enter_death_state(fixture.engine, {killing});
        require(!fixture.world.registry().all_of<LeapLockout>(fixture.entity));
        const glm::vec3 died_at = fixture.position();
        require(fixture.fly_from(fixture.tick, 60u, glm::vec2{1.0f, 0.0f}) != 0u);
        require(nearly(fixture.position().x, died_at.x, 0.02f));
    }
}

// The two ends agree: what a remote client replays from the anchor the flush
// builds -- the state at the end of the arming tick, and the ticks left to the
// landing -- puts it where the authority did, on the same tick.
void the_anchor_replays_to_the_authoritys_landing() {
    for (const float dy : {0.0f, 1.0f}) {
        std::vector<Box> boxes;
        if (dy > 0.0f) {
            boxes.push_back(Box{glm::vec3{9.0f, dy * 0.5f, 0.0f},
                                glm::vec3{4.5f, dy * 0.5f, 5.0f}});
        }
        Fixture fixture(glm::vec3{0.0f}, false, boxes);
        const std::uint32_t armed = fixture.tick;
        require(fixture.leap(glm::vec3{6.0f, dy, 0.0f}, 24u));
        const LeapLockout lockout =
            fixture.world.registry().get<LeapLockout>(fixture.entity);
        fixture.tick = armed;
        fixture.step(glm::vec2{0.0f});
        // The record, as flush_actor_impulses writes it at the end of `armed`.
        const glm::vec3 anchor_position = fixture.position();
        const glm::vec3 anchor_velocity =
            fixture.world.registry().get<Velocity>(fixture.entity).linear;
        const std::uint32_t remaining = lockout.landing_tick - armed;
        require(remaining == 23u);

        float lowest_ascending = anchor_position.y;
        for (std::uint32_t n = 1; n <= remaining; ++n) {
            const std::uint32_t this_tick = fixture.tick;
            fixture.step(glm::vec2{0.0f});
            // apply_knockback_anchor's rule for a leap: floored at the
            // landing height only on the way down.
            glm::vec3 replayed = knockback_flight_position_at(
                anchor_position, anchor_velocity, kGravity, kTick,
                static_cast<float>(n) * kTick);
            if (knockback_flight_velocity_at(
                    anchor_velocity, kGravity, static_cast<float>(n) * kTick).y < 0.0f) {
                replayed.y = std::max(replayed.y, lockout.landing_y);
            }
            const glm::vec3 authority = fixture.position();
            if (!nearly(replayed.x, authority.x, 0.001f) ||
                !nearly(replayed.y, authority.y, 0.002f) ||
                !nearly(replayed.z, authority.z, 0.001f)) {
                std::fprintf(stderr,
                             "dy %.1f tick +%u: replay (%.4f, %.4f, %.4f) "
                             "authority (%.4f, %.4f, %.4f)\n",
                             dy, n, replayed.x, replayed.y, replayed.z,
                             authority.x, authority.y, authority.z);
            }
            require(nearly(replayed.x, authority.x, 0.001f));
            require(nearly(replayed.y, authority.y, 0.002f));
            require(nearly(replayed.z, authority.z, 0.001f));
            if (knockback_flight_velocity_at(
                    anchor_velocity, kGravity, static_cast<float>(n) * kTick).y > 0.0f) {
                lowest_ascending = std::min(lowest_ascending, replayed.y);
            }
            if (n == remaining) {
                require(this_tick == lockout.landing_tick);
                require(fixture.grounded());
            }
        }
        // Going up onto the ledge it is still below the floor it lands on: a
        // floor held on the way up would have lifted it there at once.
        if (dy > 0.0f) {
            require(anchor_position.y < lockout.landing_y);
        } else {
            require(lowest_ascending >= lockout.landing_y);
        }
    }
}

}  // namespace

int main() {
    lands_on_the_solved_spot_on_the_solved_tick();
    landing_is_not_a_knockdown_and_nothing_is_refused_in_the_air();
    refused_unless_a_grounded_free_agent_with_ground_to_land_on();
    a_knockback_ends_the_leap_and_owns_the_flight();
    death_ends_the_leap();
    the_anchor_replays_to_the_authoritys_landing();
    std::fprintf(stderr, "entity_leap_test passed\n");
    return 0;
}
