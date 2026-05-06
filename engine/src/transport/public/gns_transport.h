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
    PeerId next_peer_id_ = 1;
    bool initialized_ = false;
};

}  // namespace network_example

#endif  // TRANSPORT_PUBLIC_GNS_TRANSPORT_H_
