#include "kernel/src/kernel.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <fmt/format.h>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include "kernel/public/kernel_api.h"
#include "protocol/public/m2_packets.h"
#include "protocol/public/session_packets.h"
#include "transport/public/gns_transport.h"
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

glm::vec3 input_move_to_world(const PlayerInput& input) {
    glm::vec3 move{input.move.x, 0.0f, input.move.y};
    const float length = glm::length(move);
    if (length > 1.0f) {
        move /= length;
    }
    return move;
}

void apply_input_prediction(
    EntitySnapshot* entity,
    const PlayerInput& input,
    float fixed_delta_seconds) {
    if (entity == nullptr) {
        return;
    }
    constexpr float kMoveSpeedMetersPerSecond = 5.0f;
    entity->velocity = input_move_to_world(input) * kMoveSpeedMetersPerSecond;
    entity->position += entity->velocity * fixed_delta_seconds;
}

const EntitySnapshot* find_snapshot_entity(
    const WorldSnapshot& snapshot,
    NetId net_id) {
    const auto found = std::find_if(
        snapshot.entities.begin(),
        snapshot.entities.end(),
        [net_id](const EntitySnapshot& entity) {
            return entity.net_id == net_id;
        });
    if (found == snapshot.entities.end()) {
        return nullptr;
    }
    return &(*found);
}

EntitySnapshot interpolate_entity(
    const EntitySnapshot& from,
    const EntitySnapshot& to,
    float alpha) {
    EntitySnapshot entity = to;
    entity.position = from.position + (to.position - from.position) * alpha;
    entity.rotation = glm::slerp(from.rotation, to.rotation, alpha);
    entity.velocity = from.velocity + (to.velocity - from.velocity) * alpha;
    return entity;
}

std::uint32_t history_frame_count(const TickConfig& config) {
    const TickConfig tick = with_tick_defaults(config);
    return std::max(1u, (tick.server_tick_rate * tick.history_ms) / 1000u);
}

constexpr PeerId kLocalListenPeerId = 1;
constexpr PeerId kServerPeerId = 0;
constexpr std::uint32_t kClientNonce = 0x4d330001u;

}  // namespace

KernelEngine::KernelEngine(KernelConfig config)
    : config_(with_kernel_defaults(config)),
      tick_loop_(config_.tick),
      history_buffer_(history_frame_count(config_.tick)),
      transport_(std::make_unique<NetworkSimulatorTransport>()) {
    render_states_.reserve(config_.max_render_states);
    events_.reserve(config_.max_events);
}

