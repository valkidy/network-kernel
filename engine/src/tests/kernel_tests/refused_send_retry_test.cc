// What the server does when the transport refuses a reliable send.
//
// A client that stops reading -- its process stalled or asleep while the
// connection stays up -- fills its GNS queue, and from then on every send to it
// is refused (k_EResultLimitExceeded) until it catches up. Several messages are
// sent only once, when something changes, and used to be dropped on refusal:
// the client then went on with an entity it was never told about, one it was
// never told had gone, or state that was no longer true, for good. Each of them
// is now offered again until it goes out.
//
// A disconnect or a crash refuses sends too, but ends in a Disconnected event
// that removes the session, so offering again costs nothing there.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#define private public
#include "kernel/src/kernel.h"
#undef private

#include "protocol/public/network_packets.h"
#include "transport/public/loopback_transport.h"

namespace ne = network_example;

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr ne::PeerId kPeer = 1;

// Refuses every server send while blocked, the way GNS does once a
// connection's send queue is full: the client is still connected, it has only
// stopped reading.
class RefusingTransport final : public ne::ITransport {
public:
    bool StartClient(const char* address) override {
        return loopback.StartClient(address);
    }
    bool StartServer(std::uint16_t port) override {
        return loopback.StartServer(port);
    }
    void Stop() override { loopback.Stop(); }
    bool Send(
        ne::PeerId peer,
        const void* data,
        std::uint32_t size,
        ne::SendMode mode,
        ne::ChannelId channel) override {
        return !blocked && loopback.Send(peer, data, size, mode, channel);
    }
    bool PollEvent(ne::TransportEvent& out_event) override {
        return loopback.PollEvent(out_event);
    }

    ne::LoopbackTransport loopback;
    bool blocked = false;
};

// What reached the client since the last look.
struct Inbox {
    std::vector<ne::EntitySpawnPacket> spawns;
    std::vector<ne::EntityDespawnPacket> despawns;
    std::vector<ne::ProjectileSpawnBatchPacket> projectile_batches;
    std::vector<ne::PropStateChangeBatchPacket> prop_states;
    std::vector<ne::StatusEffectStatePacket> status_effects;
    std::vector<ne::InventoryContainerClosedPacket> closed_containers;

    bool spawned(ne::NetId net_id) const {
        for (const ne::EntitySpawnPacket& spawn : spawns) {
            if (spawn.net_id == net_id) return true;
        }
        return false;
    }
    bool despawned(ne::NetId net_id) const {
        for (const ne::EntityDespawnPacket& despawn : despawns) {
            if (despawn.net_id == net_id) return true;
        }
        return false;
    }
    bool prop_state_for(ne::NetId net_id) const {
        for (const ne::PropStateChangeBatchPacket& batch : prop_states) {
            for (const ne::PropStateChangeRecord& record : batch.records) {
                if (record.net_id == net_id) return true;
            }
        }
        return false;
    }
};

Inbox drain(RefusingTransport* transport) {
    Inbox inbox;
    ne::TransportEvent event;
    while (transport->loopback.PollClientEvent(event)) {
        if (event.channel != ne::ChannelId::kReliableEvent) continue;
        const std::uint8_t* data = event.payload.data();
        const std::size_t size = event.payload.size();
        ne::EntitySpawnPacket spawn{};
        ne::EntityDespawnPacket despawn{};
        ne::ProjectileSpawnBatchPacket batch{};
        ne::PropStateChangeBatchPacket prop_state{};
        ne::StatusEffectStatePacket status{};
        ne::InventoryContainerClosedPacket closed{};
        if (ne::decode_entity_spawn_packet(data, size, &spawn)) {
            inbox.spawns.push_back(spawn);
        } else if (ne::decode_entity_despawn_packet(data, size, &despawn)) {
            inbox.despawns.push_back(despawn);
        } else if (ne::decode_projectile_spawn_batch_packet(data, size, &batch)) {
            inbox.projectile_batches.push_back(batch);
        } else if (ne::decode_prop_state_change_batch_packet(data, size, &prop_state)) {
            inbox.prop_states.push_back(prop_state);
        } else if (ne::decode_status_effect_state_packet(data, size, &status)) {
            inbox.status_effects.push_back(status);
        } else if (ne::decode_inventory_container_closed_packet(data, size, &closed)) {
            inbox.closed_containers.push_back(closed);
        }
    }
    return inbox;
}

