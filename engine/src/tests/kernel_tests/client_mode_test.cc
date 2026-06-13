#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

KernelColliderTemplateDefinition projectile_collider_template() {
    KernelColliderTemplateDefinition collider_template{};
    collider_template.struct_size = sizeof(collider_template);
    collider_template.template_id = 10;
    collider_template.shape_type = KernelColliderShapeType_Sphere;
    collider_template.radius = 0.25f;
    collider_template.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;
    collider_template.purpose_flags = KernelColliderPurpose_Damage;
    return collider_template;
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

void client_applies_server_tick_config_from_welcome() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_Client);
    require(engine.tick_loop_.fixed_delta_seconds() > 0.0333f);
    require(engine.tick_loop_.snapshot_interval_ticks() == 2);

    network_example::TransportEvent event;
    event.peer = 0;
    event.channel = network_example::ChannelId::kSession;
    event.payload = network_example::encode_welcome_packet(
        network_example::WelcomePacket{9, 77, 120, 60, 20},
        3);
    engine.handle_client_session_message(event);

    require(engine.has_welcome_);
    require(engine.local_client_peer_id_ == 9);
    require(engine.local_player_net_id_ == 77);
    require(engine.config_.tick.server_tick_rate == 60);
    require(engine.config_.tick.snapshot_rate == 20);
    require(engine.tick_loop_.fixed_delta_seconds() > 0.0166f);
    require(engine.tick_loop_.fixed_delta_seconds() < 0.0167f);
    require(engine.tick_loop_.snapshot_interval_ticks() == 3);
}

void client_clock_offset_smooths_after_initial_sync() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 1000;
    config.tick.snapshot_rate = 100;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_Client);
    engine.transport_ = std::make_unique<network_example::LoopbackTransport>();
    require(engine.transport_->StartServer(7778));

    engine.client_local_time_us_ = 456000;
    network_example::TransportEvent first_event;
    first_event.peer = 0;
    first_event.channel = network_example::ChannelId::kSession;
    first_event.payload = network_example::encode_ping_pong_packet(
        network_example::PingPongPacket{10, 123000, 0, 0},
        4);
    engine.handle_client_ping_pong(first_event);
    require(engine.has_client_clock_sync_);
    require(engine.client_clock_offset_us_ == -333000);

    engine.client_local_time_us_ = 457000;
    network_example::TransportEvent second_event;
    second_event.peer = 0;
    second_event.channel = network_example::ChannelId::kSession;
    second_event.payload = network_example::encode_ping_pong_packet(
        network_example::PingPongPacket{11, 323000, 0, 0},
        5);
    engine.handle_client_ping_pong(second_event);

    require(engine.client_clock_offset_us_ > -333000);
    require(engine.client_clock_offset_us_ < -133000);
}

