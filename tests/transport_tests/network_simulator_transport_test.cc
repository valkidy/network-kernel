#include <array>
#include <cassert>

#include "transport/public/network_simulator_transport.h"

int main() {
    network_example::NetworkSimulatorConfig config{};
    config.latency_ticks = 2;
    config.duplicate_every_nth_packet = 2;
    network_example::NetworkSimulatorTransport transport(config);
    assert(transport.StartServer(7777));

    const std::array<std::uint8_t, 1> first = {1};
    const std::array<std::uint8_t, 1> second = {2};
    assert(transport.Send(1, first.data(), first.size(), network_example::SendMode::kReliable,
                          network_example::ChannelId::kSession));
    assert(transport.Send(1, second.data(), second.size(), network_example::SendMode::kReliable,
                          network_example::ChannelId::kSession));

    network_example::TransportEvent event;
    assert(!transport.PollEvent(event));
    transport.AdvanceTicks(2);
    assert(transport.PollEvent(event));
    assert(event.payload[0] == 1);
    assert(transport.PollEvent(event));
    assert(event.payload[0] == 2);
    assert(transport.PollEvent(event));
    assert(event.payload[0] == 2);
    return 0;
}
