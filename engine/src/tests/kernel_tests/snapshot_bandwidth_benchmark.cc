// How often does one client actually hear about one agent?
//
// The per-client snapshot send budget is a hard 1200 B and the agent section
// serves whoever has waited longest, so past a certain population every agent's
// position is stale for part of the time. This measures that directly -- it
// drives the real build_relevant_snapshot / build_snapshot_send_set pair over a
// synthetic population and records, per agent, how many snapshots pass between
// appearances. It derives nothing from the budget arithmetic.
//
// Run:
//   bazel run -c opt //engine/src/tests/kernel_tests:snapshot_bandwidth_benchmark
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

#include <optional>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#define private public
#include "kernel/src/kernel.h"
#undef private

#include "protocol/public/network_packets.h"

namespace {

using network_example::EntitySnapshot;
using network_example::KernelEngine;
using network_example::NetId;
using network_example::WorldSnapshot;

// The live value: publish_snapshot passes kLargeSyncPacketWarningBytes, which
// is 1200 and is not configurable from KernelConfig.
constexpr std::size_t kSendBudgetBytes = 1200;
constexpr std::uint32_t kSnapshotsPerSecond = 15;
// Relevance keeps everything within 40 m, so the population is packed inside
// that sphere -- this measures the send-set selection, not the range filter.
constexpr float kRelevanceRadiusMeters = 40.0f;
// The bands build_snapshot_send_set weights by; kept in step with
// kSnapshotPriorityNearMeters / kSnapshotPriorityMidMeters in kernel.cc.
// Reporting one number across all of them would read the weighting as a
// regression: the far band is meant to get worse, and pays for the near one.
constexpr float kNearBandMeters = 10.0f;
constexpr float kMidBandMeters = 25.0f;

struct Row {
    std::size_t agent_count = 0;
    std::size_t relevant_agents = 0;
    double agents_per_snapshot = 0.0;
    double mean_snapshot_bytes = 0.0;
    std::size_t agent_entity_bytes = 0;
    std::size_t max_gap_snapshots = 0;
    double blackout_seconds = 0.0;
    double near_blackout_seconds = 0.0;
    double mid_blackout_seconds = 0.0;
    double far_blackout_seconds = 0.0;
    double bytes_per_second = 0.0;
};

// Spreads `count` agents through the relevance sphere deterministically.
glm::vec3 agent_position(std::size_t index, std::size_t count) {
    const double golden = 2.399963229728653;
    const double t = static_cast<double>(index) /
        static_cast<double>(count == 0 ? 1 : count);
    const double radius = kRelevanceRadiusMeters * 0.9 * std::sqrt(t);
    const double angle = static_cast<double>(index) * golden;
    return glm::vec3{
        static_cast<float>(radius * std::cos(angle)),
        0.0f,
        static_cast<float>(radius * std::sin(angle))};
}

Row measure(
    std::size_t agent_count,
    std::size_t snapshot_count,
    bool agents_acting = false) {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = kSnapshotsPerSecond;
    KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    network_example::World& world = engine.simulation_world();
    const NetId player = world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    std::vector<NetId> agents;
    agents.reserve(agent_count);
    for (std::size_t index = 0; index < agent_count; ++index) {
        agents.push_back(world.spawn_enemy(agent_position(index, agent_count)));
        if (!agents_acting) {
            continue;
        }
        // An agent mid-action carries the 20 B action timeline block. This is
        // the worst realistic case for a firing crowd.
        const std::optional<entt::entity> entity =
            world.find_entity(agents.back());
        network_example::ActionRuntimeState& action =
            world.registry().emplace<network_example::ActionRuntimeState>(*entity);
        action.action_template_id = 100u;
    }

    KernelEngine::PeerSession session{1, player, 0, true, {}};

    std::vector<std::size_t> last_seen(agent_count, 0);
    std::vector<bool> seen_once(agent_count, false);
    std::vector<std::size_t> gaps;
    // Indexed by band: 0 near, 1 mid, 2 far.
    std::array<std::vector<std::size_t>, 3> band_gaps;
    std::vector<std::size_t> agent_band(agent_count, 2);
    for (std::size_t index = 0; index < agent_count; ++index) {
        const glm::vec3 position = agent_position(index, agent_count);
        const float distance = glm::length(position);
        agent_band[index] = distance <= kNearBandMeters
            ? 0u
            : (distance <= kMidBandMeters ? 1u : 2u);
    }
    std::vector<std::size_t> packed_per_snapshot;
    std::vector<std::size_t> bytes_per_snapshot;
    std::size_t relevant_agents = 0;
    std::size_t agent_entity_bytes = 0;

    for (std::size_t tick = 0; tick < snapshot_count; ++tick) {
        const WorldSnapshot relevant = engine.build_relevant_snapshot(
            session, static_cast<std::uint32_t>(tick * 1000u / kSnapshotsPerSecond));
        if (tick == 0) {
            for (const EntitySnapshot& entity : relevant.entities) {
                if (entity.type == network_example::EntityType::kActor &&
                    entity.actor_type == network_example::ActorType::kAgent) {
                    ++relevant_agents;
                }
            }
        }
        const WorldSnapshot send =
            engine.build_snapshot_send_set(session, relevant, kSendBudgetBytes);
        bytes_per_snapshot.push_back(
            network_example::estimate_snapshot_packet_size(send));

        std::size_t packed = 0;
        for (const EntitySnapshot& entity : send.entities) {
            if (entity.type != network_example::EntityType::kActor ||
                entity.actor_type != network_example::ActorType::kAgent) {
                continue;
            }
            ++packed;
            const auto found =
                std::find(agents.begin(), agents.end(), entity.net_id);
            if (found == agents.end()) {
                continue;
            }
            const std::size_t slot =
                static_cast<std::size_t>(found - agents.begin());
            if (seen_once[slot]) {
                gaps.push_back(tick - last_seen[slot]);
                band_gaps[agent_band[slot]].push_back(tick - last_seen[slot]);
            }
            seen_once[slot] = true;
            last_seen[slot] = tick;
        }
        packed_per_snapshot.push_back(packed);
        if (tick == 0 && !send.entities.empty()) {
            for (const EntitySnapshot& entity : send.entities) {
                if (entity.type == network_example::EntityType::kActor &&
                    entity.actor_type == network_example::ActorType::kAgent) {
                    agent_entity_bytes =
                        network_example::estimate_snapshot_entity_size(entity);
                    break;
                }
            }
        }
    }

    const auto mean = [](const std::vector<std::size_t>& values) {
        if (values.empty()) {
            return 0.0;
        }
        return static_cast<double>(
                   std::accumulate(values.begin(), values.end(), std::size_t{0})) /
            static_cast<double>(values.size());
    };

    std::sort(gaps.begin(), gaps.end());
    const auto band_blackout = [&](std::size_t band) {
        if (band_gaps[band].empty()) {
            return 0.0;
        }
        const std::size_t worst =
            *std::max_element(band_gaps[band].begin(), band_gaps[band].end());
        return static_cast<double>(worst) /
            static_cast<double>(kSnapshotsPerSecond);
    };
    Row row;
    row.agent_count = agent_count;
    row.relevant_agents = relevant_agents;
    row.agents_per_snapshot = mean(packed_per_snapshot);
    row.mean_snapshot_bytes = mean(bytes_per_snapshot);
    row.agent_entity_bytes = agent_entity_bytes;
    // Reported per band, because the weighting deliberately spends the far
    // band's refresh rate on the near one. A single worst-case number across all
    // agents describes the outcome of that trade as if it were only a loss.
    row.max_gap_snapshots = gaps.empty() ? 0 : gaps.back();
    row.blackout_seconds = static_cast<double>(row.max_gap_snapshots) /
        static_cast<double>(kSnapshotsPerSecond);
    row.near_blackout_seconds = band_blackout(0);
    row.mid_blackout_seconds = band_blackout(1);
    row.far_blackout_seconds = band_blackout(2);
    row.bytes_per_second =
        row.mean_snapshot_bytes * static_cast<double>(kSnapshotsPerSecond);
    return row;
}

}  // namespace