bool KernelEngine::start_client(const char* address) {
    auto gns_transport = std::make_unique<GnsTransport>();
    if (!gns_transport->StartClient(address)) {
        push_event(KernelEventType_Error, 0, 0, 1);
        return false;
    }

    loopback_transport_ = nullptr;
    transport_ = std::move(gns_transport);
    config_.mode = KernelMode_Client;
    running_ = true;
    local_client_peer_id_ = 0;
    local_player_net_id_ = 0;
    local_last_processed_input_seq_ = 0;
    predicted_server_tick_ = tick_loop_.current_tick();
    pending_prediction_inputs_.clear();
    client_snapshot_buffer_.clear();
    local_correction_offset_ = glm::vec3{0.0f, 0.0f, 0.0f};
    client_handshake_sent_ = false;
    has_welcome_ = false;
    has_client_snapshot_ = false;
    has_predicted_local_entity_ = false;
    return true;
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
    local_client_peer_id_ = kLocalListenPeerId;
    local_player_net_id_ = player;
    local_last_processed_input_seq_ = 0;
    predicted_server_tick_ = tick_loop_.current_tick();
    pending_prediction_inputs_.clear();
    client_snapshot_buffer_.clear();
    local_correction_offset_ = glm::vec3{0.0f, 0.0f, 0.0f};
    has_welcome_ = true;
    has_predicted_local_entity_ = false;
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
    auto gns_transport = std::make_unique<GnsTransport>();
    loopback_transport_ = nullptr;
    if (!gns_transport->StartServer(port)) {
        push_event(KernelEventType_Error, 0, 0, 3);
        return false;
    }

    transport_ = std::move(gns_transport);
    config_.mode = KernelMode_DedicatedServer;
    running_ = true;
    local_client_peer_id_ = 0;
    local_player_net_id_ = 0;
    local_last_processed_input_seq_ = 0;
    pending_prediction_inputs_.clear();
    client_snapshot_buffer_.clear();
    has_client_snapshot_ = false;
    has_predicted_local_entity_ = false;
    const NetId enemy = world_.spawn_enemy(glm::vec3{8.0f, 0.0f, 0.0f});
    world_.spawn_projectile(0, glm::vec3{2.0f, 0.4f, -2.0f}, glm::vec3{0.0f, 0.0f, 2.0f});
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
        const PlayerInput input_to_send = prepare_client_input(input);
        predict_local_input(input_to_send);
        const std::vector<std::uint8_t> packet =
            encode_input_packet(local_player_id, input_to_send, next_packet_sequence_++);
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

    if (config_.mode == KernelMode_Client) {
        if (!has_welcome_) {
            push_event(KernelEventType_Error, 0, 0, 8);
            return;
        }

        const PlayerInput input_to_send = prepare_client_input(input);
        predict_local_input(input_to_send);
        const std::vector<std::uint8_t> packet =
            encode_input_packet(local_client_peer_id_, input_to_send, next_packet_sequence_++);
        if (!transport_->Send(
                kServerPeerId,
                packet.data(),
                static_cast<std::uint32_t>(packet.size()),
                SendMode::kUnreliable,
                ChannelId::kInput)) {
            push_event(KernelEventType_Error, 0, local_client_peer_id_, 4);
        }
        return;
    }

    pending_inputs_.push_back(QueuedInput{local_player_id, input, tick_loop_.current_tick()});
    local_last_processed_input_seq_ =
        std::max(local_last_processed_input_seq_, input.input_seq);
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

KernelLocalPlayerInfo KernelEngine::local_player_info() const {
    return KernelLocalPlayerInfo{
        local_client_peer_id_,
        local_player_net_id_,
        has_welcome_,
        running_ && has_welcome_ && local_client_peer_id_ != 0 &&
            local_player_net_id_ != 0,
    };
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
            if (config_.mode == KernelMode_Client) {
                send_client_handshake();
            }
        } else if (transport_event.type == TransportEventType::kDisconnected) {
            if (config_.mode == KernelMode_DedicatedServer) {
                handle_server_disconnect(transport_event);
            } else if (config_.mode == KernelMode_Client) {
                handle_client_disconnect(transport_event.peer);
            } else {
                const PeerSession* session = find_session(transport_event.peer);
                if (session != nullptr) {
                    push_event(KernelEventType_PlayerLeft, session->player, transport_event.peer);
                    remove_session(transport_event.peer);
                }
                push_event(KernelEventType_Disconnected, 0, transport_event.peer);
            }
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kSession &&
            config_.mode == KernelMode_DedicatedServer) {
            handle_server_handshake(transport_event);
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kSession &&
            config_.mode == KernelMode_Client) {
            handle_client_session_message(transport_event);
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kReliableEvent &&
            config_.mode == KernelMode_Client) {
            handle_client_reliable_event(transport_event);
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kSnapshot &&
            config_.mode == KernelMode_Client) {
            WorldSnapshot snapshot;
            if (!decode_snapshot_packet(
                    transport_event.payload.data(),
                    transport_event.payload.size(),
                    &snapshot)) {
                push_event(KernelEventType_Error, 0, transport_event.peer, 6);
                continue;
            }
            handle_client_snapshot(std::move(snapshot));
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kInput &&
            (config_.mode == KernelMode_DedicatedServer ||
             config_.mode == KernelMode_ListenServer)) {
            const PeerSession* session = find_session(transport_event.peer);
            if (config_.mode == KernelMode_DedicatedServer &&
                (session == nullptr || !session->welcomed)) {
                push_event(KernelEventType_Error, 0, transport_event.peer, 10);
                continue;
            }

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
            if (player_id != transport_event.peer) {
                push_event(KernelEventType_Error, 0, transport_event.peer, 11);
                continue;
            }
            pending_inputs_.push_back(QueuedInput{
                player_id,
                input,
                tick_loop_.current_tick(),
            });
            PeerSession* mutable_session = find_session(transport_event.peer);
            if (mutable_session != nullptr) {
                mutable_session->last_processed_input_seq =
                    std::max(mutable_session->last_processed_input_seq, input.input_seq);
            } else if (config_.mode == KernelMode_ListenServer) {
                local_last_processed_input_seq_ =
                    std::max(local_last_processed_input_seq_, input.input_seq);
            }
        }
    }
}

