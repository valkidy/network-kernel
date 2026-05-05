#include "kernel/src/kernel.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <fmt/format.h>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include "kernel/public/kernel_api.h"
#include "protocol/public/m2_packets.h"
#include "transport/public/loopback_transport.h"
#include "transport/public/network_simulator_transport.h"

namespace network_example {
namespace {

KernelConfig with_kernel_defaults(KernelConfig config) {
    config.tick = with_tick_defaults(config.tick);
    if (config.max_render_states == 0) {
        config.max_render_states = 256;
    }
    if (config.max_events == 0) {
        config.max_events = 256;
    }
    return config;
}

KernelVec3 to_kernel_vec3(const glm::vec3& value) {
    return KernelVec3{value.x, value.y, value.z};
}

KernelQuat to_kernel_quat(const glm::quat& value) {
    return KernelQuat{value.x, value.y, value.z, value.w};
}

std::uint32_t history_frame_count(const TickConfig& config) {
    const TickConfig tick = with_tick_defaults(config);
    return std::max(1u, (tick.server_tick_rate * tick.history_ms) / 1000u);
}

constexpr PeerId kLocalListenPeerId = 1;

}  // namespace

KernelEngine::KernelEngine(KernelConfig config)
    : config_(with_kernel_defaults(config)),
      tick_loop_(config_.tick),
      history_buffer_(history_frame_count(config_.tick)),
      transport_(std::make_unique<NetworkSimulatorTransport>()) {
    render_states_.reserve(config_.max_render_states);
    events_.reserve(config_.max_events);
}

bool KernelEngine::start_client(const char*) {
    push_event(KernelEventType_Error, 0, 0, 1);
    return false;
}

bool KernelEngine::start_listen_server(std::uint16_t port) {
    auto loopback_transport = std::make_unique<LoopbackTransport>();
    if (!loopback_transport->StartServer(port)) {
        push_event(KernelEventType_Error, 0, 0, 2);
        return false;
    }

    loopback_transport_ = loopback_transport.get();
    transport_ = std::move(loopback_transport);
    config_.mode = KernelMode_ListenServer;
    running_ = true;

    const NetId player =
        world_.spawn_player(kLocalListenPeerId, glm::vec3{0.0f, 0.0f, 0.0f});
    const NetId enemy = world_.spawn_enemy(glm::vec3{8.0f, 0.0f, 0.0f});
    world_.spawn_projectile(
        0,
        glm::vec3{2.0f, 0.4f, -2.0f},
        glm::vec3{0.0f, 0.0f, 2.0f});

    push_event(KernelEventType_Connected, 0, kLocalListenPeerId);
    push_event(KernelEventType_PlayerJoined, player, kLocalListenPeerId);
    push_event(KernelEventType_EntitySpawned, player, kLocalListenPeerId);
    push_event(KernelEventType_EntitySpawned, enemy, 0);
    publish_snapshot();
    poll_client_transport();
    rebuild_render_states();
    return true;
}

bool KernelEngine::start_dedicated_server(std::uint16_t port) {
    loopback_transport_ = nullptr;
    if (!transport_->StartServer(port)) {
        push_event(KernelEventType_Error, 0, 0, 3);
        return false;
    }

    config_.mode = KernelMode_DedicatedServer;
    running_ = true;
    const NetId player = world_.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const NetId enemy = world_.spawn_enemy(glm::vec3{8.0f, 0.0f, 0.0f});
    world_.spawn_projectile(0, glm::vec3{2.0f, 0.4f, -2.0f}, glm::vec3{0.0f, 0.0f, 2.0f});
    push_event(KernelEventType_PlayerJoined, player, 1);
    push_event(KernelEventType_EntitySpawned, player, 1);
    push_event(KernelEventType_EntitySpawned, enemy, 0);
    publish_snapshot();
    rebuild_render_states();
    return true;
}

void KernelEngine::update(float delta_seconds) {
    if (!running_) {
        return;
    }

    poll_transport();
    const std::uint32_t ticks_to_run = tick_loop_.accumulate(delta_seconds);
    for (std::uint32_t tick = 0; tick < ticks_to_run; ++tick) {
        simulate_tick();
    }
    poll_client_transport();
    rebuild_render_states();
}

void KernelEngine::submit_input(PeerId local_player_id, const PlayerInput& input) {
    if (config_.mode == KernelMode_ListenServer && loopback_transport_ != nullptr) {
        const std::vector<std::uint8_t> packet =
            encode_input_packet(local_player_id, input, next_packet_sequence_++);
        if (!loopback_transport_->SendClient(
                local_player_id,
                packet.data(),
                static_cast<std::uint32_t>(packet.size()),
                SendMode::kUnreliable,
                ChannelId::kInput)) {
            push_event(KernelEventType_Error, 0, local_player_id, 4);
        }
        return;
    }

    pending_inputs_.push_back(QueuedInput{local_player_id, input});
    last_processed_input_seq_ = std::max(last_processed_input_seq_, input.input_seq);
}

std::uint32_t KernelEngine::get_render_states(
    RenderEntityState* out_states,
    std::uint32_t max_states) const {
    if (out_states == nullptr || max_states == 0) {
        return 0;
    }
    const std::uint32_t count =
        std::min(max_states, static_cast<std::uint32_t>(render_states_.size()));
    std::memcpy(out_states, render_states_.data(), sizeof(RenderEntityState) * count);
    return count;
}

std::uint32_t KernelEngine::poll_events(KernelEvent* out_events, std::uint32_t max_events) {
    if (out_events == nullptr || max_events == 0) {
        return 0;
    }
    const std::uint32_t count =
        std::min(max_events, static_cast<std::uint32_t>(events_.size()));
    std::memcpy(out_events, events_.data(), sizeof(KernelEvent) * count);
    events_.erase(events_.begin(), events_.begin() + count);
    return count;
}

void KernelEngine::push_event(
    KernelEventType type,
    NetId net_id,
    PeerId peer_id,
    std::uint32_t code) {
    events_.push_back(KernelEvent{type, tick_loop_.current_tick(), net_id, peer_id, code});
}

void KernelEngine::poll_transport() {
    TransportEvent transport_event;
    while (transport_->PollEvent(transport_event)) {
        if (transport_event.type == TransportEventType::kConnected) {
            push_event(KernelEventType_Connected, 0, transport_event.peer);
        } else if (transport_event.type == TransportEventType::kDisconnected) {
            push_event(KernelEventType_Disconnected, 0, transport_event.peer);
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kInput) {
            PeerId player_id = 0;
            PlayerInput input{};
            if (!decode_input_packet(
                    transport_event.payload.data(),
                    transport_event.payload.size(),
                    &player_id,
                    &input)) {
                push_event(KernelEventType_Error, 0, transport_event.peer, 5);
                continue;
            }
            pending_inputs_.push_back(QueuedInput{player_id, input});
            last_processed_input_seq_ =
                std::max(last_processed_input_seq_, input.input_seq);
        }
    }
}

void KernelEngine::poll_client_transport() {
    if (loopback_transport_ == nullptr) {
        return;
    }

    TransportEvent transport_event;
    while (loopback_transport_->PollClientEvent(transport_event)) {
        if (transport_event.type != TransportEventType::kMessage ||
            transport_event.channel != ChannelId::kSnapshot) {
            continue;
        }

        WorldSnapshot snapshot;
        if (!decode_snapshot_packet(
                transport_event.payload.data(),
                transport_event.payload.size(),
                &snapshot)) {
            push_event(KernelEventType_Error, 0, transport_event.peer, 6);
            continue;
        }
        latest_client_snapshot_ = std::move(snapshot);
        has_client_snapshot_ = true;
    }
}

void KernelEngine::simulate_tick() {
    const float fixed_delta = tick_loop_.fixed_delta_seconds();
    simulate_player_movement(world_, pending_inputs_, fixed_delta);
    simulate_hitscan_weapons(world_, pending_inputs_, tick_loop_.current_tick(), &events_);
    simulate_projectiles(world_, fixed_delta);
    destroy_dead_entities(world_, tick_loop_.current_tick(), &events_);
    history_buffer_.write_frame(world_, tick_loop_.current_tick());
    if (tick_loop_.should_write_snapshot()) {
        publish_snapshot();
    }
    pending_inputs_.clear();
    tick_loop_.advance_tick();
}

void KernelEngine::rebuild_render_states() {
    if (config_.mode == KernelMode_ListenServer && has_client_snapshot_) {
        rebuild_render_states_from_snapshot();
        return;
    }
    rebuild_render_states_from_world();
}

void KernelEngine::rebuild_render_states_from_world() {
    render_states_.clear();
    auto view = world_.registry().view<const NetworkIdentity, const EntityKind, const Transform>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<const NetworkIdentity>(entity);
        const EntityKind& kind = view.get<const EntityKind>(entity);
        const Transform& transform = view.get<const Transform>(entity);
        render_states_.push_back(RenderEntityState{
            identity.net_id,
            static_cast<std::uint16_t>(kind.type),
            to_kernel_vec3(transform.position),
            to_kernel_quat(transform.rotation),
            0,
            0,
        });
    }
}

