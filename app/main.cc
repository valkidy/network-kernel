#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "client_app.h"
#include "dedicated_server_app.h"
#include "host_server_app.h"
#include "kernel/public/kernel_types.h"

namespace {

constexpr std::uint16_t kDefaultPort = 7777;
constexpr std::string_view kDefaultAddress = "127.0.0.1:7777";
constexpr std::string_view kDefaultGameplayCatalog =
    "game_server/gameplay_catalog.yaml";
constexpr std::string_view kDefaultGameplayCatalogEntry = "gameplay_catalog.yaml";

struct Options {
    std::string mode;
    std::string address{std::string(kDefaultAddress)};
    std::string gameplay_catalog{std::string(kDefaultGameplayCatalog)};
    std::string gameplay_catalog_bundle;
    std::string gameplay_catalog_entry{std::string(kDefaultGameplayCatalogEntry)};
    std::string gameplay_catalog_content_namespace{"default"};
    std::string gameplay_catalog_cache_directory;
    std::uint16_t port = kDefaultPort;
    std::uint32_t host_frames = 12;
    std::uint8_t network_stats_mode = 0u;
    std::uint32_t physics_simulation = 0;
    std::uint32_t physics_workers = 0;
    std::uint32_t actor_blocking = KernelActorBlockingMode_Predicted;
    bool actor_blocking_explicit = false;
    bool gameplay_catalog_explicit = false;
};

void print_usage() {
    spdlog::error(
        "usage: //app:app -- --mode=<dedicated_server|client|host_server> "
        "[--address=127.0.0.1:7777] [--port=7777] "
        "[--gameplay-catalog=game_server/gameplay_catalog.yaml] "
        "[--gameplay-catalog-bundle=path/to/bundle.zip] "
        "[--gameplay-catalog-entry=gameplay_catalog.yaml] "
        "[--catalog-content-namespace=default] "
        "[--catalog-cache-dir=path] [--host-frames=12] "
        "[--network-stats=off|basic|detailed] "
        "[--physics_simulation=0|1] [--physics-workers=0|N] "
        "[--actor-blocking=0|1] "
        "(0=disabled, 1=predicted; server default=1; predicted clients "
        "require --catalog-cache-dir)");
}

bool parse_port(std::string_view text, std::uint16_t* out_port) {
    unsigned int value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end || value == 0 || value > 65535) {
        return false;
    }

    *out_port = static_cast<std::uint16_t>(value);
    return true;
}

bool parse_u32(std::string_view text, std::uint32_t* out_value) {
    unsigned int value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end || value == 0) {
        return false;
    }
    *out_value = value;
    return true;
}

bool parse_u32_allow_zero(
    std::string_view text,
    std::uint32_t* out_value) {
    std::uint32_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end || begin == end) {
        return false;
    }
    *out_value = value;
    return true;
}

bool read_value(
    std::string_view arg,
    std::string_view name,
    int* index,
    int argc,
    char** argv,
    std::string* out_value) {
    const std::string prefix = std::string(name) + "=";
    if (arg == name) {
        if (*index + 1 >= argc) {
            return false;
        }
        *out_value = argv[++(*index)];
        return true;
    }
    if (arg.starts_with(prefix)) {
        *out_value = std::string(arg.substr(prefix.size()));
        return true;
    }
    return false;
}

bool path_is_regular_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

void append_runfiles_bundle_candidates(
    const std::filesystem::path& runfiles_root,
    std::vector<std::filesystem::path>* candidates) {
    if (runfiles_root.empty() || candidates == nullptr) {
        return;
    }
    candidates->push_back(
        runfiles_root / "network-example" / "game_server" /
        "gameplay_catalog_bundle" / "bundle.zip");
    candidates->push_back(
        runfiles_root / "_main" / "game_server" /
        "gameplay_catalog_bundle" / "bundle.zip");
}

std::string find_default_gameplay_catalog_bundle(const char* argv0) {
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(
        "game_server/gameplay_catalog_bundle/bundle.zip");
    candidates.push_back(
        "bazel-bin/game_server/gameplay_catalog_bundle/bundle.zip");

    const char* runfiles_dir = std::getenv("RUNFILES_DIR");
    if (runfiles_dir != nullptr && runfiles_dir[0] != '\0') {
        append_runfiles_bundle_candidates(runfiles_dir, &candidates);
    }

    if (argv0 != nullptr && argv0[0] != '\0') {
        append_runfiles_bundle_candidates(
            std::filesystem::path(std::string(argv0) + ".runfiles"),
            &candidates);
    }

    for (const std::filesystem::path& candidate : candidates) {
        if (path_is_regular_file(candidate)) {
            return candidate.string();
        }
    }
    return {};
}

