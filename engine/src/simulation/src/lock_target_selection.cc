#include "simulation/public/lock_target_selection.h"

#include <algorithm>
#include <cmath>

namespace network_example::lock_targets {

std::vector<Candidate> select(
    std::vector<Candidate> candidates,
    const Cone& cone,
    const LineBlocked& line_blocked) {
    std::vector<Candidate> locked;
    const float aim_length = glm::length(cone.aim);
    if (cone.max_targets == 0u || cone.range <= 0.0f ||
        cone.fov_degrees <= 0.0f || aim_length <= 0.000001f) {
        return locked;
    }
    const glm::vec3 aim = cone.aim / aim_length;
    const float half_fov_cos = std::cos(
        cone.fov_degrees * 0.5f * (std::acos(-1.0f) / 180.0f));

    struct InCone {
        Candidate candidate;
        float distance = 0.0f;
    };
    std::vector<InCone> in_cone;
    in_cone.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        const glm::vec3 offset = candidate.center - cone.origin;
        const float distance = glm::length(offset);
        // A target the shooter is standing inside has no direction to test.
        if (distance <= 0.0001f || distance > cone.range) {
            continue;
        }
        if (glm::dot(aim, offset / distance) < half_fov_cos) {
            continue;
        }
        in_cone.push_back(InCone{candidate, distance});
    }
    std::sort(
        in_cone.begin(),
        in_cone.end(),
        [](const InCone& lhs, const InCone& rhs) {
            if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
            }
            return lhs.candidate.net_id < rhs.candidate.net_id;
        });

    for (const InCone& entry : in_cone) {
        if (locked.size() >= cone.max_targets) {
            break;
        }
        if (line_blocked && line_blocked(cone.origin, entry.candidate.center)) {
            continue;
        }
        locked.push_back(entry.candidate);
    }
    return locked;
}

namespace {

std::uint64_t mix(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

// [0, 1), from the top 24 bits: exact in a float.
float unit(std::uint64_t bits) {
    return static_cast<float>(bits >> 40) / 16777216.0f;
}

}  // namespace

glm::vec3 unfilled_arc_end(
    const Cone& cone,
    std::uint32_t shooter_net_id,
    std::uint32_t action_instance_id,
    std::uint16_t commit_index,
    std::uint32_t arc_index) {
    const float aim_length = glm::length(cone.aim);
    if (aim_length <= 0.000001f || cone.range <= 0.0f) {
        return cone.origin;
    }
    const glm::vec3 forward = cone.aim / aim_length;
    const glm::vec3 helper = std::fabs(forward.y) < 0.99f
        ? glm::vec3{0.0f, 1.0f, 0.0f}
        : glm::vec3{1.0f, 0.0f, 0.0f};
    const glm::vec3 right = glm::normalize(glm::cross(forward, helper));
    const glm::vec3 up = glm::cross(right, forward);

    std::uint64_t seed = mix(
        (static_cast<std::uint64_t>(shooter_net_id) << 32) | action_instance_id);
    seed = mix(seed ^ ((static_cast<std::uint64_t>(commit_index) << 32) | arc_index));
    const float spread = unit(seed);
    const float around = unit(mix(seed + 1u));
    const float reach = unit(mix(seed + 2u));

    const float pi = std::acos(-1.0f);
    // Uniform over the cone's cross-section, not bunched at its axis.
    const float half_fov = cone.fov_degrees * 0.5f * (pi / 180.0f);
    const float tilt = half_fov * std::sqrt(spread);
    const float turn = 2.0f * pi * around;
    const glm::vec3 direction = glm::normalize(
        forward * std::cos(tilt) +
        (right * std::cos(turn) + up * std::sin(turn)) * std::sin(tilt));
    return cone.origin + direction * (cone.range * (0.4f + 0.6f * reach));
}

}  // namespace network_example::lock_targets
