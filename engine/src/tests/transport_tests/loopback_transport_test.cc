#include <array>
#include <cassert>
#include <vector>

#include "transport/public/loopback_transport.h"

int main() {
    network_example::LoopbackTransport transport;
    assert(transport.StartServer(7777));
    assert(transport.running());

    const std::array<std::uint8_t, 3> input_payload = {1, 2, 3};
    assert(transport.SendClient(
        1,
        input_payload.data(),
        input_payload.size(),
        network_example::SendMode::kUnreliable,
        network_example::ChannelId::kInput));

    network_example::TransportEvent server_event;
    assert(transport.PollEvent(server_event));
    assert(server_event.peer == 1);
    assert(server_event.channel == network_example::ChannelId::kInput);
    assert(server_event.mode == network_example::SendMode::kUnreliable);
    assert(server_event.payload ==
           std::vector<std::uint8_t>(input_payload.begin(), input_payload.end()));

    const std::array<std::uint8_t, 2> snapshot_payload = {8, 9};
    assert(transport.Send(
        1,
        snapshot_payload.data(),
        snapshot_payload.size(),
        network_example::SendMode::kReliable,
        network_example::ChannelId::kSnapshot));

    network_example::TransportEvent client_event;
    assert(transport.PollClientEvent(client_event));
    assert(client_event.peer == 1);
    assert(client_event.channel == network_example::ChannelId::kSnapshot);
    assert(client_event.mode == network_example::SendMode::kReliable);
    assert(client_event.payload ==
           std::vector<std::uint8_t>(snapshot_payload.begin(), snapshot_payload.end()));
    assert(!transport.PollEvent(server_event));
    assert(!transport.PollClientEvent(client_event));
    return 0;
}
