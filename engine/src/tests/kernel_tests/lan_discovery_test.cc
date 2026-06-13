#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "kernel/public/kernel_api.h"

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

std::uint16_t allocate_udp_port() {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    require(fd >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

    socklen_t address_size = sizeof(address);
    require(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) == 0);
    const std::uint16_t port = ntohs(address.sin_port);
    close(fd);
    return port;
}

}  // namespace

int main() {
    const std::uint16_t discovery_port = allocate_udp_port();
    constexpr std::uint16_t kServerEndpointPort = 7799;

    KernelLANDiscoveryHandle* discovery = Kernel_LANDiscovery_Create();
    require(discovery != nullptr);

    KernelLANDiscoveryServerConfig server_config{};
    server_config.struct_size = sizeof(server_config);
    server_config.discovery_port = discovery_port;
    server_config.server_endpoint_port = kServerEndpointPort;
    std::snprintf(
        server_config.server_name,
        sizeof(server_config.server_name),
        "%s",
        "Native LAN Test Server");
    require(Kernel_LANDiscovery_StartServer(discovery, &server_config));
    require(!Kernel_LANDiscovery_StartServer(discovery, &server_config));

    KernelLANDiscoveryQueryConfig query_config{};
    query_config.struct_size = sizeof(query_config);
    query_config.discovery_port = discovery_port;
    query_config.timeout_ms = 250;
    require(Kernel_LANDiscovery_Query(discovery, &query_config));

    KernelLANDiscoveryResult result{};
    std::uint32_t result_count = 0;
    for (std::uint32_t attempt = 0; attempt < 40 && result_count == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        result_count = Kernel_LANDiscovery_PollResults(discovery, &result, 1);
    }

    require(result_count == 1);
    require(result.struct_size == sizeof(KernelLANDiscoveryResult));
    require(std::strcmp(result.server_name, "Native LAN Test Server") == 0);
    require(result.server_endpoint_ip[0] != '\0');
    require(result.server_endpoint_port == kServerEndpointPort);
    require(result.module_version[0] != '\0');
    require(result.protocol_version != 0);
    require(result.snapshot_schema_version != 0);
    require(result.packet_schema_version != 0);
    require(result.git_commit[0] != '\0');
    require(result.compatible != 0u);

    Kernel_LANDiscovery_ClearResults(discovery);
    result = KernelLANDiscoveryResult{};
    require(Kernel_LANDiscovery_PollResults(discovery, &result, 1) == 0);

    Kernel_LANDiscovery_StopServer(discovery);
    Kernel_LANDiscovery_StopServer(discovery);
    Kernel_LANDiscovery_Destroy(discovery);
}
