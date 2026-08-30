#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "kernel/src/tick_loop.h"

namespace {

// assert() is compiled out under -c opt, which is how this suite is normally
// run, so anything that must actually gate uses this instead.
void require_impl(bool condition, const char* expression, int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}

#define require(condition) require_impl((condition), #condition, __LINE__)

void the_selectable_presets_are_the_measured_ones() {
    TickConfig standard{};
    require(network_example::find_netcode_preset("standard", &standard));
    require(standard.server_tick_rate == 30);
    require(standard.snapshot_rate == 15);

    TickConfig responsive{};
    require(network_example::find_netcode_preset("responsive", &responsive));
    require(responsive.server_tick_rate == 30);
    // A snapshot every tick, which is what halves the interpolation delay.
    require(responsive.snapshot_rate == responsive.server_tick_rate);

    // The 60 Hz tuning exists but is deliberately not reachable from a launch
    // flag: nothing behind the sizing in the packet size report was measured at
    // a 60 Hz tick, and a name here would make that configuration selectable.
    TickConfig unreachable{};
    require(!network_example::find_netcode_preset("shooter", &unreachable));
    require(!network_example::find_netcode_preset("", &unreachable));
    require(unreachable.server_tick_rate == 0);
    require(network_example::netcode_preset_names().size() == 2);

    // What the choice actually costs a player. The client derives its
    // interpolation delay from the snapshot rate, so the two presets differ by
    // more than smoothness.
    require(network_example::interpolation_delay_ms(standard) == 133);
    require(network_example::interpolation_delay_ms(responsive) == 66);
}

// A snapshot rate above the tick rate used to be truncated inside
// snapshot_interval_ticks, which floors the division at 1 -- so the config, and
// the welcome packet built from it, advertised a rate the server was never
// going to send at, and the client sized its interpolation delay from that.
void a_snapshot_rate_above_the_tick_rate_is_clamped_not_truncated() {
    TickConfig config{};
    config.server_tick_rate = 30;
    config.snapshot_rate = 60;
    const TickConfig resolved = network_example::with_tick_defaults(config);
    require(resolved.snapshot_rate == 30);

    network_example::TickLoop loop(config);
    require(loop.snapshot_interval_ticks() == 1);
}

}  // namespace

int main() {
    the_selectable_presets_are_the_measured_ones();
    a_snapshot_rate_above_the_tick_rate_is_clamped_not_truncated();

    const TickConfig current_preset = network_example::current_netcode_preset();
    assert(current_preset.server_tick_rate == 30);
    assert(current_preset.snapshot_rate == 15);
    assert(current_preset.history_ms == 500);
    assert(current_preset.max_ticks_per_update == 4);

    const TickConfig shooter_preset = network_example::shooter_tuning_preset();
    assert(shooter_preset.server_tick_rate == 60);
    assert(shooter_preset.snapshot_rate == 20);
    assert(shooter_preset.history_ms == 300);
    assert(shooter_preset.max_ticks_per_update == 4);

    const TickConfig defaults = network_example::with_tick_defaults(TickConfig{});
    assert(defaults.server_tick_rate == current_preset.server_tick_rate);
    assert(defaults.snapshot_rate == current_preset.snapshot_rate);
    assert(defaults.history_ms == current_preset.history_ms);
    assert(defaults.max_ticks_per_update == current_preset.max_ticks_per_update);

    TickConfig config{};
    config.server_tick_rate = 30;
    config.snapshot_rate = 15;
    config.max_ticks_per_update = 4;

    network_example::TickLoop loop(config);
    assert(loop.accumulate(0.016f) == 0);
    assert(loop.accumulate(0.018f) == 1);
    assert(loop.current_tick() == 0);
    loop.advance_tick();
    assert(loop.current_tick() == 1);
    assert(loop.snapshot_interval_ticks() == 2);

    assert(loop.accumulate(1.0f) == 4);
    return 0;
}
