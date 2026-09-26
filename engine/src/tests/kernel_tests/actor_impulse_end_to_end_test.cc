// A knockback anchor from the server's world to the client's, through the
// encoder, a transport and the decoder.
//
// The flight replay is pinned to the character solver in impulse_lockout_test
// and the client's drawing of it in remote_actor_gap_bridging_test. What
// neither sees is the flush between them: which knockbacks go out, to whom,
// and whether what arrives is the state the server actually ended the tick
// with. Two engines and a shuttle standing in for the network, the same way
// snapshot_end_to_end_test does it.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

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

void shuttle(ne::LoopbackTransport* from, ne::LoopbackTransport* to) {
    ne::TransportEvent event;
    while (from->PollClientEvent(event)) {
        require(to->SendClient(
            event.peer,
            event.payload.data(),
            static_cast<std::uint32_t>(event.payload.size()),
            event.mode,
            event.channel));
    }
}

// What the impulse handler leaves behind on an actor it knocks back: the new
// velocity and a lockout armed on the current tick. Then the hook it calls.
void knock_back(
    ne::KernelEngine* server,
    ne::NetId net_id,
    const glm::vec3& velocity) {
    const std::optional<entt::entity> entity = server->world_.find_entity(net_id);
    require(entity.has_value());
    auto& registry = server->world_.registry();
    const std::uint32_t tick = server->current_tick();
    registry.get_or_emplace<ne::Velocity>(*entity).linear = velocity;
    registry.emplace_or_replace<ne::ImpulseLockout>(
        *entity, ne::ImpulseLockout{tick + 40u, tick});
    registry.get_or_emplace<ne::MovementState>(*entity).gravity =
        glm::vec3{0.0f, -9.81f, 0.0f};
    server->queue_actor_impulse(
        net_id, registry.get<ne::Transform>(*entity).position.y);
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
        attach_loopback(&server, KernelMode_DedicatedServer, 7793);

    KernelConfig client_config = server_config;
    client_config.mode = KernelMode_Client;
    ne::KernelEngine client(client_config);
    ne::LoopbackTransport* client_link =
        attach_loopback(&client, KernelMode_Client, 7794);

    const ne::NetId player = server.world_.spawn_player(1, glm::vec3{0.0f});
    const ne::NetId near_agent = server.world_.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    // Well past the relevance radius: this client never hears of it.
    const ne::NetId far_agent = server.world_.spawn_enemy(glm::vec3{90.0f, 0.0f, 0.0f});
    server.peer_sessions_.push_back(ne::KernelEngine::PeerSession{1, player, 0, true, {}});

    // Let the spawns and the relevance set settle.
    for (int index = 0; index < 8; ++index) {
        server.simulate_tick();
        shuttle(server_link, client_link);
        client.poll_transport();
    }
    require(server.peer_sessions_.front().relevant_entities.contains(near_agent));
    require(!server.peer_sessions_.front().relevant_entities.contains(far_agent));
    client.local_player_net_id_ = player;

    knock_back(&server, near_agent, glm::vec3{8.0f, 6.0f, 0.0f});
    knock_back(&server, far_agent, glm::vec3{8.0f, 6.0f, 0.0f});
    // The owner predicts its own from the lockout block in its snapshot record.
    knock_back(&server, player, glm::vec3{-8.0f, 6.0f, 0.0f});
    // current_tick() is the tick about to be simulated; simulate_tick advances
    // it on the way out.
    const std::uint32_t struck_tick = server.current_tick();
    server.simulate_tick();
    shuttle(server_link, client_link);
    client.poll_transport();

    require(client.client_knockback_anchors_.size() == 1u);
    const auto anchor = client.client_knockback_anchors_.find(near_agent);
    require(anchor != client.client_knockback_anchors_.end());

    // Exactly the state the server ended the tick with, on the tick it ended.
    const entt::entity authority = *server.world_.find_entity(near_agent);
    const auto& registry = server.world_.registry();
    // Stamped like a snapshot taken at the end of the same tick, which is what
    // lets the client treat the anchor as one more sample of the actor.
    require(anchor->second.tick == struck_tick);
    require(anchor->second.position == registry.get<ne::Transform>(authority).position);
    require(anchor->second.velocity == registry.get<ne::Velocity>(authority).linear);
    require(anchor->second.gravity_y == -9.81f);
    const ne::ImpulseLockout& lockout = registry.get<ne::ImpulseLockout>(authority);
    require(anchor->second.end_tick > anchor->second.tick);
    require(anchor->second.end_tick < lockout.until_tick);

    // Sent once, not every tick the lockout stands.
    server.simulate_tick();
    shuttle(server_link, client_link);
    client.client_knockback_anchors_.clear();
    client.poll_transport();
    require(client.client_knockback_anchors_.empty());

    std::printf("actor_impulse_end_to_end_test: PASS\n");
    return 0;
}
