#ifndef KERNEL_SRC_KERNEL_H_
#define KERNEL_SRC_KERNEL_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kernel/public/kernel_types.h"
#include "kernel/src/kernel_api_internal.h"
#include "kernel/src/kernel_rpc.h"
#include "kernel/src/tick_loop.h"
#include "physics/public/physics_world.h"
#include "simulation/public/command.h"
#include "simulation/public/movement_solver.h"
#include "simulation/public/simulation.h"
#include "simulation/src/systems.h"
#include "sync/public/history_buffer.h"
#include "sync/public/snapshot.h"
#include "transport/public/itransport.h"
#include "world/public/world.h"

namespace network_example {

class EntityLifecycleSystem;
class EntityStateSystem;
class DirectorIntentExecutor;
class ListenServerTransport;
class MovementSystem;
struct EntityDespawnPacket;
struct EntitySpawnPacket;
struct EntityTemplateUpdatePacket;
struct LocalActionResultBatchPacket;
struct ProjectileSpawnBatchPacket;
struct RemoteActionPresentationBatchPacket;
struct WelcomePacket;

namespace simulation {
class Dispatcher;
}  // namespace simulation

class KernelEngine {
public:
    explicit KernelEngine(KernelConfig config);

    bool start_client(const char* address);
    bool start_client_catalog_sync(
        const char* address,
        const KernelGameplayCatalogSyncClientConfig& config);
    bool start_listen_server(std::uint16_t port);
    bool start_dedicated_server(std::uint16_t port);
    bool set_physics_config(const KernelPhysicsConfig& config);
    bool set_session_rules(const KernelSessionRulesConfig& config);
    bool set_static_collision_scene(
        const KernelStaticCollisionSceneConfig& config);
    bool set_gameplay_catalog_sync_bundle(
        const KernelGameplayCatalogSyncServerConfig& config,
        KernelGameplayCatalogManifest* out_manifest);
    bool get_gameplay_catalog_sync_status(
        KernelGameplayCatalogSyncStatus* out_status) const;
    bool request_gameplay_catalog_bundle();
    bool copy_gameplay_catalog_bundle(
        std::uint8_t* out_bundle,
        std::uint32_t out_capacity,
        std::uint32_t* out_bundle_size) const;
    bool continue_client_handshake();
    bool invoke_rpc(
        std::string_view request_json,
        std::uint64_t* out_request_id);
    bool poll_rpc_response(
        std::uint64_t request_id,
        char* out_response_json,
        std::uint32_t response_json_capacity,
        std::uint32_t* out_response_json_size);

    void update(float delta_seconds);
    void submit_input(PeerId local_player_id, const PlayerInput& input);
    bool load_gameplay_catalog(const KernelGameplayCatalogDefinition& catalog);
    bool load_gameplay_catalog_with_static_collision_scene(
        const KernelGameplayCatalogDefinition& catalog,
        const KernelStaticCollisionSceneConfig& scene_config,
        bool* out_static_scene_rejected);

