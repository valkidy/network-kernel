// lock_strike: an action graph's lock (docs/LOCK_STRIKE_ACTION_PLAN.md, P2,
// and its repeat, P3).
// A bottle breaks, and up to its shot's max_hit_count actors in a sphere or a
// cone around the break are struck -- at once, or by a certain missile each
// that lands on its arrival. The lock is a lock-on weapon's: nearest first,
// the thrower's own never. A repeated one is taken again every interval from
// the same spot, locking afresh each time, whatever became of the bottle. The
// catalog refuses one it could not run.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// The repeat is stepped by hand -- tick by tick, without the rest of a
// server tick -- so the test reaches the engine's tick loop and queue.
#define private public
#include "kernel/src/kernel.h"
#undef private
#include "physics/public/physics_world.h"
#include "simulation/public/action_graph.h"
#include "simulation/src/systems.h"

namespace {

using namespace network_example;

void require(
    bool condition,
    const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(
            stderr,
            "require failed at %s:%u\n",
            location.file_name(),
            location.line());
        std::abort();
    }
}

constexpr std::uint32_t kSphere = 54u;
constexpr std::uint32_t kCone = 55u;
constexpr std::uint32_t kBox = 56u;
constexpr std::uint32_t kNotDamage = 57u;
constexpr std::uint32_t kShotCollider = 58u;
constexpr std::uint32_t kShot = 35u;
constexpr std::uint32_t kLongShot = 36u;
constexpr std::uint32_t kWideShot = 37u;
constexpr std::uint32_t kBottle = 228u;
constexpr std::uint16_t kDamage = 10u;
constexpr float kReach = 10.0f;
// The shot's lifetime: a missile crosses the whole reach in this, so a metre a
// tick.
constexpr std::uint32_t kFlight = 10u;
constexpr PeerId kThrowerPeer = 1u;

KernelColliderTemplateDefinition collider(
    std::uint32_t id,
    std::uint8_t shape,
    KernelVec4 params,
    std::uint32_t purpose = KernelColliderPurpose_Damage) {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = id;
    collider.shape_type = shape;
    collider.shape_params = params;
    collider.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;
    collider.purpose_flags = purpose;
    return collider;
}

KernelProjectileTemplateDefinition shot(
    std::uint32_t id,
    std::uint32_t lifetime_ticks,
    std::uint32_t max_hit_count) {
    KernelProjectileTemplateDefinition shot{};
    shot.struct_size = sizeof(shot);
    shot.projectile_template_id = id;
    shot.mechanics.struct_size = sizeof(KernelProjectileMechanicsDefinition);
    shot.mechanics.projectile_type = KernelProjectileType_Standard;
    shot.mechanics.motion_model = KernelProjectileMotionModel_Linear;
    shot.mechanics.sync_mode = KernelProjectileSyncMode_ServerSnapshotOnly;
    shot.mechanics.hit_response = KernelProjectileHitResponse_Destroy;
    shot.mechanics.damage_shape = KernelProjectileDamageShape_DirectHit;
    shot.mechanics.damage = kDamage;
    shot.mechanics.speed = 0.1f;
    shot.mechanics.lifetime_ticks = lifetime_ticks;
    shot.mechanics.collider_template_id = kShotCollider;
    shot.mechanics.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    shot.mechanics.max_hit_count = max_hit_count;
    return shot;
}

KernelActionDefinition strike_action(
    std::uint32_t volume,
    std::uint8_t mode,
    bool aimed) {
    KernelActionDefinition action{};
    action.action_type = KernelEntityTriggerActionType_LockStrike;
    action.lock_collider_template_id = volume;
    action.spawn_projectile_template_id = kShot;
    action.lock_mode = mode;
    action.position_source = KernelEventVec3Source_Position;
    action.direction_source = aimed ? KernelEventVec3Source_Direction
                                    : KernelEventVec3Source_Position;
    return action;
}

