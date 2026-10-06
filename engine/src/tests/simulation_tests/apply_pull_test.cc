// apply_pull: land an actor at a destination, whatever it was doing.
//
// apply_impulse adds a velocity; apply_pull replaces it with the one that puts
// the target down at a chosen spot after a chosen airtime. Two things make
// that exact rather than approximate, and both are pinned here against the
// real movement solver: the vertical launch accounts for the semi-implicit
// integration the solver uses, and the velocity the target already had --
// running at the thrower, say -- is discarded instead of carried along.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
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
        std::fprintf(stderr, "apply_pull_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr float kTick = 1.0f / 30.0f;

bool nearly(float lhs, float rhs, float tolerance = 0.001f) {
    return std::fabs(lhs - rhs) < tolerance;
}

KernelActionTriggerDefinition pull_trigger(
    std::uint32_t mode,
    std::uint8_t point_source,
    float distance,
    std::uint32_t airtime_ticks,
    float max_speed,
    float strength = 10.0f) {
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(trigger);
    trigger.action_count = 1u;
    KernelActionDefinition& action = trigger.actions[0];
    action.action_type = KernelEntityTriggerActionType_ApplyPull;
    action.target_source = KernelEntityRefSource_EventTarget;
    action.pull_mode = mode;
    if (mode == KERNEL_PULL_MODE_ALONG) {
        action.direction_source = point_source;
    } else {
        action.position_source = point_source;
    }
    action.pull_distance = distance;
    action.pull_airtime_ticks = airtime_ticks;
    action.pull_max_speed = max_speed;
    action.pull_strength = strength;
    return trigger;
}

// ---------------------------------------------------------------------------
// Authoring: what the loader and the kernel validators both refuse.
// ---------------------------------------------------------------------------

void only_coherent_pulls_compile() {
    const auto compiles = [](std::uint32_t mode, std::uint8_t source,
                             float distance, std::uint32_t airtime,
                             float max_speed,
                             TriggerEventType event =
                                 TriggerEventType::kProjectileImpact) {
        return compile_action_trigger_definition(
                   event,
                   pull_trigger(mode, source, distance, airtime, max_speed))
            .has_value();
    };
    require(compiles(KERNEL_PULL_MODE_TO_POINT,
                     KernelEventVec3Source_SubjectPosition, 0.0f, 25u, 12.0f));
    require(compiles(KERNEL_PULL_MODE_ALONG,
                     KernelEventVec3Source_Direction, -3.0f, 25u, 12.0f));
    // A ring around the point is fine; a negative ring is not.
    require(compiles(KERNEL_PULL_MODE_TO_POINT,
                     KernelEventVec3Source_SubjectPosition, 2.0f, 25u, 12.0f));
    require(!compiles(KERNEL_PULL_MODE_TO_POINT,
                      KernelEventVec3Source_SubjectPosition, -1.0f, 25u, 12.0f));
    // Moving zero metres along a heading is not a pull.
    require(!compiles(KERNEL_PULL_MODE_ALONG,
                      KernelEventVec3Source_Direction, 0.0f, 25u, 12.0f));
    require(!compiles(KERNEL_PULL_MODE_TO_POINT,
                      KernelEventVec3Source_SubjectPosition, 0.0f, 0u, 12.0f));
    require(!compiles(KERNEL_PULL_MODE_TO_POINT,
                      KernelEventVec3Source_SubjectPosition, 0.0f,
                      KERNEL_MAX_IMPULSE_LOCKOUT_TICKS + 1u, 12.0f));
    require(!compiles(KERNEL_PULL_MODE_TO_POINT,
                      KernelEventVec3Source_SubjectPosition, 0.0f, 25u, 0.0f));
    require(!compiles(7u, KernelEventVec3Source_SubjectPosition, 0.0f, 25u, 12.0f));
    // Strength is the fixed number resistance is weighed against: positive
    // and finite, or there is nothing to weigh.
    for (const float strength :
         {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
          std::numeric_limits<float>::quiet_NaN()}) {
        require(!compile_action_trigger_definition(
                     TriggerEventType::kProjectileImpact,
                     pull_trigger(KERNEL_PULL_MODE_TO_POINT,
                                  KernelEventVec3Source_SubjectPosition,
                                  0.0f, 25u, 12.0f, strength))
                     .has_value());
    }
    // The subject position only exists on the projectile triggers; a
    // collision would hand the graph the world origin.
    require(!compiles(KERNEL_PULL_MODE_TO_POINT,
                      KernelEventVec3Source_SubjectPosition, 0.0f, 25u, 12.0f,
                      TriggerEventType::kCollision));
}

// ---------------------------------------------------------------------------
// The flight, against the real movement solver on flat ground.
// ---------------------------------------------------------------------------

struct MovementFixture {
    explicit MovementFixture(glm::vec3 spawn) : world(false) {
        world.set_collision_world(&physics);
        add_ground();
        actor = world.spawn_player(7, spawn);
        entity = *world.find_entity(actor);
        MovementState& movement = world.registry().get<MovementState>(entity);
        movement.speed_meters_per_second = 5.0f;
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

    // Holding a key the whole way, as every AI controller does each tick:
    // the lockout is what must keep it from steering the flight.
    void tick(const glm::vec2& move) {
        KernelPlayerInput input{};
        input.move = KernelVec2{move.x, move.y};
        const std::vector<QueuedInput> inputs{
            QueuedInput{7, input, tick_index, 0, false, 0},
        };
        simulate_actor_movement(
            world, inputs, kTick, tick_index, &events, &stats);
        sync_body();
        ++tick_index;
    }

    // What the apply_pull commit does to an actor.
    void pull_to(const glm::vec3& destination, std::uint32_t airtime_ticks) {
        const glm::vec3 position = this->position();
        world.registry().get<Velocity>(entity).linear = pull_launch_velocity(
            position, destination, airtime_ticks, kTick,
            world.registry().get<MovementState>(entity).gravity.y, 100.0f);
        world.registry().emplace_or_replace<ImpulseLockout>(
            entity, ImpulseLockout{tick_index + airtime_ticks + 2u, tick_index});
        MovementState& movement = world.registry().get<MovementState>(entity);
        movement.ground_state = MovementState::GroundState::kAirborne;
        movement.has_controller_height = false;
    }

    glm::vec3 position() const {
        return world.registry().get<Transform>(entity).position;
    }

    bool grounded() const {
        return world.registry().get<MovementState>(entity).ground_state ==
            MovementState::GroundState::kGrounded;
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

// The claim the whole action rests on: N ticks later, on the spot.
void a_pulled_actor_lands_on_its_destination_on_the_authored_tick() {
    for (const std::uint32_t airtime : {10u, 25u, 40u}) {
        for (const glm::vec3& start :
             {glm::vec3{4.0f, 0.0f, 0.0f}, glm::vec3{-1.5f, 0.0f, 2.0f},
              glm::vec3{0.0f, 0.0f, -6.0f}}) {
            MovementFixture fixture(start);
            for (int settle = 0; settle < 5; ++settle) {
                fixture.tick(glm::vec2{0.0f});
            }
            require(fixture.grounded());
            const glm::vec3 destination{0.5f, 0.0f, 0.5f};
            fixture.pull_to(destination, airtime);
            // Steering away from the destination the whole flight.
            const glm::vec2 away{start.x, start.z};
            std::uint32_t landed = 0;
            for (std::uint32_t step = 1; step <= airtime + 5u; ++step) {
                fixture.tick(glm::length(away) > 0.0f
                                 ? glm::normalize(away)
                                 : glm::vec2{0.0f});
                if (step > 1u && fixture.grounded()) {
                    landed = step;
                    break;
                }
            }
            const glm::vec3 at = fixture.position();
            if (landed != airtime ||
                !nearly(at.x, destination.x, 0.02f) ||
                !nearly(at.z, destination.z, 0.02f)) {
                std::fprintf(
                    stderr,
                    "airtime %u from (%.2f, %.2f): landed on step %u at "
                    "(%.3f, %.3f, %.3f)\n",
                    airtime, start.x, start.z, landed, at.x, at.y, at.z);
            }
            require(landed == airtime);
            require(nearly(at.x, destination.x, 0.02f));
            require(nearly(at.z, destination.z, 0.02f));
        }
    }
}

// ---------------------------------------------------------------------------
// The graph and the commit, through the engine.
// ---------------------------------------------------------------------------

struct Pulled {
    bool ok = false;
    glm::vec3 velocity{0.0f};
    bool locked = false;
};

Pulled pull(
    const KernelActionTriggerDefinition& trigger,
    const glm::vec3& target_at,
    const glm::vec3& subject_at,
    const glm::vec3& existing_velocity,
    float resistance = 0.0f) {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId subject = world.spawn_player(1, glm::vec3{0.0f, 0.0f, -20.0f});
    const NetId target = world.spawn_enemy(target_at);
    const entt::entity target_entity = *world.find_entity(target);
    world.registry().get_or_emplace<Velocity>(target_entity).linear =
        existing_velocity;
    if (resistance > 0.0f) {
        world.registry().emplace<ImpulseResistance>(
            target_entity, ImpulseResistance{resistance});
    }
    const std::optional<CompiledActionGraphBinding> binding =
        compile_action_trigger_definition(
            TriggerEventType::kProjectileImpact, trigger);
    require(binding.has_value());

    TriggerEvent event{
        TriggerEventType::kProjectileImpact, subject, subject, target,
        target_at, glm::normalize(target_at - subject_at)};
    event.subject_position = subject_at;
    ActionExecutionProvenance provenance;
    provenance.request_id = 1;
    provenance.server_tick = engine.current_tick();
    provenance.instigator = subject;
    std::vector<ActionGraphCommand> commands;
    Pulled result;
    std::string error;
    if (!evaluate_action_graph(
            *binding, subject, event, provenance, &commands, &error)) {
        std::fprintf(stderr, "evaluate: %s\n", error.c_str());
        return result;
    }
    result.ok = execute_action_graph_command_batch(
        engine,
        ActionGraphCommandBatch{event, provenance, 1u, std::move(commands)},
        0u);
    result.velocity =
        world.registry().get_or_emplace<Velocity>(target_entity).linear;
    result.locked = world.registry().all_of<ImpulseLockout>(target_entity);
    return result;
}

void the_commit_replaces_the_velocity_the_target_had() {
    const glm::vec3 centre{0.0f, 0.5f, 0.0f};
    const glm::vec3 standing{4.0f, 0.0f, 0.0f};
    // Running straight at where it is being pulled from, at 6 m/s.
    const Pulled running = pull(
        pull_trigger(KERNEL_PULL_MODE_TO_POINT,
                     KernelEventVec3Source_SubjectPosition, 0.0f, 25u, 12.0f),
        standing, centre, glm::vec3{-6.0f, 0.0f, 3.0f});
    const Pulled still = pull(
        pull_trigger(KERNEL_PULL_MODE_TO_POINT,
                     KernelEventVec3Source_SubjectPosition, 0.0f, 25u, 12.0f),
        standing, centre, glm::vec3{0.0f});
    require(running.ok && still.ok);
    require(running.locked);
    // Identical: nothing of the run survives.
    require(nearly(running.velocity.x, still.velocity.x));
    require(nearly(running.velocity.y, still.velocity.y));
    require(nearly(running.velocity.z, still.velocity.z));
    // 4 m in 25 ticks, toward the centre; up by g*dt*(N+0.5)/2.
    require(nearly(still.velocity.x, -4.0f / (25.0f * kTick)));
    require(nearly(still.velocity.z, 0.0f));
    require(nearly(still.velocity.y, 9.81f * kTick * 25.5f * 0.5f));
}

void a_ring_distance_puts_the_target_on_its_own_side() {
    const glm::vec3 centre{0.0f, 0.5f, 0.0f};
    // 5 m out, landing 2 m out: 3 m inward.
    const Pulled inward = pull(
        pull_trigger(KERNEL_PULL_MODE_TO_POINT,
                     KernelEventVec3Source_SubjectPosition, 2.0f, 30u, 12.0f),
        glm::vec3{0.0f, 0.0f, 5.0f}, centre, glm::vec3{0.0f});
    require(inward.ok);
    require(nearly(inward.velocity.z, -3.0f / (30.0f * kTick)));
    // 1 m out, landing 2 m out: pushed outward instead, on the same side.
    const Pulled outward = pull(
        pull_trigger(KERNEL_PULL_MODE_TO_POINT,
                     KernelEventVec3Source_SubjectPosition, 2.0f, 30u, 12.0f),
        glm::vec3{0.0f, 0.0f, 1.0f}, centre, glm::vec3{0.0f});
    require(outward.ok);
    require(nearly(outward.velocity.z, 1.0f / (30.0f * kTick)));
}

void along_moves_a_signed_distance_on_the_horizontal() {
    // The event direction here points from the subject to the target, +x.
    const Pulled back = pull(
        pull_trigger(KERNEL_PULL_MODE_ALONG,
                     KernelEventVec3Source_Direction, -3.0f, 30u, 12.0f),
        glm::vec3{2.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 2.0f, 0.0f},
        glm::vec3{0.0f});
    require(back.ok);
    // The direction has Y in it; only its horizontal is followed.
    require(nearly(back.velocity.x, -3.0f / (30.0f * kTick)));
    require(nearly(back.velocity.z, 0.0f));
}

void max_speed_lands_it_short_on_the_same_line() {
    const Pulled capped = pull(
        pull_trigger(KERNEL_PULL_MODE_TO_POINT,
                     KernelEventVec3Source_SubjectPosition, 0.0f, 10u, 4.0f),
        glm::vec3{6.0f, 0.0f, 8.0f}, glm::vec3{0.0f}, glm::vec3{0.0f});
    require(capped.ok);
    const float speed = std::sqrt(
        capped.velocity.x * capped.velocity.x +
        capped.velocity.z * capped.velocity.z);
    require(nearly(speed, 4.0f));
    require(nearly(capped.velocity.x / capped.velocity.z, 6.0f / 8.0f));
}

void resistance_weighs_the_authored_strength_not_the_launch() {
    const auto trigger = pull_trigger(
        KERNEL_PULL_MODE_TO_POINT,
        KernelEventVec3Source_SubjectPosition, 0.0f, 25u, 12.0f, 10.0f);
    // 6 m away launches at ~7.2 m/s, 2 m at ~4.25 (the vertical): both under
    // the strength, and neither speed may decide the outcome. Equal to the
    // strength resists, as apply_impulse's strictly-greater test does.
    for (const float x : {6.0f, 2.0f}) {
        const Pulled moved = pull(
            trigger, glm::vec3{x, 0.0f, 0.0f}, glm::vec3{0.0f},
            glm::vec3{0.0f}, 9.99f);
        const Pulled held = pull(
            trigger, glm::vec3{x, 0.0f, 0.0f}, glm::vec3{0.0f},
            glm::vec3{0.0f}, 10.0f);
        require(moved.ok && held.ok);
        require(moved.locked);
        require(nearly(moved.velocity.x, -x / (25.0f * kTick)));
        require(!held.locked);
        require(nearly(held.velocity.x, 0.0f));
    }
}

// ---------------------------------------------------------------------------
// The anchor an area effect reports.
// ---------------------------------------------------------------------------

void an_area_effect_reports_its_centre_as_the_subject_position() {
    World world;
    const glm::vec3 centre{10.0f, 0.5f, -3.0f};
    const NetId target = world.spawn_enemy(glm::vec3{12.0f, 0.0f, -2.0f});
    const entt::entity target_entity = *world.find_entity(target);
    // Without a catalog an enemy spawns at 0 hp, and a dead actor's hitbox is
    // disabled in the collision world: the query would see no one at all.
    world.registry().get<Health>(target_entity) = Health{50, 50};
    world.registry().get<Hitbox>(target_entity) =
        Hitbox{{0.0f, 0.5f, 0.0f}, {0.25f, 0.5f, 0.25f}, 0};

    const NetId area = world.spawn_projectile(0, centre, glm::vec3{0.0f});
    const entt::entity area_entity = *world.find_entity(area);
    ProjectileState& projectile =
        world.registry().get<ProjectileState>(area_entity);
    projectile.collision_mask = kCollisionMaskDamageable;
    projectile.max_lifetime_ticks = 0;
    world.registry().replace<Hitbox>(
        area_entity, Hitbox{{0.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 4.0f}, 0});
    const std::optional<CompiledActionGraphBinding> binding =
        compile_action_trigger_definition(
            TriggerEventType::kProjectileImpact,
            pull_trigger(KERNEL_PULL_MODE_TO_POINT,
                         KernelEventVec3Source_SubjectPosition, 0.0f, 25u,
                         12.0f));
    require(binding.has_value());
    ProjectileAreaEffectRuntime& area_effect =
        world.registry().emplace<ProjectileAreaEffectRuntime>(
            area_entity,
            ProjectileAreaEffectRuntime{
                4.0f, 1, 45, 0, 7, kCollisionMaskDamageable,
                ProjectileDamageFalloff::kNone, {}});
    area_effect.action_graph_binding = *binding;

    DamagePipeline pipeline;
    std::vector<KernelEvent> events;
    std::vector<ActionGraphCommandBatch> batches;
    simulate_area_effects(world, 0, 0, &events, &pipeline, &batches);
    require(batches.size() == 1u);
    require(batches[0].commands.size() == 1u);
    const auto* command =
        std::get_if<ActionApplyPullCommand>(&batches[0].commands.front());
    require(command != nullptr);
    require(command->target == target);
    // The blast's centre, not the point the target was hit at.
    require(nearly(command->point.x, centre.x));
    require(nearly(command->point.y, centre.y));
    require(nearly(command->point.z, centre.z));
}

}  // namespace

int main() {
    only_coherent_pulls_compile();
    a_pulled_actor_lands_on_its_destination_on_the_authored_tick();
    the_commit_replaces_the_velocity_the_target_had();
    a_ring_distance_puts_the_target_on_its_own_side();
    along_moves_a_signed_distance_on_the_horizontal();
    max_speed_lands_it_short_on_the_same_line();
    resistance_weighs_the_authored_strength_not_the_launch();
    an_area_effect_reports_its_centre_as_the_subject_position();
    return 0;
}
