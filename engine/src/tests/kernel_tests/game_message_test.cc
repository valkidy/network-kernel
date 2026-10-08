// Game messages (ABI 101): an opaque, typed body between a client and
// game_server, either way, that the kernel carries and never reads.
//
// Over the wire (a client and a dedicated server on loopback) and on a listen
// host, where the host's own player has no wire to its own server. The sender
// a server reports is the session's, never anything in the packet.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/public/kernel_api.h"
#include "protocol/public/network_packets.h"
#include "transport/public/loopback_transport.h"

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

constexpr ne::PeerId kPeer = 3;
constexpr ne::PeerId kStranger = 9;

// Loopback binds nothing, so the port only has to be non-zero.
ne::LoopbackTransport* attach_loopback(ne::KernelEngine* engine, KernelMode mode) {
    auto transport = std::make_unique<ne::LoopbackTransport>();
    ne::LoopbackTransport* loopback = transport.get();
    engine->transport_ = std::move(transport);
    engine->reset_runtime_state(mode);
    require(loopback->StartServer(7101));
    return loopback;
}

// Everything the client sent, to the server as from `as_peer`.
void client_to_server(
    ne::LoopbackTransport* client,
    ne::LoopbackTransport* server,
    ne::PeerId as_peer) {
    ne::TransportEvent event;
    while (client->PollClientEvent(event)) {
        require(server->SendClient(
            as_peer,
            event.payload.data(),
            static_cast<std::uint32_t>(event.payload.size()),
            event.mode,
            event.channel));
    }
}

// Everything the server sent, to the client.
void server_to_client(ne::LoopbackTransport* server, ne::LoopbackTransport* client) {
    ne::TransportEvent event;
    while (server->PollClientEvent(event)) {
        require(client->SendClient(
            event.peer,
            event.payload.data(),
            static_cast<std::uint32_t>(event.payload.size()),
            event.mode,
            event.channel));
    }
}

std::vector<std::uint8_t> bytes(std::initializer_list<std::uint8_t> values) {
    return std::vector<std::uint8_t>(values);
}

