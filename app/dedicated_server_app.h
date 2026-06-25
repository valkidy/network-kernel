#ifndef APP_DEDICATED_SERVER_APP_H_
#define APP_DEDICATED_SERVER_APP_H_

#include <cstdint>

int RunDedicatedServer(
    std::uint16_t port,
    const char* gameplay_catalog_path,
    const char* gameplay_catalog_bundle_path,
    const char* gameplay_catalog_entry_path,
    const char* gameplay_catalog_content_namespace);

#endif  // APP_DEDICATED_SERVER_APP_H_
