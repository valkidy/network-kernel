// What a client draws through an impact once the wire has latency in it.
//
// impact_prediction_bench isolates the mechanism with no server at all: a
// projectile that is never told to stop runs to its lifetime. What bounds it in
// a session is the authoritative despawn, and that arrives a round trip after
// the client's own prediction reached the wall -- so the overshoot a player sees
// is latency, not lifetime, and on a loopback it is nearly nothing.
//
// This runs the real path for that: a dedicated server and a client engine over
// loopback, with every packet held for a fixed number of ticks in each
// direction. The server spawns the rocket from its own template, the projectile
// spawn batch reaches the client, the client builds a PredictedProjectile from
// it the way a remote deterministic projectile is meant to be presented, and the
// server's impact despawn comes back over the same delayed link.
//
// The reading is the furthest the client ever drew the projectile past the
// wall's near face. With the collision gate admitting hybrid the client stops
// itself and the despawn is irrelevant; without it, the overshoot is whatever
// the round trip was worth at 35 m/s.
//
// Reported as distances, deliberately not asserted -- it measures a policy
// against a link, and the link is the variable. Run it with
//   bazel run -c opt //engine/src/tests/kernel_tests:impact_latency_bench

// Standard library first: kernel.h below is included with `private` redefined,
// and re-parsing a libc++ header in that state is a hard error.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "physics/public/collision_types.h"
#include "physics/public/physics_world.h"
#include "protocol/public/network_packets.h"
#include "simulation/public/simulation.h"
#include "transport/public/loopback_transport.h"
#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr std::uint32_t kTickRate = 30;
constexpr float kTickSeconds = 1.0f / static_cast<float>(kTickRate);
// rocket_projectile's own mechanics.
constexpr std::uint32_t kRocketTemplateId = 3;
constexpr float kSpeedMetersPerSecond = 35.0f;
constexpr std::uint32_t kLifetimeTicks = 75;
constexpr float kMetersPerTick = kSpeedMetersPerSecond * kTickSeconds;
// Far enough that the flight is several snapshots long, near enough that it is
// well inside the relevance radius the whole way.
constexpr float kWallDistanceMeters = 20.0f;
constexpr float kWallHalfExtentMeters = 0.1f;
constexpr float kWallNearFaceMeters = kWallDistanceMeters - kWallHalfExtentMeters;
constexpr std::uint32_t kRunTicks = 120;

KernelColliderTemplateDefinition rocket_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 10;
    collider.shape_type = KernelColliderShapeType_Aabb;
    collider.shape_params = KernelVec4{0.3f, 0.3f, 0.3f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Hit;
    return collider;
}

KernelProjectileTemplateDefinition rocket_template() {
    KernelProjectileTemplateDefinition definition{};
    definition.struct_size = sizeof(definition);
    definition.projectile_template_id = kRocketTemplateId;
    definition.weapon_id = 3;
    definition.mechanics.struct_size = sizeof(KernelProjectileMechanicsDefinition);
    definition.mechanics.projectile_type = KernelProjectileType_Standard;
    definition.mechanics.motion_model = KernelProjectileMotionModel_Linear;
    definition.mechanics.sync_mode =
        KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
    definition.mechanics.hit_response = KernelProjectileHitResponse_Destroy;
    definition.mechanics.damage_shape = KernelProjectileDamageShape_DirectHit;
    definition.mechanics.damage = 45;
    definition.mechanics.speed = kSpeedMetersPerSecond;
    definition.mechanics.lifetime_ticks = kLifetimeTicks;
    definition.mechanics.collider_template_id = 10;
    definition.mechanics.collision_mask =
        KERNEL_COLLISION_MASK_STATIC_WORLD;
    definition.mechanics.max_hit_count = 1;
    return definition;
}

void load_catalog(network_example::KernelEngine* engine) {
    KernelProjectileTemplateDefinition definition = rocket_template();
    KernelColliderTemplateDefinition collider = rocket_collider_template();
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.catalog_hash = 0xc0ffeeull;
    catalog.projectile_templates = &definition;
    catalog.projectile_template_count = 1;
    catalog.collider_templates = &collider;
    catalog.collider_template_count = 1;
    require(engine->load_gameplay_catalog(catalog));
}

