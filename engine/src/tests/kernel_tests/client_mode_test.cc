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

network_example::WorldSnapshot snapshot_with_entity(
    std::uint32_t server_tick,
    network_example::NetId net_id,
    network_example::EntityType type,
    float position_x) {
    network_example::WorldSnapshot snapshot;
    snapshot.header.server_tick = server_tick;
    network_example::EntitySnapshot entity;
    entity.net_id = net_id;
    entity.type = type;
    entity.position = glm::vec3{position_x, 0.0f, 0.0f};
    snapshot.entities.push_back(entity);
    return snapshot;
}

void add_snapshot_entity(
    network_example::WorldSnapshot* snapshot,
    network_example::NetId net_id,
    network_example::EntityType type,
    float position_x) {
    network_example::EntitySnapshot entity;
    entity.net_id = net_id;
    entity.type = type;
    entity.position = glm::vec3{position_x, 0.0f, 0.0f};
    snapshot->entities.push_back(entity);
}

network_example::WorldSnapshot projectile_snapshot(
    std::uint32_t server_tick,
    network_example::NetId net_id,
    network_example::PeerId owner_peer,
    std::uint32_t client_action_id,
    const glm::vec3& position,
    const glm::vec3& velocity) {
    network_example::WorldSnapshot snapshot;
    snapshot.header.server_tick = server_tick;
    network_example::EntitySnapshot entity;
    entity.net_id = net_id;
    entity.type = network_example::EntityType::kProjectile;
    entity.owner_peer = owner_peer;
    entity.position = position;
    entity.velocity = velocity;
    entity.spawn_tick = server_tick;
    entity.client_action_id = client_action_id;
    snapshot.entities.push_back(entity);
    return snapshot;
}

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
    assert(engine.has_client_clock_sync_);
    assert(engine.client_clock_offset_us_ == -333000);

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

void render_states_at_time_interpolates_and_clamps() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 1000;
    config.tick.snapshot_rate = 100;

    network_example::KernelEngine empty_engine(config);
    std::array<RenderEntityState, 4> states{};
    assert(empty_engine.get_render_states_at_time(
               31000,
               states.data(),
               static_cast<std::uint32_t>(states.size())) == 0);

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_Client);
    engine.has_client_clock_sync_ = true;
    engine.client_clock_offset_us_ = 0;
    engine.handle_client_snapshot(snapshot_with_entity(
        10,
        42,
        network_example::EntityType::kEnemy,
        0.0f));
    engine.handle_client_snapshot(snapshot_with_entity(
        12,
        42,
        network_example::EntityType::kEnemy,
        20.0f));

    std::uint32_t count = engine.get_render_states_at_time(
        31000,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    assert(count == 1);
    assert(states[0].net_id == 42);
    assert(states[0].position.x > 9.99f);
    assert(states[0].position.x < 10.01f);

    count = engine.get_render_states_at_time(
        25000,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    assert(count == 1);
    assert(states[0].position.x == 0.0f);

    count = engine.get_render_states_at_time(
        40000,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    assert(count == 1);
    assert(states[0].position.x == 20.0f);

    network_example::KernelEngine single_snapshot_engine(config);
    single_snapshot_engine.reset_runtime_state(KernelMode_Client);
    single_snapshot_engine.has_client_clock_sync_ = true;
    single_snapshot_engine.handle_client_snapshot(snapshot_with_entity(
        10,
        77,
        network_example::EntityType::kEnemy,
        7.0f));
    count = single_snapshot_engine.get_render_states_at_time(
        999999,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    assert(count == 1);
    assert(states[0].net_id == 77);
    assert(states[0].position.x == 7.0f);
}

void remote_projectile_uses_interpolated_past_timeline() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 1000;
    config.tick.snapshot_rate = 100;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_Client);
    engine.has_client_clock_sync_ = true;
    engine.client_clock_offset_us_ = 0;

    network_example::WorldSnapshot from = snapshot_with_entity(
        10,
        42,
        network_example::EntityType::kEnemy,
        0.0f);
    add_snapshot_entity(
        &from,
        43,
        network_example::EntityType::kProjectile,
        100.0f);
    network_example::WorldSnapshot to = snapshot_with_entity(
        12,
        42,
        network_example::EntityType::kEnemy,
        20.0f);
    add_snapshot_entity(
        &to,
        43,
        network_example::EntityType::kProjectile,
        120.0f);
    engine.handle_client_snapshot(from);
    engine.handle_client_snapshot(to);

    std::array<RenderEntityState, 4> states{};
    const std::uint32_t count = engine.get_render_states_at_time(
        31000,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    assert(count == 2);
    bool saw_projectile = false;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].net_id == 43) {
            saw_projectile = true;
            assert(states[index].entity_type == 3);
            assert(states[index].position.x > 109.99f);
            assert(states[index].position.x < 110.01f);
        }
    }
    assert(saw_projectile);
}

