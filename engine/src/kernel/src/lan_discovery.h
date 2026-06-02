#ifndef KERNEL_SRC_LAN_DISCOVERY_H_
#define KERNEL_SRC_LAN_DISCOVERY_H_

#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kernel/public/kernel_types.h"

namespace network_example {

class LanDiscoveryService {
public:
    LanDiscoveryService();
    ~LanDiscoveryService();

    LanDiscoveryService(const LanDiscoveryService&) = delete;
    LanDiscoveryService& operator=(const LanDiscoveryService&) = delete;

    bool start_server(const KernelLANDiscoveryServerConfig& config);
    void stop_server();

    bool query(const KernelLANDiscoveryQueryConfig& config);
    std::uint32_t poll_results(
        KernelLANDiscoveryResult* out_results,
        std::uint32_t max_results);
    void clear_results();

private:
    void server_loop(
        std::uint16_t discovery_port,
        std::uint16_t server_endpoint_port,
        std::string server_name,
        std::promise<bool> ready);
    void query_loop(std::uint16_t discovery_port, std::uint32_t timeout_ms);

    std::mutex results_mutex_;
    std::vector<KernelLANDiscoveryResult> results_;

    std::atomic_bool server_running_{false};
    std::thread server_thread_;

    std::atomic_bool query_running_{false};
    std::thread query_thread_;
};

}  // namespace network_example

#endif  // KERNEL_SRC_LAN_DISCOVERY_H_
