#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "transport/public/listen_server_transport.h"

namespace {

class FakeRemoteTransport final : public network_example::ITransport {
public:
    bool StartClient(const char*) override { return false; }
    bool StartServer(std::uint16_t port) override {
        running = port != 0;
        return running;
    }
    void Stop() override { running = false; }

    bool Send(
        network_example::PeerId peer,
        const void* data,
        std::uint32_t size,
        network_example::SendMode mode,
        network_example::ChannelId channel) override {
        if (!running || peer == 0 || (size > 0 && data == nullptr)) {
            return false;
        }
        last_send.peer = peer;
        last_send.mode = mode;
        last_send.channel = channel;
        last_send.payload.resize(size);
        if (size > 0) {
            std::memcpy(last_send.payload.data(), data, size);
        }
        return true;
    }

    bool PollEvent(network_example::TransportEvent& out_event) override {
        if (events.empty()) {
            return false;
        }
        out_event = std::move(events.front());
        events.pop_front();
        return true;
    }

    bool running = false;
    network_example::TransportEvent last_send;
    std::deque<network_example::TransportEvent> events;
};

}  // namespace

int main() {
    auto remote = std::make_unique<FakeRemoteTransport>();
    FakeRemoteTransport* remote_view = remote.get();
    network_example::ListenServerTransport transport(std::move(remote));
    assert(transport.StartServer(7777));

    const std::array<std::uint8_t, 3> local_input = {1, 2, 3};
    assert(transport.SendLocalClient(
        1,
        local_input.data(),
        local_input.size(),
        network_example::SendMode::kUnreliable,
        network_example::ChannelId::kInput));
    network_example::TransportEvent event;
    assert(transport.PollEvent(event));
    assert(event.peer == 1);
    assert(event.payload ==
           std::vector<std::uint8_t>(local_input.begin(), local_input.end()));

    const std::array<std::uint8_t, 2> local_snapshot = {4, 5};
    assert(transport.Send(
        1,
        local_snapshot.data(),
        local_snapshot.size(),
        network_example::SendMode::kReliable,
        network_example::ChannelId::kSnapshot));
    assert(transport.PollLocalClientEvent(event));
    assert(event.peer == 1);
    assert(event.payload ==
           std::vector<std::uint8_t>(local_snapshot.begin(), local_snapshot.end()));

    const std::array<std::uint8_t, 2> remote_payload = {8, 9};
    assert(transport.Send(
        7,
        remote_payload.data(),
        remote_payload.size(),
        network_example::SendMode::kReliable,
        network_example::ChannelId::kSession));
    assert(remote_view->last_send.peer == 7);
    assert(remote_view->last_send.payload ==
           std::vector<std::uint8_t>(remote_payload.begin(), remote_payload.end()));

    network_example::TransportEvent remote_event;
    remote_event.type = network_example::TransportEventType::kConnected;
    remote_event.peer = 7;
    remote_view->events.push_back(remote_event);
    assert(transport.PollEvent(event));
    assert(event.type == network_example::TransportEventType::kConnected);
    assert(event.peer == 7);

    transport.Stop();
    assert(!remote_view->running);
    return 0;
}
