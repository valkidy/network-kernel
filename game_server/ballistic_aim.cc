#include "game_server/ballistic_aim.h"

#include <algorithm>
#include <cmath>

namespace network_example::game_server {
namespace {

constexpr float kDirectionEpsilon = 0.0001f;

KernelVec3 add(const KernelVec3& lhs, const KernelVec3& rhs) {
    return KernelVec3{lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

KernelVec3 subtract(const KernelVec3& lhs, const KernelVec3& rhs) {
    return KernelVec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

KernelVec3 scale(const KernelVec3& value, float scalar) {
    return KernelVec3{value.x * scalar, value.y * scalar, value.z * scalar};
}

float dot(const KernelVec3& lhs, const KernelVec3& rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

float length_squared(const KernelVec3& value) {
    return dot(value, value);
}

bool finite(const KernelVec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

std::optional<BallisticAimSolution> vertical_solution(
    const KernelVec3& up,
    float height,
    float speed,
    float gravity_magnitude,
    float max_flight_seconds) {
    if (std::fabs(height) <= kDirectionEpsilon) {
        return std::nullopt;
    }
    const float discriminant =
        speed * speed - 2.0f * gravity_magnitude * height;
    if (discriminant < 0.0f) {
        return std::nullopt;
    }
    const float root = std::sqrt(discriminant);
    const bool target_is_above = height > 0.0f;
    const float flight_seconds =
        target_is_above ? (2.0f * height) / (speed + root)
                        : (-speed + root) / gravity_magnitude;
    if (!std::isfinite(flight_seconds) || flight_seconds <= 0.0f ||
        flight_seconds > max_flight_seconds) {
        return std::nullopt;
    }
    return BallisticAimSolution{
        scale(up, target_is_above ? 1.0f : -1.0f),
        flight_seconds,
    };
}

}  // namespace

std::optional<BallisticAimSolution> solve_low_ballistic_aim(
    const KernelVec3& launch_position,
    const KernelVec3& target_position,
    float speed,
    const KernelVec3& gravity,
    float max_flight_seconds) {
    if (!finite(launch_position) || !finite(target_position) ||
        !finite(gravity) || !std::isfinite(speed) || speed <= 0.0f ||
        !std::isfinite(max_flight_seconds) || max_flight_seconds <= 0.0f) {
        return std::nullopt;
    }

    const float gravity_squared = length_squared(gravity);
    if (gravity_squared <= kDirectionEpsilon * kDirectionEpsilon) {
        return std::nullopt;
    }
    const float gravity_magnitude = std::sqrt(gravity_squared);
    const KernelVec3 up = scale(gravity, -1.0f / gravity_magnitude);
    const KernelVec3 displacement = subtract(target_position, launch_position);
    const float height = dot(displacement, up);
    const KernelVec3 horizontal = subtract(displacement, scale(up, height));
    const float horizontal_squared = length_squared(horizontal);
    if (horizontal_squared <= kDirectionEpsilon * kDirectionEpsilon) {
        return vertical_solution(
            up,
            height,
            speed,
            gravity_magnitude,
            max_flight_seconds);
    }

    const float horizontal_distance = std::sqrt(horizontal_squared);
    const float speed_squared = speed * speed;
    const float speed_fourth = speed_squared * speed_squared;
    float discriminant = speed_fourth -
        gravity_magnitude *
            (gravity_magnitude * horizontal_squared +
             2.0f * height * speed_squared);
    const float discriminant_scale = std::max(
        1.0f,
        std::max(
            speed_fourth,
            std::fabs(
                gravity_magnitude *
                (gravity_magnitude * horizontal_squared +
                 2.0f * height * speed_squared))));
    if (discriminant < -0.00001f * discriminant_scale) {
        return std::nullopt;
    }
    discriminant = std::max(0.0f, discriminant);

    const float discriminant_root = std::sqrt(discriminant);
    const float tangent =
        (gravity_magnitude * horizontal_squared +
         2.0f * height * speed_squared) /
        (horizontal_distance * (speed_squared + discriminant_root));
    const float cosine = 1.0f / std::sqrt(1.0f + tangent * tangent);
    const float sine = tangent * cosine;
    const float flight_seconds = horizontal_distance / (speed * cosine);
    if (!std::isfinite(flight_seconds) || flight_seconds <= 0.0f ||
        flight_seconds > max_flight_seconds) {
        return std::nullopt;
    }

    const KernelVec3 horizontal_direction =
        scale(horizontal, 1.0f / horizontal_distance);
    return BallisticAimSolution{
        add(scale(horizontal_direction, cosine), scale(up, sine)),
        flight_seconds,
    };
}

}  // namespace network_example::game_server
