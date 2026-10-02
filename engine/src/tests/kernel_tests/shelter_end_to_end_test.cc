// An actor's shelter from the server's world to the client's, through the
// builder, the relevance filter, the encoder, a transport and the decoder --
// the path the owner's prediction depends on to stop pushing its player out of
// a building's walls, and every client's interior depends on to seat the
// occupants. Since schema 27 another session sees it too, seat included.
//
// Two engines and a shuttle standing in for the network, the same way
// actor_impulse_end_to_end_test does it.

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

}  // namespace

int main() {
    KernelConfig server_config{};
    server_config.mode = KernelMode_DedicatedServer;
    server_config.tick.server_tick_rate = 30;
    server_config.tick.snapshot_rate = 30;
    server_config.max_events = 1024;
    server_config.max_render_states = 256;
    ne::KernelEngine server(server_config);
    ne::LoopbackTransport* server_link =
        attach_loopback(&server, KernelMode_DedicatedServer, 7795);

    KernelConfig client_config = server_config;
    client_config.mode = KernelMode_Client;
    ne::KernelEngine client(client_config);
    ne::LoopbackTransport* client_link =
        attach_loopback(&client, KernelMode_Client, 7796);

    const ne::NetId player = server.world_.spawn_player(1, glm::vec3{0.0f});
    const ne::NetId other = server.world_.spawn_player(2, glm::vec3{3.0f, 0.0f, 0.0f});
    server.peer_sessions_.push_back(ne::KernelEngine::PeerSession{1, player, 0, true, {}});
    const auto pump = [&]() {
        server.simulate_tick();
        shuttle(server_link, client_link);
        client.poll_transport();
    };
    for (int index = 0; index < 8; ++index) {
        pump();
    }
    client.local_player_net_id_ = player;
    pump();
    require(client.predicted_shelter_net_id_ == 0u);

    // In. The building is any entity the server names; the client is told
    // its net id and nothing else.
    const ne::NetId building = 4242;
    auto& registry = server.world_.registry();
    registry.emplace<ne::Sheltered>(
        *server.world_.find_entity(player),
        ne::Sheltered{building, glm::vec3{2.0f, 0.0f, 0.0f}, 0u, 2u});
    pump();
    require(client.predicted_shelter_net_id_ == building);
    require(client.predicted_shelter_tick_ != 0u);
    KernelLocalShelterState state{};
    state.struct_size = sizeof(state);
    require(client.local_shelter_state(&state));
    require(state.shelter_net_id == building);
    // And the owner's own render state carries the seat it was given.
    client.rebuild_render_states();
    bool saw_own_seat = false;
    for (const RenderEntityState& rendered : client.render_states_) {
        if (rendered.net_id == player) {
            saw_own_seat = true;
            require(rendered.shelter_net_id == building);
            require(rendered.shelter_seat == 2u);
        }
    }
    require(saw_own_seat);

    // The other player's session sees where this player is sheltering, and
    // in which seat, so it can hide the body outside and seat it inside.
    ne::KernelEngine::PeerSession other_session{2, other, 0, true, {}};
    other_session.relevant_entities = server.peer_sessions_.front().relevant_entities;
    other_session.relevant_entities.insert(player);
    const ne::WorldSnapshot seen_by_other =
        server.build_relevant_snapshot(other_session, 0u);
    bool saw_player = false;
    for (const ne::EntitySnapshot& entity : seen_by_other.entities) {
        if (entity.net_id == player) {
            saw_player = true;
            require(entity.shelter_net_id == building);
            require(entity.shelter_seat == 2u);
        }
    }
    require(saw_player);

    // Out: the next snapshot says so.
    registry.remove<ne::Sheltered>(*server.world_.find_entity(player));
    pump();
    require(client.predicted_shelter_net_id_ == 0u);

    std::printf("shelter_end_to_end_test: PASS\n");
    return 0;
}
