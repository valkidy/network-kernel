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
#include "protocol/public/network_packets.h"
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

glm::vec3 from_kernel_vec3(const KernelVec3& value) {
    return glm::vec3{value.x, value.y, value.z};
}

glm::quat from_kernel_quat(const KernelQuat& value) {
    return glm::quat{value.w, value.x, value.y, value.z};
}

bool is_server_mode(KernelMode mode) {
    return mode == KernelMode_DedicatedServer || mode == KernelMode_ListenServer;
}

bool is_authoritative_combat_event(KernelEventType type) {
    return type == KernelEventType_FireConfirmed ||
           type == KernelEventType_HitConfirmed ||
           type == KernelEventType_DamageApplied ||
           type == KernelEventType_Explosion;
}

std::uint32_t derived_visual_flags(const World& world, entt::entity entity) {
    std::uint32_t flags = 0;
    if (world.registry().all_of<Velocity>(entity) &&
        glm::length(world.registry().get<Velocity>(entity).linear) > 0.001f) {
        flags |= kVisualFlagMoving;
    }
    if (world.registry().all_of<WeaponState>(entity) &&
        world.registry().get<WeaponState>(entity).is_reloading) {
        flags |= kVisualFlagReloading;
    }
    if (world.registry().all_of<Health>(entity) &&
        world.registry().get<Health>(entity).hp == 0) {
        flags |= kVisualFlagDead;
    }
    return flags;
}

glm::vec3 input_move_to_world(const PlayerInput& input) {
    glm::vec3 move{input.move.x, 0.0f, input.move.y};
    const float length = glm::length(move);
    if (length > 1.0f) {
        move /= length;
    }
    return move;
}

glm::vec3 input_aim_to_world(const PlayerInput& input) {
    glm::vec3 aim{input.aim_dir.x, input.aim_dir.y, input.aim_dir.z};
    if (glm::length(aim) <= 0.0001f) {
        return glm::vec3{1.0f, 0.0f, 0.0f};
    }
    return glm::normalize(aim);
}

std::uint64_t tick_time_us(std::uint32_t tick, float fixed_delta_seconds) {
    return static_cast<std::uint64_t>(
        static_cast<double>(tick) * static_cast<double>(fixed_delta_seconds) *
        1000000.0);
}

std::uint32_t tick_for_time_us(
    std::uint64_t time_us,
    float fixed_delta_seconds) {
    const double tick =
        static_cast<double>(time_us) /
        (static_cast<double>(fixed_delta_seconds) * 1000000.0);
    return static_cast<std::uint32_t>(tick);
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

KernelServerEntityState to_server_entity_state(
    const World& world,
    entt::entity entity) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(KernelServerEntityState);

    const NetworkIdentity& identity = world.registry().get<NetworkIdentity>(entity);
    const EntityKind& kind = world.registry().get<EntityKind>(entity);
    const Transform& transform = world.registry().get<Transform>(entity);
    state.net_id = identity.net_id;
    state.entity_type = static_cast<std::uint16_t>(kind.type);
    state.owner_peer = identity.owner_peer;
    state.position = to_kernel_vec3(transform.position);
    state.rotation = to_kernel_quat(transform.rotation);
    state.valid = true;

    if (world.registry().all_of<Velocity>(entity)) {
        state.velocity =
            to_kernel_vec3(world.registry().get<Velocity>(entity).linear);
    }
    if (world.registry().all_of<Health>(entity)) {
        state.hp = world.registry().get<Health>(entity).hp;
    }
    state.visual_flags = derived_visual_flags(world, entity);
    if (world.registry().all_of<ReplicationState>(entity)) {
        const ReplicationState& replication =
            world.registry().get<ReplicationState>(entity);
        state.animation_state = replication.animation_state;
        state.visual_flags |= replication.visual_flags;
    }
    return state;
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
constexpr float kPredictedProjectileSpeedMetersPerSecond = 15.0f;
constexpr std::uint32_t kMaxCompensationWindowUs = 100000u;

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
    reset_runtime_state(KernelMode_Client);
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
    reset_runtime_state(KernelMode_ListenServer);

    const NetId player =
        world_.spawn_player(kLocalListenPeerId, glm::vec3{0.0f, 0.0f, 0.0f});
    local_client_peer_id_ = kLocalListenPeerId;
    local_player_net_id_ = player;
    local_listen_session_ = PeerSession{kLocalListenPeerId, player, 0, true, {}};
    has_welcome_ = true;

    push_event(KernelEventType_Connected, 0, kLocalListenPeerId);
    push_event(KernelEventType_PlayerJoined, player, kLocalListenPeerId);
    push_event(KernelEventType_EntitySpawned, player, kLocalListenPeerId);
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
    reset_runtime_state(KernelMode_DedicatedServer);
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
        predict_local_projectile(input_to_send);
        rebuild_render_states();
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
        predict_local_projectile(input_to_send);
        rebuild_render_states();
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
    release_presentable_events();
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

bool KernelEngine::server_create_entity(
    const KernelServerEntityCreateInfo& create_info,
    NetId* out_net_id) {
    if (!running_ || !is_server_mode(config_.mode) || out_net_id == nullptr ||
        create_info.struct_size < sizeof(KernelServerEntityCreateInfo)) {
        return false;
    }

    const EntityType type = static_cast<EntityType>(create_info.entity_type);
    NetId net_id = 0;
    if (type == EntityType::kEnemy) {
        net_id = world_.spawn_enemy(from_kernel_vec3(create_info.position));
    } else {
        return false;
    }

    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    Transform& transform = world_.registry().get<Transform>(*entity);
    transform.rotation = from_kernel_quat(create_info.rotation);
    ReplicationState& replication =
        world_.registry().get_or_emplace<ReplicationState>(*entity);
    replication.animation_state = create_info.animation_state;
    replication.visual_flags = create_info.visual_flags;

    *out_net_id = net_id;
    push_event(KernelEventType_EntitySpawned, net_id, create_info.owner_peer);
    publish_snapshot();
    return true;
}

bool KernelEngine::server_destroy_entity(NetId net_id, std::uint32_t reason) {
    if (!running_ || !is_server_mode(config_.mode) || net_id == 0) {
        return false;
    }
    if (!world_.destroy(net_id)) {
        return false;
    }
    if (config_.mode == KernelMode_ListenServer && loopback_transport_ != nullptr) {
        send_entity_despawn(kLocalListenPeerId, net_id, reason);
    }
    for (PeerSession& session : peer_sessions_) {
        if (session.relevant_entities.erase(net_id) > 0) {
            send_entity_despawn(session.peer, net_id, reason);
        }
    }
    push_event(KernelEventType_EntityDestroyed, net_id, 0, reason);
    publish_snapshot();
    return true;
}

bool KernelEngine::server_set_entity_transform(
    NetId net_id,
    const KernelVec3& position,
    const KernelQuat& rotation) {
    if (!running_ || !is_server_mode(config_.mode)) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() || !world_.registry().all_of<Transform>(*entity)) {
        return false;
    }
    Transform& transform = world_.registry().get<Transform>(*entity);
    transform.position = from_kernel_vec3(position);
    transform.rotation = from_kernel_quat(rotation);
    return true;
}

