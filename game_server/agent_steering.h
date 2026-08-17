#ifndef GAME_SERVER_AGENT_STEERING_H_
#define GAME_SERVER_AGENT_STEERING_H_

#include <cstdint>

#include "game_server/agent_runtime.h"
#include "kernel/public/kernel_types.h"

// Facing and steering math shared by the agent controllers. Everything here is
// pure: it reads perception and returns a rotation or a move vector, and never
// touches the kernel.
namespace network_example::game_server::agent_steering {

KernelVec3 zero_vec3();

KernelQuat yaw_rotation(float yaw_radians);

KernelQuat apply_yaw_delta(const KernelQuat& rotation, float delta_degrees);

// Yaw that points straight at `to`. False when the two positions share a
// horizontal location and no facing can be derived.
bool facing_rotation_toward(
    const KernelVec3& from,
    const KernelVec3& to,
    KernelQuat* out_rotation);

// Same, but expressed as a delta from where the vision cone currently looks, so
// the authored `local_forward` offset survives the turn. Falls back to
// `facing_rotation_toward` when the forward vector is degenerate.
bool facing_rotation_from_vision_toward(
    const KernelVec3& from,
    const KernelVec3& to,
    const KernelVec3& current_forward,
    const KernelQuat& current_rotation,
    KernelQuat* out_rotation);

void transition_to(AgentRuntimeState* agent, AgentSentryState state);

// Advances the idle look-around timer and, on the tick it fires, writes the
// next patrol facing. False on every other tick.
bool update_patrol_facing(
    std::uint32_t rotation_interval_ticks,
    float rotation_min_degrees,
    float rotation_max_degrees,
    AgentRuntimeState* agent,
    const KernelQuat& current_rotation,
    KernelQuat* out_rotation);

// Horizontal (XZ) distance; vertical separation is ignored so that standing on
// a slope does not read as being far away.
float horizontal_distance(const KernelVec3& from, const KernelVec3& to);

// Unit XZ move vector pointing at `to`, in the world-space frame
// `KernelPlayerInput::move` uses. Zero when the two positions coincide.
KernelVec2 horizontal_move_toward(
    const KernelVec3& from,
    const KernelVec3& to,
    float magnitude);

}  // namespace network_example::game_server::agent_steering

#endif  // GAME_SERVER_AGENT_STEERING_H_