void print_table(const char* title, bool agents_acting = false) {
    std::printf("%s\n", title);
    std::printf(
        "%7s %9s %10s %8s %9s %9s %9s %9s %11s %10s\n",
        "agents", "relevant", "packed/ss", "agent B", "mean B",
        "near s", "mid s", "far s", "worst s", "B/s");
    for (const std::size_t agent_count : {16u, 64u, 128u, 256u, 500u}) {
        // Eight full cycles, so the longest gap is sampled repeatedly rather
        // than clipped by the end of the window.
        const std::size_t snapshots = std::max<std::size_t>(600, agent_count * 8);
        const Row row = measure(agent_count, snapshots, agents_acting);
        std::printf(
            "%7zu %9zu %10.2f %8zu %9.1f %9.2f %9.2f %9.2f %11.2f %10.0f\n",
            row.agent_count,
            row.relevant_agents,
            row.agents_per_snapshot,
            row.agent_entity_bytes,
            row.mean_snapshot_bytes,
            row.near_blackout_seconds,
            row.mid_blackout_seconds,
            row.far_blackout_seconds,
            row.blackout_seconds,
            row.bytes_per_second);
    }
    std::printf("\n");
}

int main() {
    std::printf("budget=%zu B  snapshot_rate=%u Hz  relevance_radius=%.0f m\n\n",
                kSendBudgetBytes, kSnapshotsPerSecond, kRelevanceRadiusMeters);
    print_table("A. Idle agents");
    print_table("B. Every agent mid-action (+20 B each)", true);
    std::printf("per-client snapshot ceiling at a continuously full budget: "
                "%zu B/s (%.0f kbit/s)\n",
                kSendBudgetBytes * kSnapshotsPerSecond,
                static_cast<double>(kSendBudgetBytes * kSnapshotsPerSecond) * 8.0 /
                    1000.0);
    return 0;
}
