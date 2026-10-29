// Agent commands (docs/AI_BEHAVIOR_PLAN.md §8, packet schema 34): a player's
// order to an agent on its side, carried to game_server, and game_server's
// answer carried back. The kernel decides nothing about them.
//
// Over the wire (a client and a dedicated server on loopback) and on a listen
// host, where the host's own player has no wire to its own server. The sender
// a server reports is the session's, never anything in the command.

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

KernelAgentCommand command(std::uint64_t request_id, std::uint8_t behavior) {
    KernelAgentCommand out{};
    out.struct_size = sizeof(out);
    out.request_id = request_id;
    out.agent_net_id = 77u;
    out.target_net_id = 88u;
    out.route_id = 5u;
    out.behavior = behavior;
    out.flags = KERNEL_AGENT_COMMAND_FLAG_POSITION;
    out.position = KernelVec3{1.5f, -2.0f, 30.25f};
    return out;
}

void the_codec_round_trips() {
    KernelAgentCommand sent = command(0x1122334455667788ull, KernelAgentCommandBehavior_LockTarget);
    // Never on the wire: the server takes the requester from the session.
    sent.requester_peer = 123u;
    sent.requester_net_id = 456u;
    const std::vector<std::uint8_t> encoded = ne::encode_agent_command_packet(sent);
    KernelAgentCommand decoded{};
    require(ne::decode_agent_command_packet(encoded.data(), encoded.size(), &decoded));
    require(decoded.struct_size == sizeof(KernelAgentCommand));
    require(decoded.request_id == sent.request_id);
    require(decoded.agent_net_id == 77u);
    require(decoded.target_net_id == 88u);
    require(decoded.route_id == 5u);
    require(decoded.behavior == KernelAgentCommandBehavior_LockTarget);
    require(decoded.flags == KERNEL_AGENT_COMMAND_FLAG_POSITION);
    require(decoded.position.x == 1.5f && decoded.position.y == -2.0f &&
            decoded.position.z == 30.25f);
    require(decoded.requester_peer == 0u && decoded.requester_net_id == 0u);
    // Short, or someone else's packet, is not a command.
    require(!ne::decode_agent_command_packet(encoded.data(), encoded.size() - 1u, &decoded));
    KernelAgentCommandOutcome outcome{};
    outcome.struct_size = sizeof(outcome);
    outcome.requester_peer = 3u;
    outcome.request_id = 42u;
    outcome.agent_net_id = 77u;
    outcome.status = KernelAgentCommandStatus_Rejected;
    outcome.rejection_reason = KernelAgentCommandRejection_TooSoon;
    const std::vector<std::uint8_t> answer = ne::encode_agent_command_outcome_packet(outcome);
    require(!ne::decode_agent_command_packet(answer.data(), answer.size(), &decoded));
    KernelAgentCommandOutcome back{};
    require(ne::decode_agent_command_outcome_packet(answer.data(), answer.size(), &back));
    require(back.request_id == 42u && back.agent_net_id == 77u);
    require(back.status == KernelAgentCommandStatus_Rejected);
    require(back.rejection_reason == KernelAgentCommandRejection_TooSoon);
    require(back.requester_peer == 0u);
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

    // Not welcomed yet, or short: nothing goes out.
    const KernelAgentCommand patrol = command(1u, KernelAgentCommandBehavior_Patrol);
    require(!client.submit_agent_command(patrol));
    client.has_welcome_ = true;
    KernelAgentCommand short_command = patrol;
    short_command.struct_size = sizeof(KernelAgentCommand) - 1u;
    require(!client.submit_agent_command(short_command));

    // Client -> server, with whatever the client claimed about the sender
    // replaced by the session.
    KernelAgentCommand forged = command(2u, KernelAgentCommandBehavior_Guard);
    forged.requester_peer = kStranger;
    forged.requester_net_id = 999u;
    require(client.submit_agent_command(patrol));
    require(client.submit_agent_command(forged));
    client_to_server(client_link, server_link, kPeer);
    server.poll_transport();
    KernelAgentCommand received[4]{};
    require(server.server_poll_agent_commands(received, 4) == 2u);
    require(received[0].struct_size == sizeof(KernelAgentCommand));
    require(received[0].requester_peer == kPeer);
    require(received[0].requester_net_id == player);
    require(received[0].request_id == 1u);
    require(received[0].behavior == KernelAgentCommandBehavior_Patrol);
    require(received[0].route_id == 5u);
    require(received[1].request_id == 2u);
    require(received[1].requester_peer == kPeer);
    require(received[1].requester_net_id == player);
    require(server.server_poll_agent_commands(received, 4) == 0u);

    // A peer with no session is not heard.
    require(client.submit_agent_command(patrol));
    client_to_server(client_link, server_link, kStranger);
    server.poll_transport();
    require(server.server_poll_agent_commands(received, 4) == 0u);

    // Server -> client: the answer, to the requester only.
    KernelAgentCommandOutcome outcome{};
    outcome.struct_size = sizeof(outcome);
    outcome.requester_peer = kPeer;
    outcome.request_id = 1u;
    outcome.agent_net_id = 77u;
    outcome.status = KernelAgentCommandStatus_Accepted;
    require(server.server_send_agent_command_outcome(outcome));
    outcome.requester_peer = kStranger;
    require(!server.server_send_agent_command_outcome(outcome));
    server_to_client(server_link, client_link);
    client.poll_transport();
    KernelAgentCommandOutcome answers[4]{};
    require(client.poll_agent_command_outcomes(answers, 4) == 1u);
    require(answers[0].struct_size == sizeof(KernelAgentCommandOutcome));
    require(answers[0].requester_peer == 0u);
    require(answers[0].request_id == 1u);
    require(answers[0].agent_net_id == 77u);
    require(answers[0].status == KernelAgentCommandStatus_Accepted);
    require(client.poll_agent_command_outcomes(answers, 4) == 0u);

    // The queue is bounded: past 256 the rest are dropped, each with an error.
    server.events_.clear();
    for (int index = 0; index < 300; ++index) {
        require(client.submit_agent_command(patrol));
    }
    client_to_server(client_link, server_link, kPeer);
    server.poll_transport();
    std::uint32_t errors = 0;
    for (const KernelEvent& event : server.events_) {
        if (event.type == KernelEventType_Error && event.code == 36u) ++errors;
    }
    require(errors == 44u);
    std::vector<KernelAgentCommand> drained(300);
    require(server.server_poll_agent_commands(drained.data(), 300u) == 256u);

    // A dedicated server has no local player to command from.
    require(!server.submit_agent_command(patrol));
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
    require(Kernel_StartListenServer(kernel, 8024));
    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(kernel, &local));

    const KernelAgentCommand lock = command(9u, KernelAgentCommandBehavior_LockTarget);
    require(Kernel_SubmitAgentCommand(kernel, &lock));
    KernelAgentCommand received[2]{};
    require(Kernel_ServerPollAgentCommands(kernel, received, 2) == 1u);
    require(received[0].requester_peer != 0u);
    require(received[0].requester_net_id == local.player_net_id);
    require(received[0].request_id == 9u);
    // Nothing leaks into the client side.
    KernelAgentCommandOutcome answers[2]{};
    require(Kernel_PollAgentCommandOutcomes(kernel, answers, 2) == 0u);

    KernelAgentCommandOutcome outcome{};
    outcome.struct_size = sizeof(outcome);
    outcome.requester_peer = received[0].requester_peer;
    outcome.request_id = 9u;
    outcome.status = KernelAgentCommandStatus_Rejected;
    outcome.rejection_reason = KernelAgentCommandRejection_NotPlayerSide;
    require(Kernel_ServerSendAgentCommandOutcome(kernel, &outcome));
    require(Kernel_PollAgentCommandOutcomes(kernel, answers, 2) == 1u);
    require(answers[0].request_id == 9u);
    require(answers[0].rejection_reason == KernelAgentCommandRejection_NotPlayerSide);
    require(Kernel_ServerPollAgentCommands(kernel, received, 2) == 0u);
    require(!Kernel_SubmitAgentCommand(kernel, nullptr));
    Kernel_Destroy(kernel);
}

void advertised_in_the_abi() {
    KernelAbiInfo info{};
    info.struct_size = sizeof(info);
    require(Kernel_GetAbiInfo(&info, sizeof(info)));
    require((info.capability_flags & KERNEL_CAPABILITY_AGENT_COMMANDS) != 0u);
}

}  // namespace

int main() {
    the_codec_round_trips();
    over_the_wire();
    on_a_listen_host();
    advertised_in_the_abi();
    std::puts("agent_command_test passed");
    return 0;
}
