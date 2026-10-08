// A player who disconnects leaves its tagged items (quest items, map weapons)
// on the ground where it stood, as a death would (design D19; the user's
// call, 2026-10-08: quest items fall where the player was). Untagged items go
// with the player, and so do its containers -- before this, the disconnect
// path removed the player outright and left both behind, owned by no one.
//
// Driven through the server's real disconnect handler. Three players: one
// standing in the open, one inside a building (its tagged items go down where
// it went in, outside), and one whose tagged items a death already dropped
// (nothing more goes down).

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/public/kernel_api.h"
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

constexpr std::uint32_t kPotion = 10u;   // fungible, untagged by default
constexpr std::uint32_t kRelic = 20u;    // stateful, quest by default
constexpr std::uint32_t kGroundProp = 200u;

KernelItemTemplateDefinition item(std::uint32_t id, bool stateful, std::uint8_t tag) {
    KernelItemTemplateDefinition definition{};
    definition.struct_size = sizeof(definition);
    definition.item_template_id = id;
    definition.item_mode = stateful ? KernelItemMode_Stateful : KernelItemMode_Fungible;
    definition.max_stack = stateful ? 1u : 10u;
    definition.capability_flags = KernelItemCapability_Pickupable;
    definition.entity_template_id = kGroundProp;
    definition.interaction_range = 3.0f;
    definition.default_drop_tag = tag;
    definition.use_policy.struct_size = sizeof(definition.use_policy);
    definition.throw_policy.struct_size = sizeof(definition.throw_policy);
    definition.item_used_trigger.struct_size = sizeof(definition.item_used_trigger);
    return definition;
}

KernelEntityTemplateDefinition ground_prop() {
    KernelEntityTemplateDefinition prop{};
    prop.struct_size = sizeof(prop);
    prop.entity_template_id = kGroundProp;
    prop.entity_type = KernelEntityType_Prop;
    prop.component_flags = KERNEL_ENTITY_COMPONENT_TRANSFORM;
    prop.ai.struct_size = sizeof(prop.ai);
    prop.movement.struct_size = sizeof(prop.movement);
    prop.prop.struct_size = sizeof(prop.prop);
    prop.prop.interaction.struct_size = sizeof(prop.prop.interaction);
    return prop;
}

struct Carried {
    ne::NetId player = 0;
    KernelInventoryContainerId container = 0;
    KernelItemInstanceId untagged = 0;   // 3 potions
    KernelItemInstanceId tagged = 0;     // 1 potion, retagged map weapon
    KernelItemInstanceId relic = 0;      // quest by template
};

Carried join(ne::KernelEngine& server, ne::PeerId peer, const glm::vec3& at) {
    Carried carried;
    carried.player = server.world_.spawn_player(peer, at);
    server.peer_sessions_.push_back(
        ne::KernelEngine::PeerSession{peer, carried.player, 0, true, {}});
    carried.container = *server.item_store_.create_container(carried.player, 8u);
    carried.untagged =
        *server.item_store_.create_inventory_item(kPotion, 3u, carried.container);
    carried.tagged =
        *server.item_store_.create_inventory_item(kPotion, 1u, carried.container);
    require(server.item_store_.set_drop_tag(carried.tagged, KERNEL_DROP_TAG_MAP_WEAPON));
    carried.relic = *server.item_store_.create_inventory_item(kRelic, 1u, carried.container);
    require(server.item_store_.find_item(carried.relic)->drop_tag == KERNEL_DROP_TAG_QUEST);
    return carried;
}

void disconnect(ne::KernelEngine& server, ne::PeerId peer) {
    ne::TransportEvent event;
    event.type = ne::TransportEventType::kDisconnected;
    event.peer = peer;
    server.handle_server_disconnect(event);
}

// Where a world item lies.
glm::vec3 lying_at(ne::KernelEngine& server, KernelItemInstanceId id) {
    const ne::ItemInstanceRecord* record = server.item_store_.find_item(id);
    require(record != nullptr && !record->terminal);
    require(record->residency.kind == KernelItemResidency_World);
    const auto prop = server.world_.find_entity(record->residency.prop_entity_id);
    require(prop.has_value());
    return server.world_.registry().get<ne::Transform>(*prop).position;
}

