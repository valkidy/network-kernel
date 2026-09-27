// W1 on a late, jittery stream: the render clock against the clock it replaced,
// on the same snapshots arriving at the same instants.
//
// One remote actor runs a circle at run speed. Its snapshots leave the server
// at 15 Hz and reach the client after a one-way delay drawn uniformly from
// [latency - jitter, latency + jitter]; some are lost. The client is drawn at
// 60 fps. Clock sync is exact (client time is server time), so what is left is
// the stream itself. For each frame it records where the actor is drawn and
// where it really was at the instant drawn.
//
//   world freezes   frames the actor did not move while it was running, and the
//                   longest run of them -- the "everything stops" of W1
//   max step        the largest move between two frames; a run step is 5 cm
//   jumps           frames that moved more than three run steps
//   error           drawn vs true position at the instant drawn (p50/p90/max)
//   lag             server now minus the instant drawn, mean and max
//   backwards       frames drawn at an earlier instant than the one before
//
// Usage: bazel run -c opt //engine/src/tests/kernel_tests:render_clock_bench

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "sync/public/snapshot.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace ne = network_example;

namespace {

constexpr ne::NetId kActor = 10;
constexpr float kRadius = 10.0f;
constexpr float kRunSpeed = 3.0f;
constexpr double kFrameSeconds = 1.0 / 60.0;
constexpr double kRunSeconds = 60.0;

struct Network {
    const char* name;
    double latency_ms;
    double jitter_ms;
    double loss;
    // Every stall_every_s, the stream stops for stall_ms: whatever is sent in
    // that window arrives together once it ends. Zero for none.
    double stall_every_s = 0.0;
    double stall_ms = 0.0;
};

glm::vec3 true_position(double seconds) {
    const double angle = seconds * kRunSpeed / kRadius;
    return glm::vec3{
        kRadius * static_cast<float>(std::cos(angle)),
        0.0f,
        kRadius * static_cast<float>(std::sin(angle))};
}

glm::vec3 true_velocity(double seconds) {
    const double angle = seconds * kRunSpeed / kRadius;
    return glm::vec3{
        -kRunSpeed * static_cast<float>(std::sin(angle)),
        0.0f,
        kRunSpeed * static_cast<float>(std::cos(angle))};
}

struct Arrival {
    double at_seconds;
    std::uint32_t tick;
};

// The same schedule for both clocks: which snapshots arrive, and when.
std::vector<Arrival> schedule(const Network& network, std::uint32_t seed) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<double> jitter(-1.0, 1.0);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::vector<Arrival> arrivals;
    const std::uint32_t last_tick = static_cast<std::uint32_t>(kRunSeconds * 30.0);
    for (std::uint32_t tick = 2; tick <= last_tick; tick += 2) {
        const double delay_ms =
            network.latency_ms + network.jitter_ms * jitter(random);
        if (unit(random) < network.loss) continue;
        const double sent = static_cast<double>(tick) / 30.0;
        double at = sent + std::max(0.0, delay_ms) / 1000.0;
        if (network.stall_every_s > 0.0) {
            const double window_start =
                std::floor(sent / network.stall_every_s) * network.stall_every_s;
            const double window_end = window_start + network.stall_ms / 1000.0;
            if (window_start > 0.0 && sent < window_end) {
                at = std::max(at, window_end + network.latency_ms / 1000.0);
            }
        }
        arrivals.push_back(Arrival{at, tick});
    }
    std::sort(arrivals.begin(), arrivals.end(), [](const Arrival& a, const Arrival& b) {
        return a.at_seconds < b.at_seconds;
    });
    return arrivals;
}

struct Result {
    int frames = 0;
    int world_freeze_frames = 0;
    double longest_freeze_ms = 0.0;
    int jumps = 0;
    double max_step = 0.0;
    std::vector<double> errors;
    double lag_sum_ms = 0.0;
    double lag_max_ms = 0.0;
    int backwards = 0;
};

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(fraction * (values.size() - 1))];
}

