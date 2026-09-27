// G1: how far from the truth is a remote agent drawn, once the snapshot budget
// starts leaving agents out?
//
// render_clock_bench showed the clock is steady; its one actor ran a smooth
// circle and was in every snapshot, so the error it saw was under 3 cm. Here
// the agents turn, reverse and stop, and there are 40 / 80 / 200 of them
// competing for one client's 1200 B snapshots.
//
// Server side: the real build_relevant_snapshot / build_snapshot_send_set pair,
// on agents moved by script, so where each one really is at any instant is
// known exactly. Each send set goes through encode / decode, then reaches the
// client after 50 +- 25 ms with 1% lost. Client side: the real render clock and
// build_interpolated_snapshot_for_server_time at 60 fps.
//
// Per distance band, over every frame of every agent in it:
//   error    drawn vs true position at the instant drawn, p50 / p90 / max (xz)
//   jumps    frames an agent moved more than three run steps (0.25 m), per
//            agent per minute
//   freeze   frames an agent did not move while it really was moving
//
// Each population runs at the live budget, at twice it (G2 option 2), and with
// no budget at all -- every agent in every snapshot, which is the floor linear
// interpolation at 15 Hz leaves on these paths.
//
// Every frame of an agent is also sorted by what the client had to draw it
// from, which says which fix could reach its error:
//   normal   the samples either side are one snapshot interval apart
//   gap      both sides are there but further apart: the agent sat out one or
//            more send sets, and the straight line between them cuts whatever
//            it did in between (Hermite interpolation, G2 option 1, bends it)
//   extrap   nothing newer has arrived yet, so it is carried along its last
//            velocity, and snapped when the next sample shows a turn. The
//            interpolation delay (133 ms) minus the one-way latency is all the
//            slack there is: an agent skipped even once runs out of it. The
//            last table adds delay to see what that buys.
//
// Usage: bazel run -c opt //engine/src/tests/kernel_tests:remote_actor_error_bench

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "protocol/public/network_packets.h"
#include "sync/public/snapshot.h"
#include "world/public/components.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace ne = network_example;