    std::uint32_t get_render_states(
        RenderEntityState* out_states,
        std::uint32_t max_states);
    std::uint32_t get_render_states_at_time(
        std::uint64_t client_render_time_us,
        RenderEntityState* out_states,
        std::uint32_t max_states);
    std::uint32_t poll_events(KernelEvent* out_events, std::uint32_t max_events);
    std::uint32_t poll_entity_lifecycle_events(
        KernelEntityLifecycleEvent* out_events,
        std::uint32_t max_events);
    std::uint32_t poll_local_action_results(
        KernelLocalActionResult* out_results,
        std::uint32_t max_results);
    std::uint32_t poll_remote_action_presentation_events(
        KernelRemoteActionPresentationEvent* out_events,
        std::uint32_t max_events);
    bool get_benchmark_stats(KernelBenchmarkStats* out_stats) const;
    bool get_network_stats(KernelNetworkStats* out_stats) const;
    std::uint32_t poll_debug_records(
        const KernelDebugRecordFilter* filter,
        KernelDebugInfo* out_records,
        std::uint32_t max_records);
    std::uint32_t query_collider_shapes(
        const KernelColliderShapeQuery* query,
        KernelColliderShapeView* out_shapes,
        std::uint32_t max_shapes) const;
    std::uint32_t query_vision_state(
        const KernelVisionStateQuery* query,
        KernelVisionStateView* out_states,
        std::uint32_t max_states) const;
    std::uint32_t get_projectile_templates(
        KernelProjectileTemplateDefinition* out_templates,
        std::uint32_t max_templates) const;
    bool get_action_template(
        std::uint32_t action_template_id,
        KernelActionTemplateDefinition* out_definition) const;
    std::uint32_t get_actor_templates(
        KernelActorTemplateDefinition* out_templates,
        std::uint32_t max_templates) const;
    std::uint32_t get_collider_templates(
        KernelColliderTemplateDefinition* out_templates,
        std::uint32_t max_templates) const;
    std::uint32_t get_collider_bindings(
        KernelColliderBindingDefinition* out_bindings,
        std::uint32_t max_bindings) const;
    KernelLocalPlayerInfo local_player_info() const;
    bool server_create_entity(
        const KernelServerEntityCreateInfo& create_info,
        NetId* out_net_id);
    bool server_destroy_entity(NetId net_id, std::uint32_t reason);
    bool server_enqueue_entity_lifecycle(
        std::uint32_t command_source,
        const KernelEntityLifecycleCommand& command);
    bool server_set_entity_transform(
        NetId net_id,
        const KernelVec3& position,
        const KernelQuat& rotation);
    bool server_set_entity_velocity(NetId net_id, const KernelVec3& velocity);
    bool server_set_entity_state(
        NetId net_id,
        std::uint16_t animation_state,
        std::uint32_t visual_flags);
    bool server_set_entity_health(NetId net_id, std::uint16_t hp);
    bool server_submit_entity_input(NetId net_id, const PlayerInput& input);
    bool server_enqueue_entity_transform(
        std::uint32_t command_source,
        NetId net_id,
        const KernelVec3& position,
        const KernelQuat& rotation);
    bool server_enqueue_entity_velocity(
        std::uint32_t command_source,
        NetId net_id,
        const KernelVec3& velocity);
    bool server_enqueue_entity_state(
        std::uint32_t command_source,
        NetId net_id,
        std::uint16_t animation_state,
        std::uint32_t visual_flags);
    bool server_enqueue_entity_input(
        std::uint32_t command_source,
        NetId net_id,
        const PlayerInput& input);
    bool server_set_entity_combat_state(
        NetId net_id,
        const KernelCombatStateDefinition& combat_state);
    bool server_set_entity_actor_template(
        NetId net_id,
        std::uint32_t actor_template_id);
    bool server_set_entity_vision_config(
        NetId net_id,
        const KernelAgentVisionConfig& vision_config);
    bool server_clear_entity_vision_config(NetId net_id);
    bool server_set_entity_weapon_mechanics(
        NetId net_id,
        const KernelWeaponMechanicsDefinition& weapon_mechanics);
    bool server_clear_entity_weapon_mechanics(
        NetId net_id,
        std::uint8_t weapon_id);
    bool server_get_entity_weapon_mechanics(
        NetId net_id,
        std::uint8_t weapon_id,
        KernelWeaponMechanicsDefinition* out_weapon_mechanics) const;
    bool server_get_homing_state(
        NetId net_id,
        KernelHomingState* out_state) const;
    bool server_get_entity_state(
        NetId net_id,
        KernelServerEntityState* out_state) const;
    std::uint32_t server_query_entities(
        EntityType entity_type_filter,
        KernelServerEntityState* out_states,
        std::uint32_t max_states) const;

private:
    friend class EntityLifecycleSystem;
    friend class EntityStateSystem;
    friend class DirectorAISystem;
    friend class DirectorIntentExecutor;
    friend class MovementSystem;
    friend class KernelRpcDispatcher;
    friend class KernelRpcWorldHandlers;
    friend class simulation::Dispatcher;

    struct ByteTokenBucket {
        std::uint64_t tokens = 0;
        std::uint64_t refill_remainder = 0;
        std::uint64_t last_refill_time_us = 0;
        bool initialized = false;
    };

