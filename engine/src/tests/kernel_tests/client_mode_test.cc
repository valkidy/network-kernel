#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/public/kernel_api.h"
#include "protocol/public/session_packets.h"
#include "transport/public/loopback_transport.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

void presentation_gate_releases_at_render_time() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    network_example::KernelEngine engine(config);
    engine.pending_presentation_events_.push_back(KernelEvent{
        KernelEventType_DamageApplied,
        3,
        11,
        0,
        7,
        100000,
        100000});

    std::array<KernelEvent, 4> events{};
    assert(engine.poll_events(events.data(), static_cast<std::uint32_t>(events.size())) == 0);

    network_example::WorldSnapshot early_snapshot;
    early_snapshot.header.server_tick = 2;
    engine.handle_client_snapshot(early_snapshot);
    engine.rebuild_render_states();
    assert(engine.poll_events(events.data(), static_cast<std::uint32_t>(events.size())) == 0);

    network_example::WorldSnapshot later_snapshot;
    later_snapshot.header.server_tick = 10;
    engine.handle_client_snapshot(later_snapshot);
    engine.rebuild_render_states();

    const std::uint32_t event_count =
        engine.poll_events(events.data(), static_cast<std::uint32_t>(events.size()));
    assert(event_count == 1);
    assert(events[0].type == KernelEventType_DamageApplied);
    assert(events[0].event_time_us == 100000);
    assert(events[0].presentation_time_us == 100000);
}

void clock_sync_ping_pong_updates_peer_offset() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 1000;
    config.tick.snapshot_rate = 100;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    for (std::uint32_t tick = 0; tick < 120; ++tick) {
        engine.tick_loop_.advance_tick();
    }

    engine.peer_sessions_.push_back(network_example::KernelEngine::PeerSession{
        7,
        11,
        0,
        true,
        {}});
    network_example::KernelEngine::PeerSession& session = engine.peer_sessions_.back();
    session.pending_clock_sync_nonce = 77;
    session.pending_clock_sync_server_time_us = 100000;

    const network_example::PingPongPacket pong{
        77,
        100000,
        150000,
        151000,
    };
    network_example::TransportEvent event;
    event.peer = 7;
    event.channel = network_example::ChannelId::kSession;
    event.payload = network_example::encode_ping_pong_packet(pong, 1);
    engine.handle_server_ping_pong(event);

    assert(session.has_clock_sync);
    assert(session.pending_clock_sync_nonce == 0);
    assert(session.clock_offset_us == -40500);
    assert(session.last_clock_sync_rtt_us == 19000);
    assert(engine.convert_client_action_time_to_server_time(7, 180500, 120000) ==
           140000);
}

void compensation_clamps_not_rejects_client_local_time() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 1000;
    config.tick.snapshot_rate = 100;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    engine.peer_sessions_.push_back(network_example::KernelEngine::PeerSession{
        7,
        11,
        0,
        true,
        {}});
    network_example::KernelEngine::PeerSession& session = engine.peer_sessions_.back();
    session.has_clock_sync = true;
    session.clock_offset_us = -50000;

    const std::uint64_t received_server_time_us = 200000;
    assert(engine.convert_client_action_time_to_server_time(
               7,
               180000,
               received_server_time_us) == 130000);

    PlayerInput input{};
    input.client_action_time_us = 180000;
    network_example::QueuedInput within_window{
        7,
        input,
        200,
        engine.convert_client_action_time_to_server_time(
            7,
            input.client_action_time_us,
            received_server_time_us),
        true,
    };
    assert(engine.compensated_action_time_us(within_window) == 130000);
    assert(engine.rewind_tick_for_input(within_window) == 130);

    input.client_action_time_us = 100000;
    network_example::QueuedInput older_than_window{
        7,
        input,
        200,
        engine.convert_client_action_time_to_server_time(
            7,
            input.client_action_time_us,
            received_server_time_us),
        true,
    };
    assert(engine.compensated_action_time_us(older_than_window) == 100000);
    assert(engine.rewind_tick_for_input(older_than_window) == 100);

    input.client_action_time_us = 275000;
    network_example::QueuedInput newer_than_receive{
        7,
        input,
        200,
        engine.convert_client_action_time_to_server_time(
            7,
            input.client_action_time_us,
            received_server_time_us),
        true,
    };
    assert(engine.compensated_action_time_us(newer_than_receive) == 200000);
    assert(engine.rewind_tick_for_input(newer_than_receive) == 200);

    session.has_clock_sync = false;
    assert(engine.convert_client_action_time_to_server_time(
               7,
               180000,
               received_server_time_us) == received_server_time_us);
}

void client_replies_to_clock_sync_ping() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 1000;
    config.tick.snapshot_rate = 100;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_Client);
    engine.transport_ = std::make_unique<network_example::LoopbackTransport>();
    assert(engine.transport_->StartServer(7777));
    engine.client_local_time_us_ = 456000;

    network_example::TransportEvent event;
    event.peer = 0;
    event.channel = network_example::ChannelId::kSession;
    event.payload = network_example::encode_ping_pong_packet(
        network_example::PingPongPacket{9, 123000, 0, 0},
        2);
    engine.handle_client_ping_pong(event);

    auto* loopback =
        static_cast<network_example::LoopbackTransport*>(engine.transport_.get());
    network_example::TransportEvent reply;
    assert(loopback->PollClientEvent(reply));
    network_example::PingPongPacket decoded_reply;
    assert(network_example::decode_ping_pong_packet(
        reply.payload.data(),
        reply.payload.size(),
        &decoded_reply));
    assert(decoded_reply.nonce == 9);
    assert(decoded_reply.server_send_time_us == 123000);
    assert(decoded_reply.client_receive_time_us == 456000);
    assert(decoded_reply.client_send_time_us == 456000);
}

}  // namespace

int main() {
    presentation_gate_releases_at_render_time();
    clock_sync_ping_pong_updates_peer_offset();
    compensation_clamps_not_rejects_client_local_time();
    client_replies_to_clock_sync_ping();

    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartClient(kernel, "127.0.0.1:9"));

    PlayerInput input{};
    input.input_seq = 1;
    input.client_action_time_us = 33333;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    Kernel_SubmitInput(kernel, 0, &input);

    std::array<KernelEvent, 8> events{};
    const std::uint32_t event_count =
        Kernel_PollEvents(kernel, events.data(), static_cast<std::uint32_t>(events.size()));
    bool saw_error = false;
    for (std::uint32_t index = 0; index < event_count; ++index) {
        saw_error = saw_error || events[index].type == KernelEventType_Error;
    }
    assert(saw_error);

    Kernel_Destroy(kernel);
    return 0;
}
