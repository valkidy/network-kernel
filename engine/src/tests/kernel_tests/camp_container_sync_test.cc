// A camp's stock reaches only those inside the camp (K9, design D9).
//
// Until K9 a container went to its owner and nobody else. A camp's stock is
// owned by the camp, and goes to whoever is inside it (Sheltered on that
// camp): in full on the way in, as deltas while inside, and closed on the way
// out -- the client drops its copy, and coming back starts from a full
// snapshot. Someone outside never sees it, and may not ask for it. A camp that
// is gone closes its stock for everyone still holding it.
//
// A dedicated server and a client over loopback; the camp is any entity, since
// what makes a container a camp's is only who owns it.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/public/kernel_api.h"
#include "protocol/public/network_packets.h"
#include "transport/public/loopback_transport.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

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

constexpr ne::PeerId kOccupant = 3;
constexpr ne::PeerId kStranger = 4;
constexpr std::uint32_t kPotion = 10u;

ne::LoopbackTransport* attach_loopback(ne::KernelEngine* engine, KernelMode mode) {
    auto transport = std::make_unique<ne::LoopbackTransport>();
    ne::LoopbackTransport* loopback = transport.get();
    engine->transport_ = std::move(transport);
    engine->reset_runtime_state(mode);
    require(loopback->StartServer(7102));
    return loopback;
}

void set_templates(ne::KernelEngine* engine) {
    KernelItemTemplateDefinition item{};
    item.struct_size = sizeof(item);
    item.item_template_id = kPotion;
    item.item_mode = KernelItemMode_Fungible;
    item.max_stack = 10u;
    item.use_policy.struct_size = sizeof(item.use_policy);
    item.throw_policy.struct_size = sizeof(item.throw_policy);
    std::string error;
    require(engine->item_store_.set_templates({&item, 1u}, &error));
}

struct Traffic {
    std::uint32_t snapshots_to_occupant = 0;
    std::uint32_t deltas_to_occupant = 0;
    std::uint32_t closed_to_occupant = 0;
    std::uint32_t to_stranger = 0;
};

// Delivers the server's packets for the occupant to its client, counts what
// went where, and lets the client apply them.
Traffic deliver(
    ne::LoopbackTransport* server,
    ne::LoopbackTransport* client_link,
    ne::KernelEngine* client) {
    Traffic traffic;
    ne::TransportEvent event;
    while (server->PollClientEvent(event)) {
        if (event.peer == kStranger) {
            ++traffic.to_stranger;
            continue;
        }
        ne::InventorySnapshotPagePacket page;
        ne::InventoryDeltaBatchPacket delta;
        ne::InventoryContainerClosedPacket closed;
        if (ne::decode_inventory_snapshot_page_packet(
                event.payload.data(), event.payload.size(), &page)) {
            ++traffic.snapshots_to_occupant;
        } else if (ne::decode_inventory_delta_batch_packet(
                       event.payload.data(), event.payload.size(), &delta)) {
            ++traffic.deltas_to_occupant;
        } else if (ne::decode_inventory_container_closed_packet(
                       event.payload.data(), event.payload.size(), &closed)) {
            ++traffic.closed_to_occupant;
        }
        require(client_link->SendClient(
            event.peer,
            event.payload.data(),
            static_cast<std::uint32_t>(event.payload.size()),
            event.mode,
            event.channel));
    }
    client->poll_transport();
    return traffic;
}

std::uint32_t quantity_on_client(
    const ne::KernelEngine& client, KernelInventoryContainerId container) {
    const ne::InventoryContainerRecord* record =
        client.item_store_.find_container(container);
    if (record == nullptr) return 0u;
    std::uint32_t total = 0;
    for (const KernelItemInstanceId id : record->slots) {
        const ne::ItemInstanceRecord* item = client.item_store_.find_item(id);
        if (item != nullptr && !item->terminal) total += item->quantity;
    }
    return total;
}

}  // namespace