void projectile_spawn_packet_uses_original_muzzle_position() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    auto loopback = std::make_unique<network_example::LoopbackTransport>();
    require(loopback->StartServer(7777));
    auto* loopback_transport = loopback.get();
    engine.transport_ = std::move(loopback);
    for (std::uint32_t tick = 0; tick < 7; ++tick) {
        engine.tick_loop_.advance_tick();
    }

    const glm::vec3 muzzle_position{-7.33337402f, 1.0f, -9.21669769f};
    const glm::vec3 compensated_position{-7.33337402f, 1.0f, -5.71669769f};
    const network_example::NetId projectile_net_id =
        engine.world_.spawn_projectile(
            7,
            compensated_position,
            glm::vec3{0.0f, 0.0f, 35.0f});
    const auto projectile_entity = engine.world_.find_entity(projectile_net_id);
    require(projectile_entity.has_value());
    network_example::ProjectileState& projectile =
        engine.world_.registry().get<network_example::ProjectileState>(
            *projectile_entity);
    projectile.spawn_tick = 4;
    projectile.spawn_position = muzzle_position;

    const network_example::WorldSnapshot snapshot =
        network_example::build_world_snapshot(engine.world_, 7, 233, 0);
    const network_example::EntitySnapshot* entity = nullptr;
    for (const network_example::EntitySnapshot& snapshot_entity : snapshot.entities) {
        if (snapshot_entity.net_id == projectile_net_id) {
            entity = &snapshot_entity;
            break;
        }
    }
    require(entity != nullptr);
    require(entity->position.z > -5.72f);
    require(entity->position.z < -5.71f);

    engine.send_entity_spawn(7, *entity);

    network_example::TransportEvent event;
    require(loopback_transport->PollClientEvent(event));
    network_example::EntitySpawnPacket packet;
    require(network_example::decode_entity_spawn_packet(
        event.payload.data(),
        event.payload.size(),
        &packet));
    require(packet.net_id == projectile_net_id);
    require(packet.position.x > -7.34f);
    require(packet.position.x < -7.33f);
    require(packet.position.y > 0.99f);
    require(packet.position.y < 1.01f);
    require(packet.position.z > -9.22f);
    require(packet.position.z < -9.21f);

    require(loopback_transport->PollClientEvent(event));
    network_example::ProjectileSpawnBatchPacket batch;
    require(network_example::decode_projectile_spawn_batch_packet(
        event.payload.data(),
        event.payload.size(),
        &batch));
    require(batch.server_tick == entity->spawn_tick);
    require(batch.groups.size() == 1);
    require(batch.groups[0].projectile_template_id == projectile.weapon_id);
    require(batch.groups[0].records.size() == 1);
    require(batch.groups[0].records[0].projectile_net_id == projectile_net_id);
    require(batch.groups[0].records[0].spawn_position.x > -7.34f);
    require(batch.groups[0].records[0].spawn_position.x < -7.33f);
    require(batch.groups[0].records[0].spawn_position.z > -9.22f);
    require(batch.groups[0].records[0].spawn_position.z < -9.21f);
    require(batch.groups[0].records[0].initial_velocity.z == 35.0f);
    KernelNetworkStats network_stats{};
    network_stats.struct_size = sizeof(network_stats);
    require(engine.get_network_stats(&network_stats));
    require(network_stats.packet_count_sent >= 2);
    require(network_stats.reliable_bytes_sent > 0);
    require(network_stats.event_bytes_sent > 0);
    require(network_stats.average_packet_size > 0);
    require(network_stats.max_packet_size > 0);
    require(network_stats.packet_serialization_cost_us > 0);
}

void snapshot_only_projectile_spawn_skips_event_batch() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    KernelProjectileTemplateDefinition projectile_template{};
    projectile_template.struct_size = sizeof(projectile_template);
    projectile_template.projectile_template_id = 3;
    projectile_template.weapon_id = 3;
    projectile_template.sync_mode = KernelProjectileSyncMode_ServerSnapshotOnly;
    projectile_template.collider_template_id = 10;
    KernelColliderTemplateDefinition collider_template = projectile_collider_template();
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.catalog_hash = 0x7777ull;
    catalog.projectile_templates = &projectile_template;
    catalog.projectile_template_count = 1;
    catalog.collider_templates = &collider_template;
    catalog.collider_template_count = 1;
    require(engine.load_gameplay_catalog(catalog));

    auto loopback = std::make_unique<network_example::LoopbackTransport>();
    require(loopback->StartServer(7778));
    auto* loopback_transport = loopback.get();
    engine.transport_ = std::move(loopback);

    const network_example::NetId projectile_net_id =
        engine.world_.spawn_projectile(
            7,
            glm::vec3{1.0f, 0.0f, 0.0f},
            glm::vec3{2.0f, 0.0f, 0.0f});
    const auto projectile_entity = engine.world_.find_entity(projectile_net_id);
    require(projectile_entity.has_value());
    network_example::ProjectileState& projectile =
        engine.world_.registry().get<network_example::ProjectileState>(
            *projectile_entity);
    projectile.weapon_id = 3;
    projectile.projectile_template_id = 3;

    const network_example::WorldSnapshot snapshot =
        network_example::build_world_snapshot(engine.world_, 1, 33, 0);
    const network_example::EntitySnapshot* entity = nullptr;
    for (const network_example::EntitySnapshot& snapshot_entity : snapshot.entities) {
        if (snapshot_entity.net_id == projectile_net_id) {
            entity = &snapshot_entity;
            break;
        }
    }
    require(entity != nullptr);

    engine.send_entity_spawn(7, *entity);

    network_example::TransportEvent event;
    require(loopback_transport->PollClientEvent(event));
    network_example::EntitySpawnPacket packet;
    require(network_example::decode_entity_spawn_packet(
        event.payload.data(),
        event.payload.size(),
        &packet));
    require(packet.net_id == projectile_net_id);
    require(!loopback_transport->PollClientEvent(event));
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

