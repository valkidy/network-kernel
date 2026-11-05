// Shared types for the kernel's random terrain (docs/KERNEL_TERRAIN_GENERATION_PLAN.md,
// docs/KERNEL_TERRAIN_IMPLEMENTATION_PLAN.md P1).
//
// Everything that runs on both client and server -- reading the assets,
// composing a recipe into a heightfield, hashing it -- must give bit-identical
// results on every platform (macOS arm64, Linux x64, Windows x64). The rules
// (plan §3.4), checked in review:
//
//   - no float or double in a computed result; fixed-width integers only
//     (int16_t / int32_t / int64_t / uint32_t / uint64_t);
//   - no `long` or `size_t` in a result (Windows long is 32-bit);
//   - no <random> distributions; noise and seeds use the integer hash below;
//   - no iteration over unordered containers, no std::hash;
//   - sorts on a total-order key (or std::stable_sort);
//   - no right shift of a negative value and no signed overflow: use
//     floor_div() for every division that can see a negative numerator;
//   - byte order written out explicitly (little-endian) wherever bytes are
//     read, written or hashed.
//
// Units: lengths in millimetres (int32), heights in the stored grid in
// centimetres (int16), fractions in Q16 (65536 = 1.0). Grids are row-major
// with row j at z = j * 0.3 m and column i at x = i * 0.3 m, measured from the
// map's low corner (the world-origin shift happens only when a result leaves
// the terrain code).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace network_example::game_server::terrain {

inline constexpr std::int32_t kCellMm = 300;        // heightfield spacing
inline constexpr std::int32_t kGridMm = 12000;      // placement grid
inline constexpr std::int32_t kCellsPerGrid = kGridMm / kCellMm;  // 40
inline constexpr std::int32_t kGroundMapSpacingMm = 1200;
inline constexpr std::int32_t kCellsPerGroundSample = kGroundMapSpacingMm / kCellMm;  // 4
inline constexpr std::int64_t kQ16 = 65536;

// Floor of a / b for b > 0, for any sign of a.
constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

// a / b rounded half up, for b > 0.
constexpr std::int64_t round_div(std::int64_t a, std::int64_t b) {
    return floor_div(a + b / 2, b);
}

constexpr std::int64_t clamp64(std::int64_t v, std::int64_t lo, std::int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// floor(sqrt(v)).
std::uint64_t isqrt64(std::uint64_t v);

// Q16 fraction of the way v lies from a to b, clamped to [0, 1]; a < b.
std::int64_t inverse_lerp_q16(std::int64_t a, std::int64_t b, std::int64_t v);

// Mathf.SmoothStep(0, 1, t) for t in Q16 (clamped): 3t^2 - 2t^3.
std::int64_t smoothstep_q16(std::int64_t t);

// FNV-1a 64 over bytes, fed incrementally.
class Fnv64 {
public:
    void bytes(const void* data, std::size_t size);
    void u32(std::uint32_t v);
    void i16(std::int16_t v);
    std::uint64_t value() const { return state_; }

private:
    std::uint64_t state_ = 0xcbf29ce484222325ull;
};

// A heightfield on the 0.3 m grid: samples_x * samples_z heights in cm.
struct HeightGrid {
    std::int32_t samples_x = 0;
    std::int32_t samples_z = 0;
    std::vector<std::int16_t> cm;

    std::int16_t at(std::int32_t i, std::int32_t j) const {
        return cm[static_cast<std::size_t>(j) * static_cast<std::size_t>(samples_x) +
                  static_cast<std::size_t>(i)];
    }
};

// The hash both ends compare after composing (plan §3.4): "NKTH", version,
// sizes and every sample, little-endian.
std::uint64_t height_grid_hash(const HeightGrid& grid);

}  // namespace network_example::game_server::terrain
