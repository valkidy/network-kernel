// A bottle thrown the way the game throws one, from the server's world to the
// client's drawing of it.
//
// The anchored flight -- the server leaving an in-flight prop out of the
// snapshot, the client drawing it from its throw record instead -- was keyed on
// the prop's ENTITY template naming a throw trajectory. Every such test set that
// field by hand. The shipped catalog never does: a bottle's trajectory lives on
// its ITEM template (`throw.trajectory_projectile`), and no entity template has
// a `throw:` block at all. So in play the bottle stayed in the snapshot,
// competing with the actors for slots, and the client drew it from whatever
// samples got through -- held when they stopped, then a jump of several metres
// when the next one arrived. Measured in play: 4-11 m jumps per throw.
//
// Here the entity template names no trajectory, as in the catalog; only the item
// template does, and the throw goes through the real gameplay request.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "simulation/public/simulation.h"
#include "transport/public/loopback_transport.h"
#include "world/public/components.h"

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

constexpr std::uint32_t kTrajectoryTemplateId = 7;
constexpr std::uint32_t kBottleEntityTemplateId = 200;
constexpr std::uint32_t kBottleItemTemplateId = 10;
constexpr ne::PeerId kPeer = 1;
// A second player, far away when the bottle is thrown: it starts seeing the
// bottle only once it is in the air.
constexpr ne::PeerId kObserverPeer = 2;

ne::LoopbackTransport* attach_loopback(
    ne::KernelEngine* engine,
    KernelMode mode,
    std::uint16_t port) {
    auto transport = std::make_unique<ne::LoopbackTransport>();
    ne::LoopbackTransport* loopback = transport.get();
    engine->transport_ = std::move(transport);
    // After the reset, which stops whatever transport it is holding.
    engine->reset_runtime_state(mode);
    require(loopback->StartServer(port));
    return loopback;
}

// The server's outgoing traffic, each packet to the client of the peer it was
// addressed to.
void shuttle(
    ne::LoopbackTransport* from,
    ne::LoopbackTransport* thrower,
    ne::LoopbackTransport* observer) {
    ne::TransportEvent event;
    while (from->PollClientEvent(event)) {
        ne::LoopbackTransport* to = event.peer == kObserverPeer ? observer : thrower;
        require(to->SendClient(
            event.peer,
            event.payload.data(),
            static_cast<std::uint32_t>(event.payload.size()),
            event.mode,
            event.channel));
    }
}

ne::RuntimeProjectileTemplate trajectory() {
    ne::RuntimeProjectileTemplate value{};
    value.projectile_template_id = kTrajectoryTemplateId;
    value.projectile_type = ne::ProjectileType::kStandard;
    value.motion_model = ne::ProjectileMotionModel::kParabolic;
    value.sync_mode = ne::ProjectileSyncMode::kLocalPredictedDeterministic;
    value.speed = 24.0f;
    value.gravity = glm::vec3{0.0f, -9.81f, 0.0f};
    return value;
}

// As the catalog loader builds a bottle's prop: no `throw:` block, so no
// trajectory on the entity template.
KernelEntityTemplateDefinition bottle_entity_template() {
    KernelEntityTemplateDefinition prop{};
    prop.struct_size = sizeof(prop);
    prop.entity_template_id = kBottleEntityTemplateId;
    prop.entity_type = KernelEntityType_Prop;
    prop.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM | KERNEL_ENTITY_COMPONENT_VELOCITY;
    prop.ai.struct_size = sizeof(prop.ai);
    prop.movement.struct_size = sizeof(prop.movement);
    prop.prop.struct_size = sizeof(prop.prop);
    prop.prop.interaction.struct_size = sizeof(prop.prop.interaction);
    return prop;
}

// The trajectory is here, as in fungible_shockwave_bottle.yaml.
KernelItemTemplateDefinition bottle_item_template() {
    KernelItemTemplateDefinition item{};
    item.struct_size = sizeof(item);
    item.item_template_id = kBottleItemTemplateId;
    item.item_mode = KernelItemMode_Fungible;
    item.max_stack = 3;
    item.capability_flags =
        KernelItemCapability_Pickupable | KernelItemCapability_Throwable;
    item.entity_template_id = kBottleEntityTemplateId;
    item.interaction_range = 3.0f;
    item.throw_policy.struct_size = sizeof(item.throw_policy);
    item.throw_policy.mode = KernelItemThrowMode_IdentityPreserving;
    item.throw_policy.trajectory_projectile_template_id = kTrajectoryTemplateId;
    item.use_policy.struct_size = sizeof(item.use_policy);
    item.item_used_trigger.struct_size = sizeof(item.item_used_trigger);
    return item;
}

