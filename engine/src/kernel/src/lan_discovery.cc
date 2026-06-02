#include "kernel/src/lan_discovery.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <spdlog/spdlog.h>

#include "kernel/src/build_info.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace network_example {
namespace {

constexpr std::array<char, 8> kMagic = {'N', 'K', 'L', 'A', 'N', '0', '1', '\0'};
constexpr std::uint8_t kQueryMessage = 1;
constexpr std::uint8_t kResponseMessage = 2;
constexpr std::size_t kResponseSize =
    kMagic.size() + 1 + sizeof(std::uint16_t) + sizeof(std::uint32_t) * 3 +
    KERNEL_LAN_DISCOVERY_TEXT_SIZE + KERNEL_BUILD_INFO_TEXT_SIZE +
    KERNEL_BUILD_INFO_TEXT_SIZE;
constexpr std::uint32_t kDefaultQueryTimeoutMs = 500;
constexpr const char* kDefaultServerName = "network_kernel dedicated server";

#if defined(_WIN32)
using SocketHandle = SOCKET;
using SocketAddressSize = int;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

bool initialize_sockets() {
    static bool initialized = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}

void close_socket(SocketHandle socket) {
    if (socket != kInvalidSocket) {
        closesocket(socket);
    }
}
#else
using SocketHandle = int;
using SocketAddressSize = socklen_t;
constexpr SocketHandle kInvalidSocket = -1;

bool initialize_sockets() {
    return true;
}

void close_socket(SocketHandle socket) {
    if (socket != kInvalidSocket) {
        close(socket);
    }
}
#endif

std::uint16_t discovery_port_or_default(std::uint16_t port) {
    return port == 0 ? static_cast<std::uint16_t>(KERNEL_LAN_DISCOVERY_DEFAULT_PORT) : port;
}

std::uint32_t timeout_or_default(std::uint32_t timeout_ms) {
    return timeout_ms == 0 ? kDefaultQueryTimeoutMs : timeout_ms;
}

template <std::size_t Size>
void copy_text(char (&destination)[Size], const char* source) {
    std::snprintf(destination, Size, "%s", source == nullptr ? "" : source);
}

bool set_reuse_address(SocketHandle socket) {
    int enabled = 1;
    return setsockopt(
               socket,
               SOL_SOCKET,
               SO_REUSEADDR,
               reinterpret_cast<const char*>(&enabled),
               sizeof(enabled)) == 0;
}

bool set_broadcast(SocketHandle socket) {
    int enabled = 1;
    return setsockopt(
               socket,
               SOL_SOCKET,
               SO_BROADCAST,
               reinterpret_cast<const char*>(&enabled),
               sizeof(enabled)) == 0;
}

bool set_receive_timeout(SocketHandle socket, std::uint32_t timeout_ms) {
#if defined(_WIN32)
    DWORD timeout = timeout_ms;
    return setsockopt(
               socket,
               SOL_SOCKET,
               SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout),
               sizeof(timeout)) == 0;
#else
    timeval timeout{};
    timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    timeout.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    return setsockopt(
               socket,
               SOL_SOCKET,
               SO_RCVTIMEO,
               &timeout,
               sizeof(timeout)) == 0;
#endif
}

SocketHandle create_udp_socket() {
    if (!initialize_sockets()) {
        return kInvalidSocket;
    }
    return socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

bool send_query(SocketHandle socket, std::uint16_t discovery_port, std::uint32_t address) {
    std::array<std::uint8_t, kMagic.size() + 1> query{};
    std::memcpy(query.data(), kMagic.data(), kMagic.size());
    query[kMagic.size()] = kQueryMessage;

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = address;
    target.sin_port = htons(discovery_port);

    return sendto(
               socket,
               reinterpret_cast<const char*>(query.data()),
               static_cast<int>(query.size()),
               0,
               reinterpret_cast<const sockaddr*>(&target),
               sizeof(target)) == static_cast<int>(query.size());
}

bool is_query_message(const std::uint8_t* data, std::size_t size) {
    return size == kMagic.size() + 1 &&
           std::memcmp(data, kMagic.data(), kMagic.size()) == 0 &&
           data[kMagic.size()] == kQueryMessage;
}

void append_u16(std::vector<std::uint8_t>* bytes, std::uint16_t value) {
    const std::uint16_t network_value = htons(value);
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&network_value);
    bytes->insert(bytes->end(), raw, raw + sizeof(network_value));
}

void append_u32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
    const std::uint32_t network_value = htonl(value);
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&network_value);
    bytes->insert(bytes->end(), raw, raw + sizeof(network_value));
}

