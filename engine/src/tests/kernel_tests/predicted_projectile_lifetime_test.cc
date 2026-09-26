// A projectile this client simulates on its own timeline -- its own shot, or a
// deterministic one it was told to fly -- used to keep flying past the end of
// its lifetime until the authority's despawn arrived, a round trip later.
// There was a local lifetime check, but it counted age_ticks, and every
// snapshot re-bases the flight on that snapshot and sets age_ticks to the ticks
// since it. With snapshots arriving, the count never got near the lifetime.
// These pin the ending to the tick the authority ends it on.
//
// Client-only setup, as in world_timeline_endings_test: no clock sync, so the
// local prediction tick is the newest snapshot's, and each step here is one
// snapshot followed by one prediction tick.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

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
constexpr std::uint32_t kLifetimeTicks = 10;
constexpr std::uint32_t kSpawnTick = 10;
// The authority fires and simulates a projectile in the same tick, so it is one
// tick old on its spawn tick and expires on spawn + lifetime - 1. The snapshot
// of that tick no longer has it.
constexpr std::uint32_t kLastAliveTick = kSpawnTick + kLifetimeTicks - 2;
constexpr ne::NetId kRocket = 61;
constexpr ne::PeerId kShooterPeer = 3;
constexpr std::uint32_t kActionInstance = 7;
const glm::vec3 kSpawnPosition{0.0f, 1.0f, 0.0f};
const glm::vec3 kVelocity{20.0f, 0.0f, 0.0f};

struct Client {
    Client() : engine(make_config()) {
        engine.reset_runtime_state(KernelMode_Client);
        engine.local_client_peer_id_ = 2;

        ne::RuntimeProjectileTemplate rocket{};
        rocket.projectile_template_id = kRocketTemplateId;
        rocket.projectile_type = ne::ProjectileType::kStandard;
        rocket.motion_model = ne::ProjectileMotionModel::kLinear;
        rocket.sync_mode = ne::ProjectileSyncMode::kLocalPredictedDeterministic;
        rocket.speed = 20.0f;
        rocket.lifetime_ticks = kLifetimeTicks;
        engine.catalog_runtime_.projectile_templates.push_back(rocket);
    }

    static KernelConfig make_config() {
        KernelConfig config{};
        config.mode = KernelMode_Client;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        return config;
    }

    float dt() const { return engine.tick_loop_.fixed_delta_seconds(); }

    void spawn_rocket() {
        ne::ProjectileSpawnBatchPacket packet{};
        packet.server_tick = kSpawnTick;
        packet.catalog_hash = engine.catalog_hash_;
        ne::ProjectileSpawnGroup group{};
        group.projectile_template_id = kRocketTemplateId;
        ne::ProjectileSpawnRecord record{};
        record.projectile_net_id = kRocket;
        record.owner_peer = kShooterPeer;
        record.action_instance_id = kActionInstance;
        record.spawn_position = kSpawnPosition;
        record.initial_velocity = kVelocity;
        group.records.push_back(record);
        packet.groups.push_back(group);
        engine.handle_client_projectile_spawn_batch(packet);
    }

    // The snapshot of `tick`, carrying the rocket while the authority still
    // has it, then the prediction tick that follows it.
    void step(std::uint32_t tick) {
        ne::WorldSnapshot snapshot;
        snapshot.header.server_tick = tick;
        if (tick >= kSpawnTick && tick <= kLastAliveTick) {
            ne::EntitySnapshot rocket{};
            rocket.net_id = kRocket;
            rocket.type = ne::EntityType::kProjectile;
            rocket.owner_peer = kShooterPeer;
            rocket.action_instance_id = kActionInstance;
            rocket.spawn_tick = kSpawnTick;
            rocket.velocity = kVelocity;
            rocket.position = kSpawnPosition +
                kVelocity * (static_cast<float>(tick - kSpawnTick + 1u) * dt());
            snapshot.entities.push_back(rocket);
        }
        engine.handle_client_snapshot(snapshot);
        engine.advance_predicted_projectiles(dt());
    }

    const RenderEntityState* drawn(ne::NetId net_id) {
        count = engine.get_render_states_at_time(
            1000000, states.data(), static_cast<std::uint32_t>(states.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            if (states[index].net_id == net_id) {
                return &states[index];
            }
        }
        return nullptr;
    }

    ne::KernelEngine engine;
    std::array<RenderEntityState, 8> states{};
    std::uint32_t count = 0;
};

// Drawn up to the last tick the authority has it, gone on the tick it expires,
// and still gone after -- with every snapshot but the despawn delivered.
void a_projectile_ends_on_its_lifetime_without_waiting_for_the_despawn() {
    Client client;
    for (std::uint32_t tick = 1; tick < kSpawnTick; ++tick) {
        client.step(tick);
    }
    client.spawn_rocket();
    std::uint32_t tick = kSpawnTick;
    // Each step draws the tick after the snapshot it delivered.
    for (; tick + 1u <= kLastAliveTick; ++tick) {
        client.step(tick);
        const RenderEntityState* rocket = client.drawn(kRocket);
        require(rocket != nullptr);
        require(rocket->status == RenderEntityStatus_Predicted);
    }
    // Well inside the second it is kept for after ending, so the despawn below
    // still finds it.
    for (; tick <= kLastAliveTick + 10u; ++tick) {
        client.step(tick);
        require(client.drawn(kRocket) == nullptr);
    }
    // Hidden, not forgotten: the despawn still has something to bind to, so it
    // is applied at once rather than held for the world timeline.
    require(client.engine.has_predicted_projectile_net_id(kRocket));
    ne::EntityDespawnPacket despawn{};
    despawn.net_id = kRocket;
    despawn.server_tick = kLastAliveTick + 1u;
    despawn.reason = KernelDespawnReason_Destroyed;
    client.engine.handle_client_despawn(despawn);
    require(!client.engine.has_predicted_projectile_net_id(kRocket));
    require(client.engine.deferred_flight_despawns_.empty());
}

}  // namespace

int main() {
    a_projectile_ends_on_its_lifetime_without_waiting_for_the_despawn();
    std::puts("predicted_projectile_lifetime_test passed");
    return 0;
}