void local_projectile_snapshot_fast_forwards_and_smooths() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 1000;
    config.tick.snapshot_rate = 100;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_Client);
    engine.local_client_peer_id_ = 7;
    for (std::uint32_t tick = 0; tick < 20; ++tick) {
        engine.tick_loop_.advance_tick();
    }

    network_example::KernelEngine::PredictedProjectile predicted;
    predicted.entity_id = 9000;
    predicted.owner_peer = 7;
    predicted.input_seq = 3;
    predicted.client_action_id = 4444;
    predicted.spawn_tick = 20;
    predicted.position = glm::vec3{6.2f, 0.0f, 0.0f};
    predicted.velocity = glm::vec3{100.0f, 0.0f, 0.0f};
    predicted.spawn_position = predicted.position;
    predicted.initial_velocity = predicted.velocity;
    predicted.motion_model = network_example::ProjectileMotionModel::kLinear;
    predicted.correction_offset = glm::vec3{0.0f, 0.0f, 0.0f};
    engine.predicted_projectiles_.push_back(predicted);

    engine.handle_client_snapshot(projectile_snapshot(
        10,
        55,
        7,
        4444,
        glm::vec3{5.0f, 0.0f, 0.0f},
        glm::vec3{100.0f, 0.0f, 0.0f}));

    assert(engine.predicted_projectiles_.size() == 1);
    const network_example::KernelEngine::PredictedProjectile& bound =
        engine.predicted_projectiles_[0];
    assert(bound.entity_id == 9000);
    assert(bound.net_id == 55);
    assert(bound.bound);
    assert(bound.spawn_position.x == 5.0f);
    assert(bound.initial_velocity.x == 100.0f);
    assert(bound.age_seconds > 0.0099f);
    assert(bound.age_seconds < 0.0101f);
    assert(bound.position.x > 5.99f);
    assert(bound.position.x < 6.01f);
    assert(bound.correction_offset.x > 0.19f);
    assert(bound.correction_offset.x < 0.21f);

    engine.rebuild_render_states();
    assert(engine.render_states_.size() == 1);
    assert(engine.render_states_[0].entity_id == 9000);
    assert(engine.render_states_[0].net_id == 55);
    assert(engine.render_states_[0].position.x > 6.19f);
    assert(engine.render_states_[0].position.x < 6.21f);

    engine.rebuild_render_states();
    assert(engine.render_states_.size() == 1);
    assert(engine.render_states_[0].position.x > 6.09f);
    assert(engine.render_states_[0].position.x < 6.11f);
}

void render_query_does_not_consume_local_correction() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 1000;
    config.tick.snapshot_rate = 100;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_Client);
    engine.has_client_clock_sync_ = true;
    engine.local_player_net_id_ = 1;
    engine.predicted_local_entity_.net_id = 1;
    engine.predicted_local_entity_.type = network_example::EntityType::kPlayer;
    engine.predicted_local_entity_.position = glm::vec3{1.0f, 0.0f, 0.0f};
    engine.has_predicted_local_entity_ = true;
    engine.local_correction_offset_ = glm::vec3{4.0f, 0.0f, 0.0f};
    engine.latest_client_snapshot_ = snapshot_with_entity(
        10,
        1,
        network_example::EntityType::kPlayer,
        0.0f);
    engine.client_snapshot_buffer_.push_back(engine.latest_client_snapshot_);
    engine.has_client_snapshot_ = true;

    std::array<RenderEntityState, 4> first_states{};
    const std::uint32_t first_count = engine.get_render_states_at_time(
        31000,
        first_states.data(),
        static_cast<std::uint32_t>(first_states.size()));
    std::array<RenderEntityState, 4> second_states{};
    const std::uint32_t second_count = engine.get_render_states_at_time(
        31000,
        second_states.data(),
        static_cast<std::uint32_t>(second_states.size()));

    assert(first_count == 1);
    assert(second_count == 1);
    assert(first_states[0].position.x == 5.0f);
    assert(second_states[0].position.x == first_states[0].position.x);
    assert(engine.local_correction_offset_.x == 4.0f);
}

}  // namespace

int main() {
    presentation_gate_releases_at_render_time();
    clock_sync_ping_pong_updates_peer_offset();
    compensation_clamps_not_rejects_client_local_time();
    client_replies_to_clock_sync_ping();
    render_states_at_time_interpolates_and_clamps();
    remote_projectile_uses_interpolated_past_timeline();
    local_projectile_snapshot_fast_forwards_and_smooths();
    render_query_does_not_consume_local_correction();

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