    struct PeerSession {
        PeerId peer = 0;
        NetId player = 0;
        std::uint32_t last_processed_input_seq = 0;
        bool welcomed = false;
        std::unordered_set<NetId> relevant_entities;
        std::uint32_t action_instance_high_water = 0;
        std::uint32_t active_action_instance_id = 0;
        std::uint32_t active_action_template_id = 0;
        std::uint16_t active_action_binding_id = 0;
        std::uint16_t active_action_commit_count = 0;
        std::unordered_map<std::uint32_t, KernelLocalActionResult>
            recent_action_results;
        std::vector<std::uint32_t> recent_action_result_order;
        std::vector<KernelLocalActionResult> pending_action_results;
        std::size_t actor_snapshot_cursor = 0;
        std::size_t projectile_snapshot_cursor = 0;
        std::uint32_t pending_clock_sync_nonce = 0;
        std::uint64_t pending_clock_sync_server_time_us = 0;
        std::uint64_t last_clock_sync_sent_server_time_us = 0;
        std::uint64_t last_clock_sync_rtt_us = 0;
        std::uint64_t last_clock_sync_jitter_us = 0;
        std::int64_t clock_offset_us = 0;
        bool has_clock_sync = false;
        ByteTokenBucket remote_presentation_budget;
        PlayerInput latest_movement_input{};
        std::uint32_t last_received_input_seq = 0;
        std::uint64_t last_movement_input_server_time_us = 0;
        bool has_received_input = false;
        bool has_movement_input = false;
    };

    struct ClientReplicatedEntity {
        NetId net_id = 0;
        EntityType type = EntityType::kUnknown;
        ActorType actor_type = ActorType::kUnknown;
        PeerId owner_peer = 0;
        std::uint32_t actor_template_id = 0;
        std::uint32_t projectile_template_id = 0;
        std::uint32_t collider_template_id = 0;
        glm::vec3 position{0.0f, 0.0f, 0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 velocity{0.0f, 0.0f, 0.0f};
        std::uint32_t snapshot_tick = 0;
        std::uint16_t hp = 0;
        std::uint16_t max_hp = 0;
        bool hp_known = false;
        bool active = false;
    };

    struct PendingPredictionInput {
        PlayerInput input{};
        std::uint32_t prediction_tick = 0;
    };

    struct ClientEntityTombstone {
        std::uint32_t tick = 0;
        std::uint32_t reason = KernelDespawnReason_Destroyed;
    };

    struct PredictedProjectile {
        std::uint64_t entity_id = 0;
        NetId net_id = 0;
        PeerId owner_peer = 0;
        std::uint32_t input_seq = 0;
        std::uint32_t action_instance_id = 0;
        std::uint32_t spawn_tick = 0;
        std::uint32_t age_ticks = 0;
        glm::vec3 position{0.0f, 0.0f, 0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 velocity{0.0f, 0.0f, 0.0f};
        glm::vec3 spawn_position{0.0f, 0.0f, 0.0f};
        glm::vec3 initial_velocity{0.0f, 0.0f, 0.0f};
        glm::vec3 gravity{0.0f, 0.0f, 0.0f};
        ProjectileMotionModel motion_model = ProjectileMotionModel::kLinear;
        std::uint32_t max_lifetime_ticks = 0;
        std::uint32_t projectile_template_id = 0;
        std::uint32_t collider_template_id = 0;
        std::uint8_t weapon_id = 0;
        std::uint8_t sync_mode = KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
        glm::vec3 correction_offset{0.0f, 0.0f, 0.0f};
        bool bound = false;
    };

    struct VisionRuntimeState {
        KernelVisionStateView view{};
        bool has_last_seen_target = false;
    };

    struct ReceiveSequenceState {
        std::uint32_t last_sequence = 0;
        std::uint64_t received_count = 0;
    };

    struct GameplayCatalogTransfer {
        std::size_t offset = 0;
    };

    struct OutstandingPredictedAction {
        std::uint32_t action_instance_id = 0;
        std::uint64_t last_activity_us = 0;
        std::uint16_t confirmed_commit_count = 0;
        std::uint16_t binding_id = 0;
        std::uint8_t weapon_id = 0;
        std::uint32_t action_template_id = 0;
        std::uint32_t primary_gate_before = 0;
        std::uint32_t last_authoritative_commit_tick = 0;
        std::uint16_t ammo_before = 0;
        NetId active_effect_before = 0;
        std::uint32_t pending_authoritative_tick = 0;
        bool terminal_correction = false;
    };

    struct PendingRemotePresentation {
        std::uint32_t batch_server_tick = 0;
        std::uint32_t expire_tick = 0;
        KernelRemoteActionPresentationEvent event{};
    };

    struct RemotePresentationDedup {
        std::uint32_t actor_net_id = 0;
        std::uint32_t action_instance_id = 0;
        std::uint16_t commit_index = 0;
        std::uint8_t event_type = 0;
        std::uint32_t expire_tick = 0;
    };

