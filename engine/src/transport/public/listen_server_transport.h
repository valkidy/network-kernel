#ifndef TRANSPORT_PUBLIC_LISTEN_SERVER_TRANSPORT_H_
#define TRANSPORT_PUBLIC_LISTEN_SERVER_TRANSPORT_H_

#include <cstdint>
#include <memory>

#include "transport/public/itransport.h"
#include "transport/public/loopback_transport.h"

namespace network_example {

class ListenServerTransport final : public ITransport {
public:
    ListenServerTransport();
    explicit ListenServerTransport(std::unique_ptr<ITransport> remote_transport);

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

    bool SendLocalClient(
        PeerId peer,
        const void* data,
        std::uint32_t size,
        SendMode mode,
        ChannelId channel);
    bool PollLocalClientEvent(TransportEvent& out_event);

private:
    LoopbackTransport local_transport_;
    std::unique_ptr<ITransport> remote_transport_;
    bool poll_remote_first_ = false;
};

}  // namespace network_example

#endif  // TRANSPORT_PUBLIC_LISTEN_SERVER_TRANSPORT_H_
