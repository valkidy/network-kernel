// A shot this client predicts used to fly straight through the actor it hit,
// because the local collision query only asked for terrain and static
// obstacles; it disappeared when the authority's despawn came back, a round
// trip later and metres past the body. Other actors' hit volumes are now put
// in the prediction world where the render pass draws them, and a shot that
// goes into one is hidden there. It is only hidden: it keeps flying, and if no
// despawn comes within a round trip and a margin it is shown again.
//
// Client-only setup, as in predicted_projectile_lifetime_test. Each step draws
// the actors (sync_client_render_colliders) and then runs one prediction tick.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "physics/public/physics_world.h"
#include "protocol/public/network_packets.h"
#include "world/public/components.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace ne = network_example;

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::uint32_t kRocketTemplateId = 31;
constexpr std::uint32_t kRocketColliderId = 90;
constexpr std::uint32_t kActorHitColliderId = 22;
constexpr std::uint32_t kCrateColliderId = 23;
constexpr std::uint32_t kLifetimeTicks = 90;
constexpr std::uint32_t kSpawnTick = 10;
constexpr ne::NetId kRocket = 61;
constexpr ne::NetId kLocalPlayer = 5;
constexpr ne::PeerId kLocalPeer = 2;
constexpr ne::PeerId kOtherPeer = 3;
constexpr std::uint32_t kActionInstance = 7;
const glm::vec3 kSpawnPosition{0.0f, 1.0f, 0.0f};
// 0.67 m a tick: several ticks to reach each actor, and one tick never steps
// over a 0.8 m wide hit box.
const glm::vec3 kVelocity{20.0f, 0.0f, 0.0f};

struct Actor {
    ne::NetId net_id = 0;
    float x = 0.0f;
    bool dead = false;
    // A placed prop instead: a static obstacle to the prediction.
    bool crate = false;
};

struct Client {
    Client() : engine(make_config()) {
        engine.reset_runtime_state(KernelMode_Client);
        engine.local_client_peer_id_ = kLocalPeer;
        engine.local_player_net_id_ = kLocalPlayer;
        engine.prediction_physics_world_ =
            std::make_unique<ne::physics::PhysicsWorld>(
                ne::physics::PhysicsWorldConfig{});
        require(engine.prediction_physics_world_->valid());

        ne::RuntimeProjectileTemplate runtime{};
        runtime.projectile_template_id = kRocketTemplateId;
        runtime.projectile_type = ne::ProjectileType::kStandard;
        runtime.motion_model = ne::ProjectileMotionModel::kLinear;
        runtime.sync_mode = ne::ProjectileSyncMode::kLocalPredictedDeterministic;
        runtime.speed = 20.0f;
        runtime.lifetime_ticks = kLifetimeTicks;
        runtime.collider_template_id = kRocketColliderId;
        engine.catalog_runtime_.projectile_templates.push_back(runtime);

        // What the collision prediction reads: the catalog's own definitions.
        KernelProjectileTemplateDefinition rocket{};
        rocket.struct_size = sizeof(rocket);
        rocket.projectile_template_id = kRocketTemplateId;
        rocket.mechanics.struct_size = sizeof(rocket.mechanics);
        rocket.mechanics.projectile_type = KernelProjectileType_Standard;
        rocket.mechanics.hit_response = KernelProjectileHitResponse_Destroy;
        rocket.mechanics.sync_mode = KernelProjectileSyncMode_LocalPredictedDeterministic;
        rocket.mechanics.speed = 20.0f;
        rocket.mechanics.lifetime_ticks = kLifetimeTicks;
        rocket.mechanics.collider_template_id = kRocketColliderId;
        // As grenade_shell authors it.
        rocket.mechanics.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE |
            KERNEL_COLLISION_LAYER_TERRAIN | KERNEL_COLLISION_LAYER_STATIC_OBSTACLE;
        engine.projectile_templates_.push_back(rocket);

        KernelColliderTemplateDefinition rocket_collider{};
        rocket_collider.struct_size = sizeof(rocket_collider);
        rocket_collider.template_id = kRocketColliderId;
        rocket_collider.shape_type = KernelColliderShapeType_Sphere;
        rocket_collider.shape_params = KernelVec4{0.1f, 0.0f, 0.0f, 0.0f};
        rocket_collider.purpose_flags = KernelColliderPurpose_Hit;
        engine.collider_templates_.push_back(rocket_collider);

        // gingerbread_hit_aabb.
        KernelColliderTemplateDefinition actor_collider{};
        actor_collider.struct_size = sizeof(actor_collider);
        actor_collider.template_id = kActorHitColliderId;
        actor_collider.shape_type = KernelColliderShapeType_Aabb;
        actor_collider.center = KernelVec3{0.0f, 0.8f, 0.0f};
        actor_collider.shape_params = KernelVec4{0.4f, 0.8f, 0.4f, 0.0f};
        actor_collider.purpose_flags = KernelColliderPurpose_Hit;
        actor_collider.layer_mask = KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
        engine.collider_templates_.push_back(actor_collider);

        // A thin wall of a prop, 0.2 m deep.
        KernelColliderTemplateDefinition crate_collider{};
        crate_collider.struct_size = sizeof(crate_collider);
        crate_collider.template_id = kCrateColliderId;
        crate_collider.shape_type = KernelColliderShapeType_Aabb;
        crate_collider.center = KernelVec3{0.0f, 1.0f, 0.0f};
        crate_collider.shape_params = KernelVec4{0.1f, 1.0f, 1.0f, 0.0f};
        crate_collider.purpose_flags = KernelColliderPurpose_Hit;
        engine.collider_templates_.push_back(crate_collider);
    }