void homing_projectile_snapshot_extrapolation_is_bounded() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 1000;
    config.tick.snapshot_rate = 100;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_Client);
    engine.local_client_peer_id_ = 7;
    for (std::uint32_t tick = 0; tick < 500; ++tick) {
        engine.tick_loop_.advance_tick();
    }

    network_example::KernelEngine::PredictedProjectile predicted;
    predicted.entity_id = 9000;
    predicted.owner_peer = 7;
    predicted.input_seq = 3;
    predicted.client_action_id = 4444;
    predicted.spawn_tick = 500;
    predicted.position = glm::vec3{6.2f, 0.0f, 0.0f};
    predicted.velocity = glm::vec3{100.0f, 0.0f, 0.0f};
    predicted.spawn_position = predicted.position;
    predicted.initial_velocity = predicted.velocity;
    predicted.motion_model = network_example::ProjectileMotionModel::kHoming;
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
    assert(bound.bound);
    assert(bound.age_seconds > 0.199f);
    assert(bound.age_seconds < 0.201f);
    assert(bound.position.x > 24.99f);
    assert(bound.position.x < 25.01f);
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

void late_snapshot_is_stored_but_not_used_for_reconciliation() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 1000;
    config.tick.snapshot_rate = 100;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_Client);
    engine.local_client_peer_id_ = 7;
    engine.local_player_net_id_ = 1;
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
    engine.predicted_projectiles_.push_back(predicted);

    network_example::WorldSnapshot newer = snapshot_with_entity(
        10,
        1,
        network_example::EntityType::kPlayer,
        10.0f);
    add_snapshot_entity(
        &newer,
        55,
        network_example::EntityType::kProjectile,
        5.0f);
    newer.entities.back().owner_peer = 7;
    newer.entities.back().velocity = glm::vec3{100.0f, 0.0f, 0.0f};
    newer.entities.back().client_action_id = 4444;
    engine.handle_client_snapshot(newer);
    require(engine.latest_client_snapshot_.header.server_tick == 10);
    require(engine.predicted_local_entity_.position.x == 10.0f);
    require(engine.predicted_projectiles_.size() == 1);
    require(engine.predicted_projectiles_[0].bound);
    require(engine.predicted_projectiles_[0].net_id == 55);

    network_example::WorldSnapshot older = snapshot_with_entity(
        8,
        1,
        network_example::EntityType::kPlayer,
        -20.0f);
    engine.handle_client_snapshot(older);

    require(engine.latest_client_snapshot_.header.server_tick == 10);
    require(engine.predicted_local_entity_.position.x == 10.0f);
    require(engine.predicted_projectiles_.size() == 1);
    require(engine.predicted_projectiles_[0].net_id == 55);
    require(std::any_of(
        engine.client_snapshot_buffer_.begin(),
        engine.client_snapshot_buffer_.end(),
        [](const network_example::WorldSnapshot& snapshot) {
            return snapshot.header.server_tick == 8;
        }));
}

