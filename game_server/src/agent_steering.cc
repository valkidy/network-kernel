#include "game_server/src/agent_steering.h"

#include <cmath>

namespace network_example::game_server::agent_steering {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 0.0001f;

KernelVec3 subtract(const KernelVec3& lhs, const KernelVec3& rhs) {
    return KernelVec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

float length_squared(const KernelVec3& value) {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

KernelQuat normalized(KernelQuat value) {
    const float length_squared =
        value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
    if (length_squared <= kEpsilon * kEpsilon) {
        return KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    return KernelQuat{
        value.x * inverse_length,
        value.y * inverse_length,
        value.z * inverse_length,
        value.w * inverse_length,
    };
}

KernelQuat multiply(const KernelQuat& lhs, const KernelQuat& rhs) {
    return normalized(KernelQuat{
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
    });
}

}  // namespace

KernelVec3 zero_vec3() {
    return KernelVec3{0.0f, 0.0f, 0.0f};
}

KernelQuat yaw_rotation(float yaw_radians) {
    const float half_yaw = yaw_radians * 0.5f;
    return KernelQuat{0.0f, -std::sin(half_yaw), 0.0f, std::cos(half_yaw)};
}

KernelQuat apply_yaw_delta(const KernelQuat& rotation, float delta_degrees) {
    const float yaw_radians = delta_degrees * (kPi / 180.0f);
    return multiply(yaw_rotation(yaw_radians), rotation);
}

bool facing_rotation_toward(
    const KernelVec3& from,
    const KernelVec3& to,
    KernelQuat* out_rotation) {
    if (out_rotation == nullptr) {
        return false;
    }
    KernelVec3 delta = subtract(to, from);
    delta.y = 0.0f;
    if (length_squared(delta) <= kEpsilon * kEpsilon) {
        return false;
    }
    *out_rotation = yaw_rotation(std::atan2(delta.z, delta.x));
    return true;
}

bool facing_rotation_from_vision_toward(
    const KernelVec3& from,
    const KernelVec3& to,
    const KernelVec3& current_forward,
    const KernelQuat& current_rotation,
    KernelQuat* out_rotation) {
    if (out_rotation == nullptr) {
        return false;
    }
    KernelVec3 target_delta = subtract(to, from);
    target_delta.y = 0.0f;
    KernelVec3 forward = current_forward;
    forward.y = 0.0f;
    if (length_squared(target_delta) <= kEpsilon * kEpsilon ||
        length_squared(forward) <= kEpsilon * kEpsilon) {
        return facing_rotation_toward(from, to, out_rotation);
    }
    const float target_yaw = std::atan2(target_delta.z, target_delta.x);
    const float forward_yaw = std::atan2(forward.z, forward.x);
    const float delta_degrees = (target_yaw - forward_yaw) * (180.0f / kPi);
    *out_rotation = apply_yaw_delta(current_rotation, delta_degrees);
    return true;
}

void transition_to(AgentRuntimeState* agent, AgentSentryState state) {
    if (agent == nullptr || agent->sentry.state == state) {
        return;
    }
    agent->sentry.state = state;
    agent->sentry.state_ticks = 0;
    agent->sentry.lost_target_ticks = 0;
    agent->sentry.patrol_rotation_tick = 0;
    agent->sentry.patrol_rotation_step = 0;
}

bool update_patrol_facing(
    std::uint32_t rotation_interval_ticks,
    float rotation_min_degrees,
    float rotation_max_degrees,
    AgentRuntimeState* agent,
    const KernelQuat& current_rotation,
    KernelQuat* out_rotation) {
    if (agent == nullptr || out_rotation == nullptr ||
        rotation_interval_ticks == 0 || rotation_min_degrees <= 0.0f ||
        rotation_max_degrees < rotation_min_degrees) {
        return false;
    }
    ++agent->sentry.patrol_rotation_tick;
    if (agent->sentry.patrol_rotation_tick < rotation_interval_ticks) {
        return false;
    }
    agent->sentry.patrol_rotation_tick = 0;
    ++agent->sentry.patrol_rotation_step;
    std::uint32_t random_bits =
        agent->net_id * 747796405u + agent->sentry.patrol_rotation_step * 2891336453u;
    random_bits ^= random_bits >> 16u;
    random_bits *= 2246822519u;
    random_bits ^= random_bits >> 13u;
    const bool positive_direction = (random_bits & 1u) == 0u;
    const float unit =
        static_cast<float>((random_bits >> 1u) & 0xffffu) / 65535.0f;
    const float magnitude =
        rotation_min_degrees +
        (rotation_max_degrees - rotation_min_degrees) * unit;
    const float delta_degrees = positive_direction ? magnitude : -magnitude;
    *out_rotation = apply_yaw_delta(current_rotation, delta_degrees);
    return true;
}

float horizontal_distance(const KernelVec3& from, const KernelVec3& to) {
    const float delta_x = to.x - from.x;
    const float delta_z = to.z - from.z;
    return std::sqrt(delta_x * delta_x + delta_z * delta_z);
}

KernelVec2 horizontal_move_toward(
    const KernelVec3& from,
    const KernelVec3& to,
    float magnitude) {
    const float delta_x = to.x - from.x;
    const float delta_z = to.z - from.z;
    const float distance = std::sqrt(delta_x * delta_x + delta_z * delta_z);
    if (distance <= kEpsilon) {
        return KernelVec2{0.0f, 0.0f};
    }
    const float scale = magnitude / distance;
    return KernelVec2{delta_x * scale, delta_z * scale};
}

}  // namespace network_example::game_server::agent_steering
