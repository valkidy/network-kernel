// lock_targets::select picks what a lock-on weapon locks: candidates inside a
// 3D cone, nearest first, ties by net id, at most max_targets, and a blocked
// target gives its place to the next nearest.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <vector>

#include <glm/glm.hpp>

#include "simulation/public/lock_target_selection.h"

namespace {

using network_example::lock_targets::Candidate;
using network_example::lock_targets::Cone;
using network_example::lock_targets::select;

void require(
    bool condition,
    const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(
            stderr,
            "require failed at %s:%u\n",
            location.file_name(),
            location.line());
        std::abort();
    }
}

// Aimed down +z from the origin: 20 m reach, 60 degrees wide, up to 3 targets.
Cone forward_cone() {
    Cone cone;
    cone.origin = glm::vec3{0.0f};
    cone.aim = glm::vec3{0.0f, 0.0f, 1.0f};
    cone.range = 20.0f;
    cone.fov_degrees = 60.0f;
    cone.max_targets = 3;
    return cone;
}

std::vector<std::uint32_t> ids(const std::vector<Candidate>& locked) {
    std::vector<std::uint32_t> out;
    for (const Candidate& candidate : locked) {
        out.push_back(candidate.net_id);
    }
    return out;
}

void picks_nearest_first_and_caps_at_max_targets() {
    const std::vector<Candidate> candidates{
        {1, {0.0f, 0.0f, 15.0f}},
        {2, {0.0f, 0.0f, 5.0f}},
        {3, {1.0f, 0.0f, 10.0f}},
        {4, {0.0f, 0.0f, 12.0f}},
    };
    const std::vector<std::uint32_t> locked =
        ids(select(candidates, forward_cone(), {}));
    require((locked == std::vector<std::uint32_t>{2, 3, 4}));
}

void range_and_angle_bound_the_cone() {
    // 25 m is past the reach. 40 degrees off the aim is past the half-angle.
    // Straight overhead is outside a cone aimed at the horizon: the angle is
    // measured in 3D, not on the ground plane the way a melee swing's is.
    const glm::vec3 forty_degrees{
        std::sin(glm::radians(40.0f)) * 10.0f,
        0.0f,
        std::cos(glm::radians(40.0f)) * 10.0f};
    const glm::vec3 twenty_degrees{
        std::sin(glm::radians(20.0f)) * 10.0f,
        0.0f,
        std::cos(glm::radians(20.0f)) * 10.0f};
    const std::vector<Candidate> candidates{
        {1, {0.0f, 0.0f, 25.0f}},
        {2, forty_degrees},
        {3, {0.0f, 10.0f, 0.5f}},
        {4, twenty_degrees},
        // Standing inside the shooter: no direction to test.
        {5, {0.0f, 0.0f, 0.0f}},
    };
    const std::vector<std::uint32_t> locked =
        ids(select(candidates, forward_cone(), {}));
    require((locked == std::vector<std::uint32_t>{4}));
}

void equal_distances_break_ties_by_net_id() {
    const std::vector<Candidate> candidates{
        {9, {1.0f, 0.0f, 8.0f}},
        {7, {-1.0f, 0.0f, 8.0f}},
        {8, {0.0f, 1.0f, 8.0f}},
        {6, {0.0f, -1.0f, 8.0f}},
    };
    const std::vector<std::uint32_t> locked =
        ids(select(candidates, forward_cone(), {}));
    require((locked == std::vector<std::uint32_t>{6, 7, 8}));
}

void a_blocked_target_is_replaced_by_the_next_nearest() {
    const std::vector<Candidate> candidates{
        {1, {0.0f, 0.0f, 4.0f}},
        {2, {0.0f, 0.0f, 6.0f}},
        {3, {0.0f, 0.0f, 8.0f}},
        {4, {0.0f, 0.0f, 10.0f}},
    };
    std::vector<std::uint32_t> asked;
    const auto behind_a_wall = [&](const glm::vec3&, const glm::vec3& to) {
        asked.push_back(static_cast<std::uint32_t>(to.z));
        return to.z == 6.0f;
    };
    const std::vector<std::uint32_t> locked =
        ids(select(candidates, forward_cone(), behind_a_wall));
    require((locked == std::vector<std::uint32_t>{1, 3, 4}));
    // Only the ones that would be locked are asked, nearest first.
    require((asked == std::vector<std::uint32_t>{4, 6, 8, 10}));
}

void nothing_is_locked_by_a_degenerate_cone() {
    const std::vector<Candidate> candidates{{1, {0.0f, 0.0f, 5.0f}}};
    Cone no_aim = forward_cone();
    no_aim.aim = glm::vec3{0.0f};
    require(select(candidates, no_aim, {}).empty());
    Cone no_targets = forward_cone();
    no_targets.max_targets = 0;
    require(select(candidates, no_targets, {}).empty());
    // An unnormalised aim is the same aim.
    Cone long_aim = forward_cone();
    long_aim.aim = glm::vec3{0.0f, 0.0f, 7.0f};
    require(ids(select(candidates, long_aim, {})) ==
            std::vector<std::uint32_t>{1});
}

}  // namespace

int main() {
    picks_nearest_first_and_caps_at_max_targets();
    range_and_angle_bound_the_cone();
    equal_distances_break_ties_by_net_id();
    a_blocked_target_is_replaced_by_the_next_nearest();
    nothing_is_locked_by_a_degenerate_cone();
    std::puts("lock_target_selection_test passed");
    return 0;
}