    void push_event(
        KernelEventType type,
        NetId net_id = 0,
        PeerId peer_id = 0,
        std::uint32_t code = 0);
    void reset_runtime_state(KernelMode mode);
    bool prepare_server_physics(
        std::unique_ptr<physics::PhysicsWorld>* out_world);
    void clear_client_action_sync_state();
    void poll_transport();
    void poll_client_transport();
    void send_gameplay_catalog_manifest_request();
    void handle_server_gameplay_catalog_manifest_request(
        const TransportEvent& transport_event);
    void handle_server_gameplay_catalog_bundle_request(
        const TransportEvent& transport_event);
    void handle_client_gameplay_catalog_manifest(
        const TransportEvent& transport_event);
    void handle_client_gameplay_catalog_bundle_chunk(
        const TransportEvent& transport_event);
    void handle_client_gameplay_catalog_sync_error(
        const TransportEvent& transport_event);
    void pump_gameplay_catalog_transfers();
    void fail_gameplay_catalog_sync(KernelGameplayCatalogSyncError error);
    void handle_server_disconnect(const TransportEvent& transport_event);
    void handle_client_disconnect(PeerId peer);
    void handle_client_reliable_event(const TransportEvent& transport_event);
    void handle_client_local_action_results(
        const LocalActionResultBatchPacket& packet);
    void handle_client_remote_action_presentation(
        const TransportEvent& transport_event);
    void handle_client_projectile_spawn_batch(
        const ProjectileSpawnBatchPacket& packet);
    void handle_client_template_update(const EntityTemplateUpdatePacket& packet);
    void handle_client_ping_pong(const TransportEvent& transport_event);
    void handle_server_ping_pong(const TransportEvent& transport_event);
    bool apply_welcome(const WelcomePacket& welcome);
    void apply_client_clock_offset_sample(std::int64_t sample_offset_us);
    void handle_client_spawn(const EntitySpawnPacket& packet);
    void handle_client_despawn(const EntityDespawnPacket& packet);
    void clear_client_session();
    void simulate_tick();
    bool enqueue_simulation_command(const simulation::Command& command);
    std::size_t drain_simulation_commands();
    void record_simulation_tick_cost(
        std::uint64_t cost_us,
        std::size_t queue_depth,
        std::size_t processed_command_count);
    void release_presentable_events();
    void release_remote_action_presentation_events();
    void broadcast_combat_events(std::size_t first_event, std::size_t last_event);
    void rebuild_render_states();
    void rebuild_render_states_at_time(
        std::uint64_t client_render_time_us,
        bool consume_correction);
    void rebuild_render_states_from_world();
    void rebuild_render_states_from_snapshot(
        std::uint64_t client_render_time_us,
        bool consume_correction);
    void report_render_state_overflow_if_needed();
    void handle_client_snapshot(WorldSnapshot snapshot);
    void store_client_snapshot(WorldSnapshot snapshot);
    bool snapshot_entity_has_required_metadata(const EntitySnapshot& entity) const;
    bool prepare_static_collision_scene(
        const KernelStaticCollisionSceneConfig& config,
        std::vector<std::uint8_t>* out_scene) const;
    void commit_static_collision_scene(
        std::vector<std::uint8_t> scene,
        const KernelStaticCollisionSceneConfig& config);
    void diagnose_client_snapshot_metadata_waits();
    void reconcile_local_prediction(const WorldSnapshot& snapshot);
    void reconcile_predicted_projectiles(const WorldSnapshot& snapshot);
    bool emit_client_input_for_tick();
    void process_client_input_command(PeerId peer, const PlayerInput& input);
    bool cache_server_movement_input(
        PeerSession* session,
        const PlayerInput& input,
        std::uint64_t received_server_time_us);
    std::vector<QueuedInput> build_effective_movement_inputs(
        std::uint64_t server_time_us);
    void acknowledge_simulated_movement_inputs(
        const std::vector<QueuedInput>& inputs);
    void predict_local_input(const PlayerInput& input);
    bool prepare_prediction_physics();
    bool build_local_character_movement_config(
        movement_solver::CharacterMovementConfig* out_config);
    bool sync_prediction_actor_proxies(
        const WorldSnapshot& snapshot,
        std::uint32_t prediction_tick);
    bool step_local_character_prediction(
        const PlayerInput& input,
        std::uint32_t prediction_tick);
    void fail_client_prediction(std::string_view diagnostic);
    bool predict_local_action(const PlayerInput& input);
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
    bool build_interpolated_snapshot(
        std::uint64_t client_render_time_us,
        WorldSnapshot* out_snapshot) const;
    bool build_interpolated_snapshot_for_server_time(
        std::uint64_t target_server_time_us,
        WorldSnapshot* out_snapshot) const;
    void append_predicted_local_render_state(bool consume_correction);
    void append_predicted_projectile_render_states(bool consume_correction);
    void advance_predicted_projectiles(float fixed_delta_seconds);
    std::uint32_t local_prediction_server_tick(std::uint32_t snapshot_tick) const;
    std::uint32_t rewind_tick_for_input(const QueuedInput& queued_input) const;
    std::uint64_t entity_id_for_net_id(NetId net_id);
    std::uint64_t allocate_predicted_entity_id();
    bool has_predicted_projectile_net_id(NetId net_id) const;
    PredictedProjectile* find_predicted_projectile(
        PeerId owner_peer,
        std::uint32_t action_instance_id);
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
    void send_projectile_spawn_batch(PeerId peer, const EntitySnapshot& entity);
    void send_entity_despawn(
        PeerId peer,
        NetId net_id,
        std::uint32_t reason);
    void send_entity_template_update(
        PeerId peer,
        NetId net_id,
        std::uint32_t actor_template_id);
    WorldSnapshot build_snapshot_send_set(
        PeerSession& session,
        const WorldSnapshot& relevant_snapshot,
        std::size_t byte_budget) const;
    void send_client_handshake();
    void send_clock_sync_ping(
        PeerSession* session,
        std::uint64_t server_time_us);
    void send_due_clock_sync_pings(std::uint64_t server_time_us);
    void send_reliable_event(PeerId peer, const KernelEvent& event);
    void broadcast_reliable_event(const KernelEvent& event);
    void prepare_server_action_intent(PeerSession* session, PlayerInput* input);
    void finalize_server_action_outcomes(
        const std::vector<ActionOutcome>& outcomes);
    void queue_local_action_result(
        PeerSession* session,
        const KernelLocalActionResult& result);
    void flush_local_action_results(PeerSession* session);
    void queue_remote_presentation_from_events(
        std::size_t first_event,
        std::size_t last_event,
        const std::unordered_set<NetId>& actors_before_tick);
    void queue_server_remote_presentation(
        const KernelRemoteActionPresentationEvent& event);
    void flush_remote_action_presentation(
        PeerSession* session,
        const std::vector<KernelRemoteActionPresentationEvent>& events);
    PeerSession* result_session_for_peer(PeerId peer);
    void record_received_packet_sequence(const TransportEvent& transport_event);
    void record_sent_packet(
        std::uint32_t packet_size,
        SendMode mode,
        ChannelId channel);
    void record_packet_deserialization_cost(std::uint64_t cost_us);
    bool network_stats_enabled() const;
    bool detailed_network_stats_enabled() const;
    void refill_byte_token_bucket(
        ByteTokenBucket* bucket,
        std::uint64_t bytes_per_second,
        std::uint64_t now_us) const;
    void queue_hit_debug_records(
        const std::vector<ConfirmedDamage>& ready_damage);
    void handle_server_handshake(const TransportEvent& transport_event);
    void handle_server_session_message(const TransportEvent& transport_event);
    void handle_client_session_message(const TransportEvent& transport_event);
    PeerSession* find_session(PeerId peer);
    const PeerSession* find_session(PeerId peer) const;
    void remove_session(PeerId peer);
    void materialize_entity_collider(NetId net_id);
    void materialize_entity_movement_collider(NetId net_id);
    void materialize_projectile_collider(NetId net_id);
    void sync_entity_colliders_from_world();
    std::uint32_t collider_template_id_for_projectile_template(
        std::uint32_t projectile_template_id) const;
    std::uint32_t collider_template_id_for_actor_template(
        std::uint32_t actor_template_id) const;
    void sync_client_render_colliders();
    void sync_client_vision_states_from_snapshot(const WorldSnapshot& snapshot);
    void update_vision_states(float delta_seconds);