void server_accepts_matching_handshake_versions() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    network_example::KernelEngine server(config);
    auto transport = std::make_unique<network_example::LoopbackTransport>();
    require(transport->StartServer(7777));
    network_example::LoopbackTransport* loopback = transport.get();
    server.transport_ = std::move(transport);
    server.reset_runtime_state(KernelMode_DedicatedServer);

    network_example::HandshakePacket handshake;
    handshake.client_nonce = 1234;
    handshake.protocol_version = network_example::kProtocolVersion;
    handshake.snapshot_schema_version = network_example::kSnapshotSchemaVersion;
    handshake.packet_schema_version = network_example::kPacketSchemaVersion;
    const std::vector<std::uint8_t> payload =
        network_example::encode_handshake_packet(handshake);

    network_example::TransportEvent event;
    event.type = network_example::TransportEventType::kMessage;
    event.peer = 7;
    event.channel = network_example::ChannelId::kSession;
    event.payload = payload;
    server.handle_server_handshake(event);

    require(server.peer_sessions_.size() == 1);
    require(server.peer_sessions_[0].welcomed);

    network_example::TransportEvent response;
    require(loopback->PollClientEvent(response));
    network_example::WelcomePacket welcome;
    require(network_example::decode_welcome_packet(
        response.payload.data(),
        response.payload.size(),
        &welcome));
    require(welcome.assigned_peer_id == 7);
}

void server_rejects_mismatched_snapshot_schema_before_welcome() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    network_example::KernelEngine server(config);
    auto transport = std::make_unique<network_example::LoopbackTransport>();
    require(transport->StartServer(7777));
    network_example::LoopbackTransport* loopback = transport.get();
    server.transport_ = std::move(transport);
    server.reset_runtime_state(KernelMode_DedicatedServer);

    network_example::HandshakePacket handshake;
    handshake.client_nonce = 1234;
    handshake.protocol_version = network_example::kProtocolVersion;
    handshake.snapshot_schema_version = network_example::kSnapshotSchemaVersion + 1;
    handshake.packet_schema_version = network_example::kPacketSchemaVersion;
    const std::vector<std::uint8_t> payload =
        network_example::encode_handshake_packet(handshake);

    network_example::TransportEvent event;
    event.type = network_example::TransportEventType::kMessage;
    event.peer = 7;
    event.channel = network_example::ChannelId::kSession;
    event.payload = payload;
    server.handle_server_handshake(event);

    require(server.peer_sessions_.empty());

    network_example::TransportEvent response;
    require(loopback->PollClientEvent(response));
    network_example::DisconnectPacket disconnect;
    require(network_example::decode_disconnect_packet(
        response.payload.data(),
        response.payload.size(),
        &disconnect));
    require(
        disconnect.reason_code ==
        network_example::kDisconnectReasonSnapshotSchemaMismatch);
}

void server_validates_catalog_hash_before_welcome() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    network_example::KernelEngine server(config);
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 5;
    catalog.catalog_hash = 0xaabbccddeeff0011ull;
    require(server.load_gameplay_catalog(catalog));
    auto transport = std::make_unique<network_example::LoopbackTransport>();
    require(transport->StartServer(7777));
    network_example::LoopbackTransport* loopback = transport.get();
    server.transport_ = std::move(transport);
    server.reset_runtime_state(KernelMode_DedicatedServer);

    network_example::HandshakePacket matching;
    matching.client_nonce = 1234;
    matching.protocol_version = network_example::kProtocolVersion;
    matching.snapshot_schema_version = network_example::kSnapshotSchemaVersion;
    matching.packet_schema_version = network_example::kPacketSchemaVersion;
    matching.catalog_version = 5;
    matching.catalog_hash = 0xaabbccddeeff0011ull;
    network_example::TransportEvent event;
    event.type = network_example::TransportEventType::kMessage;
    event.peer = 7;
    event.channel = network_example::ChannelId::kSession;
    event.payload = network_example::encode_handshake_packet(matching);
    server.handle_server_handshake(event);

    require(server.peer_sessions_.size() == 1);
    network_example::TransportEvent response;
    require(loopback->PollClientEvent(response));
    network_example::WelcomePacket welcome;
    require(network_example::decode_welcome_packet(
        response.payload.data(),
        response.payload.size(),
        &welcome));
    require(welcome.catalog_version == 5);
    require(welcome.catalog_hash == 0xaabbccddeeff0011ull);

    network_example::HandshakePacket mismatched = matching;
    mismatched.catalog_hash ^= 1u;
    event.peer = 8;
    event.payload = network_example::encode_handshake_packet(mismatched);
    server.handle_server_handshake(event);

    require(server.peer_sessions_.size() == 1);
    network_example::DisconnectPacket disconnect;
    bool found_disconnect = false;
    for (int attempt = 0; attempt < 8 && loopback->PollClientEvent(response); ++attempt) {
        if (network_example::decode_disconnect_packet(
                response.payload.data(),
                response.payload.size(),
                &disconnect)) {
            found_disconnect = true;
            break;
        }
    }
    require(found_disconnect);
    require(disconnect.reason_code == network_example::kDisconnectReasonCatalogMismatch);
    KernelNetworkStats network_stats{};
    network_stats.struct_size = sizeof(network_stats);
    require(server.get_network_stats(&network_stats));
    require(network_stats.packet_serialization_cost_us > 0);
    require(network_stats.packet_deserialization_cost_us > 0);
}

