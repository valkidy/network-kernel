#include <array>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <vector>

#include "transport/public/gns_transport.h"

namespace {

// assert is compiled out under -c opt; these checks have to run there too.
void require_impl(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}
#define require(condition) require_impl((condition), #condition, __LINE__)

void fake_network_conditions_parse_or_stay_off() {
    using network_example::parse_gns_fake_network_conditions;
    const auto off = parse_gns_fake_network_conditions(nullptr, nullptr, nullptr, nullptr);
    require(!off.any());

    const auto full = parse_gns_fake_network_conditions("100", "25", "80", "1.5");
    require(full.any());
    require(full.lag_ms == 100);
    require(full.jitter_mean_ms == 25.0f);
    require(full.jitter_max_ms == 80.0f);
    require(full.loss_pct == 1.5f);

    // No cap given: four times the mean.
    const auto uncapped = parse_gns_fake_network_conditions(nullptr, "25", nullptr, nullptr);
    require(uncapped.any());
    require(uncapped.jitter_max_ms == 100.0f);

    // Anything that is not a number in range is off, never a guess.
    const auto junk = parse_gns_fake_network_conditions("100ms", "-5", "", "101");
    require(!junk.any());
    require(parse_gns_fake_network_conditions("", nullptr, nullptr, nullptr).lag_ms == 0);
}

}  // namespace

int main() {
    fake_network_conditions_parse_or_stay_off();
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

    const std::vector<std::uint8_t> presentation_payload =
        network_example::encode_gns_payload(
            network_example::ChannelId::kPresentation,
            network_example::SendMode::kUnreliable,
            input.data(),
            static_cast<std::uint32_t>(input.size()));
    assert(network_example::decode_gns_payload(
        presentation_payload.data(),
        presentation_payload.size(),
        &channel,
        &mode,
        &payload));
    assert(channel == network_example::ChannelId::kPresentation);
    assert(mode == network_example::SendMode::kUnreliable);

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
