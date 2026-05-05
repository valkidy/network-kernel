#ifndef TRANSPORT_PUBLIC_LOOPBACK_TRANSPORT_H_
#define TRANSPORT_PUBLIC_LOOPBACK_TRANSPORT_H_

#include <deque>

#include "transport/public/itransport.h"

namespace network_example {

class LoopbackTransport final : public ITransport {
public:
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

    bool SendClient(
        PeerId peer,
        const void* data,
        std::uint32_t size,
        SendMode mode,
        ChannelId channel);

    bool PollClientEvent(TransportEvent& out_event);
    bool running() const;

private:
    static bool enqueue_message(
        std::deque<TransportEvent>* queue,
        PeerId peer,
        const void* data,
        std::uint32_t size,
        SendMode mode,
        ChannelId channel);

    std::deque<TransportEvent> client_to_server_;
    std::deque<TransportEvent> server_to_client_;
    bool running_ = false;
};

}  // namespace network_example

#endif  // TRANSPORT_PUBLIC_LOOPBACK_TRANSPORT_H_
