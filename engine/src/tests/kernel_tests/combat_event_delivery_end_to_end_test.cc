// Combat events from the server's world to three clients: the one who fired,
// the one who was hit, and one too far away to see either.
//
// A combat event is a presentation cue, and it belongs on the timeline of what
// its receiver is looking at. The one who was hit sees the shot on the world
// timeline, an interpolation delay behind the server, so the fire, the hit and
// the damage are held until the world is drawn at their tick -- or the damage
// lands before the swing that dealt it. The one who fired is looking at their
// own action, predicted now, so theirs come the moment they arrive: a hit
// marker a delay late is a hit that feels missed. And a client too far away to
// see any of it is sent none of it.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "simulation/public/simulation.h"
#include "transport/public/loopback_transport.h"
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

constexpr ne::PeerId kShooterPeer = 1;
constexpr ne::PeerId kVictimPeer = 2;
constexpr ne::PeerId kFarPeer = 3;
constexpr std::uint8_t kWeaponId = 1;

ne::LoopbackTransport* attach_loopback(
    ne::KernelEngine* engine,
    KernelMode mode,
    std::uint16_t port) {
    auto transport = std::make_unique<ne::LoopbackTransport>();
    ne::LoopbackTransport* loopback = transport.get();
    engine->transport_ = std::move(transport);
    // After the reset, which stops whatever transport it is holding.
    engine->reset_runtime_state(mode);
    require(loopback->StartServer(port));
    return loopback;
}

struct Client {
    ne::PeerId peer = 0;
    ne::KernelEngine* engine = nullptr;
    ne::LoopbackTransport* link = nullptr;
    // Each combat event this client has been handed, and the world timeline
    // instant it was drawn at when it was.
    struct Seen {
        KernelEvent event;
        std::uint64_t render_server_time_us = 0;
    };
    std::vector<Seen> seen;

    const Seen* find(KernelEventType type) const {
        for (const Seen& candidate : seen) {
            if (candidate.event.type == type) return &candidate;
        }
        return nullptr;
    }
};

bool is_combat(KernelEventType type) {
    return type == KernelEventType_FireConfirmed ||
        type == KernelEventType_HitConfirmed ||
        type == KernelEventType_DamageApplied;
}

}  // namespace

