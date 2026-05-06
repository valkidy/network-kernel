#include <cassert>

#include "kernel/src/tick_loop.h"

int main() {
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
