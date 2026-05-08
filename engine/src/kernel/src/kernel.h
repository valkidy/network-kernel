#ifndef KERNEL_SRC_KERNEL_H_
#define KERNEL_SRC_KERNEL_H_

#include <cstdint>
#include <memory>
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

private:
    struct PeerSession {
        PeerId peer = 0;
        NetId player = 0;
        std::uint32_t last_processed_input_seq = 0;
        bool welcomed = false;
    };

    void push_event(
        KernelEventType type,
        NetId net_id = 0,
        PeerId peer_id = 0,
        std::uint32_t code = 0);
    void poll_transport();
    void poll_client_transport();
    void handle_server_disconnect(const TransportEvent& transport_event);
    void handle_client_disconnect(PeerId peer);
    void handle_client_reliable_event(const TransportEvent& transport_event);
    void clear_client_session();
    void simulate_tick();
    void rebuild_render_states();
    void rebuild_render_states_from_world();
    void rebuild_render_states_from_snapshot();
    void handle_client_snapshot(WorldSnapshot snapshot);
    void store_client_snapshot(WorldSnapshot snapshot);
    void reconcile_local_prediction(const WorldSnapshot& snapshot);
    void predict_local_input(const PlayerInput& input);
    PlayerInput prepare_client_input(const PlayerInput& input);
    bool build_interpolated_snapshot(WorldSnapshot* out_snapshot) const;
    void append_predicted_local_render_state();
    std::uint32_t rewind_tick_for_input(const QueuedInput& queued_input) const;
    void publish_snapshot();
    void send_client_handshake();
    void send_reliable_event(PeerId peer, const KernelEvent& event);
    void broadcast_reliable_event(const KernelEvent& event);
    void handle_server_handshake(const TransportEvent& transport_event);
    void handle_client_session_message(const TransportEvent& transport_event);
    PeerSession* find_session(PeerId peer);
    const PeerSession* find_session(PeerId peer) const;
    void remove_session(PeerId peer);

    KernelConfig config_;
    TickLoop tick_loop_;
    World world_;
    HistoryBuffer history_buffer_;
    std::unique_ptr<ITransport> transport_;
    LoopbackTransport* loopback_transport_ = nullptr;
    std::vector<QueuedInput> pending_inputs_;
    std::vector<KernelEvent> events_;
    std::vector<RenderEntityState> render_states_;
    WorldSnapshot latest_snapshot_;
    WorldSnapshot latest_client_snapshot_;
    std::vector<WorldSnapshot> client_snapshot_buffer_;
    std::vector<PeerSession> peer_sessions_;
    std::vector<PlayerInput> pending_prediction_inputs_;
    EntitySnapshot predicted_local_entity_;
    glm::vec3 local_correction_offset_{0.0f, 0.0f, 0.0f};
    NetId local_player_net_id_ = 0;
    std::uint32_t local_last_processed_input_seq_ = 0;
    std::uint32_t predicted_server_tick_ = 0;
    std::uint32_t next_packet_sequence_ = 1;
    PeerId local_client_peer_id_ = 0;
    bool client_handshake_sent_ = false;
    bool has_welcome_ = false;
    bool has_client_snapshot_ = false;
    bool has_predicted_local_entity_ = false;
    bool running_ = false;
};

}  // namespace network_example

#endif  // KERNEL_SRC_KERNEL_H_