void projectile_spawn_batch_renders_and_binds_to_snapshot() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    network_example::KernelEngine client(config);
    client.reset_runtime_state(KernelMode_Client);
    KernelProjectileTemplateDefinition projectile_template{};
    projectile_template.struct_size = sizeof(projectile_template);
    projectile_template.projectile_template_id = 3;
    projectile_template.weapon_id = 3;
    projectile_template.motion_model = KernelProjectileMotionModel_Linear;
    projectile_template.sync_mode = KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
    projectile_template.speed = 10.0f;
    projectile_template.lifetime_seconds = 2.0f;
    projectile_template.collider_template_id = 10;
    KernelColliderTemplateDefinition collider_template = projectile_collider_template();
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 9;
    catalog.catalog_hash = 0x9999ull;
    catalog.projectile_templates = &projectile_template;
    catalog.projectile_template_count = 1;
    catalog.collider_templates = &collider_template;
    catalog.collider_template_count = 1;
    require(client.load_gameplay_catalog(catalog));

    network_example::ProjectileSpawnBatchPacket batch{};
    batch.server_tick = 3;
    batch.server_time_us = 100000;
    batch.catalog_hash = 0x9999ull;
    network_example::ProjectileSpawnGroup group{};
    group.projectile_template_id = 3;
    group.records.push_back(network_example::ProjectileSpawnRecord{
        101,
        11,
        7,
        1234,
        glm::vec3{1.0f, 0.0f, 0.0f},
        glm::vec3{10.0f, 0.0f, 0.0f},
    });
    batch.groups.push_back(group);
    client.handle_client_projectile_spawn_batch(batch);

    std::array<RenderEntityState, 4> states{};
    std::uint32_t count =
        client.get_render_states_at_time(100000, states.data(), states.size());
    require(count == 1);
    require(states[0].net_id == 101);
    require(states[0].entity_type == static_cast<std::uint16_t>(
        network_example::EntityType::kProjectile));
    require(states[0].client_action_id == 1234);

    std::array<KernelDebugInfo, 2> debug_records{};
    for (KernelDebugInfo& debug_record : debug_records) {
        debug_record.struct_size = sizeof(KernelDebugInfo);
    }
    KernelDebugRecordFilter debug_filter{};
    debug_filter.struct_size = sizeof(debug_filter);
    debug_filter.record_type_mask = KernelDebugRecordType_Projectile;
    debug_filter.projectile_net_id = 404;
    debug_filter.weapon_id = KERNEL_DEBUG_WILDCARD_U8;
    debug_filter.motion_model = KERNEL_DEBUG_WILDCARD_U8;
    debug_filter.sync_mode = KERNEL_DEBUG_WILDCARD_U8;
    require(client.poll_debug_records(
                &debug_filter,
                debug_records.data(),
                static_cast<std::uint32_t>(debug_records.size())) == 0);
    debug_filter.projectile_net_id = 101;
    require(client.poll_debug_records(
                &debug_filter,
                debug_records.data(),
                static_cast<std::uint32_t>(debug_records.size())) == 1);
    require(debug_records[0].record_type == KernelDebugRecordType_Projectile);
    require(debug_records[0].data.projectile.projectile_net_id == 101);
    require(debug_records[0].data.projectile.weapon_id == 3);
    require(client.poll_debug_records(
                &debug_filter,
                debug_records.data(),
                static_cast<std::uint32_t>(debug_records.size())) == 0);
    KernelBenchmarkStats benchmark_stats{};
    benchmark_stats.struct_size = sizeof(benchmark_stats);
    require(client.get_benchmark_stats(&benchmark_stats));
    require(benchmark_stats.total_entity_count == 1);
    require(benchmark_stats.projectile_count == 1);
    require(benchmark_stats.hybrid_projectile_count == 1);
    require(benchmark_stats.hybrid_ratio == 1.0f);
    require(benchmark_stats.render_solver_cost_us > 0);

    client.handle_client_snapshot(projectile_snapshot(
        4,
        101,
        7,
        1234,
        glm::vec3{2.0f, 0.0f, 0.0f},
        glm::vec3{10.0f, 0.0f, 0.0f}));
    require(client.predicted_projectiles_.size() == 1);
    require(client.predicted_projectiles_[0].bound);
    count = client.get_render_states_at_time(133333, states.data(), states.size());
    require(count == 1);
    require(states[0].net_id == 101);
    require(client.get_benchmark_stats(&benchmark_stats));
    require(benchmark_stats.hybrid_correction_cost_us > 0);
}