void KernelEngine::handle_server_disconnect(const TransportEvent& transport_event) {
    const PeerSession* session = find_session(transport_event.peer);
    if (session == nullptr) {
        push_event(KernelEventType_Disconnected, 0, transport_event.peer);
        return;
    }

    const KernelEvent player_left{
        KernelEventType_PlayerLeft,
        tick_loop_.current_tick(),
        session->player,
        transport_event.peer,
        0,
    };
    events_.push_back(player_left);

    // Current gameplay removes the disconnected player immediately. A later
    // policy may preserve selected entities while clearing transient state.
    if (world_.destroy(session->player)) {
        push_event(KernelEventType_EntityDestroyed, session->player, transport_event.peer);
    }

    remove_session(transport_event.peer);
    broadcast_reliable_event(player_left);
    publish_snapshot();
    push_event(KernelEventType_Disconnected, 0, transport_event.peer);
}

void KernelEngine::handle_client_disconnect(PeerId peer) {
    clear_client_session();
    push_event(KernelEventType_Disconnected, 0, peer);
}

void KernelEngine::handle_client_reliable_event(const TransportEvent& transport_event) {
    KernelEvent event{};
    if (!decode_reliable_event_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &event)) {
        push_event(KernelEventType_Error, 0, transport_event.peer, 14);
        return;
    }

    events_.push_back(event);
}

void KernelEngine::clear_client_session() {
    local_client_peer_id_ = 0;
    local_player_net_id_ = 0;
    local_last_processed_input_seq_ = 0;
    pending_prediction_inputs_.clear();
    client_snapshot_buffer_.clear();
    latest_client_snapshot_ = WorldSnapshot{};
    predicted_local_entity_ = EntitySnapshot{};
    local_correction_offset_ = glm::vec3{0.0f, 0.0f, 0.0f};
    has_welcome_ = false;
    has_client_snapshot_ = false;
    has_predicted_local_entity_ = false;
    render_states_.clear();
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
        handle_client_snapshot(std::move(snapshot));
    }
}

void KernelEngine::handle_client_snapshot(WorldSnapshot snapshot) {
    latest_client_snapshot_ = snapshot;
    has_client_snapshot_ = true;
    store_client_snapshot(std::move(snapshot));
    reconcile_local_prediction(latest_client_snapshot_);
}

void KernelEngine::store_client_snapshot(WorldSnapshot snapshot) {
    auto existing = std::find_if(
        client_snapshot_buffer_.begin(),
        client_snapshot_buffer_.end(),
        [&snapshot](const WorldSnapshot& buffered) {
            return buffered.header.server_tick == snapshot.header.server_tick;
        });
    if (existing != client_snapshot_buffer_.end()) {
        *existing = std::move(snapshot);
    } else {
        client_snapshot_buffer_.push_back(std::move(snapshot));
    }

    std::sort(
        client_snapshot_buffer_.begin(),
        client_snapshot_buffer_.end(),
        [](const WorldSnapshot& lhs, const WorldSnapshot& rhs) {
            return lhs.header.server_tick < rhs.header.server_tick;
        });
    constexpr std::size_t kMaxClientSnapshots = 32;
    if (client_snapshot_buffer_.size() > kMaxClientSnapshots) {
        client_snapshot_buffer_.erase(
            client_snapshot_buffer_.begin(),
            client_snapshot_buffer_.begin() +
                static_cast<std::ptrdiff_t>(
                    client_snapshot_buffer_.size() - kMaxClientSnapshots));
    }
}

