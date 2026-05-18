#ifndef KERNEL_SRC_KERNEL_H_
#define KERNEL_SRC_KERNEL_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kernel/public/kernel_types.h"
#include "kernel/src/tick_loop.h"
#include "simulation/public/simulation.h"
#include "sync/public/history_buffer.h"
#include "sync/public/snapshot.h"
#include "transport/public/itransport.h"
#include "world/public/world.h"

namespace network_example {

class LoopbackTransport;
struct EntityDespawnPacket;
struct EntitySpawnPacket;

class KernelEngine {
public:
    explicit KernelEngine(KernelConfig config);

    bool start_client(const char* address);
    bool start_listen_server(std::uint16_t port);
    bool start_dedicated_server(std::uint16_t port);

    void update(float delta_seconds);
    void submit_input(PeerId local_player_id, const PlayerInput& input);

    std::uint32_t get_render_states(
        RenderEntityState* out_states,
        std::uint32_t max_states) const;
    std::uint32_t poll_events(KernelEvent* out_events, std::uint32_t max_events);
    KernelLocalPlayerInfo local_player_info() const;
    bool server_create_entity(
        const KernelServerEntityCreateInfo& create_info,
        NetId* out_net_id);
    bool server_destroy_entity(NetId net_id, std::uint32_t reason);
    bool server_set_entity_transform(
        NetId net_id,
        const KernelVec3& position,
        const KernelQuat& rotation);
    bool server_set_entity_velocity(NetId net_id, const KernelVec3& velocity);
    bool server_set_entity_state(
        NetId net_id,
        std::uint16_t animation_state,
        std::uint32_t visual_flags);
    bool server_get_entity_state(
        NetId net_id,
        KernelServerEntityState* out_state) const;
    std::uint32_t server_query_entities(
        EntityType entity_type_filter,
        KernelServerEntityState* out_states,
        std::uint32_t max_states) const;

private:
    struct PeerSession {
        PeerId peer = 0;
        NetId player = 0;
        std::uint32_t last_processed_input_seq = 0;
        bool welcomed = false;
        std::unordered_set<NetId> relevant_entities;
        std::uint32_t pending_clock_sync_nonce = 0;
        std::uint64_t pending_clock_sync_server_time_us = 0;
        std::uint64_t last_clock_sync_sent_server_time_us = 0;
        std::uint64_t last_clock_sync_rtt_us = 0;
        std::int64_t clock_offset_us = 0;
        bool has_clock_sync = false;
    };

    struct ClientReplicatedEntity {
        NetId net_id = 0;
        EntityType type = EntityType::kUnknown;
        PeerId owner_peer = 0;
        glm::vec3 position{0.0f, 0.0f, 0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        bool active = false;
    };

    struct PredictedProjectile {
        std::uint64_t entity_id = 0;
        NetId net_id = 0;
        PeerId owner_peer = 0;
        std::uint32_t input_seq = 0;
        std::uint32_t client_action_id = 0;
        std::uint32_t spawn_tick = 0;
        glm::vec3 position{0.0f, 0.0f, 0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 velocity{0.0f, 0.0f, 0.0f};
        bool bound = false;
    };