void client_despawn_removes_predicted_projectile() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    network_example::KernelEngine client(config);
    client.reset_runtime_state(KernelMode_Client);

    network_example::KernelEngine::PredictedProjectile projectile;
    projectile.entity_id = 9000;
    projectile.net_id = 101;
    projectile.owner_peer = 7;
    projectile.bound = true;
    client.predicted_projectiles_.push_back(projectile);

    client.handle_client_despawn(network_example::EntityDespawnPacket{
        101,
        12,
        KernelDespawnReason_Destroyed,
    });

    require(client.predicted_projectiles_.empty());
}

void predicted_projectile_lifetime_cleanup_removes_batch_projectile() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    network_example::KernelEngine client(config);
    client.reset_runtime_state(KernelMode_Client);
    KernelProjectileTemplateDefinition projectile_template{};
    projectile_template.struct_size = sizeof(projectile_template);
    projectile_template.projectile_template_id = 3;
    projectile_template.weapon_id = 3;
    projectile_template.motion_model = KernelProjectileMotionModel_Linear;
    projectile_template.sync_mode = KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
    projectile_template.speed = 10.0f;
    projectile_template.lifetime_seconds = 0.5f;
    projectile_template.collider_template_id = 10;
    KernelColliderTemplateDefinition collider_template = projectile_collider_template();
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 9;
    catalog.catalog_hash = 0x9999ull;
    catalog.projectile_templates = &projectile_template;
    catalog.projectile_template_count = 1;
    catalog.collider_templates = &collider_template;
    catalog.collider_template_count = 1;
    require(client.load_gameplay_catalog(catalog));

    network_example::ProjectileSpawnBatchPacket batch{};
    batch.server_tick = 3;
    batch.server_time_us = 100000;
    batch.catalog_hash = 0x9999ull;
    network_example::ProjectileSpawnGroup group{};
    group.projectile_template_id = 3;
    group.records.push_back(network_example::ProjectileSpawnRecord{
        101,
        11,
        7,
        1234,
        glm::vec3{1.0f, 0.0f, 0.0f},
        glm::vec3{10.0f, 0.0f, 0.0f},
    });
    batch.groups.push_back(group);
    client.handle_client_projectile_spawn_batch(batch);
    require(client.predicted_projectiles_.size() == 1);

    client.advance_predicted_projectiles(0.6f);

    require(client.predicted_projectiles_.empty());
}

void default_kernel_config_uses_larger_render_state_cap() {
    KernelConfig config{};
    config.mode = KernelMode_Client;

    network_example::KernelEngine client(config);

    require(client.config_.max_render_states == 2048);
}