namespace {

constexpr double kRunSeconds = 30.0;
constexpr double kMeasureFromSeconds = 2.0;
constexpr double kFrameSeconds = 1.0 / 60.0;
constexpr std::uint32_t kTickRate = 30;
// gingerbread's move_speed_meters_per_second.
constexpr double kSpeed = 5.0;
constexpr double kJumpMeters = 3.0 * kSpeed * kFrameSeconds;
constexpr double kLatencyMs = 50.0;
constexpr double kJitterMs = 25.0;
constexpr double kLoss = 0.01;
// As in snapshot_bandwidth_benchmark: the live budget, the relevance radius,
// and the bands build_snapshot_send_set weights by.
constexpr std::size_t kLiveBudgetBytes = 1200;
constexpr std::size_t kNoBudgetBytes = 1u << 20;
constexpr float kRelevanceRadiusMeters = 40.0f;
constexpr float kNearBandMeters = 10.0f;
constexpr float kMidBandMeters = 25.0f;

// Spreads agents through the relevance sphere, as snapshot_bandwidth_benchmark
// does, leaving room for each one's few metres of motion.
glm::vec3 anchor(std::size_t index, std::size_t count) {
    const double golden = 2.399963229728653;
    const double t = static_cast<double>(index) / static_cast<double>(count);
    const double radius = (kRelevanceRadiusMeters - 5.0) * std::sqrt(t) + 2.0;
    const double angle = static_cast<double>(index) * golden;
    return glm::vec3{
        static_cast<float>(radius * std::cos(angle)),
        0.0f,
        static_cast<float>(radius * std::sin(angle))};
}

// The four ways an agent moves, all at run speed: a 6 m patrol that reverses at
// each end, a 4 m square with right-angle corners, a 2.5 m circle, and
// walk-2-s / stand-1-s back and forth.
enum class Motion { kReverse, kSquare, kCircle, kStopGo, kCount };

struct State {
    glm::vec3 position;
    glm::vec3 velocity;
};

State motion_offset(Motion motion, double t) {
    const double v = kSpeed;
    switch (motion) {
        case Motion::kReverse: {
            const double length = 6.0;
            const double period = 2.0 * length / v;
            const double phase = std::fmod(t, period);
            const bool out = phase < period / 2.0;
            const double s = out ? phase * v : length - (phase - period / 2.0) * v;
            return {glm::vec3{static_cast<float>(s - length / 2.0), 0.0f, 0.0f},
                    glm::vec3{static_cast<float>(out ? v : -v), 0.0f, 0.0f}};
        }
        case Motion::kSquare: {
            const double side = 4.0;
            const double s = std::fmod(t * v, 4.0 * side);
            const int edge = static_cast<int>(s / side);
            const double along = s - edge * side;
            const double h = side / 2.0;
            const glm::dvec2 corners[4] = {{-h, -h}, {h, -h}, {h, h}, {-h, h}};
            const glm::dvec2 directions[4] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
            const glm::dvec2 p = corners[edge] + directions[edge] * along;
            return {glm::vec3{static_cast<float>(p.x), 0.0f, static_cast<float>(p.y)},
                    glm::vec3{static_cast<float>(directions[edge].x * v), 0.0f,
                              static_cast<float>(directions[edge].y * v)}};
        }
        case Motion::kCircle: {
            const double radius = 2.5;
            const double angle = t * v / radius;
            return {glm::vec3{static_cast<float>(radius * std::cos(angle)), 0.0f,
                              static_cast<float>(radius * std::sin(angle))},
                    glm::vec3{static_cast<float>(-v * std::sin(angle)), 0.0f,
                              static_cast<float>(v * std::cos(angle))}};
        }
        case Motion::kStopGo:
        default: {
            // 0-2 s walk +x, 2-3 s stand, 3-5 s walk back, 5-6 s stand.
            const double phase = std::fmod(t, 6.0);
            const double reach = 2.0 * v;
            double x = 0.0;
            double vx = 0.0;
            if (phase < 2.0) {
                x = phase * v;
                vx = v;
            } else if (phase < 3.0) {
                x = reach;
            } else if (phase < 5.0) {
                x = reach - (phase - 3.0) * v;
                vx = -v;
            }
            return {glm::vec3{static_cast<float>(x - reach / 2.0), 0.0f, 0.0f},
                    glm::vec3{static_cast<float>(vx), 0.0f, 0.0f}};
        }
    }
}

struct Agent {
    ne::NetId net_id = 0;
    glm::vec3 anchor{0.0f};
    Motion motion = Motion::kReverse;
    double phase = 0.0;
    std::size_t band = 0;

    State at(double t) const {
        const State offset = motion_offset(motion, t + phase);
        return {anchor + offset.position, offset.velocity};
    }
};

struct Arrival {
    double at_seconds;
    ne::WorldSnapshot snapshot;
};

enum Source { kNormal, kGap, kExtrapolated, kSourceCount };

struct Band {
    std::size_t agents = 0;
    std::vector<double> errors;
    std::array<std::vector<double>, kSourceCount> source_errors;
    std::size_t jumps = 0;
    std::size_t moving_frames = 0;
    std::size_t freeze_frames = 0;
    std::size_t agent_frames = 0;
};

struct Result {
    std::array<Band, 3> bands;
    double lag_sum_ms = 0.0;
    std::size_t lag_frames = 0;
    double agents_per_snapshot = 0.0;
    std::size_t decode_failures = 0;
};

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(fraction * (values.size() - 1))];
}