Result run(const std::vector<Arrival>& arrivals, bool render_clock) {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    ne::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_Client);
    engine.has_client_clock_sync_ = true;
    engine.client_clock_offset_us_ = 0;
    engine.render_clock_enabled_ = render_clock;

    Result result;
    std::size_t next_arrival = 0;
    bool has_previous = false;
    glm::vec3 previous{0.0f};
    std::uint64_t previous_render_us = 0;
    double freeze_ms = 0.0;
    // Past the start-up second, before which there is no interpolation pair.
    const double measure_from = 2.0;
    for (double now = 0.0; now < kRunSeconds - 1.0; now += kFrameSeconds) {
        while (next_arrival < arrivals.size() &&
               arrivals[next_arrival].at_seconds <= now) {
            const std::uint32_t tick = arrivals[next_arrival].tick;
            const double sent_seconds = static_cast<double>(tick) / 30.0;
            ne::EntitySnapshot actor;
            actor.net_id = kActor;
            actor.type = ne::EntityType::kActor;
            actor.actor_type = ne::ActorType::kAgent;
            actor.position = true_position(sent_seconds);
            actor.velocity = true_velocity(sent_seconds);
            actor.hp = 100;
            ne::WorldSnapshot snapshot;
            snapshot.header.server_tick = tick;
            snapshot.entities.push_back(actor);
            engine.store_client_snapshot(std::move(snapshot));
            ++next_arrival;
        }
        if (engine.client_snapshot_buffer_.size() < 2u) continue;

        const std::uint64_t now_us = static_cast<std::uint64_t>(now * 1000000.0);
        // What a render pass does: advance the clock, read the instant, draw.
        engine.advance_render_clock(now_us);
        std::uint64_t render_us = 0;
        if (!engine.client_render_server_time_us(now_us, &render_us)) continue;
        ne::WorldSnapshot drawn;
        if (!engine.build_interpolated_snapshot_for_server_time(render_us, &drawn)) {
            continue;
        }
        const ne::EntitySnapshot* actor = nullptr;
        for (const ne::EntitySnapshot& entity : drawn.entities) {
            if (entity.net_id == kActor) actor = &entity;
        }
        if (actor == nullptr) continue;
        const glm::vec3 position{actor->position.x, 0.0f, actor->position.z};

        if (now >= measure_from && has_previous) {
            ++result.frames;
            const double step = glm::length(position - previous);
            result.max_step = std::max(result.max_step, step);
            if (step > 3.0 * kRunSpeed * kFrameSeconds) ++result.jumps;
            if (step < 0.001) {
                ++result.world_freeze_frames;
                freeze_ms += kFrameSeconds * 1000.0;
                result.longest_freeze_ms = std::max(result.longest_freeze_ms, freeze_ms);
            } else {
                freeze_ms = 0.0;
            }
            const double render_seconds = static_cast<double>(render_us) / 1000000.0;
            result.errors.push_back(glm::length(position - true_position(render_seconds)));
            const double lag_ms = (now - render_seconds) * 1000.0;
            result.lag_sum_ms += lag_ms;
            result.lag_max_ms = std::max(result.lag_max_ms, lag_ms);
            if (render_us < previous_render_us) ++result.backwards;
        }
        previous = position;
        previous_render_us = render_us;
        has_previous = true;
    }
    return result;
}

void print(const char* clock, const Result& result) {
    std::printf(
        "  %-12s freeze %5.1f%% (%4d frames, longest %4.0f ms)  max step %5.2f m  "
        "jumps %4d  error p50 %5.2f p90 %5.2f max %5.2f m  lag avg %4.0f max %4.0f ms  "
        "backwards %d\n",
        clock,
        100.0 * result.world_freeze_frames / std::max(1, result.frames),
        result.world_freeze_frames,
        result.longest_freeze_ms,
        result.max_step,
        result.jumps,
        percentile(result.errors, 0.5),
        percentile(result.errors, 0.9),
        percentile(result.errors, 1.0),
        result.lag_sum_ms / std::max(1, result.frames),
        result.lag_max_ms,
        result.backwards);
}

}  // namespace

int main() {
    const Network networks[] = {
        {"local (G0 play tests)", 0.0, 0.0, 0.0},
        {"LAN 20 +- 5 ms", 20.0, 5.0, 0.0},
        {"50 +- 25 ms, 1% loss", 50.0, 25.0, 0.01},
        {"100 +- 50 ms, 1% loss", 100.0, 50.0, 0.01},
        {"150 +- 75 ms, 2% loss", 150.0, 75.0, 0.02},
        {"50 +- 25 ms, 400 ms outage every 10 s", 50.0, 25.0, 0.0, 10.0, 400.0},
        {"50 +- 25 ms, 1500 ms outage every 10 s", 50.0, 25.0, 0.0, 10.0, 1500.0},
    };
    std::printf(
        "one actor at %.0f m/s, 15 Hz snapshots, 60 fps, %.0f s per run, "
        "interpolation delay 133 ms\n",
        kRunSpeed, kRunSeconds);
    for (const Network& network : networks) {
        const std::vector<Arrival> arrivals = schedule(network, 1234u);
        std::printf("%s\n", network.name);
        print("clamped", run(arrivals, false));
        print("render clock", run(arrivals, true));
    }
    return 0;
}
