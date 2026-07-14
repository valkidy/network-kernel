#ifndef APP_CLIENT_APP_H_
#define APP_CLIENT_APP_H_

#include <cstdint>

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

int RunClient(
    const char* address,
    const char* gameplay_catalog_path,
    const char* gameplay_catalog_cache_directory);

#endif  // APP_CLIENT_APP_H_