KernelActionTriggerDefinition trigger_of(const KernelActionDefinition& action) {
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(trigger);
    trigger.action_count = 1u;
    trigger.actions[0] = action;
    return trigger;
}

// A catalog with every volume and shot the tests name, and a bottle prop whose
// on_collision is `collision` (none when its struct_size is zero).
bool load(KernelEngine& engine, const KernelActionTriggerDefinition& collision) {
    const KernelColliderTemplateDefinition colliders[] = {
        collider(kSphere, KernelColliderShapeType_Sphere,
                 KernelVec4{kReach, 0.0f, 0.0f, 0.0f}),
        collider(kCone, KernelColliderShapeType_Cone,
                 KernelVec4{kReach, 60.0f, 0.0f, 0.0f}),
        collider(kBox, KernelColliderShapeType_Aabb,
                 KernelVec4{1.0f, 1.0f, 1.0f, 0.0f}),
        collider(kNotDamage, KernelColliderShapeType_Sphere,
                 KernelVec4{kReach, 0.0f, 0.0f, 0.0f},
                 KernelColliderPurpose_Hit),
        collider(kShotCollider, KernelColliderShapeType_Sphere,
                 KernelVec4{0.25f, 0.0f, 0.0f, 0.0f}),
    };
    const KernelProjectileTemplateDefinition shots[] = {
        shot(kShot, kFlight, 2u),
        shot(kLongShot, 300u, 2u),
        shot(kWideShot, kFlight, KERNEL_MAX_LOCK_ARCS + 1u),
    };
    KernelEntityTemplateDefinition bottle{};
    bottle.struct_size = sizeof(bottle);
    bottle.entity_template_id = kBottle;
    bottle.entity_type = KernelEntityType_Prop;
    bottle.actor_type = KernelActorType_Unknown;
    bottle.ai.struct_size = sizeof(bottle.ai);
    bottle.vision.struct_size = sizeof(bottle.vision);
    bottle.combat.struct_size = sizeof(bottle.combat);
    bottle.movement.struct_size = sizeof(bottle.movement);
    if (collision.struct_size != 0u) {
        bottle.collision_trigger = collision;
        bottle.collision_trigger_mask = KERNEL_COLLISION_MASK_ACTOR;
    }
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1u;
    catalog.catalog_hash = UINT64_C(1);
    catalog.collider_templates = colliders;
    catalog.collider_template_count = 5u;
    catalog.projectile_templates = shots;
    catalog.projectile_template_count = 3u;
    catalog.entity_templates = &bottle;
    catalog.entity_template_count = 1u;
    return engine.load_gameplay_catalog(catalog);
}

bool loads(const KernelActionDefinition& action) {
    KernelEngine engine(KernelConfig{});
    return load(engine, trigger_of(action));
}

KernelConfig server_config() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    return config;
}

struct Scene {
    KernelEngine engine{server_config()};
    World& world = engine.simulation_world();
    NetId thrower = 0;

    Scene() {
        require(load(engine, KernelActionTriggerDefinition{}));
        // The thrower stands nearest of all; its peer is the graph's owner.
        thrower = world.spawn_player(kThrowerPeer, glm::vec3{1.0f, 0.0f, 0.0f});
        healthy(thrower);
    }

    void healthy(NetId net_id) {
        const auto entity = world.find_entity(net_id);
        require(entity.has_value());
        world.registry().get<Health>(*entity) = Health{100, 100};
        world.registry().get<Hitbox>(*entity) =
            Hitbox{{0.0f, 0.5f, 0.0f}, {0.3f, 0.5f, 0.3f}, 0};
    }

    NetId enemy(const glm::vec3& position) {
        const NetId net_id = world.spawn_enemy(position);
        healthy(net_id);
        return net_id;
    }

    std::uint16_t hp(NetId net_id) {
        return world.registry().get<Health>(*world.find_entity(net_id)).hp;
    }

