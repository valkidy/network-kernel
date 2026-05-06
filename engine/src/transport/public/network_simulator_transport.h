#ifndef TRANSPORT_PUBLIC_NETWORK_SIMULATOR_TRANSPORT_H_
#define TRANSPORT_PUBLIC_NETWORK_SIMULATOR_TRANSPORT_H_

#include <cstdint>
#include <deque>

#include "transport/public/itransport.h"

namespace network_example {

struct NetworkSimulatorConfig {
    std::uint32_t latency_ticks = 0;
    std::uint32_t jitter_ticks = 0;
    std::uint32_t drop_every_nth_packet = 0;
    std::uint32_t duplicate_every_nth_packet = 0;
    bool reorder_pairs = false;
};

class NetworkSimulatorTransport final : public ITransport {
public:
    explicit NetworkSimulatorTransport(NetworkSimulatorConfig config = {});

    bool StartClient(const char* address) override;
    bool StartServer(std::uint16_t port) override;
    void Stop() override;

    bool Send(
        PeerId peer,
        const void* data,
        std::uint32_t size,
        SendMode mode,
        ChannelId channel) override;

    bool PollEvent(TransportEvent& out_event) override;

    void AdvanceTicks(std::uint32_t ticks);
    bool running() const;

private:
    struct ScheduledEvent {
        std::uint32_t delivery_tick = 0;
        std::uint32_t order = 0;
        TransportEvent event;
    };

    std::uint32_t delivery_tick_for_packet();
    void enqueue(ScheduledEvent event);

    NetworkSimulatorConfig config_;
    std::deque<ScheduledEvent> scheduled_events_;
    std::uint32_t current_tick_ = 0;
    std::uint32_t packet_count_ = 0;
    std::uint32_t next_order_ = 0;
    bool running_ = false;
};

}  // namespace network_example

#endif  // TRANSPORT_PUBLIC_NETWORK_SIMULATOR_TRANSPORT_H_
