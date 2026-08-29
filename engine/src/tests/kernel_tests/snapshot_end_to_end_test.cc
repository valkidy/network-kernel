// Bytes, not structs.
//
// Every other client test in this suite hands the client an in-memory
// WorldSnapshot and every measurement drives build_relevant_snapshot /
// build_snapshot_send_set directly. Nothing has taken an agent from a server's
// world, through the encoder, over a transport, back out of the decoder, and
// into the client's replicated state -- which is the entire path that the agent
// record, the snapshot schema bump and the send scheduler changed.
//
// This closes that loop. Two engines, two loopback transports, and a shuttle
// between them standing in for the network: whatever the server put on the wire
// is what the client is given, byte for byte, on the channel the server chose.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "transport/public/loopback_transport.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::size_t kAgentCount = 40;
// Comfortably more than the spawn quota needs to introduce forty agents at
// sixteen a snapshot, and more than the send set needs to reach all of them.
constexpr std::size_t kSnapshots = 30;

network_example::LoopbackTransport* attach_loopback(
    network_example::KernelEngine* engine,
    KernelMode mode,
    std::uint16_t port) {
    auto transport = std::make_unique<network_example::LoopbackTransport>();
    network_example::LoopbackTransport* loopback = transport.get();
    engine->transport_ = std::move(transport);
    // After the reset, which stops whatever transport it is holding.
    engine->reset_runtime_state(mode);
    require(loopback->StartServer(port));
    return loopback;
}

// Everything the server put on its client-bound queue, handed to the client on
// the same channel. LoopbackTransport::SendClient writes to the queue that
// PollEvent drains, which is where a client-mode engine reads from.
std::size_t shuttle(
    network_example::LoopbackTransport* from,
    network_example::LoopbackTransport* to) {
    std::size_t moved = 0;
    network_example::TransportEvent event;
    while (from->PollClientEvent(event)) {
        require(to->SendClient(
            event.peer,
            event.payload.data(),
            static_cast<std::uint32_t>(event.payload.size()),
            event.mode,
            event.channel));
        ++moved;
    }
    return moved;
}

