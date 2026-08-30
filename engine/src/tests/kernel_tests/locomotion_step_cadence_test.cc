// Locomotion steps ride the snapshot, not the tick.
//
// They are their own packet on the snapshot channel and the send budget does not
// reach them, so the cadence is the only thing deciding how much fixed packet
// overhead this channel costs -- 34 B a packet, which at a step a tick was
// 1 kB/s of pure header. It also decides whether the netcode preset governs the
// channel at all: gated on the snapshot, a server at a snapshot every tick moves
// both channels together.
//
// The steps here are injected rather than walked, because what is under test is
// the flush cadence and not the gait.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "protocol/public/network_packets.h"
#include "transport/public/loopback_transport.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}

#define require(condition) require_impl((condition), #condition, __LINE__)

struct Delivered {
    std::size_t packets = 0;
    std::size_t records = 0;
    std::size_t bytes = 0;
    std::size_t largest_start_tick_delta = 0;
};

void drain(network_example::LoopbackTransport* link, Delivered* out) {
    network_example::TransportEvent event;
    while (link->PollClientEvent(event)) {
        network_example::LocomotionStepBatchPacket batch;
        if (event.channel != network_example::ChannelId::kSnapshot ||
            !network_example::decode_locomotion_step_batch_packet(
                event.payload.data(), event.payload.size(), &batch)) {
            continue;
        }
        ++out->packets;
        out->records += batch.records.size();
        out->bytes += event.payload.size();
        for (const network_example::LocomotionStepRecord& record : batch.records) {
            out->largest_start_tick_delta = std::max<std::size_t>(
                out->largest_start_tick_delta, record.start_tick_delta);
        }
    }
}

// The budget, and what it drops first.
//
// Steps are their own packet and the snapshot budget does not reach them, so
// before this the channel was bounded only by how many legged rigs happened to
// be relevant. A cap has to cut the right ones: a footfall at the edge of the
// relevance sphere is a few pixels, one at the player's feet is not.
void the_budget_cuts_the_far_band_first() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    config.max_events = 4096;
    config.max_render_states = 512;
    network_example::KernelEngine engine(config);

    auto transport = std::make_unique<network_example::LoopbackTransport>();
    network_example::LoopbackTransport* link = transport.get();
    engine.transport_ = std::move(transport);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    require(link->StartServer(7794));

    const network_example::NetId player =
        engine.world_.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    network_example::KernelEngine::PeerSession session{1, player, 0, true, {}};

    // Far rigs outnumber near ones by a lot, which is the arrangement that
    // makes an unprioritised cap spend the whole budget on footfalls nobody can
    // see.
    constexpr std::size_t kNearRigs = 4;
    constexpr std::size_t kFarRigs = 40;
    std::vector<network_example::NetId> near_rigs;
    std::vector<network_example::NetId> far_rigs;
    for (std::size_t index = 0; index < kNearRigs; ++index) {
        near_rigs.push_back(
            engine.world_.spawn_enemy(glm::vec3{2.0f, 0.0f, 0.0f}));
    }
    for (std::size_t index = 0; index < kFarRigs; ++index) {
        far_rigs.push_back(
            engine.world_.spawn_enemy(glm::vec3{35.0f, 0.0f, 0.0f}));
    }
    for (const network_example::NetId rig : near_rigs) {
        session.relevant_entities.insert(rig);
    }
    for (const network_example::NetId rig : far_rigs) {
        session.relevant_entities.insert(rig);
    }
    engine.peer_sessions_.push_back(std::move(session));

    const auto queue_step = [&](network_example::NetId net_id, float x) {
        network_example::KernelEngine::PendingLocomotionStep step;
        step.net_id = net_id;
        step.event.leg_index = 0u;
        step.event.start_tick = engine.tick_loop_.current_tick();
        step.event.landing_target_world = glm::vec3{x, 0.0f, 0.0f};
        engine.outgoing_locomotion_steps_.push_back(step);
    };

    constexpr std::size_t kFlushes = 40;
    std::size_t near_delivered = 0;
    std::size_t far_delivered = 0;
    std::vector<std::size_t> far_hits(kFarRigs, 0);
    std::size_t largest_batch = 0;
    for (std::size_t flush = 0; flush < kFlushes; ++flush) {
        // Every rig steps on every flush: more than the budget can carry, which
        // is the whole point.
        for (const network_example::NetId rig : near_rigs) {
            queue_step(rig, 2.0f);
        }
        for (const network_example::NetId rig : far_rigs) {
            queue_step(rig, 35.0f);
        }
        // Two ticks per snapshot, and steps flush with the snapshot.
        engine.simulate_tick();
        engine.simulate_tick();

        network_example::TransportEvent event;
        while (link->PollClientEvent(event)) {
            network_example::LocomotionStepBatchPacket batch;
            if (event.channel != network_example::ChannelId::kSnapshot ||
                !network_example::decode_locomotion_step_batch_packet(
                    event.payload.data(), event.payload.size(), &batch)) {
                continue;
            }
            // The cap, in the units it is written in.
            require(
                event.payload.size() <=
                network_example::estimate_locomotion_step_batch_size(20u));
            largest_batch = std::max(largest_batch, batch.records.size());
            for (const network_example::LocomotionStepRecord& record :
                 batch.records) {
                const auto far_slot =
                    std::find(far_rigs.begin(), far_rigs.end(), record.net_id);
                if (far_slot != far_rigs.end()) {
                    ++far_delivered;
                    ++far_hits[static_cast<std::size_t>(
                        far_slot - far_rigs.begin())];
                } else {
                    ++near_delivered;
                }
            }
        }
    }

    // The channel is actually capped: 44 rigs step every flush and no batch
    // carries all of them.
    require(largest_batch < kNearRigs + kFarRigs);
    // The near band is never the thing that gets cut.
    require(near_delivered == kNearRigs * kFlushes);
    // And the far band is thinned rather than silenced -- every far rig gets
    // turns, because the order inside a band rotates with the tick. Without
    // that the same far rigs would win every flush and the rest would hold the
    // pose their baseline planted while their bodies kept moving.
    require(far_delivered > 0);
    for (const std::size_t hits : far_hits) {
        require(hits > 0);
    }
}

}  // namespace

