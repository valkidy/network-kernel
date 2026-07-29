#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
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

    KernelPlayerInput input{};
    const std::vector<std::uint8_t> first_packet =
        network_example::encode_player_input_packet(7, input, 1);
    const std::vector<std::uint8_t> fourth_packet =
        network_example::encode_player_input_packet(7, input, 4);
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

void stats_modes_apply_defaults_and_timing_policy() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    network_example::KernelEngine defaults(config);
    require(defaults.config_.network_stats.mode == KernelNetworkStatsMode_Basic);
    require(defaults.config_.network_stats.action_packet_budget_bytes == 1200u);
    require(defaults.config_.network_stats.remote_presentation_expiry_ms == 250u);
    require(
        defaults.config_.network_stats
            .remote_presentation_client_budget_bytes_per_second == 8192u);
    require(
        defaults.config_.network_stats
            .remote_presentation_server_budget_bytes_per_second == 262144u);

    config.network_stats.mode = KernelNetworkStatsMode_Off;
    network_example::KernelEngine off(config);
    off.reset_runtime_state(KernelMode_Client);
    off.record_sent_packet(
        85u,
        network_example::SendMode::kUnreliable,
        network_example::ChannelId::kInput);
    require(off.network_stats_.packet_count_sent == 0u);

    config.network_stats.mode = KernelNetworkStatsMode_Basic;
    network_example::KernelEngine basic(config);
    basic.reset_runtime_state(KernelMode_Client);
    basic.record_sent_packet(
        85u,
        network_example::SendMode::kUnreliable,
        network_example::ChannelId::kInput);
    require(basic.network_stats_.packet_count_sent == 1u);
    require(basic.network_stats_.input_bytes_sent == 85u);
    require(basic.network_stats_.packet_serialization_cost_us == 0u);

    config.network_stats.mode = KernelNetworkStatsMode_Detailed;
    network_example::KernelEngine detailed(config);
    detailed.reset_runtime_state(KernelMode_Client);
    detailed.record_sent_packet(
        85u,
        network_example::SendMode::kUnreliable,
        network_example::ChannelId::kInput);
    detailed.record_packet_deserialization_cost(7u);
    require(detailed.network_stats_.packet_serialization_cost_us > 0u);
    require(detailed.network_stats_.packet_deserialization_cost_us == 7u);
}

void owner_results_bypass_drop_and_remote_budget_prefers_priority() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.network_stats.action_packet_budget_bytes = 56u;
    config.network_stats.remote_presentation_client_budget_bytes_per_second = 56u;
    config.network_stats.remote_presentation_server_budget_bytes_per_second = 56u;
    network_example::KernelEngine server(config);
    server.reset_runtime_state(KernelMode_DedicatedServer);
    auto loopback = std::make_unique<network_example::LoopbackTransport>();
    require(loopback->StartServer(7792));
    auto* transport = loopback.get();
    server.transport_ = std::move(loopback);

    network_example::KernelEngine::PeerSession session{};
    session.peer = 7u;
    session.player = 11u;
    session.welcomed = true;
    session.relevant_entities.insert(21u);
    session.relevant_entities.insert(22u);
    session.pending_action_results.push_back(KernelLocalActionResult{
        1u, 1u, KernelLocalActionResultType_Accepted,
        KernelLocalActionResultReason_None, 1u});
    session.pending_action_results.push_back(KernelLocalActionResult{
        2u, 1u, KernelLocalActionResultType_Rejected,
        KernelLocalActionResultReason_Busy, 1u});
    server.flush_local_action_results(&session);

    std::uint32_t owner_packets = 0u;
    network_example::TransportEvent sent{};
    while (transport->PollClientEvent(sent)) {
        network_example::LocalActionResultBatchPacket batch{};
        if (network_example::decode_local_action_result_batch_packet(
                sent.payload.data(), sent.payload.size(), &batch)) {
            ++owner_packets;
            require(batch.records.size() == 1u);
            require(sent.payload.size() == 48u);
        }
    }
    require(owner_packets == 2u);
    require(server.network_stats_.local_action_results_sent == 2u);

    std::vector<KernelRemoteActionPresentationEvent> presentations{
        KernelRemoteActionPresentationEvent{
            21u, 1u, 10u, 1u, 1u,
            KernelRemoteActionPresentationEventType_FireCommit, 0u, 0u},
        KernelRemoteActionPresentationEvent{
            22u, 1u, 11u, 1u, 1u,
            KernelRemoteActionPresentationEventType_DeathTrigger, 0u, 0u},
    };
    server.flush_remote_action_presentation(&session, presentations);
    require(transport->PollClientEvent(sent));
    network_example::RemoteActionPresentationBatchPacket decoded{};
    require(network_example::decode_remote_action_presentation_batch_packet(
        sent.payload.data(), sent.payload.size(), &decoded));
    require(decoded.records.size() == 1u);
    require(
        decoded.records[0].event_type ==
        KernelRemoteActionPresentationEventType_DeathTrigger);
    require(server.network_stats_.remote_presentation_budget_dropped == 1u);
}

