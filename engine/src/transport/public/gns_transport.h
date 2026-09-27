#ifndef TRANSPORT_PUBLIC_GNS_TRANSPORT_H_
#define TRANSPORT_PUBLIC_GNS_TRANSPORT_H_

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "transport/public/itransport.h"

class ISteamNetworkingSockets;

namespace network_example {

struct GnsEndpoint {
    std::string host;
    std::uint16_t port = 0;
};

bool parse_gns_address(const char* address, GnsEndpoint* out_endpoint);

// Simulated network conditions for everything this process sends, for testing
// presentation against a late, jittery, lossy stream (the render clock, W1).
// Read from the environment when GameNetworkingSockets is initialised; set on
// the dedicated server, they shape the stream every client receives:
//
//   NETWORK_KERNEL_FAKE_LAG_MS         fixed delay, whole milliseconds
//   NETWORK_KERNEL_FAKE_JITTER_MS      mean extra delay per packet
//                                      (exponentially distributed)
//   NETWORK_KERNEL_FAKE_JITTER_MAX_MS  cap on that extra delay; four times the
//                                      mean when unset
//   NETWORK_KERNEL_FAKE_LOSS_PCT       share of packets dropped, 0-100
//
// Unset, empty, or not a number in range: that condition is off. Jitter never
// reorders packets in GameNetworkingSockets -- it clumps them -- so a late
// packet holds back the ones behind it, which is what a late stream looks like.
struct GnsFakeNetworkConditions {
    int lag_ms = 0;
    float jitter_mean_ms = 0.0f;
    float jitter_max_ms = 0.0f;
    float loss_pct = 0.0f;

    bool any() const {
        return lag_ms > 0 || jitter_mean_ms > 0.0f || loss_pct > 0.0f;
    }
};

GnsFakeNetworkConditions parse_gns_fake_network_conditions(
    const char* lag_ms,
    const char* jitter_mean_ms,
    const char* jitter_max_ms,
    const char* loss_pct);
std::vector<std::uint8_t> encode_gns_payload(
    ChannelId channel,
    SendMode mode,
    const void* data,
    std::uint32_t size);
bool decode_gns_payload(
    const std::uint8_t* data,
    std::size_t size,
    ChannelId* out_channel,
    SendMode* out_mode,
    std::vector<std::uint8_t>* out_payload);

class GnsTransport final : public ITransport {
public:
    GnsTransport();
    ~GnsTransport() override;

    bool StartClient(const char* address) override;
    bool StartServer(std::uint16_t port) override;
    void Stop() override;

    bool Send(
        PeerId peer,
        const void* data,
        std::uint32_t size,
        SendMode mode,
        ChannelId channel) override;

    bool PollEvent(TransportEvent& out_event) override;
    bool running() const;

private:
    enum class Role {
        kStopped,
        kClient,
        kServer,
    };

    using ConnectionHandle = std::uint32_t;
    using ListenSocketHandle = std::uint32_t;
    using PollGroupHandle = std::uint32_t;

    bool initialize_gns();
    void poll_connection_state_changes();
    void poll_incoming_messages();
    void push_message_event(PeerId peer, const std::uint8_t* data, std::uint32_t size);
    void handle_connection_status_changed(void* callback_info);
    PeerId peer_for_connection(ConnectionHandle connection) const;
    void erase_connection(ConnectionHandle connection);

    static void SteamNetConnectionStatusChangedCallback(void* callback_info);

    Role role_ = Role::kStopped;
    ISteamNetworkingSockets* interface_ = nullptr;
    ListenSocketHandle listen_socket_ = 0;
    PollGroupHandle poll_group_ = 0;
    ConnectionHandle server_connection_ = 0;
    std::unordered_map<PeerId, ConnectionHandle> peer_to_connection_;
    std::unordered_map<ConnectionHandle, PeerId> connection_to_peer_;
    std::deque<TransportEvent> events_;
    PeerId next_peer_id_ = 2;
    bool initialized_ = false;
};

bool gns_callback_router_has_owner_for_testing();
void gns_callback_router_set_owner_for_testing(GnsTransport* transport);
void gns_callback_router_clear_owner_for_testing(GnsTransport* transport);

}  // namespace network_example

#endif  // TRANSPORT_PUBLIC_GNS_TRANSPORT_H_