void KernelEngine::reconcile_local_prediction(const WorldSnapshot& snapshot) {
    if (local_player_net_id_ == 0) {
        return;
    }

    const EntitySnapshot* authoritative =
        find_snapshot_entity(snapshot, local_player_net_id_);
    if (authoritative == nullptr) {
        return;
    }

    pending_prediction_inputs_.erase(
        std::remove_if(
            pending_prediction_inputs_.begin(),
            pending_prediction_inputs_.end(),
            [&snapshot](const PlayerInput& input) {
                return input.input_seq <= snapshot.header.last_processed_input_seq;
            }),
        pending_prediction_inputs_.end());

    const glm::vec3 previous_render_position =
        has_predicted_local_entity_
            ? predicted_local_entity_.position + local_correction_offset_
            : authoritative->position;

    EntitySnapshot replayed = *authoritative;
    for (const PlayerInput& input : pending_prediction_inputs_) {
        apply_input_prediction(&replayed, input, tick_loop_.fixed_delta_seconds());
    }

    predicted_local_entity_ = replayed;
    has_predicted_local_entity_ = true;

    const glm::vec3 correction = previous_render_position - predicted_local_entity_.position;
    local_correction_offset_ =
        glm::length(correction) > 2.0f ? glm::vec3{0.0f, 0.0f, 0.0f} : correction;
}

void KernelEngine::predict_local_input(const PlayerInput& input) {
    if (local_player_net_id_ == 0) {
        return;
    }

    if (!has_predicted_local_entity_) {
        if (const EntitySnapshot* latest =
                find_snapshot_entity(latest_client_snapshot_, local_player_net_id_)) {
            predicted_local_entity_ = *latest;
        } else {
            predicted_local_entity_.net_id = local_player_net_id_;
            predicted_local_entity_.type = EntityType::kPlayer;
        }
        has_predicted_local_entity_ = true;
    }

    apply_input_prediction(
        &predicted_local_entity_,
        input,
        tick_loop_.fixed_delta_seconds());
    pending_prediction_inputs_.push_back(input);
}

PlayerInput KernelEngine::prepare_client_input(const PlayerInput& input) {
    PlayerInput input_to_send = input;
    if (input_to_send.client_tick == 0) {
        input_to_send.client_tick = std::max(1u, predicted_server_tick_);
    }
    predicted_server_tick_ =
        std::max(predicted_server_tick_, input_to_send.client_tick + 1);
    return input_to_send;
}

bool KernelEngine::build_interpolated_snapshot(WorldSnapshot* out_snapshot) const {
    if (out_snapshot == nullptr || client_snapshot_buffer_.empty()) {
        return false;
    }
    if (client_snapshot_buffer_.size() == 1) {
        *out_snapshot = client_snapshot_buffer_.back();
        return true;
    }

    const std::uint32_t newest_tick =
        client_snapshot_buffer_.back().header.server_tick;
    const std::uint32_t interpolation_delay =
        tick_loop_.snapshot_interval_ticks() * 2u;
    const std::uint32_t target_tick =
        newest_tick > interpolation_delay
            ? newest_tick - interpolation_delay
            : client_snapshot_buffer_.front().header.server_tick;

    const WorldSnapshot* from = &client_snapshot_buffer_.front();
    const WorldSnapshot* to = &client_snapshot_buffer_.back();
    for (std::size_t index = 0; index < client_snapshot_buffer_.size(); ++index) {
        const WorldSnapshot& snapshot = client_snapshot_buffer_[index];
        if (snapshot.header.server_tick <= target_tick) {
            from = &snapshot;
        }
        if (snapshot.header.server_tick >= target_tick) {
            to = &snapshot;
            break;
        }
    }

    if (from->header.server_tick == to->header.server_tick) {
        *out_snapshot = *from;
        return true;
    }

    const float alpha =
        static_cast<float>(target_tick - from->header.server_tick) /
        static_cast<float>(to->header.server_tick - from->header.server_tick);
    WorldSnapshot interpolated;
    interpolated.header = to->header;
    interpolated.header.server_tick = target_tick;
    interpolated.entities.reserve(to->entities.size());
    for (const EntitySnapshot& from_entity : from->entities) {
        if (const EntitySnapshot* to_entity = find_snapshot_entity(*to, from_entity.net_id)) {
            interpolated.entities.push_back(
                interpolate_entity(from_entity, *to_entity, alpha));
        } else {
            interpolated.entities.push_back(from_entity);
        }
    }
    for (const EntitySnapshot& to_entity : to->entities) {
        if (find_snapshot_entity(*from, to_entity.net_id) == nullptr) {
            interpolated.entities.push_back(to_entity);
        }
    }

    *out_snapshot = std::move(interpolated);
    return true;
}