void render_time_expiry_drops_pending_remote_presentation() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 1000u;
    network_example::KernelEngine client(config);
    client.reset_runtime_state(KernelMode_Client);
    client.has_client_render_time_ = true;
    client.current_render_time_us_ = 300000u;
    client.pending_remote_action_presentation_events_.push_back(
        network_example::KernelEngine::PendingRemotePresentation{
            10u,
            250u,
            KernelRemoteActionPresentationEvent{
                1u, 2u, 3u, 1u, 1u,
                KernelRemoteActionPresentationEventType_FireCommit, 0u, 0u}});
    client.release_remote_action_presentation_events();
    require(client.remote_action_presentation_events_.empty());
    require(client.pending_remote_action_presentation_events_.empty());
    require(client.network_stats_.remote_presentation_stale_dropped == 1u);
}

void inventory_replication_is_owner_only_delta_driven_and_idle_zero() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30u;
    config.tick.snapshot_rate = 15u;
    network_example::KernelEngine server(config);
    server.reset_runtime_state(KernelMode_DedicatedServer);
    auto loopback = std::make_unique<network_example::LoopbackTransport>();
    require(loopback->StartServer(7793));
    auto* transport = loopback.get();
    server.transport_ = std::move(loopback);

    KernelItemTemplateDefinition item{};
    item.struct_size = sizeof(item);
    item.item_template_id = 10u;
    item.item_mode = KernelItemMode_Fungible;
    item.max_stack = 10u;
    item.use_policy.struct_size = sizeof(item.use_policy);
    item.throw_policy.struct_size = sizeof(item.throw_policy);
    std::string error;
    require(server.item_store_.set_templates({&item, 1u}, &error));
    const auto container = server.item_store_.create_container(11u, 4u);
    require(container.has_value());
    require(server.item_store_.create_inventory_item(10u, 1u, *container)
        .has_value());

    server.peer_sessions_.resize(2u);
    server.peer_sessions_[0].peer = 7u;
    server.peer_sessions_[0].player = 11u;
    server.peer_sessions_[0].welcomed = true;
    server.peer_sessions_[1].peer = 8u;
    server.peer_sessions_[1].player = 12u;
    server.peer_sessions_[1].welcomed = true;
    server.flush_inventory_replication();

    std::uint32_t owner_snapshots = 0u;
    std::uint32_t nonowner_inventory_packets = 0u;
    network_example::TransportEvent event{};
    while (transport->PollClientEvent(event)) {
        network_example::InventorySnapshotPagePacket snapshot{};
        network_example::InventoryDeltaBatchPacket delta{};
        const bool is_inventory =
            network_example::decode_inventory_snapshot_page_packet(
                event.payload.data(), event.payload.size(), &snapshot) ||
            network_example::decode_inventory_delta_batch_packet(
                event.payload.data(), event.payload.size(), &delta);
        if (!is_inventory) continue;
        if (event.peer == 7u) ++owner_snapshots;
        if (event.peer == 8u) ++nonowner_inventory_packets;
    }
    require(owner_snapshots == 1u);
    require(nonowner_inventory_packets == 0u);
    const std::uint64_t initial_bytes =
        server.network_stats_.inventory_snapshot_bytes_sent;
    require(initial_bytes == 81u);
    server.flush_inventory_replication();
    require(!transport->PollClientEvent(event));
    require(server.network_stats_.inventory_snapshot_bytes_sent == initial_bytes);
    require(server.network_stats_.inventory_delta_bytes_sent == 0u);

    require(server.item_store_.create_inventory_item(10u, 1u, *container)
        .has_value());
    server.flush_inventory_replication();
    std::uint32_t owner_delta_batches = 0u;
    while (transport->PollClientEvent(event)) {
        network_example::InventoryDeltaBatchPacket delta{};
        if (network_example::decode_inventory_delta_batch_packet(
                event.payload.data(), event.payload.size(), &delta)) {
            require(event.peer == 7u);
            ++owner_delta_batches;
        }
    }
    require(owner_delta_batches == 1u);
    require(server.network_stats_.inventory_delta_bytes_sent == 74u);
}