    static KernelConfig make_config() {
        KernelConfig config{};
        config.mode = KernelMode_Client;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        return config;
    }

    float dt() const { return engine.tick_loop_.fixed_delta_seconds(); }

    void spawn_rocket(ne::PeerId owner_peer) {
        ne::ProjectileSpawnBatchPacket packet{};
        packet.server_tick = kSpawnTick;
        packet.catalog_hash = engine.catalog_hash_;
        ne::ProjectileSpawnGroup group{};
        group.projectile_template_id = kRocketTemplateId;
        ne::ProjectileSpawnRecord record{};
        record.projectile_net_id = kRocket;
        record.owner_peer = owner_peer;
        record.action_instance_id = kActionInstance;
        record.spawn_position = kSpawnPosition;
        record.initial_velocity = kVelocity;
        group.records.push_back(record);
        packet.groups.push_back(group);
        engine.handle_client_projectile_spawn_batch(packet);
        require(exists());
    }

    // Bound or not: no snapshot is delivered here, so it never binds.
    bool exists() const {
        for (const auto& projectile : engine.predicted_projectiles_) {
            if (projectile.net_id == kRocket) {
                return true;
            }
        }
        return false;
    }

    // One render pass drawing these actors standing on the x axis, then one
    // prediction tick.
    void step(const std::vector<Actor>& actors) {
        engine.render_states_.clear();
        for (const Actor& actor : actors) {
            RenderEntityState state{};
            state.net_id = actor.net_id;
            state.entity_type = static_cast<std::uint16_t>(
                actor.crate ? ne::EntityType::kProp : ne::EntityType::kActor);
            state.actor_type = static_cast<std::uint16_t>(
                actor.crate ? ne::ActorType::kUnknown : ne::ActorType::kAgent);
            state.position = KernelVec3{actor.x, 0.0f, 0.0f};
            state.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
            state.collider_template_id =
                actor.crate ? kCrateColliderId : kActorHitColliderId;
            state.visual_flags = actor.dead ? ne::kVisualFlagDead : 0u;
            engine.render_states_.push_back(state);
        }
        engine.sync_client_render_colliders();
        engine.advance_predicted_projectiles(dt());
    }

    bool drawn() {
        engine.render_states_.clear();
        engine.append_predicted_projectile_render_states();
        for (const RenderEntityState& state : engine.render_states_) {
            if (state.net_id == kRocket) {
                return true;
            }
        }
        return false;
    }

    const ne::KernelEngine::PredictedProjectile& rocket() const {
        for (const auto& projectile : engine.predicted_projectiles_) {
            if (projectile.net_id == kRocket) {
                return projectile;
            }
        }
        require(false);
        return engine.predicted_projectiles_.front();
    }

    float rocket_x() const {
        for (const auto& projectile : engine.predicted_projectiles_) {
            if (projectile.net_id == kRocket) {
                return projectile.position.x;
            }
        }
        require(false);
        return 0.0f;
    }

    ne::KernelEngine engine;
};

// Flies until the step that carries it into the box's near face at x = 4.6,
// and is not drawn from that step on. The despawn then finds it.
void own_shot_is_hidden_where_it_enters_the_actor() {
    Client client;
    client.spawn_rocket(kLocalPeer);
    const std::vector<Actor> actors{{40, 5.0f}};
    bool hidden = false;
    for (int step = 0; step < 20 && !hidden; ++step) {
        const float before = client.rocket_x();
        client.step(actors);
        hidden = !client.drawn();
        if (hidden) {
            // The step that crossed the face, not one before or after it.
            require(before < 4.6f);
            require(client.rocket_x() >= 4.6f - 0.1f);
        }
    }
    require(hidden);
    // Still flown underneath, and still there for the despawn to take.
    const float at_hit = client.rocket_x();
    client.step(actors);
    require(!client.drawn());
    require(client.rocket_x() > at_hit);
    ne::EntityDespawnPacket despawn{};
    despawn.net_id = kRocket;
    despawn.server_tick = kSpawnTick + 8u;
    despawn.reason = KernelDespawnReason_Destroyed;
    client.engine.handle_client_despawn(despawn);
    require(!client.exists());
}

