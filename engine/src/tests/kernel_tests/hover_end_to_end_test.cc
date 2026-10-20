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

constexpr std::uint32_t kStatusId = 1602u;
constexpr std::uint32_t kDurationTicks = 900u;

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

ne::RuntimeStatusEffectTemplate hover_status() {
    KernelActionTriggerDefinition on_apply{};
    on_apply.struct_size = sizeof(on_apply);
    on_apply.action_count = 1u;
    on_apply.actions[0].action_type = KernelEntityTriggerActionType_ApplyHoverMovement;
    on_apply.actions[0].target_source = KernelEntityRefSource_EventSubject;
    on_apply.actions[0].hover_height_meters = 3.0f;
    on_apply.actions[0].hover_vertical_speed = 4.0f;
    ne::RuntimeStatusEffectTemplate status;
    status.status_effect_id = kStatusId;
    status.channel_id = 62u;
    status.duration_ticks = kDurationTicks;
    status.on_apply_binding = ne::compile_action_trigger_definition(
        ne::TriggerEventType::kStatusApplied, on_apply);
    require(status.on_apply_binding.has_value());
    return status;
}

// The potion: the status onto the one who drank it.
void drink(ne::KernelEngine* server, ne::NetId player, std::uint64_t id) {
    const ne::TriggerEvent event{
        ne::TriggerEventType::kCollision, player, player, player,
        glm::vec3{0.0f}, glm::vec3{0.0f}};
    ne::ActionExecutionProvenance provenance;
    provenance.request_id = id;
    provenance.server_tick = server->current_tick();
    provenance.instigator = player;
    ne::ActionApplyStatusCommand apply{player, player, kStatusId, provenance, glm::vec3{0.0f}};
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

void step(ne::KernelEngine* server, ne::LoopbackTransport* server_link,
          ne::KernelEngine* client, ne::LoopbackTransport* client_link) {
    server->simulate_tick();
    shuttle(server_link, client_link);
    client->poll_transport();
}

}  // namespace

// A status hover from the server's world to a client's (snapshot schema 32):
// the owner's own hover block on its own record only, and a knockback anchor
// with no gravity for a remote player the hover is carrying
// (docs/PLAYER_HOVER_PLAN.md P3).
int main() {
    KernelConfig server_config{};
    server_config.mode = KernelMode_DedicatedServer;
    server_config.tick.server_tick_rate = 30;
    server_config.tick.snapshot_rate = 15;
    server_config.max_events = 1024;
    server_config.max_render_states = 256;
    ne::KernelEngine server(server_config);
    ne::LoopbackTransport* server_link =
        attach_loopback(&server, KernelMode_DedicatedServer, 7797);

    KernelConfig client_config = server_config;
    client_config.mode = KernelMode_Client;
    ne::KernelEngine client(client_config);
    ne::LoopbackTransport* client_link =
        attach_loopback(&client, KernelMode_Client, 7798);

    server.world_.set_status_effect_templates({hover_status()});
    const ne::NetId own = server.world_.spawn_player(1, glm::vec3{0.0f});
    const ne::NetId other = server.world_.spawn_player(2, glm::vec3{4.0f, 0.0f, 0.0f});
    server.peer_sessions_.push_back(ne::KernelEngine::PeerSession{1, own, 0, true, {}});
    for (int index = 0; index < 8; ++index) {
        step(&server, server_link, &client, client_link);
    }
    client.local_player_net_id_ = own;

    drink(&server, own, 1u);
    drink(&server, other, 2u);
    const std::uint32_t until = server.current_tick() + kDurationTicks;
    for (int index = 0; index < 3; ++index) {
        step(&server, server_link, &client, client_link);
    }
    const ne::EntitySnapshot* own_record = in_latest(client, own);
    const ne::EntitySnapshot* other_record = in_latest(client, other);
    require(own_record != nullptr && other_record != nullptr);
    require(own_record->has_hover);
    require(own_record->hover_height_meters == 3.0f);
    require(own_record->hover_vertical_speed == 4.0f);
    require(own_record->hover_until_tick == until);
    // Someone else's hover is theirs: the flag is what draws it.
    require(!other_record->has_hover);

    // A knockback on the other player. These players have no movement
    // capsule, so the movement step passes them by and the hovering bit stays
    // what the test sets -- standing in for a step that just hovered them.
    const entt::entity other_entity = *server.world_.find_entity(other);
    ne::MovementState& movement =
        server.world_.registry().get_or_emplace<ne::MovementState>(other_entity);
    movement.gravity = glm::vec3{0.0f, -9.81f, 0.0f};
    for (const bool hovering : {true, false}) {
        movement.hovering = hovering;
        server.world_.registry().get_or_emplace<ne::Velocity>(other_entity).linear =
            glm::vec3{6.0f, 0.0f, 0.0f};
        server.world_.registry().emplace_or_replace<ne::ImpulseLockout>(
            other_entity,
            ne::ImpulseLockout{server.current_tick() + 20u, server.current_tick()});
        server.queue_actor_impulse(other, 0.0f);
        client.client_knockback_anchors_.clear();
        step(&server, server_link, &client, client_link);
        require(movement.hovering == hovering);
        const auto anchor = client.client_knockback_anchors_.find(other);
        require(anchor != client.client_knockback_anchors_.end());
        std::printf("[anchor] hovering=%d gravity_y=%.2f\n", hovering ? 1 : 0,
                    anchor->second.gravity_y);
        // The control is the plain character's fall.
        require(anchor->second.gravity_y == (hovering ? 0.0f : -9.81f));
        server.world_.registry().remove<ne::ImpulseLockout>(other_entity);
    }

    std::printf("hover_end_to_end_test: PASS\n");
    return 0;
}
