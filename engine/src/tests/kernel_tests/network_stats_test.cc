#include <cstdio>
#include <cstdlib>
#include <memory>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/public/kernel_api.h"
#include "protocol/public/network_packets.h"
#include "protocol/public/session_packets.h"
#include "transport/public/loopback_transport.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

void require_impl(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d\n", line);
        std::abort();
    }
}

#define require(condition) require_impl((condition), __LINE__)

void client_ping_pong_applies_server_network_stats() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 1000;
    config.tick.snapshot_rate = 100;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_Client);
    engine.transport_ = std::make_unique<network_example::LoopbackTransport>();
    require(engine.transport_->StartServer(7790));
    engine.client_local_time_us_ = 456000;

    network_example::PingPongPacket ping{9, 123000, 0, 0};
    ping.server_rtt_us = 19000;
    ping.server_jitter_us = 3000;
    network_example::TransportEvent event;
    event.peer = 0;
    event.channel = network_example::ChannelId::kSession;
    event.payload = network_example::encode_ping_pong_packet(ping, 2);
    engine.handle_client_ping_pong(event);

    KernelNetworkStats stats{};
    stats.struct_size = sizeof(stats);
    require(engine.get_network_stats(&stats));
    require(stats.rtt_us == 19000);
    require(stats.jitter_us == 3000);
}

void server_clock_sync_ping_carries_session_network_stats() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 1000;
    config.tick.snapshot_rate = 100;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    auto loopback = std::make_unique<network_example::LoopbackTransport>();
    require(loopback->StartServer(7791));
    auto* loopback_transport = loopback.get();
    engine.transport_ = std::move(loopback);

    engine.peer_sessions_.push_back(network_example::KernelEngine::PeerSession{});
    network_example::KernelEngine::PeerSession& session =
        engine.peer_sessions_.back();
    session.peer = 7;
    session.player = 11;
    session.welcomed = true;
    session.last_clock_sync_rtt_us = 22000;
    session.last_clock_sync_jitter_us = 4000;

    engine.send_clock_sync_ping(&session, 100000);

    network_example::TransportEvent event;
    require(loopback_transport->PollClientEvent(event));
    network_example::PingPongPacket ping;
    require(network_example::decode_ping_pong_packet(
        event.payload.data(),
        event.payload.size(),
        &ping));
    require(ping.server_rtt_us == 22000);
    require(ping.server_jitter_us == 4000);
}

void received_packet_sequence_gaps_update_loss_ratio() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 1000;
    config.tick.snapshot_rate = 100;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_Client);

    PlayerInput input{};
    const std::vector<std::uint8_t> first_packet =
        network_example::encode_input_packet(7, input, 1);
    const std::vector<std::uint8_t> fourth_packet =
        network_example::encode_input_packet(7, input, 4);
    network_example::TransportEvent event;
    event.type = network_example::TransportEventType::kMessage;
    event.peer = 0;
    event.channel = network_example::ChannelId::kInput;

    event.payload = first_packet;
    engine.record_received_packet_sequence(event);
    event.payload = fourth_packet;
    engine.record_received_packet_sequence(event);

    KernelNetworkStats stats{};
    stats.struct_size = sizeof(stats);
    require(engine.get_network_stats(&stats));
    require(stats.loss_ratio > 0.499f);
    require(stats.loss_ratio < 0.501f);
}

}  // namespace

int main() {
    client_ping_pong_applies_server_network_stats();
    server_clock_sync_ping_carries_session_network_stats();
    received_packet_sequence_gaps_update_loss_ratio();
    return 0;
}
