#ifndef APP_HOST_SERVER_APP_H_
#define APP_HOST_SERVER_APP_H_

#include <cstdint>

int RunHostServer(
    std::uint16_t port,
    const char* gameplay_catalog_path,
    const char* gameplay_catalog_bundle_path,
    const char* gameplay_catalog_entry_path,
    const char* gameplay_catalog_content_namespace,
    std::uint32_t frame_count);

#endif  // APP_HOST_SERVER_APP_H_