int main() {
    KernelConfig server_config{};
    server_config.mode = KernelMode_DedicatedServer;
    server_config.tick.server_tick_rate = 30;
    server_config.tick.snapshot_rate = 15;
    server_config.max_events = 1024;
    server_config.max_render_states = 256;
    ne::KernelEngine server(server_config);
    ne::LoopbackTransport* server_link =
        attach_loopback(&server, KernelMode_DedicatedServer, 7801);

    KernelConfig client_config = server_config;
    client_config.mode = KernelMode_Client;
    ne::KernelEngine shooter_engine(client_config);
    ne::KernelEngine victim_engine(client_config);
    ne::KernelEngine far_engine(client_config);
    std::array<Client, 3> clients{
        Client{kShooterPeer, &shooter_engine,
               attach_loopback(&shooter_engine, KernelMode_Client, 7802)},
        Client{kVictimPeer, &victim_engine,
               attach_loopback(&victim_engine, KernelMode_Client, 7803)},
        Client{kFarPeer, &far_engine,
               attach_loopback(&far_engine, KernelMode_Client, 7804)},
    };
    Client& shooter = clients[0];
    Client& victim = clients[1];
    Client& far = clients[2];

    const ne::NetId shooter_player =
        server.world_.spawn_player(kShooterPeer, glm::vec3{0.0f});
    const ne::NetId victim_player =
        server.world_.spawn_player(kVictimPeer, glm::vec3{5.0f, 0.0f, 0.0f});
    // Well past the relevance radius of both.
    const ne::NetId far_player =
        server.world_.spawn_player(kFarPeer, glm::vec3{0.0f, 0.0f, 90.0f});
    server.peer_sessions_.push_back(
        ne::KernelEngine::PeerSession{kShooterPeer, shooter_player, 0, true, {}});
    server.peer_sessions_.push_back(
        ne::KernelEngine::PeerSession{kVictimPeer, victim_player, 0, true, {}});
    server.peer_sessions_.push_back(
        ne::KernelEngine::PeerSession{kFarPeer, far_player, 0, true, {}});
    shooter_engine.local_player_net_id_ = shooter_player;
    shooter_engine.local_client_peer_id_ = kShooterPeer;
    victim_engine.local_player_net_id_ = victim_player;
    victim_engine.local_client_peer_id_ = kVictimPeer;
    far_engine.local_player_net_id_ = far_player;
    far_engine.local_client_peer_id_ = kFarPeer;

    const std::uint64_t tick_us = static_cast<std::uint64_t>(
        server.tick_loop_.fixed_delta_seconds() * 1000000.0f);
    std::array<RenderEntityState, 16> states{};
    std::array<KernelEvent, 64> polled{};

    // Server output to each client, then each client drawn at its own clock --
    // one tick per tick, as a host draws it -- and its events collected.
    const auto deliver = [&]() {
        ne::TransportEvent event;
        while (server_link->PollClientEvent(event)) {
            for (Client& client : clients) {
                if (client.peer != event.peer) continue;
                require(client.link->SendClient(
                    event.peer,
                    event.payload.data(),
                    static_cast<std::uint32_t>(event.payload.size()),
                    event.mode,
                    event.channel));
            }
        }
        for (Client& client : clients) {
            client.engine->poll_transport();
            client.engine->get_render_states_at_time(
                client.engine->client_local_time_us_,
                states.data(),
                static_cast<std::uint32_t>(states.size()));
            const std::uint32_t count = client.engine->poll_events(
                polled.data(), static_cast<std::uint32_t>(polled.size()));
            for (std::uint32_t index = 0; index < count; ++index) {
                if (!is_combat(polled[index].type)) continue;
                client.seen.push_back(Client::Seen{
                    polled[index], client.engine->render_server_time_us_});
            }
        }
    };
    const auto step = [&]() {
        for (Client& client : clients) {
            client.engine->client_local_time_us_ += tick_us;
        }
        server.simulate_tick();
        deliver();
    };
    for (int index = 0; index < 20; ++index) {
        step();
    }
    for (const Client& client : clients) {
        require(client.seen.empty());
    }

    // The shooter fires and hits the victim, on the tick just simulated: the
    // fire as the weapon system reports it, the hit and the damage from the
    // one function every damage path ends in.
    const std::uint32_t tick = server.tick_loop_.current_tick() - 1u;
    const std::uint64_t hit_time_us = static_cast<std::uint64_t>(
        static_cast<double>(tick) *
        static_cast<double>(server.tick_loop_.fixed_delta_seconds()) * 1000000.0);
    const std::size_t first_event = server.events_.size();
    server.events_.push_back(KernelEvent{
        KernelEventType_FireConfirmed, tick, shooter_player, kShooterPeer, kWeaponId});
    ne::ConfirmedDamage damage{};
    damage.server_tick = tick;
    damage.source_net_id = shooter_player;
    damage.target_net_id = victim_player;
    damage.source_peer = kShooterPeer;
    damage.source_code = kWeaponId;
    damage.damage = 10;
    damage.hit_time_us = hit_time_us;
    (void)ne::apply_damage_applications(server.world_, {damage}, tick, &server.events_);
    server.broadcast_combat_events(first_event, server.events_.size());
    deliver();

    // The shooter: every one of its own the moment it arrives, while its world
    // timeline is still an interpolation delay short of the hit -- the
    // condition that makes "at once" mean something here.
    require(shooter.engine->render_server_time_us_ < hit_time_us);
    require(shooter.find(KernelEventType_FireConfirmed) != nullptr);
    require(shooter.find(KernelEventType_HitConfirmed) != nullptr);
    require(shooter.find(KernelEventType_DamageApplied) != nullptr);

    // The victim: none yet -- its world timeline has not reached the shot.
    require(victim.engine->render_server_time_us_ < hit_time_us);
    require(victim.seen.empty());
    for (int index = 0; index < 12; ++index) {
        step();
    }
    // Then all three, each on the frame the world timeline reached its tick.
    for (KernelEventType type :
         {KernelEventType_FireConfirmed,
          KernelEventType_HitConfirmed,
          KernelEventType_DamageApplied}) {
        const Client::Seen* seen = victim.find(type);
        require(seen != nullptr);
        require(seen->render_server_time_us >= hit_time_us);
        require(seen->render_server_time_us < hit_time_us + 2u * tick_us);
    }
    require(victim.find(KernelEventType_DamageApplied)->event.net_id == victim_player);

    // Too far to see the shooter or the victim: sent nothing.
    require(far.seen.empty());

    std::printf("combat_event_delivery_end_to_end_test: PASS\n");
    return 0;
}