// extra_delay_ms draws the world that much further behind than the kernel's
// two snapshot intervals, by skewing the client's clock-sync estimate.
// snapshot_rate is the kernel's snapshot_rate: at 30 Hz the interpolation
// delay, two intervals, halves too, so extra_delay_ms is what puts it back.
Result run(
    std::size_t agent_count,
    std::size_t budget_bytes,
    bool acting,
    double extra_delay_ms = 0.0,
    std::uint32_t snapshot_rate = 15) {
    Result result;
    std::vector<Agent> agents(agent_count);

    // --- server: what leaves, and when it arrives ---
    std::vector<Arrival> arrivals;
    {
        KernelConfig config{};
        config.mode = KernelMode_DedicatedServer;
        config.tick.server_tick_rate = kTickRate;
        config.tick.snapshot_rate = snapshot_rate;
        ne::KernelEngine server(config);
        server.reset_runtime_state(KernelMode_DedicatedServer);
        ne::World& world = server.simulation_world();
        const ne::NetId player = world.spawn_player(1, glm::vec3{0.0f});
        std::vector<entt::entity> entities;
        for (std::size_t index = 0; index < agent_count; ++index) {
            Agent& agent = agents[index];
            agent.anchor = anchor(index, agent_count);
            agent.motion = static_cast<Motion>(
                index % static_cast<std::size_t>(Motion::kCount));
            agent.phase = 0.61 * static_cast<double>(index);
            const float distance = glm::length(agent.anchor);
            agent.band = distance <= kNearBandMeters
                ? 0u
                : (distance <= kMidBandMeters ? 1u : 2u);
            ++result.bands[agent.band].agents;
            agent.net_id = world.spawn_enemy(agent.at(0.0).position);
            entities.push_back(*world.find_entity(agent.net_id));
            // spawn_enemy leaves hp at zero, which the snapshot reports as
            // dead, and the client neither extrapolates nor bridges a corpse.
            world.registry().emplace_or_replace<ne::Health>(
                entities.back(), ne::Health{100u, 100u});
            if (acting) {
                // Mid-action: the 20 B timeline block, as a fighting crowd.
                world.registry()
                    .emplace<ne::ActionRuntimeState>(entities.back())
                    .action_template_id = 100u;
            }
        }
        ne::KernelEngine::PeerSession session{1, player, 0, true, {}};

        std::mt19937 random(1234u);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::size_t packed_total = 0;
        std::size_t snapshots = 0;
        const std::uint32_t interval = kTickRate / snapshot_rate;
        const std::uint32_t last_tick =
            static_cast<std::uint32_t>(kRunSeconds * kTickRate);
        std::uint32_t sequence = 0;
        for (std::uint32_t tick = interval; tick <= last_tick; tick += interval) {
            const double t = static_cast<double>(tick) / kTickRate;
            for (std::size_t index = 0; index < agent_count; ++index) {
                const State state = agents[index].at(t);
                world.registry().get<ne::Transform>(entities[index]).position =
                    state.position;
                world.registry().emplace_or_replace<ne::Velocity>(
                    entities[index], ne::Velocity{state.velocity});
            }
            const ne::WorldSnapshot relevant = server.build_relevant_snapshot(
                session, static_cast<std::uint32_t>(t * 1000.0));
            ne::WorldSnapshot send =
                server.build_snapshot_send_set(session, relevant, budget_bytes);
            send.header.server_tick = tick;
            for (const ne::EntitySnapshot& entity : send.entities) {
                if (entity.actor_type == ne::ActorType::kAgent) ++packed_total;
            }
            ++snapshots;
            const double delay_ms =
                kLatencyMs + kJitterMs * (2.0 * unit(random) - 1.0);
            if (unit(random) < kLoss) continue;
            // What the client decodes, quantisation included.
            const std::vector<std::uint8_t> packet =
                ne::encode_snapshot_packet(send, ++sequence);
            ne::WorldSnapshot decoded;
            if (!ne::decode_snapshot_packet(packet.data(), packet.size(), &decoded)) {
                ++result.decode_failures;
                decoded = send;
            }
            decoded.header.server_tick = tick;
            arrivals.push_back(Arrival{t + delay_ms / 1000.0, std::move(decoded)});
        }
        result.agents_per_snapshot =
            static_cast<double>(packed_total) / static_cast<double>(snapshots);
        std::sort(arrivals.begin(), arrivals.end(),
                  [](const Arrival& a, const Arrival& b) {
                      return a.at_seconds < b.at_seconds;
                  });
    }

    // --- client: draw at 60 fps and compare ---
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = kTickRate;
    config.tick.snapshot_rate = snapshot_rate;
    ne::KernelEngine client(config);
    client.reset_runtime_state(KernelMode_Client);
    client.has_client_clock_sync_ = true;
    client.client_clock_offset_us_ =
        -static_cast<std::int64_t>(extra_delay_ms * 1000.0);
    const std::uint64_t interval_us = 1000000u / snapshot_rate;

    std::unordered_map<ne::NetId, std::size_t> index_of;
    for (std::size_t index = 0; index < agent_count; ++index) {
        index_of.emplace(agents[index].net_id, index);
    }
    std::vector<std::optional<glm::vec3>> previous_drawn(agent_count);
    std::vector<glm::vec3> previous_true(agent_count);
    std::size_t next = 0;
    for (double now = 0.0; now < kRunSeconds - 0.5; now += kFrameSeconds) {
        while (next < arrivals.size() && arrivals[next].at_seconds <= now) {
            client.store_client_snapshot(std::move(arrivals[next].snapshot));
            ++next;
        }
        if (client.client_snapshot_buffer_.size() < 2u) continue;
        const std::uint64_t now_us = static_cast<std::uint64_t>(now * 1000000.0);
        client.advance_render_clock(now_us);
        std::uint64_t render_us = 0;
        if (!client.client_render_server_time_us(now_us, &render_us)) continue;
        ne::WorldSnapshot drawn;
        if (!client.build_interpolated_snapshot_for_server_time(render_us, &drawn)) {
            continue;
        }
        const double render_t = static_cast<double>(render_us) / 1000000.0;
        const bool measuring = now >= kMeasureFromSeconds;
        if (measuring) {
            result.lag_sum_ms += (now - render_t) * 1000.0;
            ++result.lag_frames;
        }
        // Each agent's samples either side of the instant drawn.
        std::unordered_map<ne::NetId, std::pair<std::uint64_t, std::uint64_t>> around;
        for (const ne::WorldSnapshot& buffered : client.client_snapshot_buffer_) {
            const std::uint64_t time_us =
                static_cast<std::uint64_t>(buffered.header.server_tick) * 1000000u /
                kTickRate;
            for (const ne::EntitySnapshot& entity : buffered.entities) {
                auto& [before, after] = around[entity.net_id];
                if (time_us <= render_us) {
                    before = time_us;
                } else if (after == 0u) {
                    after = time_us;
                }
            }
        }
        for (const ne::EntitySnapshot& entity : drawn.entities) {
            const auto found = index_of.find(entity.net_id);
            if (found == index_of.end()) continue;
            const std::size_t index = found->second;
            const Agent& agent = agents[index];
            const glm::vec3 position{entity.position.x, 0.0f, entity.position.z};
            const glm::vec3 truth = agent.at(render_t).position;
            if (measuring && previous_drawn[index].has_value()) {
                Band& band = result.bands[agent.band];
                ++band.agent_frames;
                const double error = glm::length(position - truth);
                band.errors.push_back(error);
                const auto& [before, after] = around[entity.net_id];
                const Source source = after == 0u
                    ? kExtrapolated
                    // A millisecond of slack: tick times are whole microseconds.
                    : (after - before > interval_us + 1000u ? kGap : kNormal);
                band.source_errors[source].push_back(error);
                const double step = glm::length(position - *previous_drawn[index]);
                if (step > kJumpMeters) ++band.jumps;
                if (glm::length(truth - previous_true[index]) > 0.001) {
                    ++band.moving_frames;
                    if (step < 0.001) ++band.freeze_frames;
                }
            }
            previous_drawn[index] = position;
            previous_true[index] = truth;
        }
    }
    return result;
}