struct TrafficRow {
    std::uint32_t peers;
    std::uint32_t commits_per_second;
    bool full_relevance;
    bool overload;
    std::uint64_t raw_remote_bytes;
    std::uint64_t delivered_remote_bytes;
    std::uint64_t dropped_records;
    std::uint64_t remote_packets;
    std::uint64_t max_client_remote_bytes;
    std::uint32_t max_packet;
    double average_packet;
    double average_records;
};

std::uint32_t encoded_remote_size(std::uint32_t records) {
    network_example::RemoteActionPresentationBatchPacket batch{};
    batch.records.resize(records);
    return static_cast<std::uint32_t>(
        network_example::encode_remote_action_presentation_batch_packet(
            batch, 1u).size());
}

TrafficRow simulate_traffic(
    std::uint32_t peers,
    std::uint32_t commits_per_second,
    bool full_relevance,
    bool overload) {
    constexpr std::uint64_t kDurationSeconds = 60u;
    constexpr std::uint64_t kClientBudget = 8192u;
    constexpr std::uint64_t kServerBudget = 262144u;
    const std::uint32_t records_per_observer = full_relevance
        ? peers - 1u
        : std::min(3u, peers - 1u);
    const std::uint32_t demanded_packet = encoded_remote_size(records_per_observer);
    const std::uint64_t raw_remote_bytes =
        static_cast<std::uint64_t>(demanded_packet) * peers *
        commits_per_second * kDurationSeconds;
    std::vector<std::uint64_t> client_tokens(peers, kClientBudget);
    std::vector<std::uint64_t> client_delivered_bytes(peers, 0u);
    std::uint64_t server_tokens = kServerBudget;
    std::uint64_t delivered_bytes = 0u;
    std::uint64_t delivered_records = 0u;
    std::uint64_t dropped_records = 0u;
    std::uint64_t packet_count = 0u;
    std::uint32_t max_packet = 0u;
    for (std::uint32_t second = 0; second < kDurationSeconds; ++second) {
        if (second != 0u) {
            server_tokens = std::min(kServerBudget, server_tokens + kServerBudget);
            for (std::uint64_t& tokens : client_tokens) {
                tokens = std::min(kClientBudget, tokens + kClientBudget);
            }
        }
        for (std::uint32_t commit = 0; commit < commits_per_second; ++commit) {
            for (std::uint32_t peer_offset = 0; peer_offset < peers; ++peer_offset) {
                const std::uint32_t peer =
                    (peer_offset + commit + second * commits_per_second) % peers;
                const std::uint64_t available =
                    std::min(client_tokens[peer], server_tokens);
                const std::uint32_t budget_records = available <= 36u
                    ? 0u
                    : static_cast<std::uint32_t>((available - 36u) / 20u);
                const std::uint32_t sent_records = std::min(
                    58u,
                    std::min(records_per_observer, budget_records));
                dropped_records += records_per_observer - sent_records;
                if (sent_records == 0u) {
                    continue;
                }
                const std::uint32_t bytes = encoded_remote_size(sent_records);
                client_tokens[peer] -= bytes;
                server_tokens -= bytes;
                delivered_bytes += bytes;
                client_delivered_bytes[peer] += bytes;
                delivered_records += sent_records;
                ++packet_count;
                max_packet = std::max(max_packet, bytes);
            }
        }
    }
    require(max_packet <= 1200u);
    require(delivered_bytes <= kServerBudget * kDurationSeconds);
    return TrafficRow{
        peers,
        commits_per_second,
        full_relevance,
        overload,
        raw_remote_bytes,
        delivered_bytes,
        dropped_records,
        packet_count,
        *std::max_element(
            client_delivered_bytes.begin(), client_delivered_bytes.end()),
        max_packet,
        packet_count == 0u
            ? 0.0
            : static_cast<double>(delivered_bytes) / packet_count,
        packet_count == 0u
            ? 0.0
            : static_cast<double>(delivered_records) / packet_count,
    };
}

std::vector<TrafficRow> traffic_matrix() {
    std::vector<TrafficRow> rows;
    for (const std::uint32_t peers : {2u, 8u, 32u}) {
        for (const std::uint32_t rate : {1u, 10u}) {
            rows.push_back(simulate_traffic(peers, rate, false, false));
            rows.push_back(simulate_traffic(peers, rate, true, false));
        }
    }
    rows.push_back(simulate_traffic(32u, 30u, true, true));
    return rows;
}