void KernelEngine::rebuild_render_states_from_snapshot() {
    render_states_.clear();
    for (const EntitySnapshot& entity : latest_client_snapshot_.entities) {
        render_states_.push_back(RenderEntityState{
            entity.net_id,
            static_cast<std::uint16_t>(entity.type),
            to_kernel_vec3(entity.position),
            to_kernel_quat(entity.rotation),
            entity.state,
            entity.flags,
        });
    }
}

void KernelEngine::publish_snapshot() {
    latest_snapshot_ = build_world_snapshot(
        world_,
        tick_loop_.current_tick(),
        static_cast<std::uint32_t>(
            tick_loop_.current_tick() * tick_loop_.fixed_delta_seconds() * 1000.0f),
        last_processed_input_seq_);
    spdlog::debug(
        "{}",
        fmt::format(
            "snapshot tick={} entities={}",
            latest_snapshot_.header.server_tick,
            latest_snapshot_.entities.size()));

    if (config_.mode == KernelMode_ListenServer && loopback_transport_ != nullptr) {
        const std::vector<std::uint8_t> packet =
            encode_snapshot_packet(latest_snapshot_, next_packet_sequence_++);
        if (!loopback_transport_->Send(
                kLocalListenPeerId,
                packet.data(),
                static_cast<std::uint32_t>(packet.size()),
                SendMode::kUnreliable,
                ChannelId::kSnapshot)) {
            push_event(KernelEventType_Error, 0, kLocalListenPeerId, 7);
        }
    }
}

}  // namespace network_example

