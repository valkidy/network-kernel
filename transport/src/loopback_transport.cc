#include "transport/public/loopback_transport.h"

#include <cstring>
#include <utility>

namespace network_example {

bool LoopbackTransport::StartClient(const char* address) {
    running_ = address != nullptr;
    return running_;
}

bool LoopbackTransport::StartServer(std::uint16_t port) {
    running_ = port != 0;
    return running_;
}

void LoopbackTransport::Stop() {
    running_ = false;
    client_to_server_.clear();
    server_to_client_.clear();
}

bool LoopbackTransport::Send(
    PeerId peer,
    const void* data,
    std::uint32_t size,
    SendMode mode,
    ChannelId channel) {
    if (!running_) {
        return false;
    }
    return enqueue_message(&server_to_client_, peer, data, size, mode, channel);
}

bool LoopbackTransport::PollEvent(TransportEvent& out_event) {
    if (client_to_server_.empty()) {
        return false;
    }
    out_event = std::move(client_to_server_.front());
    client_to_server_.pop_front();
    return true;
}

bool LoopbackTransport::SendClient(
    PeerId peer,
    const void* data,
    std::uint32_t size,
    SendMode mode,
    ChannelId channel) {
    if (!running_) {
        return false;
    }
    return enqueue_message(&client_to_server_, peer, data, size, mode, channel);
}

bool LoopbackTransport::PollClientEvent(TransportEvent& out_event) {
    if (server_to_client_.empty()) {
        return false;
    }
    out_event = std::move(server_to_client_.front());
    server_to_client_.pop_front();
    return true;
}

bool LoopbackTransport::running() const {
    return running_;
}

bool LoopbackTransport::enqueue_message(
    std::deque<TransportEvent>* queue,
    PeerId peer,
    const void* data,
    std::uint32_t size,
    SendMode mode,
    ChannelId channel) {
    if (queue == nullptr || (size > 0 && data == nullptr)) {
        return false;
    }

    TransportEvent event;
    event.type = TransportEventType::kMessage;
    event.peer = peer;
    event.channel = channel;
    event.mode = mode;
    event.payload.resize(size);
    if (size > 0) {
        std::memcpy(event.payload.data(), data, size);
    }
    queue->push_back(std::move(event));
    return true;
}

}  // namespace network_example