void over_the_wire() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    config.max_events = 1024;
    ne::KernelEngine server(config);
    ne::LoopbackTransport* server_link =
        attach_loopback(&server, KernelMode_DedicatedServer);
    server.running_ = true;
    config.mode = KernelMode_Client;
    ne::KernelEngine client(config);
    ne::LoopbackTransport* client_link = attach_loopback(&client, KernelMode_Client);

    const ne::NetId player = server.world_.spawn_player(kPeer, glm::vec3{0.0f});
    server.peer_sessions_.push_back(
        ne::KernelEngine::PeerSession{kPeer, player, 0, true, {}});

    // Not welcomed yet: nothing goes out.
    const std::vector<std::uint8_t> hello = bytes({1, 2, 3, 0, 255});
    require(!client.send_game_message(7u, hello.data(), 5u));
    client.has_welcome_ = true;

    // Client -> server: type and body intact; sender from the session.
    require(client.send_game_message(7u, hello.data(), 5u));
    require(client.send_game_message(8u, nullptr, 0u));
    client_to_server(client_link, server_link, kPeer);
    server.poll_transport();
    KernelGameMessage received[4]{};
    require(server.server_poll_game_messages(received, 4) == 2u);
    require(received[0].struct_size == sizeof(KernelGameMessage));
    require(received[0].peer == kPeer);
    require(received[0].player_net_id == player);
    require(received[0].message_type == 7u);
    require(received[0].payload_size == 5u);
    require(std::memcmp(received[0].payload, hello.data(), 5u) == 0);
    require(received[1].message_type == 8u);
    require(received[1].payload_size == 0u);
    require(server.server_poll_game_messages(received, 4) == 0u);

    // A peer with no session is not heard.
    require(client.send_game_message(7u, hello.data(), 5u));
    client_to_server(client_link, server_link, kStranger);
    server.poll_transport();
    require(server.server_poll_game_messages(received, 4) == 0u);

    // Server -> client.
    const std::vector<std::uint8_t> reply = bytes({9, 8, 7});
    require(server.server_send_game_message(kPeer, 42u, reply.data(), 3u));
    require(!server.server_send_game_message(kStranger, 42u, reply.data(), 3u));
    server_to_client(server_link, client_link);
    client.poll_transport();
    require(client.poll_game_messages(received, 4) == 1u);
    require(received[0].peer == 0u);
    require(received[0].player_net_id == 0u);
    require(received[0].message_type == 42u);
    require(received[0].payload_size == 3u);
    require(std::memcmp(received[0].payload, reply.data(), 3u) == 0);

    // Bodies past the limit, or a size with no body, are refused at the door.
    std::vector<std::uint8_t> largest(KERNEL_MAX_GAME_MESSAGE_BYTES, 0x5a);
    require(client.send_game_message(1u, largest.data(), KERNEL_MAX_GAME_MESSAGE_BYTES));
    std::vector<std::uint8_t> too_big(KERNEL_MAX_GAME_MESSAGE_BYTES + 1u, 0x5a);
    require(!client.send_game_message(1u, too_big.data(), KERNEL_MAX_GAME_MESSAGE_BYTES + 1u));
    require(!client.send_game_message(1u, nullptr, 1u));
    require(!server.server_send_game_message(
        kPeer, 1u, too_big.data(), KERNEL_MAX_GAME_MESSAGE_BYTES + 1u));
    client_to_server(client_link, server_link, kPeer);
    server.poll_transport();
    require(server.server_poll_game_messages(received, 4) == 1u);
    require(received[0].payload_size == KERNEL_MAX_GAME_MESSAGE_BYTES);
    require(received[0].payload[KERNEL_MAX_GAME_MESSAGE_BYTES - 1u] == 0x5a);

    // A packet that claims a body past the limit is not a game message.
    ne::GameMessagePacket oversize;
    oversize.message_type = 1u;
    oversize.payload.assign(KERNEL_MAX_GAME_MESSAGE_BYTES + 1u, 0u);
    const std::vector<std::uint8_t> encoded = ne::encode_game_message_packet(oversize);
    ne::GameMessagePacket decoded;
    require(!ne::decode_game_message_packet(encoded.data(), encoded.size(), &decoded));

    // The queue is bounded: past 256 the rest are dropped, each with an error.
    server.events_.clear();
    for (int index = 0; index < 300; ++index) {
        require(client.send_game_message(5u, hello.data(), 1u));
    }
    client_to_server(client_link, server_link, kPeer);
    server.poll_transport();
    std::uint32_t errors = 0;
    for (const KernelEvent& event : server.events_) {
        if (event.type == KernelEventType_Error && event.code == 34u) ++errors;
    }
    require(errors == 44u);
    std::vector<KernelGameMessage> drained(300);
    require(server.server_poll_game_messages(drained.data(), 300u) == 256u);

    // A dedicated server has no local player to send from.
    require(!server.send_game_message(1u, hello.data(), 1u));
}

// The host's own player: straight into the server's queue and back, with no
// transport involved.
void on_a_listen_host() {
    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    config.max_events = 256;
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartListenServer(kernel, 8059));
    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(kernel, &local));

    const std::uint8_t body[2] = {4, 2};
    require(Kernel_SendGameMessage(kernel, 11u, body, 2u));
    KernelGameMessage received[2]{};
    require(Kernel_ServerPollGameMessages(kernel, received, 2) == 1u);
    require(received[0].peer != 0u);
    require(received[0].player_net_id == local.player_net_id);
    require(received[0].message_type == 11u);
    require(received[0].payload[1] == 2u);
    // Nothing leaks into the client side.
    require(Kernel_PollGameMessages(kernel, received, 2) == 0u);

    require(Kernel_ServerSendGameMessage(kernel, received[0].peer, 12u, body, 2u));
    require(Kernel_PollGameMessages(kernel, received, 2) == 1u);
    require(received[0].message_type == 12u);
    require(Kernel_ServerPollGameMessages(kernel, received, 2) == 0u);
    Kernel_Destroy(kernel);
}

void advertised_in_the_abi() {
    KernelAbiInfo info{};
    info.struct_size = sizeof(info);
    require(Kernel_GetAbiInfo(&info, sizeof(info)));
    require((info.capability_flags & KERNEL_CAPABILITY_GAME_MESSAGES) != 0u);
    require(info.game_message_size == sizeof(KernelGameMessage));
}

}  // namespace

int main() {
    over_the_wire();
    on_a_listen_host();
    advertised_in_the_abi();
    std::puts("game_message_test passed");
    return 0;
}
