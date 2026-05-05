#ifndef TRANSPORT_PUBLIC_ITRANSPORT_H_
#define TRANSPORT_PUBLIC_ITRANSPORT_H_

#include <cstdint>
#include <vector>

#include "world/public/components.h"

namespace network_example {

enum class SendMode {
    kUnreliable,
    kReliable,
};

enum class ChannelId : std::uint8_t {
    kInput = 0,
    kSnapshot = 1,
    kReliableEvent = 2,
    kSession = 3,
};

enum class TransportEventType {
    kConnected,
    kDisconnected,
    kMessage,
};

struct TransportEvent {
    TransportEventType type = TransportEventType::kMessage;
    PeerId peer = 0;
    ChannelId channel = ChannelId::kSession;
    SendMode mode = SendMode::kUnreliable;
    std::vector<std::uint8_t> payload;
};

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual bool StartClient(const char* address) = 0;
    virtual bool StartServer(std::uint16_t port) = 0;
    virtual void Stop() = 0;

    virtual bool Send(
        PeerId peer,
        const void* data,
        std::uint32_t size,
        SendMode mode,
        ChannelId channel) = 0;

    virtual bool PollEvent(TransportEvent& out_event) = 0;
};

}  // namespace network_example

#endif  // TRANSPORT_PUBLIC_ITRANSPORT_H_