    KernelConfig config_;
    TickLoop tick_loop_;
    World world_;
    HistoryBuffer history_buffer_;
    DamagePipeline damage_pipeline_;
    std::unique_ptr<ITransport> transport_;
    ListenServerTransport* listen_server_transport_ = nullptr;
    std::vector<QueuedInput> pending_inputs_;
    std::vector<KernelEvent> events_;
    std::vector<KernelEntityLifecycleEvent> lifecycle_events_;
    std::vector<KernelEvent> pending_presentation_events_;
    std::vector<KernelLocalActionResult> local_action_results_;
    std::vector<KernelRemoteActionPresentationEvent>
        remote_action_presentation_events_;
    std::vector<KernelRemoteActionPresentationEvent>
        pending_server_remote_presentations_;
    std::vector<PendingRemotePresentation>
        pending_remote_action_presentation_events_;
    std::vector<RemotePresentationDedup> remote_presentation_dedup_;
    std::vector<RenderEntityState> render_states_;
    WorldSnapshot latest_snapshot_;
    WorldSnapshot latest_client_snapshot_;
    std::vector<WorldSnapshot> client_snapshot_buffer_;
    std::vector<PeerSession> peer_sessions_;
    PeerSession local_listen_session_;
    std::vector<ClientReplicatedEntity> client_replicated_entities_;
    std::unordered_set<NetId> client_metadata_timeout_reported_entities_;
    std::unordered_map<NetId, ClientEntityTombstone> client_despawned_entities_;
    std::vector<PendingPredictionInput> pending_prediction_inputs_;
    PlayerInput latest_client_input_{};
    std::deque<PlayerInput> pending_client_action_intents_;
    std::uint64_t latest_client_input_time_us_ = 0;
    std::uint32_t next_client_input_seq_ = 1;
    PeerId latest_client_input_peer_ = 0;
    bool has_latest_client_input_ = false;
    std::vector<PredictedProjectile> predicted_projectiles_;
    std::unordered_map<std::uint32_t, OutstandingPredictedAction>
        outstanding_predicted_actions_;
    std::unordered_map<std::uint32_t, KernelLocalActionResult>
        applied_local_action_results_;
    std::vector<KernelEntityTemplateDefinition> entity_templates_;
    std::vector<KernelActorTemplateDefinition> actor_templates_;
    std::vector<KernelProjectileTemplateDefinition> projectile_templates_;
    std::vector<KernelColliderTemplateDefinition> collider_templates_;
    std::vector<KernelActionTemplateDefinition> action_templates_;
    std::vector<KernelDebugInfo> debug_records_;
    std::unordered_map<NetId, KernelAgentVisionConfig> vision_configs_;
    std::unordered_map<NetId, VisionRuntimeState> vision_states_;
    std::vector<ai::ScopedIntent> pending_director_intents_;
    simulation::CommandQueue command_queue_;
    KernelRpcMethodRegistry rpc_method_registry_;
    KernelRpcResponseStore rpc_response_store_;
    KernelRpcDispatcher rpc_dispatcher_;
    std::uint64_t rejected_simulation_command_count_ = 0;
    std::uint64_t failed_simulation_command_count_ = 0;
    std::uint32_t command_queue_capacity_warning_count_ = 0;
    std::uint32_t last_command_queue_capacity_warning_tick_ = 0;
    std::size_t last_simulation_command_queue_depth_ = 0;
    std::size_t last_simulation_command_processed_count_ = 0;
    std::size_t last_director_intent_processed_count_ = 0;
    std::uint32_t last_director_intent_created_count_ = 0;
    std::uint32_t last_director_intent_failed_count_ = 0;
    std::uint32_t last_director_intent_unsupported_count_ = 0;
    std::array<std::uint64_t, 120> simulation_tick_cost_samples_us_{};
    std::size_t simulation_tick_cost_sample_index_ = 0;
    std::uint32_t simulation_tick_cost_sample_count_ = 0;
    std::uint64_t simulation_tick_cost_sample_sum_us_ = 0;
    std::uint64_t last_simulation_tick_cost_us_ = 0;
    std::uint64_t average_simulation_tick_cost_us_ = 0;
    std::uint64_t simulation_tick_cost_warning_threshold_us_ = 0;
    std::uint32_t simulation_tick_cost_warning_count_ = 0;
    std::uint32_t last_simulation_tick_cost_warning_tick_ = 0;
    KernelNetworkStats network_stats_{};
    ByteTokenBucket server_remote_presentation_budget_;
    KernelBenchmarkStats benchmark_stats_{};
    std::uint32_t catalog_version_ = 0;
    std::uint64_t catalog_hash_ = 0;
    KernelGameplayCatalogManifest gameplay_catalog_manifest_{};
    std::vector<std::uint8_t> gameplay_catalog_sync_bundle_;
    std::vector<std::uint8_t> downloaded_gameplay_catalog_bundle_;
    KernelPhysicsConfig physics_config_{
        sizeof(KernelPhysicsConfig), 0, 0};
    KernelSessionRulesConfig session_rules_{
        sizeof(KernelSessionRulesConfig),
        KernelActorBlockingMode_Disabled};
    std::vector<std::uint8_t> static_collision_scene_;
    std::uint32_t static_collision_scene_id_ = 0;
    std::uint32_t static_collision_collider_id_ = 0;
    std::uint32_t static_collision_layer_ = 0;
    std::unique_ptr<physics::PhysicsWorld> physics_world_;
    std::unordered_set<std::uint32_t> physics_entity_collider_ids_;
    std::unique_ptr<physics::PhysicsWorld> prediction_physics_world_;
    std::unordered_map<NetId, std::uint32_t> prediction_proxy_collider_ids_;
    std::uint32_t next_prediction_proxy_collider_id_ = 0xc0000000u;
    std::unordered_map<PeerId, GameplayCatalogTransfer>
        gameplay_catalog_transfers_;
    KernelGameplayCatalogSyncState gameplay_catalog_sync_state_ =
        KernelGameplayCatalogSyncState_Idle;
    KernelGameplayCatalogSyncError gameplay_catalog_sync_error_ =
        KernelGameplayCatalogSyncError_None;
    std::uint32_t gameplay_catalog_sync_max_bundle_size_ =
        KERNEL_GAMEPLAY_CATALOG_SYNC_DEFAULT_MAX_BUNDLE_SIZE;
    std::uint32_t gameplay_catalog_sync_timeout_ms_ =
        KERNEL_GAMEPLAY_CATALOG_SYNC_DEFAULT_TIMEOUT_MS;
    std::uint64_t gameplay_catalog_sync_elapsed_us_ = 0;
    std::unordered_map<NetId, std::uint64_t> entity_ids_by_net_id_;
    EntitySnapshot predicted_local_entity_;
    movement_solver::CharacterMovementState predicted_character_state_{};
    std::uint32_t predicted_character_tick_ = 0;
    std::uint32_t predicted_action_buttons_ = 0;
    std::uint16_t predicted_action_binding_id_ = 0;
    std::uint8_t predicted_action_weapon_id_ = 0;
    std::uint32_t predicted_action_next_commit_tick_ = 0;
    std::uint32_t predicted_action_recovery_end_tick_ = 0;
    std::array<std::uint32_t, kWeaponCount>
        predicted_next_primary_commit_tick_{};
    glm::vec3 local_correction_offset_{0.0f, 0.0f, 0.0f};
    std::uint64_t next_entity_id_ = 1;
    std::uint64_t next_predicted_entity_id_ = UINT64_C(0x8000000000000000);
    NetId local_player_net_id_ = 0;
    std::uint32_t local_last_processed_input_seq_ = 0;
    std::uint32_t next_packet_sequence_ = 1;
    std::uint32_t next_server_presentation_instance_id_ = 1;
    std::uint32_t last_remote_presentation_sequence_ = 0;
    std::uint32_t next_clock_sync_nonce_ = 1;
    std::unordered_map<PeerId, ReceiveSequenceState> received_sequences_by_peer_;
    std::uint64_t received_packet_count_ = 0;
    std::uint64_t lost_packet_count_ = 0;
    std::uint64_t client_local_time_us_ = 0;
    std::int64_t client_clock_offset_us_ = 0;
    float local_player_move_speed_meters_per_second_ = 0.0f;
    std::uint64_t current_render_time_us_ = 0;
    PeerId local_client_peer_id_ = 0;
    bool client_handshake_sent_ = false;
    bool has_welcome_ = false;
    bool has_client_snapshot_ = false;
    bool has_predicted_local_entity_ = false;
    bool prediction_failed_ = false;
    bool has_client_clock_sync_ = false;
    bool has_client_render_time_ = false;
    bool has_remote_presentation_sequence_ = false;
    bool running_ = false;
};

}  // namespace network_example

#endif  // KERNEL_SRC_KERNEL_H_
