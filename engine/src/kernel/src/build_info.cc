#include "kernel/src/build_info.h"

#include <cstdio>

#include "protocol/public/packet_header.h"

#if __has_include("kernel/src/build_info_macros.h")
#include "kernel/src/build_info_macros.h"
#endif

#define NETWORK_EXAMPLE_STRINGIFY_DETAIL(value) #value
#define NETWORK_EXAMPLE_STRINGIFY(value) NETWORK_EXAMPLE_STRINGIFY_DETAIL(value)

#ifndef NETWORK_KERNEL_MODULE_VERSION
#define NETWORK_KERNEL_MODULE_VERSION "0.0.0-dev"
#endif

#ifndef NETWORK_KERNEL_GIT_COMMIT
#define NETWORK_KERNEL_GIT_COMMIT "unknown"
#endif

#ifndef NETWORK_KERNEL_BUILD_TIMESTAMP
#define NETWORK_KERNEL_BUILD_TIMESTAMP "unknown"
#endif

namespace network_example {
namespace {

#if defined(_WIN32)
constexpr const char* kModuleFileName = "network_kernel.dll";
constexpr const char* kBuildPlatform = "windows";
#elif defined(__APPLE__)
constexpr const char* kModuleFileName = "libnetwork_kernel.dylib";
constexpr const char* kBuildPlatform = "macos";
#elif defined(__linux__)
constexpr const char* kModuleFileName = "libnetwork_kernel.so";
constexpr const char* kBuildPlatform = "linux";
#else
constexpr const char* kModuleFileName = "network_kernel";
constexpr const char* kBuildPlatform = "unknown";
#endif

#if defined(NDEBUG)
constexpr const char* kBuildConfig = "release";
#else
constexpr const char* kBuildConfig = "debug";
#endif

#if defined(__clang__)
constexpr const char* kCompilerInfo = "clang " __clang_version__;
#elif defined(__GNUC__)
constexpr const char* kCompilerInfo = "gcc " __VERSION__;
#elif defined(_MSC_VER)
constexpr const char* kCompilerInfo = "msvc " NETWORK_EXAMPLE_STRINGIFY(_MSC_VER);
#else
constexpr const char* kCompilerInfo = "unknown";
#endif

template <std::size_t Size>
void copy_text(char (&destination)[Size], const char* source) {
    std::snprintf(destination, Size, "%s", source == nullptr ? "unknown" : source);
}

}  // namespace

KernelBuildInfo current_build_info() {
    KernelBuildInfo info{};
    info.struct_size = sizeof(KernelBuildInfo);
    copy_text(info.module_name, "network_kernel");
    copy_text(info.module_file_name, kModuleFileName);
    copy_text(info.module_version, NETWORK_KERNEL_MODULE_VERSION);
    info.protocol_version = kProtocolVersion;
    info.snapshot_schema_version = kSnapshotSchemaVersion;
    info.packet_schema_version = kPacketSchemaVersion;
    copy_text(info.git_commit, NETWORK_KERNEL_GIT_COMMIT);
    copy_text(info.build_timestamp, NETWORK_KERNEL_BUILD_TIMESTAMP);
    copy_text(info.build_platform, kBuildPlatform);
    copy_text(info.build_config, kBuildConfig);
    copy_text(info.compiler_info, kCompilerInfo);
    return info;
}

}  // namespace network_example
