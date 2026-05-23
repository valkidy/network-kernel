#include "transport/public/gns_transport.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

namespace network_example {
namespace {

constexpr PeerId kServerPeerId = 0;
constexpr std::size_t kTransportPrefixSize = 2;

GnsTransport*& callback_owner() {
    static GnsTransport* owner = nullptr;
    return owner;
}

void set_callback_owner(GnsTransport* transport) {
    callback_owner() = transport;
}

void clear_callback_owner(GnsTransport* transport) {
    if (callback_owner() == transport) {
        callback_owner() = nullptr;
    }
}

bool is_valid_channel(std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(ChannelId::kSession);
}

bool is_valid_mode(std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(SendMode::kReliable);
}

int send_flags_for_mode(SendMode mode) {
    return mode == SendMode::kReliable
               ? k_nSteamNetworkingSend_Reliable
               : k_nSteamNetworkingSend_UnreliableNoDelay;
}

}  // namespace

bool parse_gns_address(const char* address, GnsEndpoint* out_endpoint) {
    if (address == nullptr || out_endpoint == nullptr) {
        return false;
    }

    const std::string value(address);
    const std::size_t separator = value.rfind(':');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= value.size()) {
        return false;
    }

    const std::string host = value.substr(0, separator);
    const std::string port_text = value.substr(separator + 1);
    char* parse_end = nullptr;
    const long port = std::strtol(port_text.c_str(), &parse_end, 10);
    if (parse_end == port_text.c_str() || *parse_end != '\0' || port <= 0 ||
        port > 65535) {
        return false;
    }

    out_endpoint->host = host;
    out_endpoint->port = static_cast<std::uint16_t>(port);
    return true;
}

std::vector<std::uint8_t> encode_gns_payload(
    ChannelId channel,
    SendMode mode,
    const void* data,
    std::uint32_t size) {
    std::vector<std::uint8_t> payload;
    payload.reserve(kTransportPrefixSize + size);
    payload.push_back(static_cast<std::uint8_t>(channel));
    payload.push_back(static_cast<std::uint8_t>(mode));
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (bytes != nullptr && size > 0) {
        payload.insert(payload.end(), bytes, bytes + size);
    }
    return payload;
}

bool decode_gns_payload(
    const std::uint8_t* data,
    std::size_t size,
    ChannelId* out_channel,
    SendMode* out_mode,
    std::vector<std::uint8_t>* out_payload) {
    if (data == nullptr || out_channel == nullptr || out_mode == nullptr ||
        out_payload == nullptr || size < kTransportPrefixSize ||
        !is_valid_channel(data[0]) || !is_valid_mode(data[1])) {
        return false;
    }

    *out_channel = static_cast<ChannelId>(data[0]);
    *out_mode = static_cast<SendMode>(data[1]);
    out_payload->assign(data + kTransportPrefixSize, data + size);
    return true;
}

GnsTransport::GnsTransport() = default;

GnsTransport::~GnsTransport() {
    Stop();
}

bool GnsTransport::StartClient(const char* address) {
    GnsEndpoint endpoint;
    if (!parse_gns_address(address, &endpoint) || !initialize_gns()) {
        return false;
    }

    SteamNetworkingIPAddr server_address;
    server_address.Clear();
    if (!server_address.ParseString(address) || server_address.m_port == 0) {
        return false;
    }

    SteamNetworkingConfigValue_t option;
    option.SetPtr(
        k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
        reinterpret_cast<void*>(SteamNetConnectionStatusChangedCallback));

    server_connection_ = SteamNetworkingSockets()->ConnectByIPAddress(
        server_address,
        1,
        &option);
    if (server_connection_ == k_HSteamNetConnection_Invalid) {
        server_connection_ = 0;
        return false;
    }

    role_ = Role::kClient;
    interface_ = SteamNetworkingSockets();
    set_callback_owner(this);
    return true;
}