// The wall, in whichever physics world is asked for it. The server's own
// collision world stops the authoritative rocket; the client's prediction world
// is what lets it stop its own copy.
void install_wall(network_example::physics::PhysicsWorld* world) {
    network_example::physics::CollisionObjectDescriptor object{};
    object.identity.collider_id = 900;
    object.identity.kind = network_example::physics::CollisionObjectKind::kTerrain;
    object.identity.layer = network_example::physics::CollisionLayer::kTerrain;
    object.shape.type = network_example::physics::CollisionShapeType::kBox;
    object.shape.half_extents = glm::vec3{kWallHalfExtentMeters, 5.0f, 5.0f};
    object.position = glm::vec3{kWallDistanceMeters, 0.0f, 0.0f};
    std::string error;
    require(world->upsert_object(object, &error));
}

network_example::LoopbackTransport* attach_loopback(
    network_example::KernelEngine* engine,
    KernelMode mode,
    std::uint16_t port) {
    auto transport = std::make_unique<network_example::LoopbackTransport>();
    network_example::LoopbackTransport* loopback = transport.get();
    engine->transport_ = std::move(transport);
    engine->reset_runtime_state(mode);
    require(loopback->StartServer(port));
    return loopback;
}

// One direction of the link. Packets are taken off the sender's queue as they
// are produced and handed over `delay_ticks` later, which is what makes the
// despawn arrive a round trip after the client's prediction reached the wall.
class DelayedLink {
public:
    DelayedLink(
        network_example::LoopbackTransport* from,
        network_example::LoopbackTransport* to,
        std::uint32_t delay_ticks)
        : from_(from), to_(to), delay_ticks_(delay_ticks) {}

    void pump(std::uint32_t tick) {
        network_example::TransportEvent event;
        while (from_->PollClientEvent(event)) {
            queue_.push_back(Pending{tick + delay_ticks_, event});
        }
        while (!queue_.empty() && queue_.front().delivery_tick <= tick) {
            const network_example::TransportEvent& due = queue_.front().event;
            require(to_->SendClient(
                due.peer,
                due.payload.data(),
                static_cast<std::uint32_t>(due.payload.size()),
                due.mode,
                due.channel));
            queue_.pop_front();
        }
    }

private:
    struct Pending {
        std::uint32_t delivery_tick = 0;
        network_example::TransportEvent event;
    };
    network_example::LoopbackTransport* from_ = nullptr;
    network_example::LoopbackTransport* to_ = nullptr;
    std::uint32_t delay_ticks_ = 0;
    std::deque<Pending> queue_;
};

struct Row {
    std::uint32_t one_way_ticks = 0;
    bool client_stopped_itself = false;
    float max_drawn_x = 0.0f;
    float overshoot_m = 0.0f;
    std::uint32_t ticks_past_wall = 0;
    std::uint32_t server_impact_tick = 0;
    std::uint32_t client_removed_tick = 0;
};