void KernelEngine::append_predicted_local_render_state() {
    if (!has_predicted_local_entity_) {
        return;
    }

    EntitySnapshot local = predicted_local_entity_;
    local.position += local_correction_offset_;
    render_states_.push_back(RenderEntityState{
        local.net_id,
        static_cast<std::uint16_t>(local.type),
        to_kernel_vec3(local.position),
        to_kernel_quat(local.rotation),
        local.state,
        local.flags,
    });

    local_correction_offset_ *= 0.5f;
    if (glm::length(local_correction_offset_) < 0.001f) {
        local_correction_offset_ = glm::vec3{0.0f, 0.0f, 0.0f};
    }
}

std::uint32_t KernelEngine::rewind_tick_for_input(
    const QueuedInput& queued_input) const {
    if (queued_input.input.input_seq == 0 || queued_input.input.client_tick == 0 ||
        queued_input.input.client_tick > queued_input.received_server_tick) {
        return queued_input.received_server_tick;
    }

    const std::uint32_t observed_input_delay =
        queued_input.received_server_tick - queued_input.input.client_tick;
    const std::uint32_t estimated_rtt_ticks = observed_input_delay * 2u;
    const std::uint32_t one_way_ticks = estimated_rtt_ticks / 2u;
    if (one_way_ticks >= queued_input.received_server_tick) {
        return 0;
    }
    return queued_input.received_server_tick - one_way_ticks;
}

void KernelEngine::simulate_tick() {
    const float fixed_delta = tick_loop_.fixed_delta_seconds();
    simulate_player_movement(world_, pending_inputs_, fixed_delta);
    simulate_weapons(world_, {}, tick_loop_.current_tick(), &events_);
    for (const QueuedInput& pending_input : pending_inputs_) {
        const HistoryFrame* rewind_frame = nullptr;
        if ((pending_input.input.buttons & InputButton_Fire) != 0) {
            rewind_frame =
                history_buffer_.find_frame_clamped(rewind_tick_for_input(pending_input));
        }
        const std::vector<QueuedInput> single_input{pending_input};
        simulate_weapons(
            world_,
            single_input,
            tick_loop_.current_tick(),
            &events_,
            rewind_frame);
    }
    simulate_projectiles(world_, fixed_delta, tick_loop_.current_tick(), &events_);
    destroy_dead_entities(world_, tick_loop_.current_tick(), &events_);
    history_buffer_.write_frame(world_, tick_loop_.current_tick());
    if (tick_loop_.should_write_snapshot()) {
        publish_snapshot();
    }
    pending_inputs_.clear();
    tick_loop_.advance_tick();
}