bool GnsTransport::StartServer(std::uint16_t port) {
    if (port == 0 || !initialize_gns()) {
        return false;
    }

    SteamNetworkingIPAddr listen_address;
    listen_address.Clear();
    listen_address.m_port = port;

    SteamNetworkingConfigValue_t option;
    option.SetPtr(
        k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
        reinterpret_cast<void*>(SteamNetConnectionStatusChangedCallback));

    interface_ = SteamNetworkingSockets();
    listen_socket_ = interface_->CreateListenSocketIP(listen_address, 1, &option);
    if (listen_socket_ == k_HSteamListenSocket_Invalid) {
        listen_socket_ = 0;
        return false;
    }

    poll_group_ = interface_->CreatePollGroup();
    if (poll_group_ == k_HSteamNetPollGroup_Invalid) {
        interface_->CloseListenSocket(listen_socket_);
        listen_socket_ = 0;
        return false;
    }

    role_ = Role::kServer;
    set_callback_owner(this);
    return true;
}

void GnsTransport::Stop() {
    if (interface_ != nullptr) {
        for (const auto& entry : peer_to_connection_) {
            interface_->CloseConnection(entry.second, 0, nullptr, false);
        }
        peer_to_connection_.clear();
        connection_to_peer_.clear();

        if (server_connection_ != 0) {
            interface_->CloseConnection(server_connection_, 0, nullptr, false);
            server_connection_ = 0;
        }
        if (listen_socket_ != 0) {
            interface_->CloseListenSocket(listen_socket_);
            listen_socket_ = 0;
        }
        if (poll_group_ != 0) {
            interface_->DestroyPollGroup(poll_group_);
            poll_group_ = 0;
        }
    }

    clear_callback_owner(this);
    if (initialized_) {
        GameNetworkingSockets_Kill();
        initialized_ = false;
    }

    interface_ = nullptr;
    role_ = Role::kStopped;
    events_.clear();
    next_peer_id_ = 1;
}

bool GnsTransport::Send(
    PeerId peer,
    const void* data,
    std::uint32_t size,
    SendMode mode,
    ChannelId channel) {
    if (!running() || (data == nullptr && size > 0)) {
        return false;
    }

    ConnectionHandle connection = 0;
    if (role_ == Role::kClient) {
        if (peer != kServerPeerId || server_connection_ == 0) {
            return false;
        }
        connection = server_connection_;
    } else {
        const auto found = peer_to_connection_.find(peer);
        if (found == peer_to_connection_.end()) {
            return false;
        }
        connection = found->second;
    }

    const std::vector<std::uint8_t> payload =
        encode_gns_payload(channel, mode, data, size);
    const EResult result = interface_->SendMessageToConnection(
        connection,
        payload.data(),
        static_cast<std::uint32_t>(payload.size()),
        send_flags_for_mode(mode),
        nullptr);
    return result == k_EResultOK;
}

bool GnsTransport::PollEvent(TransportEvent& out_event) {
    if (!running()) {
        return false;
    }

    poll_connection_state_changes();
    poll_incoming_messages();

    if (events_.empty()) {
        return false;
    }

    out_event = std::move(events_.front());
    events_.pop_front();
    return true;
}

bool GnsTransport::running() const {
    return role_ != Role::kStopped && interface_ != nullptr;
}

bool GnsTransport::initialize_gns() {
    SteamDatagramErrMsg error_message;
    if (!GameNetworkingSockets_Init(nullptr, error_message)) {
        return false;
    }
    initialized_ = true;
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(
        k_ESteamNetworkingConfig_IP_AllowWithoutAuth,
        1);
    return true;
}

void GnsTransport::poll_connection_state_changes() {
    if (interface_ != nullptr) {
        set_callback_owner(this);
        interface_->RunCallbacks();
    }
}