int main() {
    the_budget_cuts_the_far_band_first();
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    // Half the tick rate, so a tick that carries a snapshot alternates with one
    // that does not.
    config.tick.snapshot_rate = 15;
    config.max_events = 1024;
    config.max_render_states = 256;
    network_example::KernelEngine engine(config);

    auto transport = std::make_unique<network_example::LoopbackTransport>();
    network_example::LoopbackTransport* link = transport.get();
    engine.transport_ = std::move(transport);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    require(link->StartServer(7793));

    const network_example::NetId player =
        engine.world_.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId walker =
        engine.world_.spawn_enemy(glm::vec3{4.0f, 0.0f, 0.0f});
    network_example::KernelEngine::PeerSession session{1, player, 0, true, {}};
    session.relevant_entities.insert(walker);
    engine.peer_sessions_.push_back(std::move(session));

    constexpr std::size_t kTicks = 40;
    Delivered delivered;
    for (std::size_t index = 0; index < kTicks; ++index) {
        // One step a tick, which is what the old cadence turned into one packet
        // a tick.
        network_example::KernelEngine::PendingLocomotionStep step;
        step.net_id = walker;
        step.event.leg_index = static_cast<std::uint32_t>(index % 4u);
        step.event.start_tick = engine.tick_loop_.current_tick();
        step.event.landing_target_world =
            glm::vec3{static_cast<float>(index), 0.0f, 0.0f};
        engine.outgoing_locomotion_steps_.push_back(step);

        engine.simulate_tick();
        drain(link, &delivered);
    }

    // The assertion the change exists for: half the packets, because a tick
    // that carries no snapshot no longer buys a packet header of its own.
    require(delivered.packets == kTicks / 2);
    // Same payload in them. What a tick-cadence flush would have cost is
    // kTicks packets of a header plus one record each; this is strictly less
    // while carrying every one of those records.
    require(delivered.bytes < kTicks * (34u + 18u));
    // Nothing is lost by waiting -- a step is either on the wire or still held
    // for the next snapshot.
    require(
        delivered.records + engine.outgoing_locomotion_steps_.size() == kTicks);
    // A step committed on a tick with no snapshot waits one tick, and
    // start_tick_delta is what carries that to the follower. Zero here would
    // mean the batch claimed every step began on the tick it was sent, and the
    // follower reconstructs an absolute tick from it.
    require(delivered.largest_start_tick_delta == 1);

    // And the tail is not stranded: the next snapshot takes what is held.
    require(!engine.outgoing_locomotion_steps_.empty());
    engine.simulate_tick();
    engine.simulate_tick();
    drain(link, &delivered);
    require(engine.outgoing_locomotion_steps_.empty());
    require(delivered.records == kTicks);

    return 0;
}