void KernelEngine::rebuild_render_states() {
    if ((config_.mode == KernelMode_ListenServer || config_.mode == KernelMode_Client) &&
        has_client_snapshot_) {
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
    append_predicted_local_render_state();

    WorldSnapshot render_snapshot;
    const WorldSnapshot& snapshot =
        build_interpolated_snapshot(&render_snapshot) ? render_snapshot : latest_client_snapshot_;
    for (const EntitySnapshot& entity : snapshot.entities) {
        if (has_predicted_local_entity_ && entity.net_id == local_player_net_id_) {
            continue;
        }
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
        local_last_processed_input_seq_);
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

    if (config_.mode == KernelMode_DedicatedServer) {
        for (const PeerSession& session : peer_sessions_) {
            if (!session.welcomed) {
                continue;
            }
            const WorldSnapshot peer_snapshot = build_world_snapshot(
                world_,
                tick_loop_.current_tick(),
                static_cast<std::uint32_t>(
                    tick_loop_.current_tick() *
                    tick_loop_.fixed_delta_seconds() * 1000.0f),
                session.last_processed_input_seq);
            const std::vector<std::uint8_t> packet =
                encode_snapshot_packet(peer_snapshot, next_packet_sequence_++);
            if (!transport_->Send(
                    session.peer,
                    packet.data(),
                    static_cast<std::uint32_t>(packet.size()),
                    SendMode::kUnreliable,
                    ChannelId::kSnapshot)) {
                push_event(KernelEventType_Error, 0, session.peer, 7);
            }
        }
    }
}

void KernelEngine::send_client_handshake() {
    if (client_handshake_sent_) {
        return;
    }

    const std::vector<std::uint8_t> packet = encode_handshake_packet(
        HandshakePacket{kClientNonce},
        next_packet_sequence_++);
    if (!transport_->Send(
            kServerPeerId,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kSession)) {
        push_event(KernelEventType_Error, 0, kServerPeerId, 9);
        return;
    }
    client_handshake_sent_ = true;
}

void KernelEngine::send_reliable_event(PeerId peer, const KernelEvent& event) {
    const std::vector<std::uint8_t> packet =
        encode_reliable_event_packet(event, next_packet_sequence_++);
    if (!transport_->Send(
            peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        push_event(KernelEventType_Error, event.net_id, peer, 15);
    }
}

void KernelEngine::broadcast_reliable_event(const KernelEvent& event) {
    for (const PeerSession& session : peer_sessions_) {
        if (!session.welcomed) {
            continue;
        }
        send_reliable_event(session.peer, event);
    }
}

void KernelEngine::handle_server_handshake(const TransportEvent& transport_event) {
    HandshakePacket handshake;
    if (!decode_handshake_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &handshake)) {
        push_event(KernelEventType_Error, 0, transport_event.peer, 9);
        return;
    }

    PeerSession* session = find_session(transport_event.peer);
    if (session == nullptr) {
        const NetId player = world_.spawn_player(
            transport_event.peer,
            glm::vec3{0.0f, 0.0f, 0.0f});
        peer_sessions_.push_back(PeerSession{transport_event.peer, player, 0, false});
        session = &peer_sessions_.back();
        push_event(KernelEventType_PlayerJoined, player, transport_event.peer);
        push_event(KernelEventType_EntitySpawned, player, transport_event.peer);
    }

    const WelcomePacket welcome{
        transport_event.peer,
        session->player,
        tick_loop_.current_tick(),
        config_.tick.server_tick_rate,
        config_.tick.snapshot_rate,
    };
    const std::vector<std::uint8_t> packet =
        encode_welcome_packet(welcome, next_packet_sequence_++);
    if (!transport_->Send(
            transport_event.peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kSession)) {
        push_event(KernelEventType_Error, 0, transport_event.peer, 12);
        return;
    }

    session->welcomed = true;
    publish_snapshot();
}

void KernelEngine::handle_client_session_message(const TransportEvent& transport_event) {
    WelcomePacket welcome;
    if (decode_welcome_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &welcome)) {
        local_client_peer_id_ = welcome.assigned_peer_id;
        local_player_net_id_ = welcome.assigned_player_net_id;
        predicted_server_tick_ = welcome.server_tick;
        has_welcome_ = true;
        push_event(KernelEventType_PlayerJoined, 0, local_client_peer_id_);
        return;
    }

    DisconnectPacket disconnect;
    if (decode_disconnect_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &disconnect)) {
        clear_client_session();
        push_event(KernelEventType_Disconnected, 0, kServerPeerId, disconnect.reason_code);
        return;
    }

    push_event(KernelEventType_Error, 0, transport_event.peer, 13);
}

KernelEngine::PeerSession* KernelEngine::find_session(PeerId peer) {
    auto found = std::find_if(
        peer_sessions_.begin(),
        peer_sessions_.end(),
        [peer](const PeerSession& session) {
            return session.peer == peer;
        });
    if (found == peer_sessions_.end()) {
        return nullptr;
    }
    return &(*found);
}

const KernelEngine::PeerSession* KernelEngine::find_session(PeerId peer) const {
    auto found = std::find_if(
        peer_sessions_.begin(),
        peer_sessions_.end(),
        [peer](const PeerSession& session) {
            return session.peer == peer;
        });
    if (found == peer_sessions_.end()) {
        return nullptr;
    }
    return &(*found);
}

void KernelEngine::remove_session(PeerId peer) {
    peer_sessions_.erase(
        std::remove_if(
            peer_sessions_.begin(),
            peer_sessions_.end(),
            [peer](const PeerSession& session) {
                return session.peer == peer;
            }),
        peer_sessions_.end());
}

}  // namespace network_example

struct KernelHandle {
    std::unique_ptr<network_example::KernelEngine> engine;
};

