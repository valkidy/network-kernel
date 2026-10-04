#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "transport/public/loopback_transport.h"

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
    network_example::LoopbackTransport transport;
    require(transport.StartServer(7777));
    require(transport.running());

    const std::array<std::uint8_t, 3> input_payload = {1, 2, 3};
    require(transport.SendClient(
        1,
        input_payload.data(),
        input_payload.size(),
        network_example::SendMode::kUnreliable,
        network_example::ChannelId::kInput));

    network_example::TransportEvent server_event;
    require(transport.PollEvent(server_event));
    require(server_event.peer == 1);
    require(server_event.channel == network_example::ChannelId::kInput);
    require(server_event.mode == network_example::SendMode::kUnreliable);
    require(server_event.payload ==
           std::vector<std::uint8_t>(input_payload.begin(), input_payload.end()));

    const std::array<std::uint8_t, 2> snapshot_payload = {8, 9};
    require(transport.Send(
        1,
        snapshot_payload.data(),
        snapshot_payload.size(),
        network_example::SendMode::kReliable,
        network_example::ChannelId::kSnapshot));

    network_example::TransportEvent client_event;
    require(transport.PollClientEvent(client_event));
    require(client_event.peer == 1);
    require(client_event.channel == network_example::ChannelId::kSnapshot);
    require(client_event.mode == network_example::SendMode::kReliable);
    require(client_event.payload ==
           std::vector<std::uint8_t>(snapshot_payload.begin(), snapshot_payload.end()));
    require(!transport.PollEvent(server_event));
    require(!transport.PollClientEvent(client_event));
    return 0;
}
