// What a player has in its hands (a Carry) is let go of when it dies and when
// it disconnects: set down where it was held, on the ground beneath (design,
// 2026-10-08: every carried prop, tagged or not). Before, a death left it
// carried by a body that can make no requests, and a disconnect left it
// hanging where it was, carried by no one who still exists -- out of
// everyone's reach for good.
//
// A dedicated server with a terrain slab whose top is y = 0. The death is the
// real one (damage applied, then enter_death_state); the disconnect goes
// through the server's own handler. In each case a second player's Pickup is
// refused while the prop is carried (Claimed) -- the control -- and committed
// once it is down.

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
#include "physics/public/physics_world.h"
#include "transport/public/loopback_transport.h"

#define private public
#include "kernel/src/kernel.h"
#include "simulation/src/item_gameplay_system.h"
#include "simulation/src/systems.h"
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

constexpr std::uint32_t kRelic = 20u;
constexpr std::uint32_t kGroundProp = 200u;

KernelItemTemplateDefinition relic() {
    KernelItemTemplateDefinition definition{};
    definition.struct_size = sizeof(definition);
    definition.item_template_id = kRelic;
    definition.item_mode = KernelItemMode_Stateful;
    definition.max_stack = 1u;
    definition.capability_flags =
        KernelItemCapability_Pickupable | KernelItemCapability_Carryable;
    definition.entity_template_id = kGroundProp;
    definition.interaction_range = 3.0f;
    definition.default_drop_tag = KERNEL_DROP_TAG_QUEST;
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

struct Server {
    Server() : engine(make_config()) {
        engine.transport_ = std::make_unique<ne::LoopbackTransport>();
        engine.reset_runtime_state(KernelMode_DedicatedServer);
        engine.running_ = true;
        engine.physics_world_ = std::make_unique<ne::physics::PhysicsWorld>();
        ne::physics::CollisionObjectDescriptor slab;
        slab.identity = ne::physics::CollisionObjectIdentity{
            0, 1u, ne::physics::kHitZoneUnscaled,
            ne::physics::CollisionObjectKind::kTerrain,
            ne::physics::CollisionLayer::kTerrain};
        slab.shape.type = ne::physics::CollisionShapeType::kBox;
        slab.shape.half_extents = glm::vec3{100.0f, 0.5f, 100.0f};
        slab.position = glm::vec3{0.0f, -0.5f, 0.0f};
        slab.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        std::string error;
        require(engine.physics_world_->upsert_object(slab, &error));
        engine.entity_templates_.push_back(ground_prop());
        engine.item_templates_ = {relic()};
        require(engine.item_store_.set_templates(engine.item_templates_, &error));
    }

    static KernelConfig make_config() {
        KernelConfig config{};
        config.mode = KernelMode_DedicatedServer;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        config.max_events = 1024;
        return config;
    }

    ne::NetId join(ne::PeerId peer, const glm::vec3& at) {
        const ne::NetId player = engine.world_.spawn_player(peer, at);
        engine.world_.registry().emplace_or_replace<ne::Health>(
            *engine.world_.find_entity(player), ne::Health{100, 100});
        engine.peer_sessions_.push_back(
            ne::KernelEngine::PeerSession{peer, player, 0, true, {}});
        // Somewhere to put what it picks up.
        require(engine.item_store_.create_container(player, 4u).has_value());
        return player;
    }

    void move(ne::NetId net_id, const glm::vec3& to) {
        engine.world_.registry().get<ne::Transform>(*engine.world_.find_entity(net_id))
            .position = to;
        ne::ItemGameplaySystem{}.update_carried_props(engine);
    }

    KernelGameplayRequestOutcome request(
        ne::PeerId peer, ne::NetId actor, std::uint8_t action,
        KernelItemInstanceId item, std::uint32_t target) {
        KernelGameplayRequest value{};
        value.struct_size = sizeof(value);
        value.requester_peer = peer;
        value.request_id = next_request++;
        value.instigator_net_id = actor;
        value.domain_action = action;
        value.selected_item_instance_id = item;
        value.target_net_id = target;
        value.requested_quantity = 1;
        require(ne::ItemGameplaySystem{}.submit_request(engine, value));
        return engine.processed_gameplay_requests_.back();
    }

    // A relic laid by `player` and taken up in its hands. Returns {item, prop}.
    std::pair<KernelItemInstanceId, std::uint32_t> carry(ne::PeerId peer, ne::NetId player) {
        const KernelInventoryContainerId bag =
            engine.item_store_.find_container_for_owner(player)->inventory_container_id;
        const KernelItemInstanceId item =
            *engine.item_store_.create_inventory_item(kRelic, 1u, bag);
        const glm::vec3 at =
            engine.world_.registry().get<ne::Transform>(*engine.world_.find_entity(player))
                .position + glm::vec3{1.0f, 0.0f, 0.0f};
        std::uint32_t prop = 0;
        require(engine.server_drop_inventory_item(item, KernelVec3{at.x, at.y, at.z}, &prop));
        require(request(peer, player, KernelDomainAction_Carry, item, prop).status ==
                KernelGameplayRequestStatus_Committed);
        require(carried(prop));
        return {item, prop};
    }

    bool carried(std::uint32_t prop) {
        const entt::entity entity = *engine.world_.find_entity(prop);
        return engine.world_.registry().all_of<ne::CarriedBy>(entity) &&
            engine.world_.registry().get<ne::PropWorldMode>(entity).mode ==
                ne::PropMode::kCarrying;
    }

    glm::vec3 position(std::uint32_t net_id) {
        return engine.world_.registry().get<ne::Transform>(*engine.world_.find_entity(net_id))
            .position;
    }

    ne::KernelEngine engine;
    std::uint64_t next_request = 1;
};

// The prop is set down: placed, carried by nobody, its item placed too.
void require_set_down(Server& server, KernelItemInstanceId item, std::uint32_t prop) {
    require(!server.carried(prop));
    const entt::entity entity = *server.engine.world_.find_entity(prop);
    require(server.engine.world_.registry().get<ne::PropWorldMode>(entity).mode ==
            ne::PropMode::kPlaced);
    const ne::ItemInstanceRecord* record = server.engine.item_store_.find_item(item);
    require(record->residency.kind == KernelItemResidency_World);
    require(record->residency.world_mode == KernelWorldItemMode_Placed);
    require(record->drop_tag == KERNEL_DROP_TAG_QUEST);
}

}  // namespace

