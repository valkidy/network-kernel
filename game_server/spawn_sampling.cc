#include "game_server/spawn_sampling.h"

#include <cmath>

namespace network_example::game_server {
namespace {

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

std::uint64_t next_random(std::uint64_t* state) {
    *state += 0x9e3779b97f4a7c15ull;
    std::uint64_t value = *state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

float next_unit(std::uint64_t* state) {
    return static_cast<float>(
        static_cast<double>(next_random(state) >> 40) * (1.0 / 16777216.0));
}

std::uint32_t next_in_range(
    std::uint64_t* state,
    std::uint32_t low,
    std::uint32_t high) {
    if (high <= low) {
        return low;
    }
    return low + static_cast<std::uint32_t>(next_random(state) % (high - low + 1u));
}

KernelVec3 sample_area(
    const SpawnAreaConfig& area,
    const KernelVec3& origin,
    std::uint64_t* state) {
    const KernelVec3 center{
        origin.x + area.center.x,
        origin.y + area.center.y,
        origin.z + area.center.z,
    };
    if (area.shape == SpawnAreaShape::kRect) {
        return KernelVec3{
            center.x + (next_unit(state) * 2.0f - 1.0f) * area.half_extents.x,
            center.y,
            center.z + (next_unit(state) * 2.0f - 1.0f) * area.half_extents.z,
        };
    }
    // Uniform over the disc rather than over (radius, angle): sampling the
    // radius directly would crowd the middle.
    const float angle = next_unit(state) * 2.0f * kPi;
    const float radius = area.half_extents.x * std::sqrt(next_unit(state));
    return KernelVec3{
        center.x + std::cos(angle) * radius,
        center.y,
        center.z + std::sin(angle) * radius,
    };
}

std::string validate_spawn_area(const SpawnAreaConfig& area) {
    if (area.shape == SpawnAreaShape::kRect &&
        (area.half_extents.x <= 0.0f || area.half_extents.z <= 0.0f)) {
        return "rect area has no extent";
    }
    if (area.shape == SpawnAreaShape::kCircle && area.half_extents.x <= 0.0f) {
        return "circle area has no radius";
    }
    return {};
}

std::string validate_spawn_composition(
    const std::vector<SpawnCompositionEntry>& composition,
    std::uint32_t count_min,
    std::uint32_t count_max) {
    if (composition.empty()) {
        return "composition is empty";
    }
    if (count_min == 0 || count_max < count_min) {
        return "count range is empty";
    }
    std::uint32_t floor_total = 0;
    std::uint32_t ceiling_total = 0;
    for (const SpawnCompositionEntry& entry : composition) {
        if (entry.max_count < entry.min_count) {
            return "composition range is empty for " + entry.entity_template_ref;
        }
        floor_total += entry.min_count;
        ceiling_total += entry.max_count;
    }
    if (floor_total > count_min) {
        return "composition minimums exceed the smallest group size";
    }
    if (ceiling_total < count_max) {
        return "composition maximums cannot fill the largest group size";
    }
    return {};
}

std::vector<std::uint32_t> draw_spawn_composition(
    const std::vector<SpawnCompositionEntry>& composition,
    std::uint32_t count,
    std::uint64_t* random_state) {
    std::vector<std::uint32_t> drawn(composition.size(), 0u);
    std::uint32_t assigned = 0;
    for (std::size_t index = 0; index < composition.size(); ++index) {
        drawn[index] = composition[index].min_count;
        assigned += drawn[index];
    }
    // Weighted by the room each entry has left, which is the same thing as
    // dealing the remainder into the spare slots uniformly. See the header for
    // why uniform-among-entries is wrong.
    while (assigned < count) {
        std::uint32_t capacity_total = 0;
        for (std::size_t index = 0; index < drawn.size(); ++index) {
            capacity_total += composition[index].max_count - drawn[index];
        }
        if (capacity_total == 0) {
            break;
        }
        auto pick =
            static_cast<std::uint32_t>(next_random(random_state) % capacity_total);
        for (std::size_t index = 0; index < drawn.size(); ++index) {
            const std::uint32_t capacity =
                composition[index].max_count - drawn[index];
            if (pick < capacity) {
                ++drawn[index];
                break;
            }
            pick -= capacity;
        }
        ++assigned;
    }
    return drawn;
}

}  // namespace network_example::game_server
