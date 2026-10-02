// A prop's lifecycle spawn tick from the server's world to a client's render
// state, through the spawn packet -- the only thing that says when a tent
// expires, since snapshots carry spawn_tick for projectiles only. A client
// that hears of the prop late still gets the tick it was spawned on, not the
// tick it was told.
//
// Two engines and a shuttle standing in for the network, the same way
// shelter_end_to_end_test does it.

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
#include "kernel/src/render_state_builder.h"

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

std::optional<RenderEntityState> render_state_of(
    ne::KernelEngine* client,
    ne::NetId net_id) {
    client->rebuild_render_states();
    for (const RenderEntityState& state : client->render_states_) {
        if (state.net_id == net_id) {
            return state;
        }
    }
    return std::nullopt;
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
        attach_loopback(&server, KernelMode_DedicatedServer, 7797);

    KernelConfig client_config = server_config;
    client_config.mode = KernelMode_Client;
    ne::KernelEngine early(client_config);
    ne::LoopbackTransport* early_link =
        attach_loopback(&early, KernelMode_Client, 7798);
    ne::KernelEngine late(client_config);
    ne::LoopbackTransport* late_link =
        attach_loopback(&late, KernelMode_Client, 7799);

    const ne::NetId first = server.world_.spawn_player(1, glm::vec3{0.0f});
    server.peer_sessions_.push_back(ne::KernelEngine::PeerSession{1, first, 0, true, {}});
    // The shuttle hands every packet to whichever client is listening; only
    // one is attached at a time, so each sees its own session's traffic.
    ne::LoopbackTransport* listening = early_link;
    ne::KernelEngine* listener = &early;
    const auto pump = [&]() {
        server.simulate_tick();
        shuttle(server_link, listening);
        listener->poll_transport();
    };
    for (int index = 0; index < 8; ++index) {
        pump();
    }
    early.local_player_net_id_ = first;

    // A tent-like prop: a lifecycle, spawned on the tick it appears.
    const std::uint32_t spawn_tick = server.tick_loop_.current_tick();
    const ne::NetId prop = server.world_.spawn_entity(
        ne::EntityType::kProp,
        ne::ActorType::kUnknown,
        0,
        glm::vec3{2.0f, 0.0f, 0.0f});
    server.world_.registry().emplace_or_replace<ne::PropLifecycle>(
        *server.world_.find_entity(prop),
        ne::PropLifecycle{spawn_tick, 5400u, 0u});
    for (int index = 0; index < 4; ++index) {
        pump();
    }
    const std::optional<RenderEntityState> seen_early = render_state_of(&early, prop);
    require(seen_early.has_value());
    require(seen_early->spawn_tick == spawn_tick);

    // Long after the spawn, a second player joins. Its spawn packet goes out
    // now, but the tick it carries is still the prop's.
    for (int index = 0; index < 90; ++index) {
        pump();
    }
    require(server.tick_loop_.current_tick() > spawn_tick + 60u);
    const ne::NetId second = server.world_.spawn_player(2, glm::vec3{1.0f, 0.0f, 1.0f});
    server.peer_sessions_.clear();
    server.peer_sessions_.push_back(ne::KernelEngine::PeerSession{2, second, 0, true, {}});
    listening = late_link;
    listener = &late;
    for (int index = 0; index < 8; ++index) {
        pump();
    }
    late.local_player_net_id_ = second;
    pump();
    const std::optional<RenderEntityState> seen_late = render_state_of(&late, prop);
    require(seen_late.has_value());
    require(seen_late->spawn_tick == spawn_tick);

    // A listen server draws from its own world, and reports the same tick.
    const std::optional<entt::entity> world_prop = server.world_.find_entity(prop);
    require(world_prop.has_value());
    require(ne::render_state_from_world_entity(server.world_, *world_prop, 1u)
                .spawn_tick == spawn_tick);

    std::printf("prop_spawn_tick_end_to_end_test: PASS\n");
    return 0;
}