    // The bottle breaking at the origin, heading +z, as its collision
    // trigger reports it: nobody instigates, the thrower's peer owns it.
    bool strike(const KernelActionDefinition& action) {
        const std::optional<CompiledActionGraphBinding> binding =
            compile_action_trigger_definition(
                TriggerEventType::kCollision, trigger_of(action));
        require(binding.has_value());
        const TriggerEvent event{
            TriggerEventType::kCollision,
            0u,
            0u,
            0u,
            glm::vec3{0.0f},
            glm::vec3{0.0f, 0.0f, 1.0f},
            std::nullopt,
        };
        ActionExecutionProvenance provenance;
        provenance.request_id = 1u;
        provenance.server_tick = engine.current_tick();
        provenance.owner_peer = kThrowerPeer;
        std::vector<ActionGraphCommand> commands;
        std::string error;
        if (!evaluate_action_graph(
                *binding, 0u, event, provenance, &commands, &error)) {
            std::fprintf(stderr, "evaluate: %s\n", error.c_str());
            return false;
        }
        return execute_action_graph_command_batch(
            engine,
            ActionGraphCommandBatch{event, provenance, 1u, std::move(commands)},
            0u);
    }

    // One tick on: the repeats due on it struck, then its damage drained.
    void step() {
        engine.tick_loop_.advance_tick();
        const std::uint32_t tick = engine.current_tick();
        engine.run_due_lock_strikes(static_cast<std::uint64_t>(
            tick * engine.fixed_delta_seconds() * 1000000.0f));
        drain(tick);
    }

    void drain(std::uint32_t tick) {
        std::vector<KernelEvent> events;
        engine.damage_pipeline().confirm_ready(
            world,
            static_cast<std::uint64_t>(
                tick * engine.fixed_delta_seconds() * 1000000.0f),
            tick,
            &events);
    }
};

// A sphere locks the nearest two of any side and any direction -- behind and
// overhead included -- but never the thrower, though it stands nearest, and
// nothing beyond its radius. Struck at once.
void an_instant_sphere_strikes_the_nearest_around_it() {
    Scene scene;
    const NetId behind = scene.enemy(glm::vec3{0.0f, 0.0f, -3.0f});
    const NetId beside = scene.enemy(glm::vec3{-5.0f, 0.0f, 0.0f});
    const NetId ahead = scene.enemy(glm::vec3{0.0f, 0.0f, 8.0f});
    const NetId beyond = scene.enemy(glm::vec3{0.0f, 0.0f, 12.0f});
    require(scene.strike(
        strike_action(kSphere, KernelLockStrikeMode_Instant, false)));
    scene.drain(scene.engine.current_tick());
    require(scene.hp(behind) == 100 - kDamage);
    require(scene.hp(beside) == 100 - kDamage);
    require(scene.hp(ahead) == 100);
    require(scene.hp(beyond) == 100);
    require(scene.hp(scene.thrower) == 100);
}

// A cone aimed along the bottle's heading leaves out what is behind it.
void an_instant_cone_strikes_only_ahead() {
    Scene scene;
    const NetId behind = scene.enemy(glm::vec3{0.0f, 0.0f, -3.0f});
    const NetId ahead = scene.enemy(glm::vec3{0.0f, 0.0f, 8.0f});
    require(scene.strike(
        strike_action(kCone, KernelLockStrikeMode_Instant, true)));
    scene.drain(scene.engine.current_tick());
    require(scene.hp(behind) == 100);
    require(scene.hp(ahead) == 100 - kDamage);
}