bool KernelEngine::server_set_entity_velocity(
    NetId net_id,
    const KernelVec3& velocity) {
    if (!running_ || !is_server_mode(config_.mode)) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() || !world_.registry().all_of<Velocity>(*entity)) {
        return false;
    }
    world_.registry().get<Velocity>(*entity).linear = from_kernel_vec3(velocity);
    return true;
}

bool KernelEngine::server_set_entity_state(
    NetId net_id,
    std::uint16_t animation_state,
    std::uint32_t visual_flags) {
    if (!running_ || !is_server_mode(config_.mode)) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    ReplicationState& replication =
        world_.registry().get_or_emplace<ReplicationState>(*entity);
    replication.animation_state = animation_state;
    replication.visual_flags = visual_flags;
    return true;
}

bool KernelEngine::server_get_entity_state(
    NetId net_id,
    KernelServerEntityState* out_state) const {
    if (!running_ || !is_server_mode(config_.mode) || out_state == nullptr ||
        out_state->struct_size < sizeof(KernelServerEntityState)) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    *out_state = to_server_entity_state(world_, *entity);
    return true;
}

std::uint32_t KernelEngine::server_query_entities(
    EntityType entity_type_filter,
    KernelServerEntityState* out_states,
    std::uint32_t max_states) const {
    if (!running_ || !is_server_mode(config_.mode) || out_states == nullptr ||
        max_states == 0) {
        return 0;
    }

    std::uint32_t count = 0;
    auto view =
        world_.registry().view<const NetworkIdentity, const EntityKind, const Transform>();
    for (const entt::entity entity : view) {
        const EntityKind& kind = view.get<const EntityKind>(entity);
        if (entity_type_filter != EntityType::kUnknown &&
            kind.type != entity_type_filter) {
            continue;
        }
        if (count >= max_states) {
            break;
        }
        out_states[count++] = to_server_entity_state(world_, entity);
    }
    return count;
}

void KernelEngine::push_event(
    KernelEventType type,
    NetId net_id,
    PeerId peer_id,
    std::uint32_t code) {
    events_.push_back(KernelEvent{type, tick_loop_.current_tick(), net_id, peer_id, code});
}

