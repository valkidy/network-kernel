#include <array>
#include <cstdio>
#include <cstdlib>

#include "transport/public/network_simulator_transport.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

}  // namespace

#define require(condition) \
    require_impl(static_cast<bool>(condition), #condition, __LINE__)

int main() {
    network_example::NetworkSimulatorConfig duplicate_config{};
    duplicate_config.latency_ticks = 2;
    duplicate_config.duplicate_every_nth_packet = 2;
    network_example::NetworkSimulatorTransport transport(duplicate_config);
    require(transport.StartServer(7777));

    const std::array<std::uint8_t, 1> first = {1};
    const std::array<std::uint8_t, 1> second = {2};
    require(transport.Send(1, first.data(), first.size(), network_example::SendMode::kReliable,
                          network_example::ChannelId::kSession));
    require(transport.Send(1, second.data(), second.size(), network_example::SendMode::kReliable,
                          network_example::ChannelId::kSession));

    network_example::TransportEvent event;
    require(!transport.PollEvent(event));
    transport.AdvanceTicks(2);
    require(transport.PollEvent(event));
    require(event.payload[0] == 1);
    require(transport.PollEvent(event));
    require(event.payload[0] == 2);
    require(transport.PollEvent(event));
    require(event.payload[0] == 2);

    network_example::NetworkSimulatorConfig sim_config{};
    sim_config.latency_ticks = 3;
    sim_config.drop_every_nth_packet = 20;
    sim_config.jitter_ticks = 1;
    sim_config.reorder_pairs = true;
    network_example::NetworkSimulatorTransport sim_transport(sim_config);
    require(sim_transport.StartServer(7777));

    std::uint32_t accepted_sends = 0;
    for (std::uint8_t value = 1; value <= 40; ++value) {
        const std::array<std::uint8_t, 1> payload = {value};
        require(sim_transport.Send(
            1,
            payload.data(),
            payload.size(),
            network_example::SendMode::kUnreliable,
            network_example::ChannelId::kInput));
        ++accepted_sends;
    }
    require(accepted_sends == 40);

    require(!sim_transport.PollEvent(event));
    sim_transport.AdvanceTicks(3);
    require(sim_transport.PollEvent(event));
    require(event.payload[0] == 2);

    std::uint32_t delivered = 1;
    while (sim_transport.PollEvent(event)) {
        ++delivered;
    }
    sim_transport.AdvanceTicks(10);
    while (sim_transport.PollEvent(event)) {
        require(event.payload[0] != 20);
        require(event.payload[0] != 40);
        ++delivered;
    }
    require(delivered == 38);
    return 0;
}