template <std::size_t Size>
void append_text(std::vector<std::uint8_t>* bytes, const char (&text)[Size]) {
    const auto* raw = reinterpret_cast<const std::uint8_t*>(text);
    bytes->insert(bytes->end(), raw, raw + Size);
}

std::vector<std::uint8_t> build_response(
    std::uint16_t server_endpoint_port,
    const std::string& server_name) {
    const KernelBuildInfo build_info = current_build_info();
    char response_server_name[KERNEL_LAN_DISCOVERY_TEXT_SIZE]{};
    copy_text(response_server_name, server_name.c_str());

    std::vector<std::uint8_t> response;
    response.reserve(kResponseSize);
    response.insert(response.end(), kMagic.begin(), kMagic.end());
    response.push_back(kResponseMessage);
    append_u16(&response, server_endpoint_port);
    append_u32(&response, build_info.protocol_version);
    append_u32(&response, build_info.snapshot_schema_version);
    append_u32(&response, build_info.packet_schema_version);
    append_text(&response, response_server_name);
    append_text(&response, build_info.module_version);
    append_text(&response, build_info.git_commit);
    return response;
}

std::uint16_t read_u16(const std::uint8_t** cursor) {
    std::uint16_t value = 0;
    std::memcpy(&value, *cursor, sizeof(value));
    *cursor += sizeof(value);
    return ntohs(value);
}

std::uint32_t read_u32(const std::uint8_t** cursor) {
    std::uint32_t value = 0;
    std::memcpy(&value, *cursor, sizeof(value));
    *cursor += sizeof(value);
    return ntohl(value);
}

template <std::size_t Size>
void read_text(const std::uint8_t** cursor, char (&destination)[Size]) {
    std::memcpy(destination, *cursor, Size);
    destination[Size - 1] = '\0';
    *cursor += Size;
}

bool parse_response(
    const std::uint8_t* data,
    std::size_t size,
    const sockaddr_in& source,
    KernelLANDiscoveryResult* out_result) {
    if (data == nullptr || out_result == nullptr || size != kResponseSize ||
        std::memcmp(data, kMagic.data(), kMagic.size()) != 0 ||
        data[kMagic.size()] != kResponseMessage) {
        return false;
    }

    const std::uint8_t* cursor = data + kMagic.size() + 1;
    KernelLANDiscoveryResult result{};
    result.struct_size = sizeof(KernelLANDiscoveryResult);
    result.server_endpoint_port = read_u16(&cursor);
    result.protocol_version = read_u32(&cursor);
    result.snapshot_schema_version = read_u32(&cursor);
    result.packet_schema_version = read_u32(&cursor);
    read_text(&cursor, result.server_name);
    read_text(&cursor, result.module_version);
    read_text(&cursor, result.git_commit);

    char source_ip[KERNEL_LAN_DISCOVERY_TEXT_SIZE]{};
    if (inet_ntop(AF_INET, &source.sin_addr, source_ip, sizeof(source_ip)) == nullptr) {
        return false;
    }
    copy_text(result.server_endpoint_ip, source_ip);

    const KernelBuildInfo local_info = current_build_info();
    result.compatible =
        result.protocol_version == local_info.protocol_version &&
        result.snapshot_schema_version == local_info.snapshot_schema_version &&
        result.packet_schema_version == local_info.packet_schema_version;
    *out_result = result;
    return true;
}

bool same_server(const KernelLANDiscoveryResult& left, const KernelLANDiscoveryResult& right) {
    return left.server_endpoint_port == right.server_endpoint_port &&
           std::strcmp(left.server_endpoint_ip, right.server_endpoint_ip) == 0;
}

}  // namespace

LanDiscoveryService::LanDiscoveryService() = default;

LanDiscoveryService::~LanDiscoveryService() {
    stop_server();
    if (query_thread_.joinable()) {
        query_thread_.join();
    }
}

