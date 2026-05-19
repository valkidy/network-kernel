#include <cassert>

#include "kernel/src/tick_loop.h"

int main() {
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