void KernelEngine::reset_runtime_state(KernelMode mode) {
    config_.mode = mode;
    tick_loop_ = TickLoop(config_.tick);
    world_ = World{};
    history_buffer_ = HistoryBuffer(history_frame_count(config_.tick));
    damage_pipeline_.clear();
    pending_inputs_.clear();
    events_.clear();
    pending_presentation_events_.clear();
    render_states_.clear();
    latest_snapshot_ = WorldSnapshot{};
    latest_client_snapshot_ = WorldSnapshot{};
    client_snapshot_buffer_.clear();
    peer_sessions_.clear();
    local_listen_session_ = PeerSession{};
    client_replicated_entities_.clear();
    client_despawned_entities_.clear();
    pending_prediction_inputs_.clear();
    predicted_projectiles_.clear();
    entity_ids_by_net_id_.clear();
    predicted_local_entity_ = EntitySnapshot{};
    local_correction_offset_ = glm::vec3{0.0f, 0.0f, 0.0f};
    next_entity_id_ = 1;
    next_predicted_entity_id_ = UINT64_C(0x8000000000000000);
    local_player_net_id_ = 0;
    local_last_processed_input_seq_ = 0;
    predicted_server_tick_ = tick_loop_.current_tick();
    next_packet_sequence_ = 1;
    current_render_time_us_ = 0;
    local_client_peer_id_ = 0;
    client_handshake_sent_ = false;
    has_welcome_ = false;
    has_client_snapshot_ = false;
    has_predicted_local_entity_ = false;
    has_client_render_time_ = false;
    running_ = true;
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
    if (decode_reliable_event_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &event)) {
        if (event.presentation_time_us != 0) {
            pending_presentation_events_.push_back(event);
            return;
        }
        events_.push_back(event);
        return;
    }

    EntitySpawnPacket spawn{};
    if (decode_entity_spawn_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &spawn)) {
        handle_client_spawn(spawn);
        return;
    }

    EntityDespawnPacket despawn{};
    if (decode_entity_despawn_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &despawn)) {
        handle_client_despawn(despawn);
        return;
    }

    push_event(KernelEventType_Error, 0, transport_event.peer, 14);
}

void KernelEngine::handle_client_spawn(const EntitySpawnPacket& packet) {
    client_despawned_entities_.erase(packet.net_id);
    auto found = std::find_if(
        client_replicated_entities_.begin(),
        client_replicated_entities_.end(),
        [&packet](const ClientReplicatedEntity& entity) {
            return entity.net_id == packet.net_id;
        });
    if (found == client_replicated_entities_.end()) {
        client_replicated_entities_.push_back(ClientReplicatedEntity{
            packet.net_id,
            packet.entity_type,
            packet.owner_peer,
            packet.position,
            packet.rotation,
            false,
        });
    } else {
        found->type = packet.entity_type;
        found->owner_peer = packet.owner_peer;
        found->position = packet.position;
        found->rotation = packet.rotation;
        found->active = false;
    }
    events_.push_back(KernelEvent{
        KernelEventType_EntitySpawned,
        packet.server_tick,
        packet.net_id,
        packet.owner_peer,
        static_cast<std::uint32_t>(packet.entity_type),
    });
}

void KernelEngine::handle_client_despawn(const EntityDespawnPacket& packet) {
    client_despawned_entities_.insert(packet.net_id);
    client_replicated_entities_.erase(
        std::remove_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [&packet](const ClientReplicatedEntity& entity) {
                return entity.net_id == packet.net_id;
            }),
        client_replicated_entities_.end());
    events_.push_back(KernelEvent{
        KernelEventType_EntityDestroyed,
        packet.server_tick,
        packet.net_id,
        0,
        packet.reason,
    });
}

void KernelEngine::clear_client_session() {
    local_client_peer_id_ = 0;
    local_player_net_id_ = 0;
    local_last_processed_input_seq_ = 0;
    pending_prediction_inputs_.clear();
    predicted_projectiles_.clear();
    pending_presentation_events_.clear();
    client_snapshot_buffer_.clear();
    client_replicated_entities_.clear();
    client_despawned_entities_.clear();
    latest_client_snapshot_ = WorldSnapshot{};
    predicted_local_entity_ = EntitySnapshot{};
    local_correction_offset_ = glm::vec3{0.0f, 0.0f, 0.0f};
    has_welcome_ = false;
    has_client_snapshot_ = false;
    has_predicted_local_entity_ = false;
    has_client_render_time_ = false;
    current_render_time_us_ = 0;
    render_states_.clear();
}

void KernelEngine::release_presentable_events() {
    if (!has_client_render_time_ || pending_presentation_events_.empty()) {
        return;
    }

    std::vector<KernelEvent> still_pending;
    still_pending.reserve(pending_presentation_events_.size());
    for (const KernelEvent& event : pending_presentation_events_) {
        if (event.presentation_time_us <= current_render_time_us_) {
            events_.push_back(event);
        } else {
            still_pending.push_back(event);
        }
    }
    pending_presentation_events_ = std::move(still_pending);
}

