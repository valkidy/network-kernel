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

private:
    void push_event(
        KernelEventType type,
        NetId net_id = 0,
        PeerId peer_id = 0,
        std::uint32_t code = 0);
    void poll_transport();
    void poll_client_transport();
    void simulate_tick();
    void rebuild_render_states();
    void rebuild_render_states_from_world();
    void rebuild_render_states_from_snapshot();
    void publish_snapshot();

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
    std::uint32_t last_processed_input_seq_ = 0;
    std::uint32_t next_packet_sequence_ = 1;
    bool has_client_snapshot_ = false;
    bool running_ = false;
};

}  // namespace network_example

#endif  // KERNEL_SRC_KERNEL_H_
