#include "transport/public/listen_server_transport.h"

#include <memory>
#include <utility>

#include "transport/public/gns_transport.h"

namespace network_example {

ListenServerTransport::ListenServerTransport()
    : ListenServerTransport(std::make_unique<GnsTransport>()) {}

ListenServerTransport::ListenServerTransport(
    std::unique_ptr<ITransport> remote_transport)
    : remote_transport_(std::move(remote_transport)) {}

bool ListenServerTransport::StartClient(const char*) {
    return false;
}

bool ListenServerTransport::StartServer(std::uint16_t port) {
    if (remote_transport_ == nullptr ||
        !local_transport_.StartServer(port)) {
        return false;
    }
    if (!remote_transport_->StartServer(port)) {
        local_transport_.Stop();
        return false;
    }
    poll_remote_first_ = false;
    return true;
}

void ListenServerTransport::Stop() {
    local_transport_.Stop();
    if (remote_transport_ != nullptr) {
        remote_transport_->Stop();
    }
}

bool ListenServerTransport::Send(
    PeerId peer,
    const void* data,
    std::uint32_t size,
    SendMode mode,
    ChannelId channel) {
    if (peer == 1) {
        return local_transport_.Send(peer, data, size, mode, channel);
    }
    return remote_transport_ != nullptr &&
           remote_transport_->Send(peer, data, size, mode, channel);
}

bool ListenServerTransport::PollEvent(TransportEvent& out_event) {
    bool polled = false;
    if (poll_remote_first_) {
        polled =
            remote_transport_ != nullptr && remote_transport_->PollEvent(out_event);
        if (!polled) {
            polled = local_transport_.PollEvent(out_event);
        }
    } else {
        polled = local_transport_.PollEvent(out_event);
        if (!polled && remote_transport_ != nullptr) {
            polled = remote_transport_->PollEvent(out_event);
        }
    }
    if (polled) {
        poll_remote_first_ = !poll_remote_first_;
    }
    return polled;
}

bool ListenServerTransport::SendLocalClient(
    PeerId peer,
    const void* data,
    std::uint32_t size,
    SendMode mode,
    ChannelId channel) {
    return peer == 1 &&
           local_transport_.SendClient(peer, data, size, mode, channel);
}

bool ListenServerTransport::PollLocalClientEvent(TransportEvent& out_event) {
    return local_transport_.PollClientEvent(out_event);
}

}  // namespace network_example