void render_state_overflow_reports_error_event() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    config.max_render_states = 1;

    network_example::KernelEngine client(config);
    client.reset_runtime_state(KernelMode_Client);
    network_example::WorldSnapshot snapshot;
    snapshot.header.server_tick = 10;
    add_snapshot_entity(&snapshot, 1, network_example::EntityType::kPlayer, 0.0f);
    add_snapshot_entity(&snapshot, 2, network_example::EntityType::kEnemy, 1.0f);
    client.handle_client_snapshot(snapshot);

    std::array<RenderEntityState, 1> states{};
    require(client.get_render_states_at_time(
                333333,
                states.data(),
                static_cast<std::uint32_t>(states.size())) == 1);

    std::array<KernelEvent, 4> events{};
    const std::uint32_t event_count =
        client.poll_events(events.data(), static_cast<std::uint32_t>(events.size()));
    bool saw_overflow_error = false;
    for (std::uint32_t index = 0; index < event_count; ++index) {
        saw_overflow_error =
            saw_overflow_error ||
            (events[index].type == KernelEventType_Error && events[index].code == 25);
    }
    require(saw_overflow_error);
}

void hit_debug_records_filter_and_drain() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    std::vector<network_example::ConfirmedDamage> ready_damage{
        network_example::ConfirmedDamage{
            12,
            77,
            100,
            200,
            7,
            3,
            25,
            123456,
            glm::vec3{1.0f, 2.0f, 3.0f},
        },
    };
    engine.queue_hit_debug_records(ready_damage);

    KernelDebugRecordFilter filter{};
    filter.struct_size = sizeof(filter);
    filter.record_type_mask = KernelDebugRecordType_Hit;
    filter.weapon_id = KERNEL_DEBUG_WILDCARD_U8;
    filter.source_net_id = 999;
    std::array<KernelDebugInfo, 2> debug_records{};
    require(engine.poll_debug_records(
                &filter,
                debug_records.data(),
                static_cast<std::uint32_t>(debug_records.size())) == 0);

    filter.source_net_id = 100;
    require(engine.poll_debug_records(
                &filter,
                debug_records.data(),
                static_cast<std::uint32_t>(debug_records.size())) == 1);
    require(debug_records[0].record_type == KernelDebugRecordType_Hit);
    require(debug_records[0].data.hit.source_net_id == 100);
    require(debug_records[0].data.hit.target_net_id == 200);
    require(debug_records[0].data.hit.weapon_id == 3);
    require(debug_records[0].data.hit.position.y == 2.0f);
    require(engine.poll_debug_records(
                &filter,
                debug_records.data(),
                static_cast<std::uint32_t>(debug_records.size())) == 0);
}

}  // namespace

int main() {
    presentation_gate_releases_at_render_time();
    clock_sync_ping_pong_updates_peer_offset();
    compensation_clamps_not_rejects_client_local_time();
    client_replies_to_clock_sync_ping();
    client_applies_server_tick_config_from_welcome();
    client_clock_offset_smooths_after_initial_sync();
    projectile_spawn_packet_uses_original_muzzle_position();
    snapshot_only_projectile_spawn_skips_event_batch();
    render_states_at_time_interpolates_and_clamps();
    remote_projectile_uses_interpolated_past_timeline();
    local_projectile_snapshot_fast_forwards_and_smooths();
    homing_projectile_snapshot_extrapolation_is_bounded();
    render_query_does_not_consume_local_correction();
    late_snapshot_is_stored_but_not_used_for_reconciliation();
    server_accepts_matching_handshake_versions();
    server_rejects_mismatched_snapshot_schema_before_welcome();
    server_validates_catalog_hash_before_welcome();
    projectile_spawn_batch_renders_and_binds_to_snapshot();
    client_despawn_removes_predicted_projectile();
    predicted_projectile_lifetime_cleanup_removes_batch_projectile();
    default_kernel_config_uses_larger_render_state_cap();
    render_state_overflow_reports_error_event();
    hit_debug_records_filter_and_drain();

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