float horizontal(const glm::vec3& a, const glm::vec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

}  // namespace

int main() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    config.max_events = 1024;
    ne::KernelEngine server(config);
    server.transport_ = std::make_unique<ne::LoopbackTransport>();
    server.reset_runtime_state(KernelMode_DedicatedServer);
    server.running_ = true;
    server.entity_templates_.push_back(ground_prop());
    server.item_templates_ = {
        item(kPotion, false, KERNEL_DROP_TAG_NONE),
        item(kRelic, true, KERNEL_DROP_TAG_QUEST)};
    std::string error;
    require(server.item_store_.set_templates(server.item_templates_, &error));

    // In the open: the two tagged go down round where it stood, as
    // themselves; the untagged stack goes with it; nothing is left owned by it.
    {
        const glm::vec3 stood{4.0f, 0.0f, -3.0f};
        const Carried carried = join(server, 3u, stood);
        disconnect(server, 3u);
        require(!server.world_.find_entity(carried.player).has_value());
        for (const KernelItemInstanceId id : {carried.tagged, carried.relic}) {
            require(horizontal(lying_at(server, id), stood) < 1.01f);
        }
        require(server.item_store_.find_item(carried.relic)->drop_tag ==
                KERNEL_DROP_TAG_QUEST);
        require(server.item_store_.find_item(carried.tagged)->drop_tag ==
                KERNEL_DROP_TAG_MAP_WEAPON);
        require(horizontal(lying_at(server, carried.tagged),
                           lying_at(server, carried.relic)) > 1.0f);
        require(server.item_store_.find_item(carried.untagged)->terminal);
        require(server.item_store_.containers_for_owner(carried.player).empty());
        require(server.item_store_.find_container(carried.container) == nullptr);
    }

    // Inside a building: down where it went in, not at the building.
    {
        const glm::vec3 building{30.0f, 0.0f, 0.0f};
        const glm::vec3 entered_from{26.0f, 0.0f, 0.0f};
        const Carried carried = join(server, 4u, building);
        ne::Sheltered sheltered{};
        sheltered.shelter_net_id = server.world_.spawn_player(0u, building);
        sheltered.entry_position = entered_from;
        server.world_.registry().emplace<ne::Sheltered>(
            *server.world_.find_entity(carried.player), sheltered);
        disconnect(server, 4u);
        for (const KernelItemInstanceId id : {carried.tagged, carried.relic}) {
            require(horizontal(lying_at(server, id), entered_from) < 1.01f);
        }
        require(server.item_store_.find_item(carried.untagged)->terminal);
    }

    // Dropped at a death, then disconnected: nothing more goes down, and what
    // the death dropped stays where it fell.
    {
        const glm::vec3 fell{-20.0f, 0.0f, 10.0f};
        const Carried carried = join(server, 5u, fell);
        std::uint32_t dropped = 0;
        require(server.server_drop_tagged_items(carried.player, nullptr, &dropped));
        require(dropped == 2u);
        const std::uint32_t relic_prop =
            server.item_store_.find_item(carried.relic)->residency.prop_entity_id;
        const glm::vec3 relic_at = lying_at(server, carried.relic);
        // The body moves before the disconnect; the drop does not follow it.
        server.world_.registry().get<ne::Transform>(
            *server.world_.find_entity(carried.player)).position = glm::vec3{-40.0f, 0.0f, 10.0f};
        disconnect(server, 5u);
        require(server.item_store_.find_item(carried.relic)->residency.prop_entity_id ==
                relic_prop);
        require(lying_at(server, carried.relic) == relic_at);
        require(server.item_store_.find_item(carried.untagged)->terminal);
    }

    // Nothing to drop is not an error, and an unknown owner is.
    {
        const ne::NetId bare = server.world_.spawn_player(6u, glm::vec3{0.0f});
        std::uint32_t dropped = 7;
        require(server.server_drop_tagged_items(bare, nullptr, &dropped));
        require(dropped == 0u);
        require(!server.server_drop_tagged_items(999999u, nullptr, &dropped));
    }

    std::puts("disconnect_drop_test passed");
    return 0;
}