void print(std::size_t agent_count, const char* budget, const Result& result) {
    static const char* const kBandNames[3] = {"near", "mid", "far"};
    std::printf("  %3zu agents, %-9s  %5.1f agents/snapshot  lag %3.0f ms%s\n",
                agent_count, budget, result.agents_per_snapshot,
                result.lag_sum_ms / static_cast<double>(std::max<std::size_t>(1, result.lag_frames)),
                result.decode_failures > 0 ? "  (some sent undecoded)" : "");
    for (std::size_t band = 0; band < 3; ++band) {
        const Band& b = result.bands[band];
        if (b.agents == 0) continue;
        const double minutes =
            static_cast<double>(b.agent_frames) * kFrameSeconds / 60.0;
        std::printf(
            "      %-4s %3zu   error p50 %4.2f p90 %4.2f max %5.2f m   "
            "jumps %6.1f /agent/min   freeze %5.1f%%",
            kBandNames[band], b.agents,
            percentile(b.errors, 0.5), percentile(b.errors, 0.9),
            percentile(b.errors, 1.0),
            minutes > 0.0 ? static_cast<double>(b.jumps) / minutes : 0.0,
            b.moving_frames > 0
                ? 100.0 * static_cast<double>(b.freeze_frames) /
                      static_cast<double>(b.moving_frames)
                : 0.0);
        static const char* const kSourceNames[kSourceCount] = {"normal", "gap", "extrap"};
        for (int source = 0; source < kSourceCount; ++source) {
            const std::vector<double>& errors = b.source_errors[source];
            std::printf(
                "   %s %4.1f%% p90 %4.2f",
                kSourceNames[source],
                b.errors.empty() ? 0.0
                    : 100.0 * static_cast<double>(errors.size()) /
                          static_cast<double>(b.errors.size()),
                percentile(errors, 0.9));
        }
        std::printf("\n");
    }
}

}  // namespace