// The authority missed: nothing comes back. Shown again once a round trip and
// the margin are up -- with no measured round trip, 100 ms + 150 ms, 8 ticks --
// where it has flown to, and it then passes through a second actor in view.
void unconfirmed_hit_is_shown_again_and_not_hidden_twice() {
    Client client;
    client.spawn_rocket(kLocalPeer);
    const std::vector<Actor> actors{{40, 5.0f}, {41, 14.0f}};
    int hidden_steps = 0;
    int step = 0;
    for (; step < 20; ++step) {
        client.step(actors);
        if (!client.drawn()) {
            ++hidden_steps;
        } else if (hidden_steps > 0) {
            break;
        }
    }
    require(hidden_steps == 8);
    require(client.rocket_x() > 5.4f);
    // Past the second actor, drawn every step.
    while (client.rocket_x() < 16.0f) {
        client.step(actors);
        require(client.drawn());
    }
}

// Only our own shot: another peer's deterministic projectile is flown here but
// was not aimed at this client's picture.
void another_peers_shot_is_not_hidden() {
    Client client;
    client.spawn_rocket(kOtherPeer);
    const std::vector<Actor> actors{{40, 5.0f}};
    while (client.rocket_x() < 7.0f) {
        client.step(actors);
        require(client.drawn());
    }
}

// Neither a dead actor, whose volume the authority disables, nor the local
// player, whom the authority never lets its own shot hit.
void dead_actors_and_the_local_player_are_not_hit() {
    Client client;
    client.spawn_rocket(kLocalPeer);
    std::vector<Actor> actors{{40, 3.0f, true}, {kLocalPlayer, 6.0f}};
    while (client.rocket_x() < 8.0f) {
        client.step(actors);
        require(client.drawn());
    }
}

// A wall in front of the actor, both met in one step (4.0 -> 4.67 m crosses
// the wall's face at 4.2 and the actor's at 4.6): the wall stops it, which is
// an ending, not an actor hit waiting to be confirmed.
void a_wall_in_front_of_the_actor_ends_it() {
    Client client;
    client.spawn_rocket(kLocalPeer);
    const std::vector<Actor> actors{{40, 5.0f}, {70, 4.3f, false, true}};
    while (!client.rocket().locally_terminated) {
        require(client.rocket_x() < 4.5f);
        client.step(actors);
    }
    require(!client.rocket().hidden_by_actor_hit);
    require(client.rocket_x() < 4.3f);
}

// The actor volume is a hit volume, not a wall: the local player's movement
// never meets it.
void actor_hit_volume_is_not_movement_geometry() {
    Client client;
    client.step({{40, 5.0f}});
    require(client.engine.prediction_obstacle_collider_ids_.contains(40));
    ne::physics::ShapeCastRequest request{};
    request.shape.type = ne::physics::CollisionShapeType::kCapsule;
    request.shape.radius = 0.3f;
    request.shape.capsule_half_height = 0.6f;
    request.start = glm::vec3{0.0f, 0.9f, 0.0f};
    request.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    request.displacement = glm::vec3{10.0f, 0.0f, 0.0f};
    request.filter.collision_mask = ne::physics::kMovementCollisionMask;
    require(client.engine.prediction_physics_world_->shape_cast_all(request).empty());
    // While a query that asks for damageable volumes does meet it: the cast
    // above found nothing because of its filter, not because nothing is there.
    request.filter.collision_mask = ne::physics::collision_layer_bit(
        ne::physics::CollisionLayer::kDamageable);
    request.filter.gameplay_category_mask = KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
    require(!client.engine.prediction_physics_world_->shape_cast_all(request).empty());
    // The actor leaving the picture takes its volume out.
    client.step({});
    require(!client.engine.prediction_obstacle_collider_ids_.contains(40));
}

}  // namespace

int main() {
    own_shot_is_hidden_where_it_enters_the_actor();
    unconfirmed_hit_is_shown_again_and_not_hidden_twice();
    another_peers_shot_is_not_hidden();
    dead_actors_and_the_local_player_are_not_hit();
    a_wall_in_front_of_the_actor_ends_it();
    actor_hit_volume_is_not_movement_geometry();
    std::puts("predicted_projectile_actor_hit_test passed");
    return 0;
}