// A missile for each it locked, all launched now: each lands on its own
// arrival -- a metre a tick from the lifted origin -- and not a tick before.
void missiles_land_on_each_ones_arrival() {
    Scene scene;
    const NetId near = scene.enemy(glm::vec3{0.0f, 0.0f, -3.0f});
    const NetId far = scene.enemy(glm::vec3{-7.0f, 0.0f, 0.0f});
    KernelActionDefinition action =
        strike_action(kSphere, KernelLockStrikeMode_Missile, false);
    action.lock_origin_lift = 0.5f;
    const std::uint32_t start = scene.engine.current_tick();
    require(scene.strike(action));
    // From (0, 0.5, 0) to each hitbox centre, half a metre up.
    const std::uint32_t near_arrival = start + 3u;
    const std::uint32_t far_arrival = start + 7u;
    for (std::uint32_t tick = start; tick < near_arrival; ++tick) {
        scene.drain(tick);
    }
    require(scene.hp(near) == 100);
    scene.drain(near_arrival);
    require(scene.hp(near) == 100 - kDamage);
    for (std::uint32_t tick = near_arrival; tick < far_arrival; ++tick) {
        scene.drain(tick);
    }
    require(scene.hp(far) == 100);
    scene.drain(far_arrival);
    require(scene.hp(far) == 100 - kDamage);
}

// The lift is where the lock is taken from: lifted 4 m, the same two targets
// are further away and their missiles land later.
void the_lift_moves_the_origin_up() {
    Scene scene;
    const NetId near = scene.enemy(glm::vec3{0.0f, 0.0f, -3.0f});
    KernelActionDefinition action =
        strike_action(kSphere, KernelLockStrikeMode_Missile, false);
    action.lock_origin_lift = 4.0f;
    const std::uint32_t start = scene.engine.current_tick();
    require(scene.strike(action));
    // From (0, 4, 0) to (0, 0.5, -3): 4.61 m, five ticks.
    for (std::uint32_t tick = start; tick < start + 5u; ++tick) {
        scene.drain(tick);
    }
    require(scene.hp(near) == 100);
    scene.drain(start + 5u);
    require(scene.hp(near) == 100 - kDamage);
}

// The catalog takes a well-formed one in either volume, and refuses what it
// could not run: a volume that is neither a damage cone nor a damage sphere, a
// cone with no heading, a shot that locks more than KERNEL_MAX_LOCK_ARCS, a
// missile whose flight a record cannot carry, a lift past the ceiling, an
// unknown mode, and -- until P3 -- a repeat.
void the_catalog_refuses_what_it_could_not_run() {
    require(loads(strike_action(kSphere, KernelLockStrikeMode_Instant, false)));
    require(loads(strike_action(kSphere, KernelLockStrikeMode_Missile, true)));
    require(loads(strike_action(kCone, KernelLockStrikeMode_Instant, true)));

    require(!loads(strike_action(kCone, KernelLockStrikeMode_Instant, false)));
    require(!loads(strike_action(kBox, KernelLockStrikeMode_Instant, false)));
    require(!loads(strike_action(kNotDamage, KernelLockStrikeMode_Instant, false)));
    require(!loads(strike_action(999u, KernelLockStrikeMode_Instant, false)));

    KernelActionDefinition wide =
        strike_action(kSphere, KernelLockStrikeMode_Instant, false);
    wide.spawn_projectile_template_id = kWideShot;
    require(!loads(wide));
    KernelActionDefinition long_flight =
        strike_action(kSphere, KernelLockStrikeMode_Missile, false);
    long_flight.spawn_projectile_template_id = kLongShot;
    require(!loads(long_flight));
    // The same shot is fine struck at once: it has no flight to report.
    long_flight.lock_mode = KernelLockStrikeMode_Instant;
    require(loads(long_flight));

    KernelActionDefinition high =
        strike_action(kSphere, KernelLockStrikeMode_Instant, false);
    high.lock_origin_lift = KERNEL_MAX_LOCK_ORIGIN_LIFT + 0.5f;
    require(!loads(high));
    KernelActionDefinition mode =
        strike_action(kSphere, KernelLockStrikeMode_Instant, false);
    mode.lock_mode = 2u;
    require(!loads(mode));
    // Repeats: 5 x 10 ticks loads; a count past the ceiling, a repeat with no
    // interval, an interval with no repeat and a run longer than
    // KERNEL_MAX_LOCK_STRIKE_SPAN_TICKS do not.
    const auto repeated = [](std::uint16_t count, std::uint32_t interval) {
        KernelActionDefinition action =
            strike_action(kSphere, KernelLockStrikeMode_Instant, false);
        action.lock_repeat_count = count;
        action.lock_repeat_interval_ticks = interval;
        return action;
    };
    require(loads(repeated(5u, 10u)));
    require(loads(repeated(1u, 0u)));
    require(loads(repeated(16u, 17u)));  // 15 x 17 = 255
    require(!loads(repeated(17u, 1u)));
    require(!loads(repeated(5u, 0u)));
    require(!loads(repeated(1u, 10u)));
    require(!loads(repeated(16u, 18u)));  // 270
}