const network_example::KernelEngine::ClientReplicatedEntity* find_replicated(
    const network_example::KernelEngine& client,
    network_example::NetId net_id) {
    for (const network_example::KernelEngine::ClientReplicatedEntity& entity :
         client.client_replicated_entities_) {
        if (entity.net_id == net_id) {
            return &entity;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    KernelConfig server_config{};
    server_config.mode = KernelMode_DedicatedServer;
    server_config.tick.server_tick_rate = 30;
    server_config.tick.snapshot_rate = 15;
    server_config.max_events = 1024;
    server_config.max_render_states = 256;
    network_example::KernelEngine server(server_config);
    network_example::LoopbackTransport* server_link =
        attach_loopback(&server, KernelMode_DedicatedServer, 7791);

    KernelConfig client_config = server_config;
    client_config.mode = KernelMode_Client;
    network_example::KernelEngine client(client_config);
    network_example::LoopbackTransport* client_link =
        attach_loopback(&client, KernelMode_Client, 7792);

    const network_example::NetId player =
        server.world_.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    std::vector<network_example::NetId> agents;
    for (std::size_t index = 0; index < kAgentCount; ++index) {
        // Spread across all three distance bands the send weighting uses, and
        // given a facing and a velocity so that the fields the agent record
        // quantises are not all zero on the wire.
        const float angle = static_cast<float>(index) * 0.618f;
        const float radius = 2.0f + static_cast<float>(index) * 0.8f;
        const network_example::NetId agent = server.world_.spawn_enemy(glm::vec3{
            radius * std::cos(angle), 0.0f, radius * std::sin(angle)});
        const std::optional<entt::entity> entity =
            server.world_.find_entity(agent);
        require(entity.has_value());
        server.world_.registry().get<network_example::Transform>(*entity)
            .rotation = glm::angleAxis(angle, glm::vec3{0.0f, 1.0f, 0.0f});
        server.world_.registry().get<network_example::Velocity>(*entity)
            .linear = glm::vec3{std::cos(angle) * 2.5f, 0.0f, std::sin(angle) * 2.5f};
        agents.push_back(agent);
    }

    network_example::KernelEngine::PeerSession session{1, player, 0, true, {}};
    server.peer_sessions_.push_back(std::move(session));

    std::size_t packets = 0;
    for (std::size_t index = 0; index < kSnapshots; ++index) {
        // Two ticks per snapshot at a 30 Hz tick and a 15 Hz snapshot rate.
        server.simulate_tick();
        server.simulate_tick();
        packets += shuttle(server_link, client_link);
        client.poll_transport();
    }

    // The loop is actually carrying traffic, and the client actually decoded a
    // snapshot out of it rather than silently rejecting every packet.
    require(packets > kSnapshots);
    require(client.has_client_snapshot_);

    // Every agent reached the client -- spawned over the reliable channel under
    // the introduction quota, then positioned by the agent section.
    for (const network_example::NetId agent : agents) {
        const network_example::KernelEngine::ClientReplicatedEntity* replicated =
            find_replicated(client, agent);
        require(replicated != nullptr);
        require(replicated->active);
        require(replicated->actor_type == network_example::ActorType::kAgent);

        const std::optional<entt::entity> entity =
            server.world_.find_entity(agent);
        require(entity.has_value());
        const network_example::Transform& authority =
            server.world_.registry().get<network_example::Transform>(*entity);
        const network_example::Velocity& authority_velocity =
            server.world_.registry().get<network_example::Velocity>(*entity);

        // Position is still three floats on the wire, so it survives exactly.
        require(replicated->position == authority.position);
        // Velocity is i16 at 1/256 m/s.
        require(
            glm::length(replicated->velocity - authority_velocity.linear) < 0.005f);
        // Facing is compared as the direction it means: the agent record keeps
        // the yaw of the authority's rotation and nothing else, which is the
        // point of it.
        const glm::vec3 replicated_forward =
            replicated->rotation * glm::vec3{1.0f, 0.0f, 0.0f};
        const glm::vec3 authority_forward =
            authority.rotation * glm::vec3{1.0f, 0.0f, 0.0f};
        require(glm::length(replicated_forward - authority_forward) < 0.002f);
    }

    // Positive proof that the agents went through the narrow agent record and
    // not the wider actor one: the agent record keeps a yaw and discards
    // everything else about the rotation, so a pitched agent has to come back
    // upright. Round-tripping correctly is not the same as round-tripping
    // through the section under test, and every assertion above would pass just
    // as well if agents were still riding the actor record.
    const network_example::NetId pitched = agents.front();
    const std::optional<entt::entity> pitched_entity =
        server.world_.find_entity(pitched);
    require(pitched_entity.has_value());
    server.world_.registry().get<network_example::Transform>(*pitched_entity)
        .rotation = glm::angleAxis(0.6f, glm::vec3{0.0f, 0.0f, 1.0f});
    for (std::size_t index = 0; index < 4u; ++index) {
        server.simulate_tick();
        server.simulate_tick();
        shuttle(server_link, client_link);
        client.poll_transport();
    }
    const network_example::KernelEngine::ClientReplicatedEntity* pitched_replicated =
        find_replicated(client, pitched);
    require(pitched_replicated != nullptr);
    const glm::vec3 pitched_forward =
        pitched_replicated->rotation * glm::vec3{1.0f, 0.0f, 0.0f};
    require(std::fabs(pitched_forward.y) < 0.001f);

    // And the player, which rides the wider actor record rather than the agent
    // one -- both sections have to survive the same round trip.
    const network_example::KernelEngine::ClientReplicatedEntity* replicated_player =
        find_replicated(client, player);
    require(replicated_player != nullptr);
    require(replicated_player->actor_type == network_example::ActorType::kPlayer);

    return 0;
}