bool LanDiscoveryService::start_server(const KernelLANDiscoveryServerConfig& config) {
    if (config.struct_size < sizeof(KernelLANDiscoveryServerConfig) ||
        config.server_endpoint_port == 0 || server_running_.load()) {
        return false;
    }

    const std::uint16_t discovery_port = discovery_port_or_default(config.discovery_port);
    const char* configured_name =
        config.server_name[0] == '\0' ? kDefaultServerName : config.server_name;

    std::promise<bool> ready;
    std::future<bool> ready_result = ready.get_future();
    server_running_.store(true);
    server_thread_ = std::thread(
        &LanDiscoveryService::server_loop,
        this,
        discovery_port,
        config.server_endpoint_port,
        std::string(configured_name),
        std::move(ready));

    if (ready_result.get()) {
        return true;
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    return false;
}

void LanDiscoveryService::stop_server() {
    server_running_.store(false);
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

bool LanDiscoveryService::query(const KernelLANDiscoveryQueryConfig& config) {
    if (config.struct_size < sizeof(KernelLANDiscoveryQueryConfig) ||
        query_running_.load()) {
        return false;
    }

    if (query_thread_.joinable()) {
        query_thread_.join();
    }

    query_running_.store(true);
    query_thread_ = std::thread(
        &LanDiscoveryService::query_loop,
        this,
        discovery_port_or_default(config.discovery_port),
        timeout_or_default(config.timeout_ms));
    return true;
}

std::uint32_t LanDiscoveryService::poll_results(
    KernelLANDiscoveryResult* out_results,
    std::uint32_t max_results) {
    if (out_results == nullptr || max_results == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(results_mutex_);
    const std::uint32_t count =
        std::min(max_results, static_cast<std::uint32_t>(results_.size()));
    for (std::uint32_t index = 0; index < count; ++index) {
        out_results[index] = results_[index];
    }
    return count;
}

void LanDiscoveryService::clear_results() {
    std::lock_guard<std::mutex> lock(results_mutex_);
    results_.clear();
}

void LanDiscoveryService::server_loop(
    std::uint16_t discovery_port,
    std::uint16_t server_endpoint_port,
    std::string server_name,
    std::promise<bool> ready) {
    const SocketHandle socket_handle = create_udp_socket();
    if (socket_handle == kInvalidSocket) {
        spdlog::error("LAN discovery server failed to create UDP socket");
        server_running_.store(false);
        ready.set_value(false);
        return;
    }

    set_reuse_address(socket_handle);
    set_receive_timeout(socket_handle, 50);

    sockaddr_in listen_address{};
    listen_address.sin_family = AF_INET;
    listen_address.sin_addr.s_addr = htonl(INADDR_ANY);
    listen_address.sin_port = htons(discovery_port);
    if (bind(
            socket_handle,
            reinterpret_cast<const sockaddr*>(&listen_address),
            sizeof(listen_address)) != 0) {
        spdlog::error("LAN discovery server failed to bind UDP port {}", discovery_port);
        close_socket(socket_handle);
        server_running_.store(false);
        ready.set_value(false);
        return;
    }
    ready.set_value(true);

    const std::vector<std::uint8_t> response =
        build_response(server_endpoint_port, server_name);
    while (server_running_.load()) {
        std::array<std::uint8_t, 64> buffer{};
        sockaddr_in sender{};
        SocketAddressSize sender_size = sizeof(sender);
        const int bytes_received = recvfrom(
            socket_handle,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &sender_size);
        if (bytes_received <= 0) {
            continue;
        }
        if (!is_query_message(buffer.data(), static_cast<std::size_t>(bytes_received))) {
            continue;
        }

        sendto(
            socket_handle,
            reinterpret_cast<const char*>(response.data()),
            static_cast<int>(response.size()),
            0,
            reinterpret_cast<const sockaddr*>(&sender),
            sender_size);
    }

    close_socket(socket_handle);
}

void LanDiscoveryService::query_loop(
    std::uint16_t discovery_port,
    std::uint32_t timeout_ms) {
    const SocketHandle socket_handle = create_udp_socket();
    if (socket_handle == kInvalidSocket) {
        spdlog::error("LAN discovery query failed to create UDP socket");
        query_running_.store(false);
        return;
    }

    set_broadcast(socket_handle);
    set_receive_timeout(socket_handle, 25);

    send_query(socket_handle, discovery_port, htonl(INADDR_BROADCAST));
    send_query(socket_handle, discovery_port, htonl(INADDR_LOOPBACK));

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::array<std::uint8_t, kResponseSize> buffer{};
        sockaddr_in sender{};
        SocketAddressSize sender_size = sizeof(sender);
        const int bytes_received = recvfrom(
            socket_handle,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &sender_size);
        if (bytes_received <= 0) {
            continue;
        }

        KernelLANDiscoveryResult result{};
        if (!parse_response(
                buffer.data(),
                static_cast<std::size_t>(bytes_received),
                sender,
                &result)) {
            continue;
        }

        std::lock_guard<std::mutex> lock(results_mutex_);
        const bool duplicate = std::any_of(
            results_.begin(),
            results_.end(),
            [&](const KernelLANDiscoveryResult& existing) {
                return same_server(existing, result);
            });
        if (!duplicate) {
            results_.push_back(result);
        }
    }

    close_socket(socket_handle);
    query_running_.store(false);
}

}  // namespace network_example