int main() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    config.max_events = 1024;
    ne::KernelEngine server(config);
    ne::LoopbackTransport* server_link =
        attach_loopback(&server, KernelMode_DedicatedServer);
    server.running_ = true;
    config.mode = KernelMode_Client;
    ne::KernelEngine client(config);
    ne::LoopbackTransport* client_link = attach_loopback(&client, KernelMode_Client);
    client.has_welcome_ = true;
    set_templates(&server);
    set_templates(&client);

    const ne::NetId occupant = server.world_.spawn_player(kOccupant, glm::vec3{0.0f});
    const ne::NetId stranger = server.world_.spawn_player(kStranger, glm::vec3{5.0f});
    const ne::NetId camp = server.world_.spawn_player(0u, glm::vec3{10.0f});
    server.peer_sessions_.push_back(
        ne::KernelEngine::PeerSession{kOccupant, occupant, 0, true, {}});
    server.peer_sessions_.push_back(
        ne::KernelEngine::PeerSession{kStranger, stranger, 0, true, {}});

    const auto own = server.item_store_.create_container(occupant, 4u);
    const auto stock = server.item_store_.create_container(camp, 4u);
    require(own.has_value() && stock.has_value());
    const auto stock_potions = server.item_store_.create_inventory_item(kPotion, 5u, *stock);
    require(stock_potions.has_value());

    // Outside: the occupant gets its own container -- the control that
    // packets flow at all -- and nobody gets the camp's.
    server.flush_inventory_replication();
    Traffic traffic = deliver(server_link, client_link, &client);
    require(traffic.snapshots_to_occupant == 1u);
    require(traffic.to_stranger == 0u);
    require(client.item_store_.find_container(*own) != nullptr);
    require(client.item_store_.find_container(*stock) == nullptr);

    // Inside: the stock arrives in full.
    const entt::entity occupant_entity = *server.world_.find_entity(occupant);
    server.world_.registry().emplace<ne::Sheltered>(occupant_entity, ne::Sheltered{camp});
    server.flush_inventory_replication();
    traffic = deliver(server_link, client_link, &client);
    require(traffic.snapshots_to_occupant == 1u);
    require(traffic.to_stranger == 0u);
    require(quantity_on_client(client, *stock) == 5u);
    require(client.item_store_.find_container(*stock)->owner_entity_id == camp);

    // While inside, a take reaches it as deltas on both containers.
    require(server.item_store_.transfer_to_container(*stock_potions, 2u, *own).has_value());
    server.flush_inventory_replication();
    traffic = deliver(server_link, client_link, &client);
    require(traffic.deltas_to_occupant == 2u);
    require(traffic.snapshots_to_occupant == 0u);
    require(traffic.to_stranger == 0u);
    require(quantity_on_client(client, *stock) == 3u);
    require(quantity_on_client(client, *own) == 2u);

    // Someone outside may not ask for it; the occupant may.
    {
        const std::vector<std::uint8_t> request =
            ne::encode_inventory_snapshot_request_packet(
                ne::InventorySnapshotRequestPacket{*stock, 0u});
        server.events_.clear();
        require(server_link->SendClient(
            kStranger, request.data(), static_cast<std::uint32_t>(request.size()),
            ne::SendMode::kReliable, ne::ChannelId::kReliableEvent));
        server.poll_transport();
        std::uint32_t refused = 0;
        for (const KernelEvent& event : server.events_) {
            if (event.type == KernelEventType_Error && event.code == 33u) ++refused;
        }
        require(refused == 1u);
        require(deliver(server_link, client_link, &client).to_stranger == 0u);
        require(server_link->SendClient(
            kOccupant, request.data(), static_cast<std::uint32_t>(request.size()),
            ne::SendMode::kReliable, ne::ChannelId::kReliableEvent));
        server.poll_transport();
        require(deliver(server_link, client_link, &client).snapshots_to_occupant == 1u);
    }

    // Out again: closed, and the client's copy is gone; its own stays.
    server.world_.registry().remove<ne::Sheltered>(occupant_entity);
    server.flush_inventory_replication();
    traffic = deliver(server_link, client_link, &client);
    require(traffic.closed_to_occupant == 1u);
    require(client.item_store_.find_container(*stock) == nullptr);
    require(quantity_on_client(client, *own) == 2u);
    // Nothing more while outside, however the stock changes.
    require(server.item_store_.transfer_to_container(*stock_potions, 1u, *own).has_value());
    server.flush_inventory_replication();
    traffic = deliver(server_link, client_link, &client);
    require(traffic.snapshots_to_occupant == 0u);
    require(traffic.closed_to_occupant == 0u);
    require(client.item_store_.find_container(*stock) == nullptr);
    require(quantity_on_client(client, *own) == 3u);

    // Back in: a full snapshot of what is left, not deltas from where it was.
    server.world_.registry().emplace<ne::Sheltered>(occupant_entity, ne::Sheltered{camp});
    server.flush_inventory_replication();
    traffic = deliver(server_link, client_link, &client);
    require(traffic.snapshots_to_occupant == 1u);
    require(quantity_on_client(client, *stock) == 2u);

    // The camp's stock is destroyed (the camp went): closed while inside.
    require(server.item_store_.destroy_container(*stock));
    require(server.item_store_.find_item(*stock_potions)->terminal);
    server.flush_inventory_replication();
    traffic = deliver(server_link, client_link, &client);
    require(traffic.closed_to_occupant == 1u);
    require(client.item_store_.find_container(*stock) == nullptr);
    require(client.item_store_.find_container(*own) != nullptr);

    std::puts("camp_container_sync_test passed");
    return 0;
}