bool parse_args(int argc, char** argv, Options* options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        std::string value;
        if (arg == "--help" || arg == "-h") {
            return false;
        }
        if (read_value(arg, "--mode", &index, argc, argv, &value)) {
            options->mode = value;
            continue;
        }
        if (read_value(arg, "--address", &index, argc, argv, &value)) {
            options->address = value;
            continue;
        }
        if (read_value(arg, "--port", &index, argc, argv, &value)) {
            if (!parse_port(value, &options->port)) {
                spdlog::error("invalid port: {}", value);
                return false;
            }
            continue;
        }
        if (read_value(arg, "--gameplay-catalog", &index, argc, argv, &value)) {
            options->gameplay_catalog = value;
            options->gameplay_catalog_explicit = true;
            continue;
        }
        if (read_value(arg, "--gameplay-catalog-bundle", &index, argc, argv, &value)) {
            options->gameplay_catalog_bundle = value;
            continue;
        }
        if (read_value(arg, "--gameplay-catalog-entry", &index, argc, argv, &value)) {
            options->gameplay_catalog_entry = value;
            continue;
        }
        if (read_value(arg, "--catalog-content-namespace", &index, argc, argv, &value)) {
            options->gameplay_catalog_content_namespace = value;
            continue;
        }
        if (read_value(arg, "--catalog-cache-dir", &index, argc, argv, &value)) {
            options->gameplay_catalog_cache_directory = value;
            continue;
        }
        if (read_value(arg, "--host-frames", &index, argc, argv, &value)) {
            if (!parse_u32(value, &options->host_frames)) {
                spdlog::error("invalid host frame count: {}", value);
                return false;
            }
            continue;
        }
        if (read_value(arg, "--network-stats", &index, argc, argv, &value)) {
            if (value == "off") {
                options->network_stats_mode = 1u;
            } else if (value == "basic") {
                options->network_stats_mode = 2u;
            } else if (value == "detailed") {
                options->network_stats_mode = 3u;
            } else {
                spdlog::error("invalid network stats mode: {}", value);
                return false;
            }
            continue;
        }
        if (read_value(arg, "--physics_simulation", &index, argc, argv, &value)) {
            if (!parse_u32_allow_zero(value, &options->physics_simulation) ||
                options->physics_simulation > 1) {
                spdlog::error("invalid physics simulation mode: {}", value);
                return false;
            }
            continue;
        }
        if (read_value(arg, "--physics-workers", &index, argc, argv, &value)) {
            if (!parse_u32_allow_zero(value, &options->physics_workers)) {
                spdlog::error("invalid physics worker count: {}", value);
                return false;
            }
            continue;
        }
        if (read_value(arg, "--actor-blocking", &index, argc, argv, &value)) {
            if (!parse_u32_allow_zero(value, &options->actor_blocking) ||
                options->actor_blocking > KernelActorBlockingMode_Predicted) {
                spdlog::error("invalid actor blocking mode: {}", value);
                return false;
            }
            options->actor_blocking_explicit = true;
            continue;
        }

        spdlog::error("unknown argument: {}", arg);
        return false;
    }

    if (options->mode.empty()) {
        spdlog::error("missing required --mode");
        return false;
    }
    if (options->mode == "client" && options->actor_blocking_explicit) {
        spdlog::error("--actor-blocking is server-only");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    Options options;
    if (!parse_args(argc, argv, &options)) {
        print_usage();
        return 2;
    }
    SetAppNetworkStatsMode(options.network_stats_mode);

    if (options.mode == "dedicated_server") {
        std::string gameplay_catalog_bundle = options.gameplay_catalog_bundle;
        if (gameplay_catalog_bundle.empty() && !options.gameplay_catalog_explicit) {
            gameplay_catalog_bundle = find_default_gameplay_catalog_bundle(
                argc > 0 ? argv[0] : nullptr);
            if (gameplay_catalog_bundle.empty()) {
                spdlog::error(
                    "default gameplay catalog bundle not found; build "
                    "//game_server/gameplay_catalog_bundle:bundle.zip or pass "
                    "--gameplay-catalog-bundle=path/to/bundle.zip");
                return 1;
            }
            spdlog::info(
                "using default gameplay catalog bundle={}",
                gameplay_catalog_bundle);
        } else if (gameplay_catalog_bundle.empty()) {
            spdlog::warn(
                "dedicated server using legacy gameplay catalog file; "
                "gameplay catalog sync is disabled");
        }
        return RunDedicatedServer(
            options.port,
            options.gameplay_catalog.c_str(),
            gameplay_catalog_bundle.c_str(),
            options.gameplay_catalog_entry.c_str(),
            options.gameplay_catalog_content_namespace.c_str(),
            options.physics_simulation,
            options.physics_workers,
            options.actor_blocking);
    }
    if (options.mode == "client") {
        return RunClient(
            options.address.c_str(),
            options.gameplay_catalog.c_str(),
            options.gameplay_catalog_cache_directory.c_str());
    }
    if (options.mode == "host_server") {
        std::string gameplay_catalog_bundle = options.gameplay_catalog_bundle;
        if (gameplay_catalog_bundle.empty() && !options.gameplay_catalog_explicit) {
            gameplay_catalog_bundle = find_default_gameplay_catalog_bundle(
                argc > 0 ? argv[0] : nullptr);
            if (gameplay_catalog_bundle.empty()) {
                spdlog::error(
                    "default gameplay catalog bundle not found; build "
                    "//game_server/gameplay_catalog_bundle:bundle.zip or pass "
                    "--gameplay-catalog-bundle=path/to/bundle.zip");
                return 1;
            }
            spdlog::info(
                "using default gameplay catalog bundle={}",
                gameplay_catalog_bundle);
        }
        return RunHostServer(
            options.port,
            options.gameplay_catalog.c_str(),
            gameplay_catalog_bundle.c_str(),
            options.gameplay_catalog_entry.c_str(),
            options.gameplay_catalog_content_namespace.c_str(),
            options.physics_simulation,
            options.physics_workers,
            options.actor_blocking,
            options.host_frames);
    }

    spdlog::error("unknown mode: {}", options.mode);
    print_usage();
    return 2;
}
