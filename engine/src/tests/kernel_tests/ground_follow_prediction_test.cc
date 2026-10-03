// A ground-following area effect (a tornado) is locally predicted
// deterministic: after its spawn record nothing about it is ever sent, and each
// client steps it on its own over its own copy of the static world. These pin
// that a client's copy is the authority's, tick for tick -- both from a spawn
// record that arrives on time and from one that arrives many ticks late, which
// is what a session that only now finds the field relevant is sent.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "physics/public/physics_world.h"
#include "protocol/public/network_packets.h"
#include "simulation/public/simulation.h"
#include "world/public/components.h"
#include "world/public/world.h"

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

constexpr std::uint32_t kTornadoTemplateId = 44;
constexpr std::uint32_t kLifetimeTicks = 300;
constexpr std::uint32_t kSpawnTick = 100;
constexpr ne::NetId kTornado = 71;
constexpr ne::PeerId kShooterPeer = 3;
constexpr std::uint32_t kActionInstance = 9;
constexpr float kDt = 1.0f / 30.0f;

ne::RuntimeProjectileTemplate tornado_template() {
    ne::RuntimeProjectileTemplate tornado{};
    tornado.projectile_template_id = kTornadoTemplateId;
    tornado.projectile_type = ne::ProjectileType::kAreaEffect;
    tornado.motion_model = ne::ProjectileMotionModel::kLinear;
    tornado.sync_mode = ne::ProjectileSyncMode::kLocalPredictedDeterministic;
    tornado.speed = 6.0f;
    tornado.lifetime_ticks = kLifetimeTicks;
    tornado.area_radius = 2.0f;
    tornado.damage = 1;
    tornado.damage_interval_ticks = 30;
    tornado.collision_mask = ne::kCollisionMaskDamageable;
    tornado.area_motion_collision_mask = KERNEL_COLLISION_LAYER_TERRAIN;
    tornado.area_shape = ne::AreaEffectShape::kCylinder;
    tornado.area_half_height = 1.0f;
    tornado.area_ground_follow =
        ne::AreaEffectGroundFollow{true, 1.0f, 50.0f, 0.5f, 0.5f};
    return tornado;
}

void add_box(
    ne::physics::PhysicsWorld& physics,
    std::uint32_t collider_id,
    const glm::vec3& center,
    const glm::vec3& half_extents,
    const glm::quat& rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f}) {
    ne::physics::CollisionObjectDescriptor box;
    box.identity = ne::physics::CollisionObjectIdentity{
        0,
        collider_id,
        ne::physics::kHitZoneUnscaled,
        ne::physics::CollisionObjectKind::kTerrain,
        ne::physics::CollisionLayer::kTerrain,
    };
    box.shape.type = ne::physics::CollisionShapeType::kBox;
    box.shape.half_extents = half_extents;
    box.position = center;
    box.rotation = rotation;
    std::string error;
    require(physics.upsert_object(box, &error));
}

// A course with every kind of ground the solver tells apart: flat, a 30
// degree climb, a level top, a cliff edge, lower flat beyond it, and a wall
// that parks the field before its lifetime is out.
void build_course(ne::physics::PhysicsWorld& physics) {
    add_box(physics, 1, glm::vec3{-5.0f, -0.5f, 0.0f}, glm::vec3{8.0f, 0.5f, 5.0f});
    const float angle = glm::radians(30.0f);
    const glm::quat tilt = glm::angleAxis(angle, glm::vec3{0.0f, 0.0f, 1.0f});
    const glm::vec3 ramp_top{2.0f + 2.0f * std::cos(angle), 2.0f * std::sin(angle), 0.0f};
    add_box(physics, 2, ramp_top - tilt * glm::vec3{0.0f, 0.5f, 0.0f},
            glm::vec3{2.0f, 0.5f, 5.0f}, tilt);
    // The top, from the ramp's crest (x ~ 5.46, y = 2) to the cliff at 9.
    add_box(physics, 3, glm::vec3{7.0f, 1.0f, 0.0f}, glm::vec3{2.0f, 1.0f, 5.0f});
    add_box(physics, 4, glm::vec3{20.0f, -0.5f, 0.0f}, glm::vec3{11.0f, 0.5f, 5.0f});
    add_box(physics, 5, glm::vec3{17.5f, 3.0f, 0.0f}, glm::vec3{0.5f, 3.0f, 5.0f});
}

struct Authority {
    Authority() {
        world.set_collision_world(&physics);
        build_course(physics);
        world.set_projectile_templates({tornado_template()});
        require(ne::spawn_action_graph_projectile(
            world,
            kTornadoTemplateId,
            kShooterPeer,
            0,
            0,
            glm::vec3{0.0f, 1.5f, 0.0f},
            glm::vec3{1.0f, 0.0f, 0.0f},
            kSpawnTick,
            kDt));
        for (const entt::entity entity :
             world.registry().view<ne::ProjectileAreaEffectRuntime>()) {
            tornado = entity;
        }
        require(tornado != entt::null);
        const ne::ProjectileState& state =
            world.registry().get<ne::ProjectileState>(tornado);
        spawn_position = state.spawn_position;
        initial_velocity = state.initial_velocity;
    }

    // Where it is after each of `ticks` steps.
    std::vector<glm::vec3> run(std::uint32_t ticks) {
        std::vector<glm::vec3> trace;
        for (std::uint32_t tick = 0; tick < ticks; ++tick) {
            ne::simulate_projectiles(world, kDt);
            trace.push_back(world.registry().get<ne::Transform>(tornado).position);
        }
        return trace;
    }