extern "C" {

KernelHandle* Kernel_Create(const KernelConfig* config) {
    try {
        if (config == nullptr) {
            return nullptr;
        }
        auto* handle = new KernelHandle;
        handle->engine = std::make_unique<network_example::KernelEngine>(*config);
        return handle;
    } catch (...) {
        return nullptr;
    }
}

void Kernel_Destroy(KernelHandle* kernel) {
    try {
        delete kernel;
    } catch (...) {
    }
}

bool Kernel_GetAbiInfo(KernelAbiInfo* out_info, uint32_t out_info_size) {
    try {
        if (out_info == nullptr || out_info_size < sizeof(KernelAbiInfo)) {
            return false;
        }
        std::memset(out_info, 0, sizeof(KernelAbiInfo));
        out_info->struct_size = sizeof(KernelAbiInfo);
        out_info->abi_version = KERNEL_ABI_VERSION;
        out_info->kernel_config_size = sizeof(KernelConfig);
        out_info->player_input_size = sizeof(PlayerInput);
        out_info->render_entity_state_size = sizeof(RenderEntityState);
        out_info->kernel_event_size = sizeof(KernelEvent);
        out_info->local_player_info_size = sizeof(KernelLocalPlayerInfo);
        out_info->capability_flags =
            KERNEL_CAPABILITY_CLIENT_MODE |
            KERNEL_CAPABILITY_LISTEN_SERVER_MODE |
            KERNEL_CAPABILITY_DEDICATED_SERVER_MODE |
            KERNEL_CAPABILITY_INPUT_SUBMISSION |
            KERNEL_CAPABILITY_RENDER_STATES |
            KERNEL_CAPABILITY_EVENT_POLLING |
            KERNEL_CAPABILITY_CLIENT_PREDICTION |
            KERNEL_CAPABILITY_SNAPSHOT_INTERPOLATION |
            KERNEL_CAPABILITY_LAG_COMPENSATED_HITSCAN |
            KERNEL_CAPABILITY_LOCAL_PLAYER_INFO;
        return true;
    } catch (...) {
        return false;
    }
}

bool Kernel_GetLocalPlayerInfo(
    KernelHandle* kernel,
    KernelLocalPlayerInfo* out_info) {
    try {
        if (kernel == nullptr || out_info == nullptr) {
            return false;
        }
        *out_info = kernel->engine->local_player_info();
        return true;
    } catch (...) {
        return false;
    }
}

bool Kernel_StartClient(KernelHandle* kernel, const char* address) {
    try {
        return kernel != nullptr && address != nullptr && address[0] != '\0' &&
               kernel->engine->start_client(address);
    } catch (...) {
        return false;
    }
}

bool Kernel_StartListenServer(KernelHandle* kernel, uint16_t port) {
    try {
        return kernel != nullptr && kernel->engine->start_listen_server(port);
    } catch (...) {
        return false;
    }
}

bool Kernel_StartDedicatedServer(KernelHandle* kernel, uint16_t port) {
    try {
        return kernel != nullptr && kernel->engine->start_dedicated_server(port);
    } catch (...) {
        return false;
    }
}

void Kernel_Update(KernelHandle* kernel, float delta_seconds) {
    try {
        if (kernel != nullptr) {
            kernel->engine->update(delta_seconds);
        }
    } catch (...) {
    }
}

void Kernel_SubmitInput(
    KernelHandle* kernel,
    uint32_t local_player_id,
    const PlayerInput* input) {
    try {
        if (kernel != nullptr && input != nullptr) {
            kernel->engine->submit_input(local_player_id, *input);
        }
    } catch (...) {
    }
}

uint32_t Kernel_GetRenderStates(
    KernelHandle* kernel,
    RenderEntityState* out_states,
    uint32_t max_states) {
    try {
        if (kernel == nullptr) {
            return 0;
        }
        return kernel->engine->get_render_states(out_states, max_states);
    } catch (...) {
        return 0;
    }
}

uint32_t Kernel_PollEvents(
    KernelHandle* kernel,
    KernelEvent* out_events,
    uint32_t max_events) {
    try {
        if (kernel == nullptr) {
            return 0;
        }
        return kernel->engine->poll_events(out_events, max_events);
    } catch (...) {
        return 0;
    }
}

}  // extern "C"
