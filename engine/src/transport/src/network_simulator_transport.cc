#include "transport/public/network_simulator_transport.h"

#include <algorithm>
#include <cstring>

namespace network_example {

NetworkSimulatorTransport::NetworkSimulatorTransport(NetworkSimulatorConfig config)
    : config_(config) {}

bool NetworkSimulatorTransport::StartClient(const char* address) {
    running_ = address != nullptr;
    return running_;
}

bool NetworkSimulatorTransport::StartServer(std::uint16_t port) {
    running_ = port != 0;
    return running_;
}

void NetworkSimulatorTransport::Stop() {
    running_ = false;
    scheduled_events_.clear();
}

bool NetworkSimulatorTransport::Send(
    PeerId peer,
    const void* data,
    std::uint32_t size,
    SendMode mode,
    ChannelId channel) {
    if (!running_ || (size > 0 && data == nullptr)) {
        return false;
    }

    ++packet_count_;
    if (config_.drop_every_nth_packet != 0 &&
        packet_count_ % config_.drop_every_nth_packet == 0) {
        return true;
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

    ScheduledEvent scheduled_event{delivery_tick_for_packet(), next_order_++, event};
    enqueue(scheduled_event);

    if (config_.duplicate_every_nth_packet != 0 &&
        packet_count_ % config_.duplicate_every_nth_packet == 0) {
        ScheduledEvent duplicate = scheduled_event;
        duplicate.order = next_order_++;
        enqueue(duplicate);
    }

    return true;
}

bool NetworkSimulatorTransport::PollEvent(TransportEvent& out_event) {
    auto iter = std::min_element(
        scheduled_events_.begin(),
        scheduled_events_.end(),
        [](const ScheduledEvent& lhs, const ScheduledEvent& rhs) {
            if (lhs.delivery_tick != rhs.delivery_tick) {
                return lhs.delivery_tick < rhs.delivery_tick;
            }
            return lhs.order < rhs.order;
        });
    if (iter == scheduled_events_.end() || iter->delivery_tick > current_tick_) {
        return false;
    }

    out_event = iter->event;
    scheduled_events_.erase(iter);
    return true;
}

void NetworkSimulatorTransport::AdvanceTicks(std::uint32_t ticks) {
    current_tick_ += ticks;
}

bool NetworkSimulatorTransport::running() const {
    return running_;
}

std::uint32_t NetworkSimulatorTransport::delivery_tick_for_packet() {
    std::uint32_t delivery_tick = current_tick_ + config_.latency_ticks;
    if (config_.jitter_ticks != 0) {
        delivery_tick += packet_count_ % (config_.jitter_ticks + 1);
    }
    if (config_.reorder_pairs && packet_count_ % 2 == 1) {
        delivery_tick += 1;
    }
    return delivery_tick;
}

void NetworkSimulatorTransport::enqueue(ScheduledEvent event) {
    scheduled_events_.push_back(std::move(event));
}

}  // namespace network_example
