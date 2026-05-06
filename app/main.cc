#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "client_app.h"
#include "dedicated_server_app.h"
#include "host_server_app.h"

namespace {

constexpr std::uint16_t kDefaultPort = 7777;
constexpr std::string_view kDefaultAddress = "127.0.0.1:7777";

struct Options {
    std::string mode;
    std::string address{std::string(kDefaultAddress)};
    std::uint16_t port = kDefaultPort;
};

void print_usage() {
    spdlog::error(
        "usage: //app:app -- --mode=<dedicated_server|client|host_server> "
        "[--address=127.0.0.1:7777] [--port=7777]");
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

        spdlog::error("unknown argument: {}", arg);
        return false;
    }

    if (options->mode.empty()) {
        spdlog::error("missing required --mode");
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

    if (options.mode == "dedicated_server") {
        return RunDedicatedServer(options.port);
    }
    if (options.mode == "client") {
        return RunClient(options.address.c_str());
    }
    if (options.mode == "host_server") {
        return RunHostServer(options.port);
    }

    spdlog::error("unknown mode: {}", options.mode);
    print_usage();
    return 2;
}
