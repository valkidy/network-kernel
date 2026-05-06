#include <array>
#include <cassert>

#include "transport/public/network_simulator_transport.h"

int main() {
    network_example::NetworkSimulatorConfig duplicate_config{};
    duplicate_config.latency_ticks = 2;
    duplicate_config.duplicate_every_nth_packet = 2;
    network_example::NetworkSimulatorTransport transport(duplicate_config);
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

    network_example::NetworkSimulatorConfig m5_config{};
    m5_config.latency_ticks = 3;
    m5_config.drop_every_nth_packet = 20;
    m5_config.jitter_ticks = 1;
    m5_config.reorder_pairs = true;
    network_example::NetworkSimulatorTransport m5_transport(m5_config);
    assert(m5_transport.StartServer(7777));

    std::uint32_t accepted_sends = 0;
    for (std::uint8_t value = 1; value <= 40; ++value) {
        const std::array<std::uint8_t, 1> payload = {value};
        assert(m5_transport.Send(
            1,
            payload.data(),
            payload.size(),
            network_example::SendMode::kUnreliable,
            network_example::ChannelId::kInput));
        ++accepted_sends;
    }
    assert(accepted_sends == 40);

    assert(!m5_transport.PollEvent(event));
    m5_transport.AdvanceTicks(3);
    assert(m5_transport.PollEvent(event));
    assert(event.payload[0] == 2);

    std::uint32_t delivered = 1;
    while (m5_transport.PollEvent(event)) {
        ++delivered;
    }
    m5_transport.AdvanceTicks(10);
    while (m5_transport.PollEvent(event)) {
        assert(event.payload[0] != 20);
        assert(event.payload[0] != 40);
        ++delivered;
    }
    assert(delivered == 38);
    return 0;
}
