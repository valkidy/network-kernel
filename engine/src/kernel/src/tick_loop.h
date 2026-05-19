#ifndef KERNEL_SRC_TICK_LOOP_H_
#define KERNEL_SRC_TICK_LOOP_H_

#include <cstdint>

#include "kernel/public/kernel_types.h"

namespace network_example {

TickConfig current_netcode_preset();
TickConfig shooter_tuning_preset();
TickConfig with_tick_defaults(TickConfig config);

class TickLoop {
public:
    explicit TickLoop(TickConfig config);

    std::uint32_t accumulate(float delta_seconds);
    float fixed_delta_seconds() const;
    std::uint32_t current_tick() const;
    std::uint32_t snapshot_interval_ticks() const;
    bool should_write_snapshot() const;

    void reset(TickConfig config, std::uint32_t current_tick = 0);
    void advance_tick();

private:
    TickConfig config_;
    double accumulator_seconds_ = 0.0;
    std::uint32_t current_tick_ = 0;
};

}  // namespace network_example

#endif  // KERNEL_SRC_TICK_LOOP_H_