// A dedicated server with one welcomed session whose player stands at the
// origin.
struct Server {
    explicit Server(std::uint16_t port) : engine(make_config()) {
        auto owned = std::make_unique<RefusingTransport>();
        transport = owned.get();
        engine.transport_ = std::move(owned);
        // Started after reset_runtime_state, not before: the reset stops the
        // transport, and a stopped LoopbackTransport drops every Send.
        engine.reset_runtime_state(KernelMode_DedicatedServer);
        require(transport->StartServer(port));
        player = engine.world_.spawn_player(kPeer, glm::vec3{0.0f});
        engine.peer_sessions_.push_back(
            ne::KernelEngine::PeerSession{kPeer, player, 0, true, {}});
    }

    static KernelConfig make_config() {
        KernelConfig config{};
        config.mode = KernelMode_DedicatedServer;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        return config;
    }

    ne::KernelEngine::PeerSession& session() { return engine.peer_sessions_[0]; }

    // One relevance pass, as publish_snapshot runs it.
    Inbox relevance() {
        const ne::WorldSnapshot snapshot =
            engine.build_relevant_snapshot(session(), 0);
        engine.sync_session_relevance(&session(), snapshot);
        return drain(transport);
    }

    void move(ne::NetId net_id, const glm::vec3& position) {
        const std::optional<entt::entity> entity = engine.world_.find_entity(net_id);
        require(entity.has_value());
        engine.world_.registry().get<ne::Transform>(*entity).position = position;
    }

    ne::KernelEngine engine;
    RefusingTransport* transport = nullptr;
    ne::NetId player = 0;
};

// The incident this suite came from: refused spawns used to count as
// announced, so every later snapshot named actors the client had never heard
// of, and once it caught up it ended its prediction session over them
// ("remote actor movement metadata never arrived").
void a_refused_spawn_is_offered_again() {
    Server server(7791);
    require(server.relevance().spawned(server.player));

    server.transport->blocked = true;
    const ne::NetId enemy = server.engine.world_.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    for (int snapshot = 0; snapshot < 4; ++snapshot) {
        server.relevance();
        // Never announced, so no snapshot may name it either.
        require(!server.session().relevant_entities.contains(enemy));
    }

    server.transport->blocked = false;
    require(server.relevance().spawned(enemy));
    require(server.session().relevant_entities.contains(enemy));
}

// A projectile's spawn is only half its metadata; the template comes in the
// spawn batch. A batch refused after its spawn went out offers both again.
void a_refused_projectile_batch_offers_its_spawn_again() {
    Server server(7792);
    require(server.relevance().spawned(server.player));

    // Owned by the session's peer, so it is relevant wherever it is.
    const ne::NetId projectile = server.engine.world_.spawn_projectile(
        kPeer, glm::vec3{1.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 10.0f});
    // Let the spawn through and refuse what follows it.
    struct OneSend final : ne::ITransport {
        explicit OneSend(RefusingTransport* inner) : inner(inner) {}
        bool StartClient(const char*) override { return true; }
        bool StartServer(std::uint16_t) override { return true; }
        void Stop() override {}
        bool Send(
            ne::PeerId peer,
            const void* data,
            std::uint32_t size,
            ne::SendMode mode,
            ne::ChannelId channel) override {
            return sends_left-- > 0 && inner->Send(peer, data, size, mode, channel);
        }
        bool PollEvent(ne::TransportEvent&) override { return false; }
        RefusingTransport* inner;
        int sends_left = 1;
    };
    std::unique_ptr<ne::ITransport> refusing = std::move(server.engine.transport_);
    server.engine.transport_ = std::make_unique<OneSend>(server.transport);
    Inbox inbox = server.relevance();
    require(inbox.spawned(projectile));
    require(inbox.projectile_batches.empty());
    require(!server.session().relevant_entities.contains(projectile));

    server.engine.transport_ = std::move(refusing);
    inbox = server.relevance();
    require(inbox.spawned(projectile));
    require(inbox.projectile_batches.size() == 1u);
    require(server.session().relevant_entities.contains(projectile));
}

// Unity keeps a view until its despawn arrives, so a refused one used to leave
// a frozen entity on the client for good.
void a_refused_despawn_is_sent_again() {
    Server server(7793);
    const ne::NetId enemy = server.engine.world_.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    require(server.relevance().spawned(enemy));

    // Past the 44 m exit radius.
    server.move(enemy, glm::vec3{60.0f, 0.0f, 0.0f});
    server.transport->blocked = true;
    for (int snapshot = 0; snapshot < 3; ++snapshot) {
        server.relevance();
        require(server.session().relevant_entities.contains(enemy));
    }

    server.transport->blocked = false;
    require(server.relevance().despawned(enemy));
    require(!server.session().relevant_entities.contains(enemy));
}

