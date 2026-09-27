// W4: a remote presentation record that goes stale is dropped in one of two
// places on the client -- on arrival, or while it waits for render time -- and
// KernelNetworkStats only counts the total. The client also logs, once per
// five-second window that dropped anything, which place, which event types,
// how late, and how unevenly the host drained. This drives both drop points
// and a release through one window and reads the line back.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "kernel/public/kernel_api.h"
#include "protocol/public/network_packets.h"
#include "transport/public/itransport.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(
            stderr,
            "remote_presentation_stale_diagnostics_test:%d: requirement failed: %s\n",
            line,
            expression);
        std::exit(1);
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

using Engine = network_example::KernelEngine;

// Render time at a whole tick at 30 Hz, a hair past it.
std::uint64_t render_at(std::uint32_t tick) {
    return static_cast<std::uint64_t>(tick) * 33334u;
}

KernelRemoteActionPresentationEvent event_of(std::uint8_t type) {
    KernelRemoteActionPresentationEvent event{};
    event.actor_net_id = 21u;
    event.action_instance_id = 5u;
    event.first_commit_index = 1u;
    event.commit_count = 1u;
    event.event_type = type;
    return event;
}

std::vector<std::string> drain_stale_lines() {
    std::vector<std::string> lines;
    KernelLogMessage messages[16]{};
    std::uint32_t count = 0u;
    while ((count = Kernel_PollLogMessages(messages, 16u)) > 0u) {
        for (std::uint32_t index = 0u; index < count; ++index) {
            if (std::strstr(messages[index].text, "remote presentation stale") != nullptr) {
                lines.emplace_back(messages[index].text);
            }
        }
    }
    return lines;
}

void release_at(Engine& client, std::uint64_t client_us, std::uint32_t render_tick) {
    client.client_local_time_us_ = client_us;
    client.current_render_time_us_ = render_at(render_tick);
    client.release_remote_action_presentation_events();
}

void one_window_names_both_drop_points() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30u;
    config.tick.snapshot_rate = 15u;
    Engine client(config);
    client.reset_runtime_state(KernelMode_Client);
    client.has_client_render_time_ = true;
    client.client_clock_offset_us_ = 0;

    // A hit that will wait past its expiry, and a fire that will not.
    client.pending_remote_action_presentation_events_.push_back(
        Engine::PendingRemotePresentation{
            100u, 108u, event_of(KernelRemoteActionPresentationEventType_HitReaction), 1000000u});
    client.pending_remote_action_presentation_events_.push_back(
        Engine::PendingRemotePresentation{
            120u, 128u, event_of(KernelRemoteActionPresentationEventType_FireCommit), 1000000u});
    release_at(client, 1000000u, 90u);  // opens the window; neither is due
    require(client.pending_remote_action_presentation_events_.size() == 2u);

    release_at(client, 1400000u, 112u);  // the hit is 4 ticks past expiry
    require(client.network_stats_.remote_presentation_stale_dropped == 1u);
    release_at(client, 1450000u, 121u);  // the fire goes out with 7 ticks left
    require(client.remote_action_presentation_events_.size() == 1u);
    require(client.pending_remote_action_presentation_events_.empty());

    // A batch from tick 150 arriving when the newest snapshot is at 200.
    client.client_local_time_us_ = 1500000u;
    client.has_client_snapshot_ = true;
    client.latest_client_snapshot_.header.server_tick = 200u;
    network_example::RemoteActionPresentationBatchPacket batch{};
    batch.server_tick = 150u;
    batch.records.push_back(event_of(KernelRemoteActionPresentationEventType_HitReaction));
    network_example::TransportEvent transport_event{};
    transport_event.channel = network_example::ChannelId::kPresentation;
    transport_event.payload =
        network_example::encode_remote_action_presentation_batch_packet(batch, 1u);
    client.handle_client_remote_action_presentation(transport_event);
    require(client.network_stats_.remote_presentation_stale_dropped == 2u);

    // Nothing is written until the window is five seconds old.
    require(drain_stale_lines().empty());
    release_at(client, 6100000u, 121u);
    const std::vector<std::string> lines = drain_stale_lines();
    require(lines.size() == 1u);
    std::fprintf(stderr, "%s\n", lines[0].c_str());
    const std::string& line = lines[0];
    require(line.find("(expiry 8 ticks)") != std::string::npos);
    require(line.find("arrival 1 [hit=1] late<=42t batch_age<=50t") != std::string::npos);
    require(line.find("pending 1 [hit=1] late<=4t waited<=400ms") != std::string::npos);
    require(line.find("released 1 min_margin=7t") != std::string::npos);
    require(line.find("release gap<=4600ms render_step<=733ms") != std::string::npos);

    // A window with no drops stays quiet.
    release_at(client, 11200000u, 130u);
    require(drain_stale_lines().empty());
}

void stats_off_logs_nothing() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30u;
    config.network_stats.mode = KernelNetworkStatsMode_Off;
    Engine client(config);
    client.reset_runtime_state(KernelMode_Client);
    client.has_client_render_time_ = true;
    client.pending_remote_action_presentation_events_.push_back(
        Engine::PendingRemotePresentation{
            100u, 108u, event_of(KernelRemoteActionPresentationEventType_HitReaction), 0u});
    release_at(client, 1000000u, 90u);
    release_at(client, 1400000u, 112u);
    require(client.pending_remote_action_presentation_events_.empty());
    release_at(client, 7000000u, 112u);
    require(drain_stale_lines().empty());
}

}  // namespace

int main() {
    require(Kernel_PollLogMessages(nullptr, 0u) == 0u);  // start capture
    one_window_names_both_drop_points();
    stats_off_logs_nothing();
    std::printf("remote_presentation_stale_diagnostics_test passed\n");
    return 0;
}