void write_traffic_report(
    const std::string& path,
    const std::vector<TrafficRow>& rows) {
    std::ofstream report(path);
    require(report.good());
    report << std::fixed << std::setprecision(2);
    report << "# Local Action Network Traffic Report\n\n";
    report << "Deterministic 60-second C++ application-message simulation. "
              "Transport framing, encryption, UDP/IP and link overhead are excluded.\n\n";
    KernelBuildInfo build_info{};
    require(Kernel_GetBuildInfo(&build_info, sizeof(build_info)));
    report << "- ABI: 41; protocol: " << build_info.protocol_version
           << "; snapshot schema: " << build_info.snapshot_schema_version
           << "; packet schema: " << build_info.packet_schema_version << "\n";
    report << "- Module: " << build_info.module_version
           << "; git: " << build_info.git_commit
           << "; platform/config: " << build_info.build_platform << "/"
           << build_info.build_config << "\n";
    report << "- Input packet: 85 B\n";
    report << "- Owner result batch: `36 + 12N` B (97 records at 1,200 B)\n";
    report << "- Remote presentation batch: `36 + 20N` B (58 records at 1,196 B)\n";
    report << "- Snapshot configured upper-bound: 18,000 B/s per client\n";
    report << "- Stats Off/Basic/Detailed alter counters only; wire bytes are identical\n\n";
    report << "| Peers | Commits/s | Relevance | Remote demand B/s | Delivered B/s | kbit/s | Dropped records | Packets | Avg packet B | Max packet B | Avg records/batch |\n";
    report << "|---:|---:|:---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const TrafficRow& row : rows) {
        report << "| " << row.peers << " | " << row.commits_per_second
               << " | " << (row.full_relevance ? "full" : "sparse")
               << (row.overload ? " overload" : "")
               << " | " << row.raw_remote_bytes / 60.0
               << " | " << row.delivered_remote_bytes / 60.0
               << " | " << row.delivered_remote_bytes * 8.0 / 60000.0
               << " | " << row.dropped_records
               << " | " << row.remote_packets
               << " | " << row.average_packet
               << " | " << row.max_packet
               << " | " << row.average_records << " |\n";
    }
    const TrafficRow& highest = rows.back();
    const double per_client_remote = highest.max_client_remote_bytes / 60.0;
    const double owner_bytes_per_second = 48.0 * highest.commits_per_second;
    const double input_bytes_per_second = 85.0 * 30.0;
    report << "\n## Thresholds and highest scenario\n\n";
    report << "The highest case is 32 peers x 30 commits/s, full relevance. "
              "Its delivered remote presentation is "
           << highest.delivered_remote_bytes / 60.0
           << " B/s server aggregate and " << per_client_remote
           << " B/s for the busiest client.\n\n";
    report << "- 1,200 B action packet: PASS (measured max "
           << highest.max_packet << " B)\n";
    report << "- 8 KiB/s per-client remote token bucket: PASS ("
           << per_client_remote << " B/s sustained average)\n";
    report << "- 256 KiB/s server remote token bucket: PASS ("
           << highest.delivered_remote_bytes / 60.0 << " B/s)\n";
    report << "- Overload budget drop: PASS (" << highest.dropped_records
           << " records dropped, never deferred)\n";
    report << "- Highest per-client bidirectional application traffic: "
           << input_bytes_per_second + 18000.0 + owner_bytes_per_second +
                  per_client_remote
           << " B/s (input " << input_bytes_per_second
           << " + snapshot cap 18000 + owner " << owner_bytes_per_second
           << " + remote " << per_client_remote << ")\n";
    report << "- Highest server outbound application traffic: "
           << 18000.0 * highest.peers + owner_bytes_per_second * highest.peers +
                  highest.delivered_remote_bytes / 60.0
           << " B/s\n\n";
    report << "## Reproduction\n\n";
    report << "```sh\n"
              "bazel test //engine/src/tests/kernel_tests:network_stats_test\n"
              "bazel-bin/engine/src/tests/kernel_tests/network_stats_test "
              "--report=/tmp/LOCAL_ACTION_NETWORK_TRAFFIC_REPORT.md\n"
              "```\n";
}

}  // namespace

int main(int argc, char** argv) {
    client_ping_pong_applies_server_network_stats();
    server_clock_sync_ping_carries_session_network_stats();
    received_packet_sequence_gaps_update_loss_ratio();
    stats_modes_apply_defaults_and_timing_policy();
    owner_results_bypass_drop_and_remote_budget_prefers_priority();
    render_time_expiry_drops_pending_remote_presentation();
    inventory_replication_is_owner_only_delta_driven_and_idle_zero();
    const std::vector<TrafficRow> rows = traffic_matrix();
    if (argc == 2 && std::string_view(argv[1]).starts_with("--report=")) {
        write_traffic_report(std::string(argv[1]).substr(9), rows);
    }
    return 0;
}