    ne::World world;
    ne::physics::PhysicsWorld physics;
    entt::entity tornado = entt::null;
    glm::vec3 spawn_position{0.0f};
    glm::vec3 initial_velocity{0.0f};
};

struct Client {
    Client() : engine(make_config()) {
        engine.reset_runtime_state(KernelMode_Client);
        engine.local_client_peer_id_ = 2;
        engine.catalog_runtime_.projectile_templates.push_back(tornado_template());
        engine.prediction_physics_world_ =
            std::make_unique<ne::physics::PhysicsWorld>();
        build_course(*engine.prediction_physics_world_);
    }

    static KernelConfig make_config() {
        KernelConfig config{};
        config.mode = KernelMode_Client;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        return config;
    }

    // The client's clock reads the authority's `server_tick`.
    void set_server_tick(std::uint32_t server_tick) {
        engine.has_client_clock_sync_ = true;
        engine.client_clock_offset_us_ = 0;
        engine.client_local_time_us_ = static_cast<std::uint64_t>(
            static_cast<double>(server_tick) * kDt * 1000000.0) + 100u;
    }

    void receive_spawn(const Authority& authority) {
        ne::ProjectileSpawnBatchPacket packet{};
        packet.server_tick = kSpawnTick;
        packet.catalog_hash = engine.catalog_hash_;
        ne::ProjectileSpawnGroup group{};
        group.projectile_template_id = kTornadoTemplateId;
        ne::ProjectileSpawnRecord record{};
        record.projectile_net_id = kTornado;
        record.owner_peer = kShooterPeer;
        record.action_instance_id = kActionInstance;
        record.spawn_position = authority.spawn_position;
        record.initial_velocity = authority.initial_velocity;
        group.records.push_back(record);
        packet.groups.push_back(group);
        engine.handle_client_projectile_spawn_batch(packet);
        require(engine.predicted_projectiles_.size() == 1u);
    }

    const ne::KernelEngine::PredictedProjectile& tornado() const {
        return engine.predicted_projectiles_.front();
    }

    std::vector<glm::vec3> run(std::uint32_t ticks) {
        std::vector<glm::vec3> trace;
        for (std::uint32_t tick = 0; tick < ticks; ++tick) {
            engine.advance_predicted_projectiles(kDt);
            trace.push_back(tornado().position);
        }
        return trace;
    }

    ne::KernelEngine engine;
};

// The whole course, on time: every tick the client draws is exactly where the
// authority put the field -- bit for bit, since both run the same solver over
// identical terrain. The course climbs, crosses a cliff and parks, so a client
// that skipped any of those would part company with the authority somewhere.
void a_client_steps_a_ground_follower_exactly_as_the_authority_does() {
    Authority authority;
    // Settled from its 1.5 m launch onto the 1 m hover, and level.
    require(std::abs(authority.spawn_position.y - 1.0f) < 0.0001f);
    require(authority.initial_velocity == glm::vec3(6.0f, 0.0f, 0.0f));
    const std::vector<glm::vec3> expected = authority.run(120);

    Client client;
    client.set_server_tick(kSpawnTick);
    client.receive_spawn(authority);
    require(client.tornado().position == authority.spawn_position);
    const std::vector<glm::vec3> drawn = client.run(120);

    require(drawn == expected);
    // And the course really did all of it.
    float highest = 0.0f;
    for (const glm::vec3& position : expected) {
        highest = std::max(highest, position.y);
    }
    require(std::abs(highest - 3.0f) < 0.01f);           // rode the top at 2 + 1
    require(std::abs(expected.back().x - 17.0f) < 0.01f);  // parked on the wall
    require(std::abs(expected.back().y - 3.0f) < 0.01f);   // at the cliff's height
    require(client.tornado().initial_velocity == glm::vec3(0.0f));
    require(client.tornado().velocity == glm::vec3(0.0f));
}

// A record that arrives 40 ticks after the spawn is caught up on receipt to
// where the authority has the field now, then carries on in step with it.
void a_late_spawn_record_is_stepped_to_where_the_authority_is() {
    constexpr std::uint32_t kLate = 40;
    Authority authority;
    const std::vector<glm::vec3> expected = authority.run(kLate + 30);

    Client client;
    client.set_server_tick(kSpawnTick + kLate);
    client.receive_spawn(authority);
    require(client.tornado().position == expected[kLate - 1]);
    require(client.tornado().lifetime_elapsed_ticks == kLate);
    const std::vector<glm::vec3> drawn = client.run(30);
    require(drawn ==
            std::vector<glm::vec3>(expected.begin() + kLate, expected.end()));
}

// A ground follower's lifetime is kept locally, as a standard projectile's is:
// nothing else would end it on a client that has left its relevance.
void a_ground_follower_ends_on_its_lifetime() {
    Authority authority;
    Client client;
    client.set_server_tick(kSpawnTick);
    client.receive_spawn(authority);
    require(client.tornado().ends_on_lifetime);
    client.run(kLifetimeTicks - 1);
    require(!client.tornado().locally_terminated);
    client.run(1);
    require(client.tornado().locally_terminated);
}

}  // namespace

int main() {
    a_client_steps_a_ground_follower_exactly_as_the_authority_does();
    a_late_spawn_record_is_stepped_to_where_the_authority_is();
    a_ground_follower_ends_on_its_lifetime();
    return 0;
}
