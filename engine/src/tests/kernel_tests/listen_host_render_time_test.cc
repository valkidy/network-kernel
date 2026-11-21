// A listen host draws its own world through its client half, interpolating the
// loopback snapshots an interpolation delay behind the server's present. The
// host's presentation clock is not that present: the tick loop drops whatever a
// long frame owes past max_ticks_per_update, while the host's clock counts the
// whole frame. Taking the host's time as the server's put the target past the
// newest snapshot after one hitch, and the world stepped once per snapshot
// interval from then on (issue_20261121_215915: 66 ms holds on a Mac host).
//
// Every standard and third-party header goes in before the `private` define.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "protocol/public/network_packets.h"
#include "world/public/components.h"
#define private public
#include "kernel/src/kernel.h"
#undef private

namespace ne = network_example;

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "listen_host_render_time_test:%d: %s\n", line, expression);
        std::exit(1);
    }
}
#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr double kFrameSeconds = 1.0 / 120.0;

struct Host {
    Host() : engine(make_config()) {
        engine.reset_runtime_state(KernelMode_ListenServer);
    }

    static KernelConfig make_config() {
        KernelConfig config{};
        config.mode = KernelMode_ListenServer;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        return config;
    }

    // The kernel's own tick_time_us, which is local to kernel.cc.
    std::uint64_t tick_us(std::uint32_t tick) const {
        return static_cast<std::uint64_t>(
            static_cast<double>(tick) *
            static_cast<double>(engine.tick_loop_.fixed_delta_seconds()) * 1000000.0);
    }

    // One host frame as GameRunner runs it: the same delta to the tick loop and
    // to the presentation clock, the loopback snapshots of the ticks it ran,
    // then the render pass at the presentation clock. Returns the instant drawn.
    std::uint64_t frame(double delta_seconds) {
        const std::uint32_t ticks =
            engine.tick_loop_.accumulate(static_cast<float>(delta_seconds));
        for (std::uint32_t i = 0; i < ticks; ++i) {
            engine.tick_loop_.advance_tick();
            const std::uint32_t tick = engine.tick_loop_.current_tick();
            if (tick % engine.tick_loop_.snapshot_interval_ticks() == 0u) {
                ne::WorldSnapshot snapshot;
                snapshot.header.server_tick = tick;
                engine.handle_client_snapshot(snapshot);
                newest_tick = tick;
            }
        }
        host_seconds += delta_seconds;
        const std::uint64_t host_time_us =
            static_cast<std::uint64_t>(host_seconds * 1000000.0);
        engine.get_render_states_at_time(
            host_time_us, states.data(), static_cast<std::uint32_t>(states.size()));
        return engine.render_server_time_us_;
    }

    ne::KernelEngine engine;
    double host_seconds = 0.0;
    std::uint32_t newest_tick = 0;
    std::array<RenderEntityState, 8> states{};
};

// Frames in a row at 120 Hz that drew the same instant. Interpolating, every
// frame moves on; pinned to the newest snapshot, the instant holds for the
// eight frames of a 15 Hz snapshot interval.
int longest_hold(Host* host, int frames) {
    std::uint64_t last = host->frame(kFrameSeconds);
    int hold = 0;
    int longest = 0;
    for (int i = 1; i < frames; ++i) {
        const std::uint64_t drawn = host->frame(kFrameSeconds);
        require(drawn >= last);
        require(drawn <= host->tick_us(host->newest_tick));
        if (drawn == last) {
            longest = std::max(longest, ++hold);
        } else {
            hold = 0;
        }
        last = drawn;
    }
    return longest;
}

void a_hitch_does_not_pin_the_host_to_the_newest_snapshot() {
    Host host;
    for (int i = 0; i < 240; ++i) host.frame(kFrameSeconds);
    const int before = longest_hold(&host, 240);
    std::printf("longest hold before the hitch: %d frames\n", before);
    require(before == 0);

    // One long frame, as building a level's terrain on the main thread does:
    // far more than max_ticks_per_update runs, so the tick loop drops the rest.
    const std::uint32_t tick_before = host.engine.tick_loop_.current_tick();
    host.frame(1.5);
    require(host.engine.tick_loop_.current_tick() - tick_before < 45u);
    for (int i = 0; i < 60; ++i) host.frame(kFrameSeconds);
    const int after = longest_hold(&host, 240);
    std::printf("longest hold after the hitch: %d frames\n", after);
    require(after == 0);
}

}  // namespace

int main() {
    a_hitch_does_not_pin_the_host_to_the_newest_snapshot();
    std::printf("listen_host_render_time_test: PASS\n");
    return 0;
}
