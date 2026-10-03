#ifndef SIMULATION_PUBLIC_GROUND_FOLLOW_H_
#define SIMULATION_PUBLIC_GROUND_FOLLOW_H_

#include <cstdint>

#include <glm/glm.hpp>

#include "physics/public/physics_world.h"

namespace network_example::ground_follow {

// The motion of a field that rides the ground at a fixed height -- a tornado --
// stepped one tick at a time. It is the one solver both the authority and every
// client run, so it reads nothing but the static world it is handed and the
// state it is given: no registry, no clock, no randomness. Two callers handing
// it the same terrain and the same state get the same answer, which is the
// whole of what a locally predicted field has to go on, since nothing corrects
// it afterwards.
//
// It is path dependent, unlike projectile_position_at: where the field is now
// depends on every slope it crossed, so there is no closed form from the spawn
// point and the state has to be carried tick to tick.
struct Config {
    // Only x and z are read. The field's height comes from the ground, never
    // from its own velocity, so a vertical component here would be a second
    // answer to a question the probe already answers.
    glm::vec3 horizontal_velocity{0.0f};
    // From the ground to the field's centre.
    float hover_height = 1.0f;
    // The character controller's rule, and its default: ground whose normal is
    // within this many degrees of straight up is climbed or descended, anything
    // steeper stops the field.
    float max_slope_degrees = 50.0f;
    // How far above the field's current ground level the probe starts. This is
    // the tallest rise the field takes in one tick, so it has to cover the
    // steepest walkable climb over one tick of travel -- tan(max_slope) times
    // the step -- and anything shorter than it is stepped onto rather than
    // stopped at, the way step_height works for a character.
    float step_up = 0.5f;
    // How far below the current ground level the probe still counts as ground.
    // Ground further down than this is a cliff: the field holds its height and
    // carries on. It has to cover the steepest walkable descent over one tick,
    // or a walkable downhill reads as a cliff.
    float probe_depth = 0.5f;
    // The sphere swept along each step to find what stops the field. Zero
    // sweeps a ray. It must stay under hover_height, or the sweep grazes the
    // ground the field is riding.
    float sweep_radius = 0.0f;
    physics::CollisionQueryFilter filter{};
};

struct State {
    // The field's centre.
    glm::vec3 position{0.0f};
    // Set once something too steep to climb stopped it. A parked field stays
    // where it is and runs no query for the rest of its life.
    bool parked = false;
};

struct StepResult {
    // What it moved by this tick over the tick's length; zero once parked.
    glm::vec3 velocity{0.0f};
    // The probe found ground the field could stand over this tick.
    bool grounded = false;
    // This is the tick it parked on.
    bool stopped = false;
};

// Puts a field that has just spawned onto the ground beneath it: a hand-held
// launch does not start at hover height, and one spawned where a bottle broke
// starts on the ground itself. Looks from step_up above the spawn point to at
// most max_drop below it, and returns false, leaving the state alone, if there
// is no walkable ground in that span -- the field then simply holds its spawn
// height until the ground comes up to meet it.
bool settle(
    const physics::PhysicsWorld& physics_world,
    const Config& config,
    float max_drop,
    State* state);

// One tick of travel.
StepResult step(
    const physics::PhysicsWorld& physics_world,
    const Config& config,
    float fixed_delta_seconds,
    State* state);

}  // namespace network_example::ground_follow

#endif  // SIMULATION_PUBLIC_GROUND_FOLLOW_H_
