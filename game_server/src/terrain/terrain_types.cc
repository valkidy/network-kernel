#include "game_server/src/terrain/terrain_types.h"

#include <cmath>

namespace network_example::game_server::terrain {

std::uint64_t isqrt64(std::uint64_t v) {
    // IEEE 754 requires sqrt to be correctly rounded on every platform, so the
    // double estimate is the same everywhere; the integer fix-up then makes it
    // exactly floor(sqrt(v)). (A Newton loop gave the same answer ~10x slower,
    // and the cone pass calls this tens of millions of times on a 1 km map.)
    if (v == 0) {
        return 0;
    }
    std::uint64_t r = static_cast<std::uint64_t>(std::sqrt(static_cast<double>(v)));
    while (r > 0 && r > v / r) --r;                      // r * r > v
    while ((r + 1) <= v / (r + 1)) ++r;                  // (r + 1)^2 <= v
    return r;
}

std::int64_t inverse_lerp_q16(std::int64_t a, std::int64_t b, std::int64_t v) {
    if (v <= a) {
        return 0;
    }
    if (v >= b) {
        return kQ16;
    }
    return floor_div((v - a) * kQ16, b - a);
}

std::int64_t smoothstep_q16(std::int64_t t) {
    t = clamp64(t, 0, kQ16);
    const std::int64_t t2 = t * t / kQ16;            // t >= 0: plain division is floor
    return t2 * (3 * kQ16 - 2 * t) / kQ16;
}

void Fnv64::bytes(const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t k = 0; k < size; ++k) {
        state_ ^= p[k];
        state_ *= 0x100000001b3ull;
    }
}

void Fnv64::u32(std::uint32_t v) {
    const std::uint8_t b[4] = {
        static_cast<std::uint8_t>(v),
        static_cast<std::uint8_t>(v >> 8),
        static_cast<std::uint8_t>(v >> 16),
        static_cast<std::uint8_t>(v >> 24),
    };
    bytes(b, sizeof(b));
}

void Fnv64::i16(std::int16_t v) {
    const auto u = static_cast<std::uint16_t>(v);
    const std::uint8_t b[2] = {
        static_cast<std::uint8_t>(u),
        static_cast<std::uint8_t>(u >> 8),
    };
    bytes(b, sizeof(b));
}

std::uint64_t height_grid_hash(const HeightGrid& grid) {
    Fnv64 h;
    h.bytes("NKTH", 4);
    h.u32(1);
    h.u32(static_cast<std::uint32_t>(grid.samples_x));
    h.u32(static_cast<std::uint32_t>(grid.samples_z));
    for (const std::int16_t v : grid.cm) {
        h.i16(v);
    }
    return h.value();
}

}  // namespace network_example::game_server::terrain