// The same entity back in range before its despawn went out: the client never
// lost it, so it needs neither a despawn nor a new spawn.
void an_entity_back_before_its_despawn_went_out_needs_nothing() {
    Server server(7794);
    const ne::NetId enemy = server.engine.world_.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    require(server.relevance().spawned(enemy));

    server.move(enemy, glm::vec3{60.0f, 0.0f, 0.0f});
    server.transport->blocked = true;
    server.relevance();

    server.move(enemy, glm::vec3{10.0f, 0.0f, 0.0f});
    server.transport->blocked = false;
    const Inbox inbox = server.relevance();
    require(!inbox.despawned(enemy));
    require(!inbox.spawned(enemy));
    require(server.session().relevant_entities.contains(enemy));
}

// A dormant placed prop is not in the snapshot at all: what the client knows
// of it comes only from prop-state records, which are sent when it changes.
void a_refused_prop_state_is_sent_again() {
    Server server(7795);
    const ne::NetId prop = server.engine.world_.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    const entt::entity prop_entity = *server.engine.world_.find_entity(prop);
    auto& registry = server.engine.world_.registry();
    registry.replace<ne::EntityKind>(
        prop_entity, ne::EntityKind{ne::EntityType::kProp, ne::ActorType::kUnknown});
    registry.emplace<ne::PropWorldMode>(prop_entity, ne::PropWorldMode{ne::PropMode::kPlaced});
    registry.get<ne::Velocity>(prop_entity).linear = glm::vec3{0.0f};
    registry.replace<ne::Health>(prop_entity, ne::Health{40, 100});
    const Inbox introduced = server.relevance();
    require(introduced.spawned(prop));
    require(introduced.prop_state_for(prop));

    server.transport->blocked = true;
    server.engine.queue_prop_state_change(prop);
    server.engine.flush_prop_state_changes();
    require(server.session().prop_state_retries.contains(prop));

    // Nothing new is pending; the retry alone sends it.
    server.transport->blocked = false;
    server.engine.flush_prop_state_changes();
    require(drain(server.transport).prop_state_for(prop));
    require(server.session().prop_state_retries.empty());
}

// The own player's status effects go whole, and only when they change.
void a_refused_status_effect_state_is_sent_again() {
    Server server(7796);
    require(server.relevance().spawned(server.player));
    ne::StatusEffectState& state =
        server.engine.world_.registry().get_or_emplace<ne::StatusEffectState>(
            *server.engine.world_.find_entity(server.player));
    ne::ActiveStatusEffect active;
    active.instance_id = 1u;
    active.status_effect_id = 1401u;
    active.expire_tick = 100000u;
    active.source = server.player;
    active.stack_count = 1u;
    state.active.push_back(active);
    state.revision = 1u;

    server.transport->blocked = true;
    server.engine.publish_status_effect_state(server.player);
    require(server.session().status_effect_state_retry);

    // Called as the tick calls it. A whole tick would not show the retry:
    // the status system rewrites a state with no catalog behind it, and
    // publishes that itself.
    server.transport->blocked = false;
    server.engine.retry_status_effect_states();
    const Inbox inbox = drain(server.transport);
    require(inbox.status_effects.size() == 1u);
    require(inbox.status_effects.front().revision == 1u);
    require(inbox.status_effects.front().records.size() == 1u);
    require(!server.session().status_effect_state_retry);
}

// A container the session stops seeing is closed, so its client drops its
// copy. A refused close used to forget the container anyway.
void a_refused_container_close_is_sent_again() {
    Server server(7797);
    const auto container = server.engine.item_store_.create_container(server.player, 4u);
    require(container.has_value());
    server.engine.flush_inventory_replication();
    require(server.session().inventory_revisions.contains(*container));
    drain(server.transport);

    require(server.engine.item_store_.destroy_container(*container));
    server.transport->blocked = true;
    server.engine.flush_inventory_replication();
    require(server.session().inventory_revisions.contains(*container));

    server.transport->blocked = false;
    server.engine.flush_inventory_replication();
    const Inbox inbox = drain(server.transport);
    require(inbox.closed_containers.size() == 1u);
    require(inbox.closed_containers.front().inventory_container_id == *container);
    require(!server.session().inventory_revisions.contains(*container));
}

}  // namespace

int main() {
    a_refused_spawn_is_offered_again();
    a_refused_projectile_batch_offers_its_spawn_again();
    a_refused_despawn_is_sent_again();
    an_entity_back_before_its_despawn_went_out_needs_nothing();
    a_refused_prop_state_is_sent_again();
    a_refused_status_effect_state_is_sent_again();
    a_refused_container_close_is_sent_again();
    std::puts("refused_send_retry_test passed");
    return 0;
}
