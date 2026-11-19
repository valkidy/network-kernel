#pragma once

// The one source of gameplay randomness (docs/PLAYER_WEAPON_REVAMP_PLAN.md I1).
//
// Every random choice that changes the simulation -- a repeated spawn's count
// and scatter, a descent's angle, an action's possibility -- is a pure function
// of facts every machine that runs the step already has: who acted, which
// action instance, which iteration, which target. The same inputs give the
// same answer on every platform, so a client can reproduce a choice from what
// the server replicated (derived chains do today; predicted hits could later).
//
// Integers only. Do not reach for std::mt19937, std::uniform_*_distribution or
// rand(): the distributions are implementation-defined, so Windows, Linux and
// macOS standard libraries may disagree on the same seed.

#include <cstdint>

namespace network_example {

// splitmix64's finalizer: a bijective mix of one 64-bit word.
constexpr std::uint64_t deterministic_mix(std::uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

// Folds one more word into a seed.
constexpr std::uint64_t deterministic_combine(std::uint64_t seed, std::uint64_t word) {
    return deterministic_mix(seed ^ word);
}

// True with `percent` in 100. 0 or 100 and above never roll: an unauthored
// possibility is stored as 0 and means always, as 100 does.
constexpr bool deterministic_roll_percent(std::uint64_t seed, std::uint32_t percent) {
    if (percent == 0u || percent >= 100u) {
        return true;
    }
    // The modulo bias over 2^64 is below 1e-17: nothing a game can measure.
    return (seed % 100u) < percent;
}

}  // namespace network_example
