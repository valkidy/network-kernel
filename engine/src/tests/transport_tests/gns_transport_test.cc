#include <array>
#include <cassert>
#include <vector>

#include "transport/public/gns_transport.h"

int main() {
    network_example::GnsEndpoint endpoint;
    assert(network_example::parse_gns_address("127.0.0.1:7777", &endpoint));
    assert(endpoint.host == "127.0.0.1");
    assert(endpoint.port == 7777);
    assert(!network_example::parse_gns_address(nullptr, &endpoint));
    assert(!network_example::parse_gns_address("127.0.0.1", &endpoint));
    assert(!network_example::parse_gns_address("127.0.0.1:not-a-port", &endpoint));
    assert(!network_example::parse_gns_address("127.0.0.1:0", &endpoint));

    const std::array<std::uint8_t, 3> input = {1, 2, 3};
    const std::vector<std::uint8_t> encoded = network_example::encode_gns_payload(
        network_example::ChannelId::kInput,
        network_example::SendMode::kUnreliable,
        input.data(),
        static_cast<std::uint32_t>(input.size()));

    network_example::ChannelId channel = network_example::ChannelId::kSession;
    network_example::SendMode mode = network_example::SendMode::kReliable;
    std::vector<std::uint8_t> payload;
    assert(network_example::decode_gns_payload(
        encoded.data(),
        encoded.size(),
        &channel,
        &mode,
        &payload));
    assert(channel == network_example::ChannelId::kInput);
    assert(mode == network_example::SendMode::kUnreliable);
    assert(payload == std::vector<std::uint8_t>(input.begin(), input.end()));

    const std::vector<std::uint8_t> session_payload = network_example::encode_gns_payload(
        network_example::ChannelId::kSession,
        network_example::SendMode::kReliable,
        nullptr,
        0);
    assert(network_example::decode_gns_payload(
        session_payload.data(),
        session_payload.size(),
        &channel,
        &mode,
        &payload));
    assert(channel == network_example::ChannelId::kSession);
    assert(mode == network_example::SendMode::kReliable);
    assert(payload.empty());

    std::vector<std::uint8_t> bad_channel = encoded;
    bad_channel[0] = 255;
    assert(!network_example::decode_gns_payload(
        bad_channel.data(),
        bad_channel.size(),
        &channel,
        &mode,
        &payload));

    network_example::GnsTransport first;
    network_example::GnsTransport second;
    assert(!network_example::gns_callback_router_has_owner_for_testing());
    network_example::gns_callback_router_set_owner_for_testing(&first);
    assert(network_example::gns_callback_router_has_owner_for_testing());
    network_example::gns_callback_router_clear_owner_for_testing(&second);
    assert(network_example::gns_callback_router_has_owner_for_testing());
    network_example::gns_callback_router_clear_owner_for_testing(&first);
    assert(!network_example::gns_callback_router_has_owner_for_testing());

    network_example::GnsTransport server;
    assert(server.StartServer(7797));
    assert(server.running());
    server.Stop();
    return 0;
}
