// A status suspension from the server's world to a client's, through the
// encoder, a transport and the decoder (snapshot schema 29): the rise's
// zero-gravity anchor for a remote actor, the drop's anchor when it ends, the
// bubble's visual flag, and the owner's own suspension block. Two engines and
// a shuttle standing in for the network, as actor_impulse_end_to_end_test
// does it.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "simulation/public/action_graph.h"
#include "transport/public/loopback_transport.h"
#include "world/public/components.h"

#define private public
#include "kernel/src/kernel.h"
#undef private
#include "simulation/src/systems.h"

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

constexpr std::uint32_t kStatusId = 1601u;
constexpr std::uint32_t kDurationTicks = 12u;
constexpr float kRise = 1.5f;
constexpr float kDrift = 0.5f;

ne::LoopbackTransport* attach_loopback(
    ne::KernelEngine* engine,
    KernelMode mode,
    std::uint16_t port) {
    auto transport = std::make_unique<ne::LoopbackTransport>();
    ne::LoopbackTransport* loopback = transport.get();
    engine->transport_ = std::move(transport);
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

ne::RuntimeStatusEffectTemplate bubble() {
    KernelActionTriggerDefinition on_apply{};
    on_apply.struct_size = sizeof(on_apply);
    on_apply.action_count = 1u;
    on_apply.actions[0].action_type = KernelEntityTriggerActionType_ApplySuspendMovement;
    on_apply.actions[0].target_source = KernelEntityRefSource_EventSubject;
    on_apply.actions[0].suspend_rise_speed = kRise;
    on_apply.actions[0].suspend_drift_speed = kDrift;
    ne::RuntimeStatusEffectTemplate status;
    status.status_effect_id = kStatusId;
    status.channel_id = 61u;
    status.duration_ticks = kDurationTicks;
    status.on_apply_binding = ne::compile_action_trigger_definition(
        ne::TriggerEventType::kStatusApplied, on_apply);
    require(status.on_apply_binding.has_value());
    return status;
}

// The balloon's hit: the status, carrying the throw's direction (+x).
void encase(ne::KernelEngine* server, ne::NetId source, ne::NetId target, std::uint64_t id) {
    ne::World& world = server->world_;
    world.registry().get_or_emplace<ne::MovementState>(*world.find_entity(target)).gravity =
        glm::vec3{0.0f, -9.81f, 0.0f};
    const ne::TriggerEvent event{
        ne::TriggerEventType::kCollision, source, source, target,
        glm::vec3{0.0f}, glm::vec3{1.0f, 0.0f, 0.0f}};
    ne::ActionExecutionProvenance provenance;
    provenance.request_id = id;
    provenance.server_tick = server->current_tick();
    provenance.instigator = source;
    ne::ActionApplyStatusCommand apply{
        source, target, kStatusId, provenance, glm::vec3{1.0f, 0.0f, 0.0f}};
    require(ne::execute_action_graph_command_batch(
        *server,
        ne::ActionGraphCommandBatch{event, provenance, static_cast<std::uint32_t>(id), {apply}},
        0u));
}

const ne::EntitySnapshot* in_latest(const ne::KernelEngine& client, ne::NetId net_id) {
    for (const ne::EntitySnapshot& entity : client.latest_client_snapshot_.entities) {
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
    ne::KernelEngine server(server_config);
    ne::LoopbackTransport* server_link =
        attach_loopback(&server, KernelMode_DedicatedServer, 7795);

    KernelConfig client_config = server_config;
    client_config.mode = KernelMode_Client;
    ne::KernelEngine client(client_config);
    ne::LoopbackTransport* client_link =
        attach_loopback(&client, KernelMode_Client, 7796);

    server.world_.set_status_effect_templates({bubble()});
    const ne::NetId player = server.world_.spawn_player(1, glm::vec3{0.0f});
    const ne::NetId agent = server.world_.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    server.world_.registry().get_or_emplace<ne::Health>(
        *server.world_.find_entity(agent)) = ne::Health{50, 50};
    server.peer_sessions_.push_back(ne::KernelEngine::PeerSession{1, player, 0, true, {}});

    for (int index = 0; index < 8; ++index) {
        server.simulate_tick();
        shuttle(server_link, client_link);
        client.poll_transport();
    }
    require(server.peer_sessions_.front().relevant_entities.contains(agent));
    client.local_player_net_id_ = player;

    encase(&server, player, agent, 1u);
    encase(&server, agent, player, 2u);
    const std::uint32_t encased_tick = server.current_tick();
    const std::uint32_t until = encased_tick + kDurationTicks;

    // The rise's anchor: a straight line, nothing pulling on it, to the end tick.
    client.client_knockback_anchors_.clear();
    server.simulate_tick();
    shuttle(server_link, client_link);
    client.poll_transport();
    require(client.client_knockback_anchors_.size() == 1u);
    const auto rise = client.client_knockback_anchors_.find(agent);
    require(rise != client.client_knockback_anchors_.end());
    require(rise->second.gravity_y == 0.0f);
    require(rise->second.velocity == glm::vec3(kDrift, kRise, 0.0f));
    require(rise->second.tick == encased_tick);
    // The owner gets no anchor: its own record carries the suspension.
    require(client.client_knockback_anchors_.find(player) ==
            client.client_knockback_anchors_.end());

    // A snapshot or two later: the bubble flag on the remote actor, and the
    // owner's suspension block on its own record only.
    for (int index = 0; index < 2; ++index) {
        server.simulate_tick();
        shuttle(server_link, client_link);
        client.poll_transport();
    }
    const ne::EntitySnapshot* remote = in_latest(client, agent);
    const ne::EntitySnapshot* own = in_latest(client, player);
    require(remote != nullptr && own != nullptr);
    require((remote->flags & ne::kVisualFlagSuspended) != 0u);
    require((own->flags & ne::kVisualFlagSuspended) != 0u);
    require(!remote->has_suspension);
    require(own->has_suspension);
    require(own->suspension_velocity == glm::vec3(kDrift, kRise, 0.0f));
    require(own->suspension_until_tick == until);

    // The status ends: the drop's anchor, falling under gravity, and the flag
    // gone with the bubble.
    client.client_knockback_anchors_.clear();
    while (server.current_tick() <= until) {
        server.simulate_tick();
        shuttle(server_link, client_link);
        client.poll_transport();
    }
    const auto drop = client.client_knockback_anchors_.find(agent);
    require(drop != client.client_knockback_anchors_.end());
    require(drop->second.gravity_y == -9.81f);
    require(drop->second.velocity == glm::vec3(0.0f));
    server.simulate_tick();
    shuttle(server_link, client_link);
    client.poll_transport();
    server.simulate_tick();
    shuttle(server_link, client_link);
    client.poll_transport();
    remote = in_latest(client, agent);
    own = in_latest(client, player);
    require(remote != nullptr && own != nullptr);
    require((remote->flags & ne::kVisualFlagSuspended) == 0u);
    require(!own->has_suspension);

    std::printf("suspension_end_to_end_test: PASS\n");
    return 0;
}