Row measure(std::uint32_t one_way_ticks, std::uint16_t port) {
    KernelConfig server_config{};
    server_config.mode = KernelMode_DedicatedServer;
    server_config.tick.server_tick_rate = kTickRate;
    server_config.tick.snapshot_rate = 15;
    server_config.max_events = 1024;
    server_config.max_render_states = 256;
    network_example::KernelEngine server(server_config);
    network_example::LoopbackTransport* server_link =
        attach_loopback(&server, KernelMode_DedicatedServer, port);
    load_catalog(&server);
    // A dedicated server builds its collision world from the static scene
    // artifact it is given; this bench has no baked scene, so the world is
    // handed over directly and the wall put in it by hand. Everything past that
    // point -- the sweep that stops the rocket, the destroy, the despawn -- is
    // the engine's own.
    auto server_collision =
        std::make_unique<network_example::physics::PhysicsWorld>(
            network_example::physics::PhysicsWorldConfig{});
    require(server_collision->valid());
    install_wall(server_collision.get());
    server.world_.set_collision_world(server_collision.get());

    KernelConfig client_config = server_config;
    client_config.mode = KernelMode_Client;
    network_example::KernelEngine client(client_config);
    network_example::LoopbackTransport* client_link =
        attach_loopback(&client, KernelMode_Client, static_cast<std::uint16_t>(port + 1u));
    load_catalog(&client);
    client.prediction_physics_world_ =
        std::make_unique<network_example::physics::PhysicsWorld>(
            network_example::physics::PhysicsWorldConfig{});
    require(client.prediction_physics_world_->valid());
    install_wall(client.prediction_physics_world_.get());

    DelayedLink to_client(server_link, client_link, one_way_ticks);
    DelayedLink to_server(client_link, server_link, one_way_ticks);

    const network_example::NetId player =
        server.world_.spawn_player(1, glm::vec3{-2.0f, 0.0f, 0.0f});
    server.peer_sessions_.push_back(
        network_example::KernelEngine::PeerSession{1, player, 0, true, {}});
    client.local_client_peer_id_ = 1;
    client.local_player_net_id_ = player;

    Row row;
    row.one_way_ticks = one_way_ticks;
    network_example::NetId rocket = 0;
    bool rocket_seen_on_client = false;

    for (std::uint32_t tick = 1; tick <= kRunTicks; ++tick) {
        if (tick == 5) {
            // Fired from the server's own template, so everything downstream --
            // the spawn batch, the client's prediction, the impact despawn --
            // is the shipping path rather than a stand-in for it.
            require(network_example::spawn_action_graph_projectile(
                server.world_,
                kRocketTemplateId,
                1,
                player,
                1234u,
                glm::vec3{0.0f, 0.0f, 0.0f},
                glm::vec3{1.0f, 0.0f, 0.0f},
                server.tick_loop_.current_tick(),
                kTickSeconds));
            for (const network_example::NetId net_id : server.world_.net_ids()) {
                if (net_id != player) {
                    rocket = net_id;
                }
            }
            require(rocket != 0);
        }

        server.update(kTickSeconds);
        to_client.pump(tick);
        client.update(kTickSeconds);
        to_server.pump(tick);

        if (rocket != 0 && row.server_impact_tick == 0 &&
            !server.world_.find_entity(rocket).has_value()) {
            row.server_impact_tick = tick;
        }

        bool present_on_client = false;
        for (const network_example::KernelEngine::PredictedProjectile& predicted :
             client.predicted_projectiles_) {
            if (predicted.net_id != rocket || predicted.locally_terminated) {
                continue;
            }
            present_on_client = true;
            rocket_seen_on_client = true;
            row.max_drawn_x = std::max(row.max_drawn_x, predicted.position.x);
            if (predicted.position.x > kWallNearFaceMeters) {
                ++row.ticks_past_wall;
            }
        }
        for (const network_example::KernelEngine::PredictedProjectile& predicted :
             client.predicted_projectiles_) {
            if (predicted.net_id == rocket && predicted.locally_terminated) {
                row.client_stopped_itself = true;
            }
        }
        if (rocket_seen_on_client && !present_on_client &&
            row.client_removed_tick == 0) {
            row.client_removed_tick = tick;
        }
    }

    row.overshoot_m = std::max(0.0f, row.max_drawn_x - kWallNearFaceMeters);
    return row;
}

}  // namespace

int main() {
    std::printf(
        "IMPACT OVERSHOOT AGAINST LINK LATENCY "
        "(rocket %.0f m/s, wall near face %.1f m, %.2f m per tick)\n",
        kSpeedMetersPerSecond,
        kWallNearFaceMeters,
        kMetersPerTick);
    std::printf(
        "%9s %9s %14s %11s %13s %13s %13s\n",
        "one-way",
        "rtt ms",
        "client stops",
        "max x m",
        "overshoot m",
        "ticks past",
        "removed tick");
    std::uint16_t port = 7830;
    for (const std::uint32_t one_way_ticks : {0u, 1u, 2u, 3u}) {
        const Row row = measure(one_way_ticks, port);
        port = static_cast<std::uint16_t>(port + 2u);
        std::printf(
            "%7u tk %9.0f %14s %11.2f %13.2f %13u %13u\n",
            row.one_way_ticks,
            static_cast<double>(row.one_way_ticks) * 2.0 * 1000.0 / kTickRate,
            row.client_stopped_itself ? "yes" : "no",
            row.max_drawn_x,
            row.overshoot_m,
            row.ticks_past_wall,
            row.client_removed_tick);
    }
    std::printf("\n");
    return 0;
}