    void push_event(
        KernelEventType type,
        NetId net_id = 0,
        PeerId peer_id = 0,
        std::uint32_t code = 0);
    void reset_runtime_state(KernelMode mode);
    void poll_transport();
    void poll_client_transport();
    void handle_server_disconnect(const TransportEvent& transport_event);
    void handle_client_disconnect(PeerId peer);
    void handle_client_reliable_event(const TransportEvent& transport_event);
    void handle_client_ping_pong(const TransportEvent& transport_event);
    void handle_server_ping_pong(const TransportEvent& transport_event);
    void handle_client_spawn(const EntitySpawnPacket& packet);
    void handle_client_despawn(const EntityDespawnPacket& packet);
    void clear_client_session();
    void simulate_tick();
    void release_presentable_events();
    void broadcast_combat_events(std::size_t first_event, std::size_t last_event);
    void rebuild_render_states();
    void rebuild_render_states_from_world();
    void rebuild_render_states_from_snapshot();
    void handle_client_snapshot(WorldSnapshot snapshot);
    void store_client_snapshot(WorldSnapshot snapshot);
    void reconcile_local_prediction(const WorldSnapshot& snapshot);
    void reconcile_predicted_projectiles(const WorldSnapshot& snapshot);
    void predict_local_input(const PlayerInput& input);
    void predict_local_projectile(const PlayerInput& input);
    PlayerInput prepare_client_input(const PlayerInput& input);
    std::uint64_t client_local_action_time_us() const;
    std::uint64_t current_server_time_us() const;
    std::uint64_t convert_client_action_time_to_server_time(
        PeerId peer,
        std::uint64_t client_action_time_us,
        std::uint64_t received_server_time_us) const;
    std::uint64_t uncompensated_action_time_us(const QueuedInput& queued_input) const;
    std::uint64_t clamp_compensated_action_time_us(
        std::uint64_t action_server_time_us,
        std::uint64_t received_server_time_us) const;
    std::uint64_t compensated_action_time_us(const QueuedInput& queued_input) const;
    bool build_interpolated_snapshot(WorldSnapshot* out_snapshot) const;
    void append_predicted_local_render_state();
    void append_predicted_projectile_render_states();
    void advance_predicted_projectiles(float fixed_delta_seconds);
    std::uint32_t rewind_tick_for_input(const QueuedInput& queued_input) const;
    std::uint64_t entity_id_for_net_id(NetId net_id);
    std::uint64_t allocate_predicted_entity_id();
    bool has_predicted_projectile_net_id(NetId net_id) const;
    PredictedProjectile* find_predicted_projectile(
        PeerId owner_peer,
        std::uint32_t client_action_id);
    void publish_snapshot();
    WorldSnapshot build_relevant_snapshot(
        const PeerSession& session,
        std::uint32_t server_time_ms) const;
    bool is_entity_relevant_to_session(
        const PeerSession& session,
        const EntitySnapshot& entity,
        const EntitySnapshot* player_entity) const;
    void sync_session_relevance(
        PeerSession* session,
        const WorldSnapshot& snapshot);
    void send_entity_spawn(PeerId peer, const EntitySnapshot& entity);
    void send_entity_despawn(
        PeerId peer,
        NetId net_id,
        std::uint32_t reason);
    void send_client_handshake();
    void send_clock_sync_ping(
        PeerSession* session,
        std::uint64_t server_time_us);
    void send_due_clock_sync_pings(std::uint64_t server_time_us);
    void send_reliable_event(PeerId peer, const KernelEvent& event);
    void broadcast_reliable_event(const KernelEvent& event);
    void handle_server_handshake(const TransportEvent& transport_event);
    void handle_server_session_message(const TransportEvent& transport_event);
    void handle_client_session_message(const TransportEvent& transport_event);
    PeerSession* find_session(PeerId peer);
    const PeerSession* find_session(PeerId peer) const;
    void remove_session(PeerId peer);

    KernelConfig config_;
    TickLoop tick_loop_;
    World world_;
    HistoryBuffer history_buffer_;
    DamagePipeline damage_pipeline_;
    std::unique_ptr<ITransport> transport_;
    LoopbackTransport* loopback_transport_ = nullptr;
    std::vector<QueuedInput> pending_inputs_;
    std::vector<KernelEvent> events_;
    std::vector<KernelEvent> pending_presentation_events_;
    std::vector<RenderEntityState> render_states_;
    WorldSnapshot latest_snapshot_;
    WorldSnapshot latest_client_snapshot_;
    std::vector<WorldSnapshot> client_snapshot_buffer_;
    std::vector<PeerSession> peer_sessions_;
    PeerSession local_listen_session_;
    std::vector<ClientReplicatedEntity> client_replicated_entities_;
    std::unordered_set<NetId> client_despawned_entities_;
    std::vector<PlayerInput> pending_prediction_inputs_;
    std::vector<PredictedProjectile> predicted_projectiles_;
    std::unordered_map<NetId, std::uint64_t> entity_ids_by_net_id_;
    EntitySnapshot predicted_local_entity_;
    glm::vec3 local_correction_offset_{0.0f, 0.0f, 0.0f};
    std::uint64_t next_entity_id_ = 1;
    std::uint64_t next_predicted_entity_id_ = UINT64_C(0x8000000000000000);
    NetId local_player_net_id_ = 0;
    std::uint32_t local_last_processed_input_seq_ = 0;
    std::uint32_t next_packet_sequence_ = 1;
    std::uint32_t next_clock_sync_nonce_ = 1;
    std::uint64_t client_local_time_us_ = 0;
    std::uint64_t current_render_time_us_ = 0;
    PeerId local_client_peer_id_ = 0;
    bool client_handshake_sent_ = false;
    bool has_welcome_ = false;
    bool has_client_snapshot_ = false;
    bool has_predicted_local_entity_ = false;
    bool has_client_render_time_ = false;
    bool running_ = false;
};

}  // namespace network_example

#endif  // KERNEL_SRC_KERNEL_H_
