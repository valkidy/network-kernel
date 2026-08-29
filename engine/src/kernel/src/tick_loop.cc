#include "kernel/src/tick_loop.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace network_example {

TickConfig current_netcode_preset() {
    return TickConfig{
        30,
        15,
        500,
        4,
    };
}

TickConfig shooter_tuning_preset() {
    return TickConfig{
        60,
        20,
        300,
        4,
    };
}

TickConfig standard_netcode_preset() {
    return current_netcode_preset();
}

TickConfig responsive_netcode_preset() {
    TickConfig config = current_netcode_preset();
    config.snapshot_rate = config.server_tick_rate;
    return config;
}

bool find_netcode_preset(std::string_view name, TickConfig* out_tick) {
    if (out_tick == nullptr) {
        return false;
    }
    if (name == "standard") {
        *out_tick = standard_netcode_preset();
        return true;
    }
    if (name == "responsive") {
        *out_tick = responsive_netcode_preset();
        return true;
    }
    return false;
}

std::vector<std::string_view> netcode_preset_names() {
    return {"standard", "responsive"};
}

std::uint32_t interpolation_delay_ms(const TickConfig& tick) {
    const TickConfig resolved = with_tick_defaults(tick);
    const std::uint32_t interval_ticks =
        std::max(1u, resolved.server_tick_rate / resolved.snapshot_rate);
    return interval_ticks * 2u * 1000u / resolved.server_tick_rate;
}

TickConfig with_tick_defaults(TickConfig config) {
    const TickConfig current = current_netcode_preset();
    if (config.server_tick_rate == 0) {
        config.server_tick_rate = current.server_tick_rate;
    }
    if (config.snapshot_rate == 0) {
        config.snapshot_rate = current.snapshot_rate;
    }
    if (config.history_ms == 0) {
        config.history_ms = current.history_ms;
    }
    if (config.max_ticks_per_update == 0) {
        config.max_ticks_per_update = current.max_ticks_per_update;
    }
    // Clamped here rather than left to snapshot_interval_ticks, which divides
    // the two and floors at 1: a snapshot rate above the tick rate used to be
    // truncated silently, so the welcome packet told the client a rate the
    // server was never going to send at, and the client sized its interpolation
    // delay from that. Clamping makes the config say what actually happens.
    if (config.snapshot_rate > config.server_tick_rate) {
        config.snapshot_rate = config.server_tick_rate;
    }
    return config;
}

TickLoop::TickLoop(TickConfig config) : config_(with_tick_defaults(config)) {}

void TickLoop::reset(TickConfig config, std::uint32_t current_tick) {
    config_ = with_tick_defaults(config);
    accumulator_seconds_ = 0.0;
    current_tick_ = current_tick;
}

std::uint32_t TickLoop::accumulate(float delta_seconds) {
    if (delta_seconds <= 0.0f) {
        return 0;
    }

    accumulator_seconds_ += static_cast<double>(delta_seconds);
    const double fixed_delta = static_cast<double>(fixed_delta_seconds());
    std::uint32_t ticks = 0;
    while (accumulator_seconds_ + 0.0000001 >= fixed_delta &&
           ticks < config_.max_ticks_per_update) {
        accumulator_seconds_ -= fixed_delta;
        ++ticks;
    }
    if (ticks == config_.max_ticks_per_update && accumulator_seconds_ >= fixed_delta) {
        accumulator_seconds_ = std::min(accumulator_seconds_, fixed_delta);
    }
    return ticks;
}

float TickLoop::fixed_delta_seconds() const {
    return 1.0f / static_cast<float>(config_.server_tick_rate);
}

std::uint32_t TickLoop::current_tick() const {
    return current_tick_;
}

std::uint32_t TickLoop::snapshot_interval_ticks() const {
    return std::max(1u, config_.server_tick_rate / config_.snapshot_rate);
}

bool TickLoop::should_write_snapshot() const {
    return current_tick_ % snapshot_interval_ticks() == 0;
}

void TickLoop::advance_tick() {
    ++current_tick_;
}

}  // namespace network_example