struct KernelHandle {
    std::unique_ptr<network_example::KernelEngine> engine;
};

extern "C" {

KernelHandle* Kernel_Create(const KernelConfig* config) {
    if (config == nullptr) {
        return nullptr;
    }
    auto* handle = new KernelHandle;
    handle->engine = std::make_unique<network_example::KernelEngine>(*config);
    return handle;
}

void Kernel_Destroy(KernelHandle* kernel) {
    delete kernel;
}

bool Kernel_StartClient(KernelHandle* kernel, const char* address) {
    return kernel != nullptr && kernel->engine->start_client(address);
}

bool Kernel_StartListenServer(KernelHandle* kernel, uint16_t port) {
    return kernel != nullptr && kernel->engine->start_listen_server(port);
}

bool Kernel_StartDedicatedServer(KernelHandle* kernel, uint16_t port) {
    return kernel != nullptr && kernel->engine->start_dedicated_server(port);
}

void Kernel_Update(KernelHandle* kernel, float delta_seconds) {
    if (kernel != nullptr) {
        kernel->engine->update(delta_seconds);
    }
}

void Kernel_SubmitInput(
    KernelHandle* kernel,
    uint32_t local_player_id,
    const PlayerInput* input) {
    if (kernel != nullptr && input != nullptr) {
        kernel->engine->submit_input(local_player_id, *input);
    }
}

uint32_t Kernel_GetRenderStates(
    KernelHandle* kernel,
    RenderEntityState* out_states,
    uint32_t max_states) {
    if (kernel == nullptr) {
        return 0;
    }
    return kernel->engine->get_render_states(out_states, max_states);
}

uint32_t Kernel_PollEvents(
    KernelHandle* kernel,
    KernelEvent* out_events,
    uint32_t max_events) {
    if (kernel == nullptr) {
        return 0;
    }
    return kernel->engine->poll_events(out_events, max_events);
}

}  // extern "C"