void GnsTransport::poll_incoming_messages() {
    if (interface_ == nullptr) {
        return;
    }

    while (role_ == Role::kServer && poll_group_ != 0) {
        ISteamNetworkingMessage* message = nullptr;
        const int count = interface_->ReceiveMessagesOnPollGroup(poll_group_, &message, 1);
        if (count <= 0) {
            break;
        }
        const PeerId peer = peer_for_connection(message->m_conn);
        push_message_event(
            peer,
            static_cast<const std::uint8_t*>(message->m_pData),
            static_cast<std::uint32_t>(message->m_cbSize));
        message->Release();
    }

    while (role_ == Role::kClient && server_connection_ != 0) {
        ISteamNetworkingMessage* message = nullptr;
        const int count =
            interface_->ReceiveMessagesOnConnection(server_connection_, &message, 1);
        if (count <= 0) {
            break;
        }
        push_message_event(
            kServerPeerId,
            static_cast<const std::uint8_t*>(message->m_pData),
            static_cast<std::uint32_t>(message->m_cbSize));
        message->Release();
    }
}

void GnsTransport::push_message_event(
    PeerId peer,
    const std::uint8_t* data,
    std::uint32_t size) {
    ChannelId channel = ChannelId::kSession;
    SendMode mode = SendMode::kUnreliable;
    std::vector<std::uint8_t> payload;
    if (!decode_gns_payload(data, size, &channel, &mode, &payload)) {
        return;
    }

    TransportEvent event;
    event.type = TransportEventType::kMessage;
    event.peer = peer;
    event.channel = channel;
    event.mode = mode;
    event.payload = std::move(payload);
    events_.push_back(std::move(event));
}

void GnsTransport::handle_connection_status_changed(void* callback_info) {
    auto* info = static_cast<SteamNetConnectionStatusChangedCallback_t*>(callback_info);
    if (info == nullptr || interface_ == nullptr) {
        return;
    }

    switch (info->m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting: {
            if (role_ != Role::kServer) {
                break;
            }
            if (interface_->AcceptConnection(info->m_hConn) != k_EResultOK) {
                interface_->CloseConnection(info->m_hConn, 0, nullptr, false);
                break;
            }
            if (!interface_->SetConnectionPollGroup(info->m_hConn, poll_group_)) {
                interface_->CloseConnection(info->m_hConn, 0, nullptr, false);
                break;
            }

            const PeerId peer = next_peer_id_++;
            peer_to_connection_[peer] = info->m_hConn;
            connection_to_peer_[info->m_hConn] = peer;

            TransportEvent event;
            event.type = TransportEventType::kConnected;
            event.peer = peer;
            events_.push_back(std::move(event));
            break;
        }
        case k_ESteamNetworkingConnectionState_Connected: {
            if (role_ == Role::kClient) {
                TransportEvent event;
                event.type = TransportEventType::kConnected;
                event.peer = kServerPeerId;
                events_.push_back(std::move(event));
            }
            break;
        }
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            const PeerId peer =
                role_ == Role::kClient ? kServerPeerId : peer_for_connection(info->m_hConn);
            interface_->CloseConnection(info->m_hConn, 0, nullptr, false);
            erase_connection(info->m_hConn);
            if (role_ == Role::kClient) {
                server_connection_ = 0;
            }

            TransportEvent event;
            event.type = TransportEventType::kDisconnected;
            event.peer = peer;
            events_.push_back(std::move(event));
            break;
        }
        default:
            break;
    }
}

PeerId GnsTransport::peer_for_connection(ConnectionHandle connection) const {
    const auto found = connection_to_peer_.find(connection);
    if (found == connection_to_peer_.end()) {
        return 0;
    }
    return found->second;
}

void GnsTransport::erase_connection(ConnectionHandle connection) {
    const auto found = connection_to_peer_.find(connection);
    if (found == connection_to_peer_.end()) {
        return;
    }
    peer_to_connection_.erase(found->second);
    connection_to_peer_.erase(found);
}

void GnsTransport::SteamNetConnectionStatusChangedCallback(void* callback_info) {
    if (callback_owner() != nullptr) {
        callback_owner()->handle_connection_status_changed(callback_info);
    }
}

bool gns_callback_router_has_owner_for_testing() {
    return callback_owner() != nullptr;
}

void gns_callback_router_set_owner_for_testing(GnsTransport* transport) {
    set_callback_owner(transport);
}

void gns_callback_router_clear_owner_for_testing(GnsTransport* transport) {
    clear_callback_owner(transport);
}

}  // namespace network_example