int main() {
    // Death in mid-air: down on the ground beneath where it was held.
    {
        Server server;
        const ne::NetId holder = server.join(3u, glm::vec3{0.0f, 1.0f, 0.0f});
        const ne::NetId other = server.join(4u, glm::vec3{0.0f, 1.0f, 2.0f});
        const auto [item, prop] = server.carry(3u, holder);
        server.move(holder, glm::vec3{5.0f, 6.0f, 5.0f});
        const glm::vec3 held = server.position(prop);
        require(held.y > 5.0f);
        // The control: carried, nobody else may take it.
        server.move(other, glm::vec3{held.x, 1.0f, held.z + 1.0f});
        require(server.request(4u, other, KernelDomainAction_Pickup, item, prop)
                    .rejection_reason == KernelGameplayRequestRejection_Claimed);

        ne::ConfirmedDamage hit;
        hit.source_net_id = other;
        hit.target_net_id = holder;
        hit.source_peer = 4u;
        hit.damage = 1000u;
        const std::vector<ne::ConfirmedDamage> depleted = ne::apply_damage_applications(
            server.engine.world_, {hit}, server.engine.current_tick(),
            &server.engine.events_);
        require(depleted.size() == 1u);
        ne::EntityLifecycleSystem{}.enter_death_state(server.engine, depleted);

        require_set_down(server, item, prop);
        const glm::vec3 lying = server.position(prop);
        require(std::fabs(lying.x - held.x) < 1e-4f && std::fabs(lying.z - held.z) < 1e-4f);
        require(std::fabs(lying.y - 0.1f) < 1e-3f);  // on the ground, not at 6 m
        // The body moving on does not take it along.
        server.move(holder, glm::vec3{-5.0f, 1.0f, -5.0f});
        require(server.position(prop) == lying);
        require(server.request(4u, other, KernelDomainAction_Pickup, item, prop).status ==
                KernelGameplayRequestStatus_Committed);
    }

    // Disconnect: set down where it was held, then anyone may take it.
    {
        Server server;
        const ne::NetId holder = server.join(3u, glm::vec3{0.0f, 1.0f, 0.0f});
        const ne::NetId other = server.join(4u, glm::vec3{0.0f, 1.0f, 2.0f});
        const auto [item, prop] = server.carry(3u, holder);
        server.move(holder, glm::vec3{8.0f, 1.0f, 0.0f});
        const glm::vec3 held = server.position(prop);
        server.move(other, glm::vec3{held.x, 1.0f, held.z + 1.0f});
        require(server.request(4u, other, KernelDomainAction_Pickup, item, prop)
                    .rejection_reason == KernelGameplayRequestRejection_Claimed);

        ne::TransportEvent event;
        event.type = ne::TransportEventType::kDisconnected;
        event.peer = 3u;
        server.engine.handle_server_disconnect(event);
        require(!server.engine.world_.find_entity(holder).has_value());
        require_set_down(server, item, prop);
        const glm::vec3 lying = server.position(prop);
        require(std::fabs(lying.x - held.x) < 1e-4f && std::fabs(lying.z - held.z) < 1e-4f);
        require(std::fabs(lying.y - 0.1f) < 1e-3f);
        require(server.request(4u, other, KernelDomainAction_Pickup, item, prop).status ==
                KernelGameplayRequestStatus_Committed);
    }

    std::puts("carry_drop_test passed");
    return 0;
}
