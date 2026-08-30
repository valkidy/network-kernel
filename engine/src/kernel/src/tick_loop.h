#ifndef KERNEL_SRC_TICK_LOOP_H_
#define KERNEL_SRC_TICK_LOOP_H_

#include <cstdint>
#include <string_view>
#include <vector>

#include <cstdint>

#include "kernel/public/kernel_types.h"

namespace network_example {

TickConfig current_netcode_preset();
TickConfig shooter_tuning_preset();
TickConfig with_tick_defaults(TickConfig config);

// The presets a server may be started with, named by what they cost a
// deployment rather than by their rate: the number that decides between them is
// bandwidth per client and the interpolation delay a player sees, not hertz.
//
// `shooter_tuning_preset` is deliberately not among them. It runs a 60 Hz tick,
// and every CPU measurement behind the sizing in
// docs/SERVER_DATA_SYNC_PACKET_SIZE_REPORT.md was taken at 30 -- reaching it
// from a launch flag would make an unmeasured configuration selectable.
struct NetcodePreset {
    std::string_view name;
    TickConfig tick;
};

// Snapshots at half the tick rate. The default.
TickConfig standard_netcode_preset();
// A snapshot every tick: twice the bandwidth per client, and half the
// interpolation delay, because the client derives that delay from the rate.
TickConfig responsive_netcode_preset();

// Resolves a preset by name. False leaves `out_tick` untouched.
bool find_netcode_preset(std::string_view name, TickConfig* out_tick);
// The selectable names, in the order they should be offered.
std::vector<std::string_view> netcode_preset_names();

// How long the client holds a snapshot before displaying it, which is two
// snapshot intervals. Reported so that a server can say what it costs a player.
std::uint32_t interpolation_delay_ms(const TickConfig& tick);

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