void install_catalog(ne::KernelEngine* engine) {
    engine->world_.set_projectile_templates({trajectory()});
    engine->catalog_runtime_.projectile_templates.push_back(trajectory());
    engine->entity_templates_.push_back(bottle_entity_template());
    engine->item_templates_.push_back(bottle_item_template());
    std::string error;
    require(engine->item_store_.set_templates(engine->item_templates_, &error));
}

}  // namespace

int main() {
    KernelConfig server_config{};
    server_config.mode = KernelMode_DedicatedServer;
    server_config.tick.server_tick_rate = 30;
    server_config.tick.snapshot_rate = 15;
    server_config.max_events = 1024;
    server_config.max_render_states = 256;
    ne::KernelEngine server(server_config);
    ne::LoopbackTransport* server_link =
        attach_loopback(&server, KernelMode_DedicatedServer, 7795);
    install_catalog(&server);

    KernelConfig client_config = server_config;
    client_config.mode = KernelMode_Client;
    ne::KernelEngine client(client_config);
    ne::LoopbackTransport* client_link =
        attach_loopback(&client, KernelMode_Client, 7796);
    install_catalog(&client);
    ne::KernelEngine observer(client_config);
    ne::LoopbackTransport* observer_link =
        attach_loopback(&observer, KernelMode_Client, 7797);
    install_catalog(&observer);

    const ne::NetId player = server.world_.spawn_player(kPeer, glm::vec3{0.0f});
    server.peer_sessions_.push_back(
        ne::KernelEngine::PeerSession{kPeer, player, 0, true, {}});
    // Well past the relevance radius.
    const ne::NetId observer_player =
        server.world_.spawn_player(kObserverPeer, glm::vec3{0.0f, 0.0f, -90.0f});
    server.peer_sessions_.push_back(
        ne::KernelEngine::PeerSession{kObserverPeer, observer_player, 0, true, {}});
    KernelInventoryContainerId container = 0;
    require(server.server_create_inventory_container(player, 2, &container));
    KernelItemInstanceId stack = 0;
    require(server.server_create_inventory_item(
        kBottleItemTemplateId, 3, container, &stack));

    const auto step = [&]() {
        server.simulate_tick();
        shuttle(server_link, client_link, observer_link);
        client.poll_transport();
        observer.poll_transport();
    };
    for (int index = 0; index < 8; ++index) {
        step();
    }
    client.local_player_net_id_ = player;
    client.local_client_peer_id_ = kPeer;
    observer.local_player_net_id_ = observer_player;
    observer.local_client_peer_id_ = kObserverPeer;

    KernelGameplayRequest throw_request{};
    throw_request.struct_size = sizeof(throw_request);
    throw_request.requester_peer = kPeer;
    throw_request.request_id = 1;
    throw_request.instigator_net_id = player;
    throw_request.domain_action = KernelDomainAction_Throw;
    throw_request.requested_quantity = 1;
    throw_request.selected_item_instance_id = stack;
    throw_request.throw_direction = KernelVec3{1.0f, 0.0f, 0.0f};
    // current_tick() is the tick about to be simulated. The throw happens before
    // it, and that tick's movement is the flight's first: age 1 at its end.
    const std::uint32_t throw_tick = server.current_tick();
    require(server.server_submit_gameplay_request(throw_request));
    KernelGameplayRequestOutcome outcome{};
    require(server.poll_gameplay_request_outcomes(&outcome, 1) == 1);
    require(outcome.status == KernelGameplayRequestStatus_Committed);
    const ne::NetId bottle = outcome.prop_entity_id;
    const entt::entity thrown = *server.world_.find_entity(bottle);
    require(server.world_.registry().all_of<ne::ThrownPropMotion>(thrown));

    // Server: an in-flight bottle the client can draw on its own is not a
    // snapshot sample worth a slot.
    require(server.is_anchored_in_flight_prop(bottle));

    step();
    const auto replicated_on = [&](const ne::KernelEngine& engine)
        -> const ne::KernelEngine::ClientReplicatedEntity* {
        for (const ne::KernelEngine::ClientReplicatedEntity& entity :
             engine.client_replicated_entities_) {
            if (entity.net_id == bottle) return &entity;
        }
        return nullptr;
    };
    const auto replicated = [&]() { return replicated_on(client); };
    require(replicated_on(observer) == nullptr);
    require(replicated() != nullptr);
    require(replicated()->has_thrown_anchor);

    // Client: drawn from the throw record, on the curve the server flies it on,
    // at the instant the world timeline is drawn -- every frame until it lands.
    const ne::ThrownPropMotion motion =
        server.world_.registry().get<ne::ThrownPropMotion>(thrown);
    const float dt = server.tick_loop_.fixed_delta_seconds();
    std::array<RenderEntityState, 16> states{};
    int compared_with_authority = 0;
    for (int frame = 0; frame < 12; ++frame) {
        step();
        const std::uint32_t count = client.get_render_states_at_time(
            1000000, states.data(), static_cast<std::uint32_t>(states.size()));
        const RenderEntityState* drawn = nullptr;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (states[index].net_id == bottle) drawn = &states[index];
        }
        require(drawn != nullptr);
        glm::vec3 anchored_position{0.0f};
        glm::vec3 anchored_velocity{0.0f};
        require(client.thrown_prop_render_transform(
            *replicated(),
            client.render_server_time_us_,
            &anchored_position,
            &anchored_velocity));
        require(glm::length(
                    glm::vec3{drawn->position.x, drawn->position.y, drawn->position.z} -
                    anchored_position) < 0.01f);
        // And that curve is the one the server flies: age a is the end of tick
        // throw_tick - 1 + a. Only once the render instant is past the throw --
        // before it the curve is held at its start.
        const double render_seconds =
            static_cast<double>(client.render_server_time_us_) / 1000000.0;
        const double flight_start_seconds =
            static_cast<double>(throw_tick - 1u) * static_cast<double>(dt);
        if (render_seconds > flight_start_seconds + dt) {
            const glm::vec3 authority = ne::projectile_position_at(
                motion.spawn_position,
                motion.initial_velocity,
                motion.motion_model,
                motion.gravity,
                static_cast<float>(render_seconds - flight_start_seconds));
            require(glm::length(anchored_position - authority) < 0.05f);
            ++compared_with_authority;
        }
    }
    // The render instant trails by the interpolation delay; most of the frames
    // above are past the throw, and they have to be for this to prove anything.
    require(compared_with_authority >= 6);

    // The observer walks up to the bottle while it is still in the air. It was
    // never sent the record that started the flight, and the server has stopped
    // sampling the bottle for everyone, so the spawn alone would leave it frozen
    // where it appeared until it lands. It has to be handed the flight with it.
    {
        const glm::vec3 bottle_now =
            server.world_.registry().get<ne::Transform>(thrown).position;
        server.world_.registry().get<ne::Transform>(
            *server.world_.find_entity(observer_player)).position =
            glm::vec3{bottle_now.x, 0.0f, bottle_now.z - 5.0f};
    }
    for (int index = 0; index < 4 && replicated_on(observer) == nullptr; ++index) {
        step();
    }
    require(replicated_on(observer) != nullptr);
    require(replicated_on(observer)->has_thrown_anchor);
    int observer_compared = 0;
    for (int frame = 0; frame < 12; ++frame) {
        step();
        const std::uint32_t count = observer.get_render_states_at_time(
            1000000, states.data(), static_cast<std::uint32_t>(states.size()));
        const RenderEntityState* drawn = nullptr;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (states[index].net_id == bottle) drawn = &states[index];
        }
        require(drawn != nullptr);
        const ne::KernelEngine::ClientReplicatedEntity& seen = *replicated_on(observer);
        const double render_seconds =
            static_cast<double>(observer.render_server_time_us_) / 1000000.0;
        const double anchor_seconds =
            static_cast<double>(seen.thrown_anchor_tick) * static_cast<double>(dt);
        if (render_seconds > anchor_seconds) {
            const double flight_start_seconds =
                static_cast<double>(throw_tick - 1u) * static_cast<double>(dt);
            const glm::vec3 authority = ne::projectile_position_at(
                motion.spawn_position,
                motion.initial_velocity,
                motion.motion_model,
                motion.gravity,
                static_cast<float>(render_seconds - flight_start_seconds));
            require(glm::length(
                        glm::vec3{drawn->position.x, drawn->position.y, drawn->position.z} -
                        authority) < 0.05f);
            ++observer_compared;
        }
    }
    require(observer_compared >= 6);

    // A prop flying some other way -- launched by an impulse, which moves it on
    // the linear model -- is not on the curve the client would draw, so it keeps
    // its snapshot samples.
    server.world_.registry().get<ne::ThrownPropMotion>(thrown).motion_model =
        ne::ProjectileMotionModel::kLinear;
    require(!server.is_anchored_in_flight_prop(bottle));
    std::printf("item_thrown_prop_end_to_end_test: PASS\n");
    return 0;
}