void KernelEngine::poll_client_transport() {
    if (loopback_transport_ == nullptr) {
        return;
    }

    TransportEvent transport_event;
    while (loopback_transport_->PollClientEvent(transport_event)) {
        if (transport_event.type != TransportEventType::kMessage) {
            continue;
        }
        if (transport_event.channel == ChannelId::kReliableEvent) {
            handle_client_reliable_event(transport_event);
            continue;
        }
        if (transport_event.channel != ChannelId::kSnapshot) {
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
    reconcile_predicted_projectiles(latest_client_snapshot_);
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

void KernelEngine::reconcile_predicted_projectiles(const WorldSnapshot& snapshot) {
    std::unordered_set<NetId> snapshot_projectiles;
    for (const EntitySnapshot& entity : snapshot.entities) {
        if (entity.type != EntityType::kProjectile) {
            continue;
        }
        snapshot_projectiles.insert(entity.net_id);
        if (entity.client_action_id == 0) {
            continue;
        }
        PredictedProjectile* predicted =
            find_predicted_projectile(entity.owner_peer, entity.client_action_id);
        if (predicted == nullptr) {
            continue;
        }
        predicted->net_id = entity.net_id;
        predicted->position = entity.position;
        predicted->rotation = entity.rotation;
        predicted->velocity = entity.velocity;
        predicted->spawn_tick = entity.spawn_tick;
        predicted->bound = true;
        entity_ids_by_net_id_[entity.net_id] = predicted->entity_id;
    }

    predicted_projectiles_.erase(
        std::remove_if(
            predicted_projectiles_.begin(),
            predicted_projectiles_.end(),
            [&snapshot](const PredictedProjectile& projectile) {
                return !projectile.bound &&
                       projectile.input_seq != 0 &&
                       projectile.input_seq <= snapshot.header.last_processed_input_seq;
            }),
        predicted_projectiles_.end());
    predicted_projectiles_.erase(
        std::remove_if(
            predicted_projectiles_.begin(),
            predicted_projectiles_.end(),
            [&snapshot_projectiles](const PredictedProjectile& projectile) {
                return projectile.bound &&
                       snapshot_projectiles.find(projectile.net_id) ==
                           snapshot_projectiles.end();
            }),
        predicted_projectiles_.end());
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

void KernelEngine::predict_local_projectile(const PlayerInput& input) {
    if (local_client_peer_id_ == 0 || input.client_action_id == 0 ||
        (input.buttons & InputButton_Fire) == 0 ||
        input.selected_weapon != kWeaponGrenade ||
        find_predicted_projectile(local_client_peer_id_, input.client_action_id) != nullptr) {
        return;
    }

    glm::vec3 player_position{0.0f, 0.0f, 0.0f};
    if (has_predicted_local_entity_) {
        player_position = predicted_local_entity_.position;
    } else if (
        const EntitySnapshot* latest =
            find_snapshot_entity(latest_client_snapshot_, local_player_net_id_)) {
        player_position = latest->position;
    }

    const glm::vec3 origin = player_position + glm::vec3{0.0f, 1.0f, 0.0f};
    const glm::vec3 direction = input_aim_to_world(input);
    predicted_projectiles_.push_back(PredictedProjectile{
        allocate_predicted_entity_id(),
        0,
        local_client_peer_id_,
        input.input_seq,
        input.client_action_id,
        tick_loop_.current_tick(),
        origin,
        glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
        direction * kPredictedProjectileSpeedMetersPerSecond,
        false,
    });
}

PlayerInput KernelEngine::prepare_client_input(const PlayerInput& input) {
    PlayerInput input_to_send = input;
    if (input_to_send.client_action_time_us == 0) {
        input_to_send.client_action_time_us = tick_time_us(
            std::max(1u, predicted_server_tick_),
            tick_loop_.fixed_delta_seconds());
    }
    predicted_server_tick_ =
        std::max(
            predicted_server_tick_,
            tick_for_time_us(
                input_to_send.client_action_time_us,
                tick_loop_.fixed_delta_seconds()) +
                1u);
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
        entity_id_for_net_id(local.net_id),
        local.net_id,
        static_cast<std::uint16_t>(local.type),
        local.owner_peer,
        to_kernel_vec3(local.position),
        to_kernel_quat(local.rotation),
        to_kernel_vec3(local.velocity),
        local.state,
        local.flags,
        local.spawn_tick,
        local.client_action_id,
    });

    local_correction_offset_ *= 0.5f;
    if (glm::length(local_correction_offset_) < 0.001f) {
        local_correction_offset_ = glm::vec3{0.0f, 0.0f, 0.0f};
    }
}

void KernelEngine::append_predicted_projectile_render_states() {
    for (const PredictedProjectile& projectile : predicted_projectiles_) {
        render_states_.push_back(RenderEntityState{
            projectile.entity_id,
            projectile.net_id,
            static_cast<std::uint16_t>(EntityType::kProjectile),
            projectile.owner_peer,
            to_kernel_vec3(projectile.position),
            to_kernel_quat(projectile.rotation),
            to_kernel_vec3(projectile.velocity),
            0,
            kVisualFlagMoving,
            projectile.spawn_tick,
            projectile.client_action_id,
        });
    }
}

void KernelEngine::advance_predicted_projectiles(float fixed_delta_seconds) {
    for (PredictedProjectile& projectile : predicted_projectiles_) {
        projectile.position += projectile.velocity * fixed_delta_seconds;
    }
}

std::uint32_t KernelEngine::rewind_tick_for_input(
    const QueuedInput& queued_input) const {
    const std::uint64_t received_time_us = tick_time_us(
        queued_input.received_server_tick,
        tick_loop_.fixed_delta_seconds());
    if (queued_input.input.client_action_time_us == 0 ||
        queued_input.input.client_action_time_us >= received_time_us) {
        return queued_input.received_server_tick;
    }

    const std::uint64_t earliest_compensated_time =
        received_time_us > kMaxCompensationWindowUs
            ? received_time_us - kMaxCompensationWindowUs
            : 0;
    const std::uint64_t clamped_action_time =
        std::max(queued_input.input.client_action_time_us, earliest_compensated_time);
    return tick_for_time_us(clamped_action_time, tick_loop_.fixed_delta_seconds());
}

std::uint64_t KernelEngine::entity_id_for_net_id(NetId net_id) {
    if (net_id == 0) {
        return 0;
    }
    auto found = entity_ids_by_net_id_.find(net_id);
    if (found != entity_ids_by_net_id_.end()) {
        return found->second;
    }
    const std::uint64_t entity_id = next_entity_id_++;
    entity_ids_by_net_id_.emplace(net_id, entity_id);
    return entity_id;
}

std::uint64_t KernelEngine::allocate_predicted_entity_id() {
    return next_predicted_entity_id_++;
}

bool KernelEngine::has_predicted_projectile_net_id(NetId net_id) const {
    if (net_id == 0) {
        return false;
    }
    return std::any_of(
        predicted_projectiles_.begin(),
        predicted_projectiles_.end(),
        [net_id](const PredictedProjectile& projectile) {
            return projectile.bound && projectile.net_id == net_id;
        });
}

KernelEngine::PredictedProjectile* KernelEngine::find_predicted_projectile(
    PeerId owner_peer,
    std::uint32_t client_action_id) {
    if (client_action_id == 0) {
        return nullptr;
    }
    auto found = std::find_if(
        predicted_projectiles_.begin(),
        predicted_projectiles_.end(),
        [owner_peer, client_action_id](const PredictedProjectile& projectile) {
            return projectile.owner_peer == owner_peer &&
                   projectile.client_action_id == client_action_id;
        });
    if (found == predicted_projectiles_.end()) {
        return nullptr;
    }
    return &(*found);
}

void KernelEngine::simulate_tick() {
    const float fixed_delta = tick_loop_.fixed_delta_seconds();
    const std::uint64_t server_time_us =
        tick_time_us(tick_loop_.current_tick(), fixed_delta);
    const std::size_t first_tick_event = events_.size();
    advance_predicted_projectiles(fixed_delta);
    for (const QueuedInput& pending_input : pending_inputs_) {
        damage_pipeline_.ingest_defensive_input(
            pending_input.owner_peer,
            pending_input.input,
            server_time_us);
    }
    simulate_player_movement(world_, pending_inputs_, fixed_delta);
    simulate_velocity_movement(world_, fixed_delta);
    simulate_weapons(world_, {}, tick_loop_.current_tick(), &events_);
    for (const QueuedInput& pending_input : pending_inputs_) {
        const HistoryFrame* rewind_frame = nullptr;
        std::uint32_t rewind_tick = tick_loop_.current_tick();
        if ((pending_input.input.buttons & InputButton_Fire) != 0) {
            rewind_tick = rewind_tick_for_input(pending_input);
            rewind_frame = history_buffer_.find_frame_clamped(rewind_tick);
            if (rewind_frame != nullptr) {
                rewind_tick = rewind_frame->server_tick;
            }
        }
        const std::vector<QueuedInput> single_input{pending_input};
        simulate_weapons(
            world_,
            single_input,
            WeaponSimulationContext{
                &history_buffer_,
                rewind_frame,
                rewind_tick,
                tick_loop_.current_tick(),
                fixed_delta,
                pending_input.input.client_action_time_us != 0
                    ? pending_input.input.client_action_time_us
                    : tick_time_us(rewind_tick, fixed_delta)},
            &events_);
    }
    simulate_projectiles(
        world_,
        fixed_delta,
        tick_loop_.current_tick(),
        &events_,
        &damage_pipeline_);
    damage_pipeline_.confirm_ready(
        world_,
        server_time_us,
        tick_loop_.current_tick(),
        &events_);
    destroy_dead_entities(world_, tick_loop_.current_tick(), &events_);
    const std::size_t last_tick_event = events_.size();
    broadcast_combat_events(first_tick_event, last_tick_event);
    history_buffer_.write_frame(world_, tick_loop_.current_tick());
    if (tick_loop_.should_write_snapshot()) {
        publish_snapshot();
    }
    pending_inputs_.clear();
    tick_loop_.advance_tick();
}

WorldSnapshot KernelEngine::build_relevant_snapshot(
    const PeerSession& session,
    std::uint32_t server_time_ms) const {
    WorldSnapshot full_snapshot = build_world_snapshot(
        world_,
        tick_loop_.current_tick(),
        server_time_ms,
        session.last_processed_input_seq);
    const EntitySnapshot* player_entity =
        find_snapshot_entity(full_snapshot, session.player);

    WorldSnapshot filtered;
    filtered.header = full_snapshot.header;
    filtered.entities.reserve(full_snapshot.entities.size());
    for (const EntitySnapshot& entity : full_snapshot.entities) {
        if (is_entity_relevant_to_session(session, entity, player_entity)) {
            filtered.entities.push_back(entity);
        }
    }
    return filtered;
}

bool KernelEngine::is_entity_relevant_to_session(
    const PeerSession& session,
    const EntitySnapshot& entity,
    const EntitySnapshot* player_entity) const {
    if (entity.net_id == session.player) {
        return true;
    }
    if (entity.type == EntityType::kProjectile) {
        const std::optional<entt::entity> world_entity = world_.find_entity(entity.net_id);
        if (world_entity.has_value() &&
            world_.registry().all_of<NetworkIdentity>(*world_entity) &&
            world_.registry().get<NetworkIdentity>(*world_entity).owner_peer == session.peer) {
            return true;
        }
    }
    if (player_entity == nullptr) {
        return false;
    }

    constexpr float kRelevantDistanceMeters = 40.0f;
    constexpr float kProjectileRelevantDistanceMeters = 80.0f;
    const float distance = glm::length(entity.position - player_entity->position);
    if (distance <= kRelevantDistanceMeters) {
        return true;
    }
    if (entity.type == EntityType::kProjectile &&
        distance <= kProjectileRelevantDistanceMeters) {
        const glm::vec3 to_player = player_entity->position - entity.position;
        if (glm::length(entity.velocity) > 0.001f &&
            glm::length(to_player) > 0.001f &&
            glm::dot(glm::normalize(entity.velocity), glm::normalize(to_player)) > 0.5f) {
            return true;
        }
    }
    return false;
}

void KernelEngine::sync_session_relevance(
    PeerSession* session,
    const WorldSnapshot& snapshot) {
    if (session == nullptr || !session->welcomed) {
        return;
    }

    std::unordered_set<NetId> next_relevant;
    for (const EntitySnapshot& entity : snapshot.entities) {
        next_relevant.insert(entity.net_id);
        if (session->relevant_entities.find(entity.net_id) ==
            session->relevant_entities.end()) {
            send_entity_spawn(session->peer, entity);
        }
    }

    for (NetId net_id : session->relevant_entities) {
        if (next_relevant.find(net_id) == next_relevant.end()) {
            send_entity_despawn(
                session->peer,
                net_id,
                KernelDespawnReason_OutOfRange);
        }
    }
    session->relevant_entities = std::move(next_relevant);
}

void KernelEngine::send_entity_spawn(PeerId peer, const EntitySnapshot& entity) {
    PeerId owner_peer = 0;
    const std::optional<entt::entity> world_entity = world_.find_entity(entity.net_id);
    if (world_entity.has_value() &&
        world_.registry().all_of<NetworkIdentity>(*world_entity)) {
        owner_peer = world_.registry().get<NetworkIdentity>(*world_entity).owner_peer;
    }
    const std::vector<std::uint8_t> packet = encode_entity_spawn_packet(
        EntitySpawnPacket{
            entity.net_id,
            entity.type,
            owner_peer,
            tick_loop_.current_tick(),
            entity.position,
            entity.rotation,
        },
        next_packet_sequence_++);
    if (!transport_->Send(
            peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        push_event(KernelEventType_Error, entity.net_id, peer, 16);
    }
}

void KernelEngine::send_entity_despawn(
    PeerId peer,
    NetId net_id,
    std::uint32_t reason) {
    const std::vector<std::uint8_t> packet = encode_entity_despawn_packet(
        EntityDespawnPacket{net_id, tick_loop_.current_tick(), reason},
        next_packet_sequence_++);
    if (!transport_->Send(
            peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        push_event(KernelEventType_Error, net_id, peer, 17);
    }
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
        KernelVec3 velocity{0.0f, 0.0f, 0.0f};
        std::uint16_t animation_state = 0;
        std::uint32_t visual_flags = derived_visual_flags(world_, entity);
        std::uint32_t spawn_tick = 0;
        std::uint32_t client_action_id = 0;
        if (world_.registry().all_of<Velocity>(entity)) {
            velocity = to_kernel_vec3(world_.registry().get<Velocity>(entity).linear);
        }
        if (world_.registry().all_of<ReplicationState>(entity)) {
            const ReplicationState& replication =
                world_.registry().get<ReplicationState>(entity);
            animation_state = replication.animation_state;
            visual_flags |= replication.visual_flags;
        }
        if (world_.registry().all_of<ProjectileState>(entity)) {
            const ProjectileState& projectile =
                world_.registry().get<ProjectileState>(entity);
            spawn_tick = projectile.spawn_tick;
            client_action_id = projectile.client_action_id;
        }
        render_states_.push_back(RenderEntityState{
            entity_id_for_net_id(identity.net_id),
            identity.net_id,
            static_cast<std::uint16_t>(kind.type),
            identity.owner_peer,
            to_kernel_vec3(transform.position),
            to_kernel_quat(transform.rotation),
            velocity,
            animation_state,
            visual_flags,
            spawn_tick,
            client_action_id,
        });
    }
}

void KernelEngine::rebuild_render_states_from_snapshot() {
    render_states_.clear();
    append_predicted_local_render_state();
    append_predicted_projectile_render_states();

    WorldSnapshot render_snapshot;
    const WorldSnapshot& snapshot =
        build_interpolated_snapshot(&render_snapshot) ? render_snapshot : latest_client_snapshot_;
    current_render_time_us_ =
        tick_time_us(snapshot.header.server_tick, tick_loop_.fixed_delta_seconds());
    has_client_render_time_ = true;
    std::unordered_set<NetId> rendered_entities;
    for (const EntitySnapshot& entity : snapshot.entities) {
        if (has_predicted_local_entity_ && entity.net_id == local_player_net_id_) {
            continue;
        }
        if (client_despawned_entities_.find(entity.net_id) !=
            client_despawned_entities_.end()) {
            continue;
        }
        if (has_predicted_projectile_net_id(entity.net_id)) {
            rendered_entities.insert(entity.net_id);
            continue;
        }
        render_states_.push_back(RenderEntityState{
            entity_id_for_net_id(entity.net_id),
            entity.net_id,
            static_cast<std::uint16_t>(entity.type),
            entity.owner_peer,
            to_kernel_vec3(entity.position),
            to_kernel_quat(entity.rotation),
            to_kernel_vec3(entity.velocity),
            entity.state,
            entity.flags,
            entity.spawn_tick,
            entity.client_action_id,
        });
        rendered_entities.insert(entity.net_id);
        auto replicated = std::find_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [&entity](const ClientReplicatedEntity& replicated_entity) {
                return replicated_entity.net_id == entity.net_id;
            });
        if (replicated != client_replicated_entities_.end()) {
            replicated->active = true;
            replicated->position = entity.position;
            replicated->rotation = entity.rotation;
        }
    }

    for (const ClientReplicatedEntity& entity : client_replicated_entities_) {
        if (entity.net_id == local_player_net_id_ ||
            rendered_entities.find(entity.net_id) != rendered_entities.end() ||
            client_despawned_entities_.find(entity.net_id) !=
                client_despawned_entities_.end()) {
            continue;
        }
        render_states_.push_back(RenderEntityState{
            entity_id_for_net_id(entity.net_id),
            entity.net_id,
            static_cast<std::uint16_t>(entity.type),
            entity.owner_peer,
            to_kernel_vec3(entity.position),
            to_kernel_quat(entity.rotation),
            KernelVec3{0.0f, 0.0f, 0.0f},
            0,
            0,
            0,
            0,
        });
    }
}

void KernelEngine::publish_snapshot() {
    const std::uint32_t server_time_ms = static_cast<std::uint32_t>(
        tick_loop_.current_tick() * tick_loop_.fixed_delta_seconds() * 1000.0f);
    latest_snapshot_ = build_world_snapshot(
        world_,
        tick_loop_.current_tick(),
        server_time_ms,
        local_last_processed_input_seq_);
    spdlog::debug(
        "{}",
        fmt::format(
            "snapshot tick={} entities={}",
            latest_snapshot_.header.server_tick,
            latest_snapshot_.entities.size()));

    if (config_.mode == KernelMode_ListenServer && loopback_transport_ != nullptr) {
        local_listen_session_.peer = kLocalListenPeerId;
        local_listen_session_.player = local_player_net_id_;
        local_listen_session_.last_processed_input_seq = local_last_processed_input_seq_;
        local_listen_session_.welcomed = local_player_net_id_ != 0;
        const WorldSnapshot peer_snapshot =
            build_relevant_snapshot(local_listen_session_, server_time_ms);
        sync_session_relevance(&local_listen_session_, peer_snapshot);
        const std::vector<std::uint8_t> packet =
            encode_snapshot_packet(peer_snapshot, next_packet_sequence_++);
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
        for (PeerSession& session : peer_sessions_) {
            if (!session.welcomed) {
                continue;
            }
            const WorldSnapshot peer_snapshot =
                build_relevant_snapshot(session, server_time_ms);
            sync_session_relevance(&session, peer_snapshot);
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

void KernelEngine::broadcast_combat_events(
    std::size_t first_event,
    std::size_t last_event) {
    if (!is_server_mode(config_.mode) || transport_ == nullptr) {
        return;
    }
    const std::size_t capped_last = std::min(last_event, events_.size());
    for (std::size_t index = first_event; index < capped_last; ++index) {
        const KernelEvent& event = events_[index];
        if (!is_authoritative_combat_event(event.type)) {
            continue;
        }
        broadcast_reliable_event(event);
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
        out_info->server_entity_create_info_size =
            sizeof(KernelServerEntityCreateInfo);
        out_info->server_entity_state_size = sizeof(KernelServerEntityState);
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
            KERNEL_CAPABILITY_LOCAL_PLAYER_INFO |
            KERNEL_CAPABILITY_SERVER_ENTITY_CREATE |
            KERNEL_CAPABILITY_SERVER_ENTITY_DESTROY |
            KERNEL_CAPABILITY_SERVER_ENTITY_TRANSFORM_WRITE |
            KERNEL_CAPABILITY_SERVER_ENTITY_VELOCITY_WRITE |
            KERNEL_CAPABILITY_SERVER_ENTITY_STATE_WRITE |
            KERNEL_CAPABILITY_SERVER_ENTITY_QUERY |
            KERNEL_CAPABILITY_SERVER_RELEVANCE_FILTER |
            KERNEL_CAPABILITY_LAG_COMPENSATED_PROJECTILE |
            KERNEL_CAPABILITY_EVENT_PRESENTATION_TIME;
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

bool Kernel_ServerCreateEntity(
    KernelHandle* kernel,
    const KernelServerEntityCreateInfo* create_info,
    uint32_t* out_net_id) {
    try {
        return kernel != nullptr && create_info != nullptr &&
               kernel->engine->server_create_entity(*create_info, out_net_id);
    } catch (...) {
        return false;
    }
}

bool Kernel_ServerDestroyEntity(
    KernelHandle* kernel,
    uint32_t net_id,
    uint32_t reason) {
    try {
        return kernel != nullptr &&
               kernel->engine->server_destroy_entity(net_id, reason);
    } catch (...) {
        return false;
    }
}

bool Kernel_ServerSetEntityTransform(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelVec3* position,
    const KernelQuat* rotation) {
    try {
        return kernel != nullptr && position != nullptr && rotation != nullptr &&
               kernel->engine->server_set_entity_transform(
                   net_id,
                   *position,
                   *rotation);
    } catch (...) {
        return false;
    }
}

bool Kernel_ServerSetEntityVelocity(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelVec3* velocity) {
    try {
        return kernel != nullptr && velocity != nullptr &&
               kernel->engine->server_set_entity_velocity(net_id, *velocity);
    } catch (...) {
        return false;
    }
}

bool Kernel_ServerSetEntityState(
    KernelHandle* kernel,
    uint32_t net_id,
    uint16_t animation_state,
    uint32_t visual_flags) {
    try {
        return kernel != nullptr &&
               kernel->engine->server_set_entity_state(
                   net_id,
                   animation_state,
                   visual_flags);
    } catch (...) {
        return false;
    }
}

bool Kernel_ServerGetEntityState(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelServerEntityState* out_state) {
    try {
        return kernel != nullptr &&
               kernel->engine->server_get_entity_state(net_id, out_state);
    } catch (...) {
        return false;
    }
}

uint32_t Kernel_ServerQueryEntities(
    KernelHandle* kernel,
    uint16_t entity_type_filter,
    KernelServerEntityState* out_states,
    uint32_t max_states) {
    try {
        if (kernel == nullptr) {
            return 0;
        }
        return kernel->engine->server_query_entities(
            static_cast<network_example::EntityType>(entity_type_filter),
            out_states,
            max_states);
    } catch (...) {
        return 0;
    }
}

}  // extern "C"
