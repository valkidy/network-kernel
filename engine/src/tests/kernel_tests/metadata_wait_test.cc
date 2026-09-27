// A snapshot can name an entity before its spawn has arrived: the snapshot is
// unreliable and sent every interval, the spawn is reliable and, when its packet
// is lost, comes back a round trip or two later. With actors blocking each other
// the client used to end its whole prediction session over that -- once any
// entity had waited two ticks, and at once when an actor without its spawn was
// in the snapshot the prediction step read. Played with 100 ms of lag and 1%
// loss, that happened within a minute.
//
// Waiting is now just waiting. Only an actor whose spawn never comes -- a
// protocol fault, not a network one -- still fails the prediction.

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

#include "physics/public/physics_world.h"
#include "sync/public/snapshot.h"

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

constexpr ne::NetId kProjectile = 50;
constexpr ne::NetId kAgent = 60;

struct Client {
    Client() : engine(make_config()) {
        engine.reset_runtime_state(KernelMode_Client);
        engine.session_rules_.actor_blocking_mode = KernelActorBlockingMode_Predicted;
    }

    static KernelConfig make_config() {
        KernelConfig config{};
        config.mode = KernelMode_Client;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        return config;
    }

    // A snapshot naming whatever is asked for, and nothing about any of it
    // having been spawned.
    void snapshot(std::uint32_t tick, bool projectile, bool agent) {
        ne::WorldSnapshot snapshot;
        snapshot.header.server_tick = tick;
        if (projectile) {
            ne::EntitySnapshot entity;
            entity.net_id = kProjectile;
            entity.type = ne::EntityType::kProjectile;
            snapshot.entities.push_back(entity);
        }
        if (agent) {
            ne::EntitySnapshot entity;
            entity.net_id = kAgent;
            entity.type = ne::EntityType::kActor;
            entity.actor_type = ne::ActorType::kAgent;
            snapshot.entities.push_back(entity);
        }
        engine.handle_client_snapshot(snapshot);
    }

    ne::KernelEngine engine;
};

// Waiting well past the old two ticks -- the round trip a lost spawn takes --
// is not a failure, for a projectile or an actor.
void a_late_spawn_is_waited_for() {
    Client client;
    for (std::uint32_t tick = 2; tick <= 40; tick += 2) {
        client.snapshot(tick, true, true);
    }
    require(!client.engine.prediction_failed_);
    // It was noticed, though: both are waiting.
    require(client.engine.client_metadata_timeout_reported_entities_.contains(kProjectile));
    require(client.engine.client_metadata_timeout_reported_entities_.contains(kAgent));
}

// An actor whose spawn never comes is a fault, and still fails the
// prediction; a projectile that never gets one does not, as nothing the
// prediction steps depends on it.
void only_an_actor_that_never_arrives_fails() {
    Client projectile_only;
    for (std::uint32_t tick = 2; tick <= 400; tick += 2) {
        projectile_only.snapshot(tick, true, false);
    }
    require(!projectile_only.engine.prediction_failed_);

    Client with_agent;
    std::uint32_t tick = 2;
    for (; tick <= 150; tick += 2) {
        with_agent.snapshot(tick, false, true);
    }
    require(!with_agent.engine.prediction_failed_);
    for (; tick <= 170; tick += 2) {
        with_agent.snapshot(tick, false, true);
    }
    require(with_agent.engine.prediction_failed_);
}

// The prediction step builds a movement proxy for every actor in the newest
// snapshot. One whose spawn has not arrived gets none yet, instead of failing
// the step; one whose spawn names a template this client does not have is a
// catalog fault and still does.
void an_actor_without_its_spawn_gets_no_proxy_yet() {
    Client client;
    client.engine.prediction_physics_world_ =
        std::make_unique<ne::physics::PhysicsWorld>(ne::physics::PhysicsWorldConfig{});
    client.snapshot(2, false, true);
    require(client.engine.sync_prediction_actor_proxies(
        client.engine.latest_client_snapshot_, 3));
    require(client.engine.prediction_proxy_collider_ids_.empty());

    ne::KernelEngine::ClientReplicatedEntity replicated{};
    replicated.net_id = kAgent;
    replicated.type = ne::EntityType::kActor;
    replicated.actor_type = ne::ActorType::kAgent;
    replicated.actor_template_id = 999;
    client.engine.client_replicated_entities_.push_back(replicated);
    require(!client.engine.sync_prediction_actor_proxies(
        client.engine.latest_client_snapshot_, 3));
}

}  // namespace

int main() {
    a_late_spawn_is_waited_for();
    only_an_actor_that_never_arrives_fails();
    an_actor_without_its_spawn_gets_no_proxy_yet();
    std::printf("metadata_wait_test: PASS\n");
    return 0;
}
