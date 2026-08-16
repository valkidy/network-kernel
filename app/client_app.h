#ifndef APP_CLIENT_APP_H_
#define APP_CLIENT_APP_H_

#include <cmath>
#include <cstdint>

#include "kernel/public/kernel_types.h"

inline std::uint8_t& AppNetworkStatsModeStorage() {
    static std::uint8_t mode = 0u;
    return mode;
}

inline void SetAppNetworkStatsMode(std::uint8_t mode) {
    AppNetworkStatsModeStorage() = mode;
}

inline std::uint8_t GetAppNetworkStatsMode() {
    return AppNetworkStatsModeStorage();
}

inline float& AppAimPitchDegreesStorage() {
    static float degrees = 0.0f;
    return degrees;
}

inline void SetAppAimPitchDegrees(float degrees) {
    AppAimPitchDegreesStorage() = degrees;
}

inline KernelVec3 GetAppAimDirection() {
    constexpr float kDegreesToRadians =
        3.14159265358979323846f / 180.0f;
    const float pitch_radians =
        AppAimPitchDegreesStorage() * kDegreesToRadians;
    return KernelVec3{
        std::cos(pitch_radians),
        std::sin(pitch_radians),
        0.0f,
    };
}

int RunClient(
    const char* address,
    const char* gameplay_catalog_path,
    const char* gameplay_catalog_cache_directory);

#endif  // APP_CLIENT_APP_H_
