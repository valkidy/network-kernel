#ifndef SIMULATION_PUBLIC_LOCK_TARGET_SELECTION_H_
#define SIMULATION_PUBLIC_LOCK_TARGET_SELECTION_H_

#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

namespace network_example::lock_targets {

// Which actors a lock-on weapon locks onto when it commits. The authority runs
// it on the rewound frame to decide who is hurt; every client runs it on what
// it draws to decide where the arcs go. Both have to agree on the rule, not on
// the inputs -- the client's view is the server's past -- so it reads nothing
// but what it is handed: no registry, no clock.
//
// Who may be a candidate at all (alive, an actor, on a side the weapon hits,
// not the shooter) is the caller's question, because the two callers answer it
// from different places. This decides only which of them are locked.
struct Candidate {
    std::uint32_t net_id = 0;
    // Where the target is tested: its hitbox centre. Not the nearest point on
    // its surface -- a client knows only the centre of what it draws.
    glm::vec3 center{0.0f};
};

struct Cone {
    glm::vec3 origin{0.0f};
    // Need not be normalised. A zero aim locks nothing.
    glm::vec3 aim{0.0f, 0.0f, 1.0f};
    float range = 0.0f;
    // The full angle of the cone, measured in 3D: a target straight overhead
    // is outside a cone aimed at the horizon, unlike a melee swing's, which is
    // flattened onto the ground plane.
    float fov_degrees = 0.0f;
    std::uint32_t max_targets = 1;
    // A sphere of radius `range` instead of a cone: every direction is in, so
    // neither aim nor fov_degrees is read. An action graph's lock_strike may
    // lock so where it has no heading worth aiming along -- a bottle that
    // broke on the ground.
    bool omnidirectional = false;
};

// True when something static stands between `from` and `to`. Asked only of
// targets that would otherwise be locked, nearest first, so a blocked one is
// replaced by the next nearest rather than costing every candidate a ray.
using LineBlocked =
    std::function<bool(const glm::vec3& from, const glm::vec3& to)>;

// At most cone.max_targets candidates, nearest first, ties broken by net id.
// A candidate is in the cone when its centre is within range and within half
// the fov of the aim; in an omnidirectional one, within range alone.
// `line_blocked` may be empty, meaning nothing blocks.
std::vector<Candidate> select(
    std::vector<Candidate> candidates,
    const Cone& cone,
    const LineBlocked& line_blocked);

// Where the index-th unfilled arc of a lock ends, when fewer than
// cone.max_targets were locked: a point inside the cone, at 40% to 100% of its
// range -- in any direction when the cone is omnidirectional. Purely presentation -- the server never asks -- but a function of
// the shot alone (shooter, action instance, commit, arc index), not of any
// clock or random state, so every client draws the same spray for the same
// shot from the same aim.
glm::vec3 unfilled_arc_end(
    const Cone& cone,
    std::uint32_t shooter_net_id,
    std::uint32_t action_instance_id,
    std::uint16_t commit_index,
    std::uint32_t arc_index);

}  // namespace network_example::lock_targets

#endif  // SIMULATION_PUBLIC_LOCK_TARGET_SELECTION_H_