KernelActionDefinition repeated_strike(
    std::uint8_t mode,
    std::uint16_t count,
    std::uint32_t interval) {
    KernelActionDefinition action = strike_action(kSphere, mode, false);
    action.lock_repeat_count = count;
    action.lock_repeat_interval_ticks = interval;
    return action;
}

// Every 10 ticks, five times: struck on the tick the graph ran and on each
// tenth after it, never in between and never a sixth time.
void a_repeat_strikes_on_its_schedule_and_stops() {
    Scene scene;
    const NetId target = scene.enemy(glm::vec3{0.0f, 0.0f, -3.0f});
    require(scene.strike(
        repeated_strike(KernelLockStrikeMode_Instant, 5u, 10u)));
    scene.drain(scene.engine.current_tick());
    std::uint32_t struck = 1u;
    require(scene.hp(target) == 100 - kDamage);
    for (std::uint32_t elapsed = 1u; elapsed <= 60u; ++elapsed) {
        scene.step();
        if (elapsed % 10u == 0u && elapsed <= 40u) {
            ++struck;
        }
        require(scene.hp(target) == 100 - struck * kDamage);
    }
    require(struck == 5u);
    require(scene.engine.pending_lock_strikes_.empty());
}

// Each strike locks afresh: one of the first two dies between strikes, and
// the next strike takes the next nearest in its place.
void each_repeat_locks_afresh() {
    Scene scene;
    const NetId first = scene.enemy(glm::vec3{0.0f, 0.0f, -3.0f});
    const NetId second = scene.enemy(glm::vec3{-5.0f, 0.0f, 0.0f});
    const NetId third = scene.enemy(glm::vec3{0.0f, 0.0f, 8.0f});
    require(scene.strike(
        repeated_strike(KernelLockStrikeMode_Instant, 2u, 10u)));
    scene.drain(scene.engine.current_tick());
    require(scene.hp(third) == 100);
    scene.world.registry().get<Health>(*scene.world.find_entity(first)).hp = 0;
    for (std::uint32_t elapsed = 1u; elapsed <= 10u; ++elapsed) {
        scene.step();
    }
    require(scene.hp(second) == 100 - 2 * kDamage);
    require(scene.hp(third) == 100 - kDamage);
}

// A repeated missile strike's missiles carry which strike of the run they
// belong to, so a client can tell the run's volleys apart.
void repeated_missiles_carry_their_strike() {
    Scene scene;
    scene.enemy(glm::vec3{0.0f, 0.0f, -3.0f});
    require(scene.strike(
        repeated_strike(KernelLockStrikeMode_Missile, 3u, 4u)));
    for (std::uint32_t elapsed = 1u; elapsed <= 8u; ++elapsed) {
        scene.step();
    }
    const std::vector<LockMissileLaunch>& launches =
        scene.engine.pending_lock_missiles_;
    require(launches.size() == 3u);
    for (std::uint16_t index = 0; index < 3u; ++index) {
        require(launches[index].commit_index == index);
        require(launches[index].launch_tick ==
                launches[0].launch_tick + index * 4u);
    }
}