int main() {
    std::printf(
        "agents at %.0f m/s (reverse / square / circle / stop-go), 15 Hz snapshots unless noted, "
        "%.0f +- %.0f ms, %.0f%% loss, 60 fps, %.0f s per run\n"
        "bands: near <= %.0f m, mid <= %.0f m, far beyond\n",
        kSpeed, kLatencyMs, kJitterMs, kLoss * 100.0, kRunSeconds,
        kNearBandMeters, kMidBandMeters);
    for (const bool acting : {false, true}) {
        std::printf("\n%s\n", acting ? "agents mid-action (52 B each)"
                                     : "agents idle (32 B each)");
        for (const std::size_t count : {40u, 80u, 200u}) {
            print(count, "1200 B", run(count, kLiveBudgetBytes, acting));
            print(count, "2400 B", run(count, 2 * kLiveBudgetBytes, acting));
            print(count, "unlimited", run(count, kNoBudgetBytes, acting));
        }
    }
    std::printf("\ninterpolation delay, at the live 1200 B budget\n");
    for (const bool acting : {false, true}) {
        for (const std::size_t count : {80u, 200u}) {
            std::printf("%s\n", acting ? "agents mid-action" : "agents idle");
            for (const double extra_ms : {0.0, 67.0, 133.0, 267.0}) {
                char label[32];
                std::snprintf(label, sizeof(label), "+%.0f ms", extra_ms);
                print(count, label, run(count, kLiveBudgetBytes, acting, extra_ms));
            }
        }
    }
    // The other way to spend twice the bytes: 1200 B packets, one MTU each as
    // now, twice as often. The kernel's own delay would drop to 67 ms with the
    // interval, so 67 ms is added back to compare at the same 133 ms.
    std::printf("\n30 Hz snapshots of 1200 B, delay held at 133 ms\n");
    for (const bool acting : {false, true}) {
        std::printf("%s\n", acting ? "agents mid-action" : "agents idle");
        for (const std::size_t count : {80u, 200u}) {
            print(count, "30 Hz", run(count, kLiveBudgetBytes, acting, 67.0, 30));
        }
    }
    return 0;
}
