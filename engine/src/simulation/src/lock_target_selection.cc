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

}  // namespace network_example::lock_targets