// The queue holds 256 runs. A repeated strike past that is refused whole --
// its batch, a bottle's self-damage included -- and strikes nothing; one that
// is taken once still runs.
void a_full_queue_refuses_a_repeat_but_not_a_single_strike() {
    Scene scene;
    const NetId target = scene.enemy(glm::vec3{0.0f, 0.0f, -3.0f});
    KernelEngine::PendingLockStrike idle;
    idle.next_tick = scene.engine.current_tick() + 200u;
    idle.remaining = 1u;
    scene.engine.pending_lock_strikes_.assign(256u, idle);
    require(!scene.strike(
        repeated_strike(KernelLockStrikeMode_Instant, 5u, 10u)));
    scene.drain(scene.engine.current_tick());
    require(scene.hp(target) == 100);
    require(scene.engine.pending_lock_strikes_.size() == 256u);
    require(scene.strike(strike_action(kSphere, KernelLockStrikeMode_Instant, false)));
    scene.drain(scene.engine.current_tick());
    require(scene.hp(target) == 100 - kDamage);
}

// A static box, as a map's walls and roofs reach the authority's queries.
physics::CollisionObjectDescriptor static_box(
    std::uint32_t collider_id,
    const glm::vec3& center,
    const glm::vec3& half_extents) {
    physics::CollisionObjectDescriptor object{};
    object.identity.collider_id = collider_id;
    object.identity.kind = physics::CollisionObjectKind::kStaticObstacle;
    object.identity.layer = physics::CollisionLayer::kStaticObstacle;
    object.shape.type = physics::CollisionShapeType::kBox;
    object.shape.half_extents = half_extents;
    object.position = center;
    return object;
}

// Against real geometry (P5): a wall between where the bottle broke and the
// nearest actor hides it, and the next nearest is struck in its place; a roof
// lower than the lift stops the origin just under it.
void walls_hide_targets_and_a_roof_stops_the_lift() {
    Scene scene;
    physics::PhysicsWorld physics;
    require(physics.valid());
    std::string error;
    // A wall across +x at 2 m, a metre and a half high either side of y 0.5.
    require(physics.upsert_object(
        static_box(9001u, {2.0f, 0.75f, 0.0f}, {0.1f, 1.5f, 2.0f}), &error));
    // A roof 0.6 m over the break, small enough to stand clear of every line
    // of sight below it.
    require(physics.upsert_object(
        static_box(9002u, {0.0f, 0.7f, 0.0f}, {0.4f, 0.1f, 0.4f}), &error));
    physics.optimize_broad_phase();
    scene.world.set_collision_world(&physics);

    const NetId behind_wall = scene.enemy(glm::vec3{3.0f, 0.0f, 0.0f});
    const NetId open = scene.enemy(glm::vec3{0.0f, 0.0f, -5.0f});
    const NetId further = scene.enemy(glm::vec3{-6.0f, 0.0f, 0.0f});
    KernelActionDefinition action =
        strike_action(kSphere, KernelLockStrikeMode_Instant, false);
    action.lock_origin_lift = 1.0f;
    require(scene.strike(action));
    scene.drain(scene.engine.current_tick());
    require(scene.hp(behind_wall) == 100);
    require(scene.hp(open) == 100 - kDamage);
    require(scene.hp(further) == 100 - kDamage);

    // Lifted a metre, it would have been taken at y 1; the roof's underside
    // is at 0.6, and it stops a tenth short of it.
    const auto& records = scene.engine.pending_lock_strike_records_;
    require(records.size() == 1u);
    require(std::fabs(records[0].origin.y - 0.5f) < 0.001f);
    require(records[0].origin.x == 0.0f && records[0].origin.z == 0.0f);
    scene.world.set_collision_world(nullptr);
}

}  // namespace

int main() {
    an_instant_sphere_strikes_the_nearest_around_it();
    an_instant_cone_strikes_only_ahead();
    missiles_land_on_each_ones_arrival();
    the_lift_moves_the_origin_up();
    the_catalog_refuses_what_it_could_not_run();
    a_repeat_strikes_on_its_schedule_and_stops();
    each_repeat_locks_afresh();
    repeated_missiles_carry_their_strike();
    a_full_queue_refuses_a_repeat_but_not_a_single_strike();
    walls_hide_targets_and_a_roof_stops_the_lift();
    std::puts("lock_strike_test passed");
    return 0;
}
