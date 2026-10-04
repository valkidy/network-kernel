// W6: a throw this client makes is drawn on its own timeline.
//
// A thrown bottle used to appear a round trip and an interpolation delay after
// the throw, because the prop only existed once the server's spawn arrived and
// was then drawn on the world timeline like everyone else's. A player running
// forward saw it leave from a metre behind them. Now the client starts the
// flight the moment it sends the request, on the instant its own player is
// drawn at, and when the outcome names the prop it re-bases on the authority's
// throw record and keeps drawing it there -- in the view the prop will have --
// instead of letting the world timeline draw it again later.
//
// Server and client over loopback, the throw sent by the client through
// submit_gameplay_request. The client's prediction timeline is pinned to the
// server's newest tick, which is where a client with no lead would have it.

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

#include "kernel/public/kernel_api.h"
#include "physics/public/physics_world.h"
#include "protocol/public/network_packets.h"
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
const glm::vec3 kPlayerAt{2.0f, 0.0f, -3.0f};

ne::LoopbackTransport* attach_loopback(
    ne::KernelEngine* engine,
    KernelMode mode,
    std::uint16_t port) {
    auto transport = std::make_unique<ne::LoopbackTransport>();
    ne::LoopbackTransport* loopback = transport.get();
    engine->transport_ = std::move(transport);
    engine->reset_runtime_state(mode);
    require(loopback->StartServer(port));
    return loopback;
}

// Everything one engine sent, handed to the other. What the client sends is
// addressed to the server; the server has to see it come from the client's
// peer, as a real connection would say.
void shuttle(ne::LoopbackTransport* from, ne::LoopbackTransport* to, ne::PeerId as_peer) {
    ne::TransportEvent event;
    while (from->PollClientEvent(event)) {
        require(to->SendClient(
            as_peer == 0u ? event.peer : as_peer,
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

// As 3008_fungible_shockwave_bottle.yaml: identity preserving, trajectory here.
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

struct Game {
    Game()
        : server(config(KernelMode_DedicatedServer)),
          client(config(KernelMode_Client)) {
        server_link = attach_loopback(&server, KernelMode_DedicatedServer, 7801);
        client_link = attach_loopback(&client, KernelMode_Client, 7802);
        install_catalog(&server);
        install_catalog(&client);
        player = server.world_.spawn_player(kPeer, kPlayerAt);
        server.peer_sessions_.push_back(
            ne::KernelEngine::PeerSession{kPeer, player, 0, true, {}});
        KernelInventoryContainerId container = 0;
        require(server.server_create_inventory_container(player, 2, &container));
        require(server.server_create_inventory_item(
            kBottleItemTemplateId, 3, container, &stack));
        tick_us = static_cast<std::uint64_t>(
            server.tick_loop_.fixed_delta_seconds() * 1000000.0f);
        for (int index = 0; index < 8; ++index) {
            step();
        }
        client.local_player_net_id_ = player;
        client.local_client_peer_id_ = kPeer;
        client.has_welcome_ = true;
        // The player's prediction, standing where the server has it.
        client.has_predicted_local_entity_ = true;
        client.predicted_local_entity_.net_id = player;
        client.predicted_local_entity_.position = kPlayerAt;
        pin_prediction();
        // The client learned the stack from the inventory sync.
        require(client.item_store_.find_item(stack) != nullptr);
    }

    static KernelConfig config(KernelMode mode) {
        KernelConfig value{};
        value.mode = mode;
        value.tick.server_tick_rate = 30;
        value.tick.snapshot_rate = 15;
        value.max_events = 1024;
        value.max_render_states = 256;
        return value;
    }

    // The prediction timeline at the server's newest simulated tick.
    void pin_prediction() {
        client.predicted_character_tick_ = server.current_tick() - 1u;
        client.predicted_local_state_time_us_ = client.client_local_time_us_;
    }

    // One tick: the client's requests reach the server before it simulates,
    // and whatever the server sent reaches the client after.
    void step() {
        client.client_local_time_us_ += tick_us;
        shuttle(client_link, server_link, kPeer);
        server.poll_transport();
        server.simulate_tick();
        shuttle(server_link, client_link, 0u);
        client.poll_transport();
        client.advance_predicted_throws(server.tick_loop_.fixed_delta_seconds());
        if (client.has_predicted_local_entity_) pin_prediction();
    }

    KernelGameplayRequest throw_request(std::uint64_t request_id, std::uint32_t quantity) const {
        KernelGameplayRequest request{};
        request.struct_size = sizeof(request);
        request.request_id = request_id;
        request.instigator_net_id = player;
        request.domain_action = KernelDomainAction_Throw;
        request.requested_quantity = quantity;
        request.selected_item_instance_id = stack;
        request.throw_direction = KernelVec3{1.0f, 0.0f, 0.0f};
        return request;
    }

    std::vector<RenderEntityState> drawn_props() {
        std::array<RenderEntityState, 32> states{};
        const std::uint32_t count = client.get_render_states_at_time(
            client.client_local_time_us_,
            states.data(),
            static_cast<std::uint32_t>(states.size()));
        std::vector<RenderEntityState> props;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (states[index].entity_type ==
                static_cast<std::uint16_t>(ne::EntityType::kProp)) {
                props.push_back(states[index]);
            }
        }
        return props;
    }

    ne::KernelEngine server;
    ne::KernelEngine client;
    ne::LoopbackTransport* server_link = nullptr;
    ne::LoopbackTransport* client_link = nullptr;
    ne::NetId player = 0;
    KernelItemInstanceId stack = 0;
    std::uint64_t tick_us = 0;
};

glm::vec3 at(const RenderEntityState& state) {
    return glm::vec3{state.position.x, state.position.y, state.position.z};
}

// Drawn from the moment it is sent, at the hand; then drawn where the server
// flies it at the same instant, in the prop's own view, and only once.
void a_throw_is_drawn_on_the_throwers_timeline() {
    Game game;
    // The prediction has the player a little off where the server does, as it
    // will: the curve it starts on is then not the server's, and only
    // re-basing on the throw record puts the bottle where the server has it.
    const glm::vec3 predicted_at = kPlayerAt + glm::vec3{0.4f, 0.0f, 0.0f};
    game.client.predicted_local_entity_.position = predicted_at;
    require(game.drawn_props().empty());
    const std::uint32_t throw_tick = game.server.current_tick();
    require(game.client.submit_gameplay_request(game.throw_request(1, 1)));

    // Nothing has reached the server yet.
    const std::vector<RenderEntityState> first = game.drawn_props();
    require(first.size() == 1u);
    require(first[0].status == RenderEntityStatus_Predicted);
    require(first[0].net_id == 0u);
    require(first[0].entity_id != 0u);
    require(first[0].template_id == kBottleItemTemplateId);
    require(glm::length(at(first[0]) - (predicted_at + glm::vec3{0.0f, 1.0f, 0.0f})) < 0.01f);

    // The request lands, the server throws, the outcome and the throw record
    // come back.
    game.step();
    KernelGameplayRequestOutcome outcome{};
    require(game.client.poll_gameplay_request_outcomes(&outcome, 1) == 1u);
    require(outcome.status == KernelGameplayRequestStatus_Committed);
    const ne::NetId bottle = outcome.prop_entity_id;
    require(bottle != 0u);
    require(game.client.predicted_throws_.size() == 1u);
    require(game.client.predicted_throws_[0].net_id == bottle);

    const entt::entity thrown = *game.server.world_.find_entity(bottle);
    const ne::ThrownPropMotion motion =
        game.server.world_.registry().get<ne::ThrownPropMotion>(thrown);
    const float dt = game.server.tick_loop_.fixed_delta_seconds();
    const double flight_start_seconds =
        static_cast<double>(throw_tick - 1u) * static_cast<double>(dt);
    int compared = 0;
    for (int frame = 0; frame < 12; ++frame) {
        game.step();
        // Let any correction from re-basing run out, as frames do.
        game.client.advance_predicted_projectile_corrections(0.5f);
        const std::vector<RenderEntityState> props = game.drawn_props();
        require(props.size() == 1u);
        require(props[0].net_id == bottle);
        require(props[0].entity_id == game.client.entity_id_for_net_id(bottle));
        require(props[0].status == RenderEntityStatus_Predicted);
        // At the instant the player is drawn, not an interpolation delay ago.
        const double now_seconds =
            static_cast<double>(game.client.prediction_timeline_now_us()) / 1000000.0;
        const glm::vec3 authority = ne::projectile_position_at(
            motion.spawn_position,
            motion.initial_velocity,
            motion.motion_model,
            motion.gravity,
            static_cast<float>(now_seconds - flight_start_seconds));
        require(glm::length(at(props[0]) - authority) < 0.05f);
        // And well ahead of where the world timeline has it.
        const double world_seconds =
            static_cast<double>(game.client.render_server_time_us_) / 1000000.0;
        require(now_seconds - world_seconds > 0.1);
        ++compared;
    }
    require(compared == 12);
    require(game.client.predicted_throws_[0].anchored);

    // It ends where it is drawn: the despawn is applied on arrival instead of
    // being held for the world timeline, and the prediction goes with it.
    ne::EntityLifecycleSystem{}.destroy_entity(
        game.server, bottle, KernelDespawnReason_Destroyed);
    game.step();
    require(game.client.predicted_throws_.empty());
    require(game.client.deferred_flight_despawns_.empty());
    require(game.drawn_props().empty());
}

// Until the authority says where the flight ended, the client holds it where it
// sees it strike rather than drawing it through the wall for a round trip --
// and still does once the curve is re-based on the throw record. Here only the
// client has the wall, so the server's bottle flies on and no end ever comes.
// A wall 6 m ahead that only the client has.
float add_client_wall(Game& game) {
    game.client.prediction_physics_world_ =
        std::make_unique<ne::physics::PhysicsWorld>(ne::physics::PhysicsWorldConfig{});
    const float wall_x = kPlayerAt.x + 6.0f;
    ne::physics::CollisionObjectDescriptor wall{};
    wall.identity.collider_id = 900;
    wall.identity.kind = ne::physics::CollisionObjectKind::kStaticObstacle;
    wall.identity.layer = ne::physics::CollisionLayer::kStaticObstacle;
    wall.shape.type = ne::physics::CollisionShapeType::kBox;
    wall.shape.half_extents = glm::vec3{0.5f, 5.0f, 5.0f};
    wall.position = glm::vec3{wall_x + 0.5f, 0.0f, kPlayerAt.z};
    wall.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    std::string error;
    require(game.client.prediction_physics_world_->upsert_object(wall, &error));
    return wall_x;
}

void a_throw_stops_where_the_client_sees_it_strike() {
    Game game;
    const float wall_x = add_client_wall(game);

    require(game.client.submit_gameplay_request(game.throw_request(3, 1)));
    // 24 m/s: through where the wall is inside a few ticks, then on for many.
    float furthest = 0.0f;
    for (int frame = 0; frame < 20; ++frame) {
        game.step();
        game.client.advance_predicted_projectile_corrections(0.5f);
        const std::vector<RenderEntityState> props = game.drawn_props();
        require(props.size() == 1u);
        furthest = std::max(furthest, props[0].position.x);
    }
    require(game.client.predicted_throws_.size() == 1u);
    require(game.client.predicted_throws_[0].anchored);
    require(game.client.predicted_throws_[0].landed);
    require(std::abs(furthest - wall_x) < 0.05f);
    // The server's copy is metres past it.
    const glm::vec3 server_bottle = game.server.world_.registry()
        .get<ne::Transform>(*game.server.world_.find_entity(
            game.client.predicted_throws_[0].net_id))
        .position;
    require(server_bottle.x > wall_x + 5.0f);
}

// A throw the server refuses leaves nothing behind.
void a_refused_throw_is_taken_back() {
    Game game;
    // More than the stack holds: the server refuses; the client cannot tell.
    require(game.client.submit_gameplay_request(game.throw_request(5, 7)));
    require(game.drawn_props().size() == 1u);
    game.step();
    KernelGameplayRequestOutcome outcome{};
    require(game.client.poll_gameplay_request_outcomes(&outcome, 1) == 1u);
    require(outcome.status != KernelGameplayRequestStatus_Committed);
    require(game.client.predicted_throws_.empty());
    require(game.drawn_props().empty());
}

// With no answer at all, it is not drawn forever.
void an_unanswered_throw_times_out() {
    Game game;
    require(game.client.submit_gameplay_request(game.throw_request(9, 1)));
    // Swallow the request instead of delivering it.
    ne::TransportEvent dropped;
    while (game.client_link->PollClientEvent(dropped)) {
    }
    for (int index = 0; index < 59; ++index) {
        game.step();
    }
    require(game.client.predicted_throws_.size() == 1u);
    game.step();
    game.step();
    require(game.client.predicted_throws_.empty());
}


std::vector<std::string> own_throw_lines() {
    std::vector<std::string> lines;
    KernelLogMessage messages[16]{};
    std::uint32_t count = 0u;
    while ((count = Kernel_PollLogMessages(messages, 16u)) > 0u) {
        for (std::uint32_t index = 0u; index < count; ++index) {
            const std::string text = messages[index].text;
            if (text.rfind("own throw ", 0) == 0) {
                lines.push_back(text);
            }
        }
    }
    return lines;
}

// Throws until the client has it land on its wall, then runs `extra` ticks.
// Returns the index of the landed throw.
void land_on_the_wall(Game& game, std::uint64_t request_id) {
    require(game.client.submit_gameplay_request(game.throw_request(request_id, 1)));
    for (int frame = 0; frame < 20; ++frame) {
        game.step();
        const auto& throws = game.client.predicted_throws_;
        if (!throws.empty() && throws.back().landed) {
            // Seen at this tick, landed somewhere inside the tick before it:
            // dated back to then, not to when the sweep noticed.
            require(throws.back().landed_client_us < game.client.client_local_time_us_);
            require(throws.back().landed_client_us + game.tick_us >
                game.client.client_local_time_us_);
            return;
        }
    }
    require(false);
}

void own_blast(Game& game, ne::NetId blast, const glm::vec3& at) {
    ne::ProjectileSpawnBatchPacket packet{};
    packet.server_tick = game.server.current_tick();
    packet.catalog_hash = game.client.catalog_hash_;
    ne::ProjectileSpawnGroup group{};
    group.projectile_template_id = kTrajectoryTemplateId;
    ne::ProjectileSpawnRecord record{};
    record.projectile_net_id = blast;
    record.owner_peer = kPeer;
    record.action_instance_id = 4000u + blast;
    record.spawn_position = at;
    group.records.push_back(record);
    packet.groups.push_back(group);
    game.client.handle_client_projectile_spawn_batch(packet);
}

std::string expect_ms(std::uint64_t from_us, std::uint64_t to_us) {
    return std::to_string((to_us - from_us) / 1000u) + " ms after its predicted landing";
}

// W6 follow-up measurement: how long a bottle sits where the client saw it
// land before the authority's despawn and blast reach it, and how far that
// blast is from the landing. Logged whichever of the two arrives first.
void the_wait_after_a_predicted_landing_is_logged() {
    require(Kernel_PollLogMessages(nullptr, 0u) == 0u);  // start capture
    own_throw_lines();
    Game game;
    add_client_wall(game);

    // Despawn first, then the blast.
    land_on_the_wall(game, 11);
    const ne::KernelEngine::PredictedThrow first = game.client.predicted_throws_.back();
    for (int frame = 0; frame < 4; ++frame) {
        game.step();
    }
    ne::EntityLifecycleSystem{}.destroy_entity(
        game.server, first.net_id, KernelDespawnReason_Destroyed);
    game.step();
    require(game.client.predicted_throws_.empty());
    const std::uint64_t despawned_us = game.client.client_local_time_us_;
    own_blast(game, 801, first.landed_position + glm::vec3{0.5f, 0.0f, 0.0f});
    std::vector<std::string> lines = own_throw_lines();
    require(lines.size() == 2u);
    const std::string id = "own throw " + std::to_string(first.net_id) + ": ";
    require(lines[0] == id + "despawn arrived " + expect_ms(first.landed_client_us, despawned_us));
    require(lines[1].rfind(id + "blast 801 (projectile template ", 0) == 0);
    require(lines[1].find(expect_ms(first.landed_client_us, despawned_us) + ", 0.50 m from it") !=
        std::string::npos);

    // A second blast is not pinned on the same landing.
    own_blast(game, 802, first.landed_position);
    require(own_throw_lines().empty());

    // The blast first, while the throw is still predicted.
    land_on_the_wall(game, 12);
    const ne::KernelEngine::PredictedThrow second = game.client.predicted_throws_.back();
    game.step();
    own_blast(game, 803, second.landed_position);
    lines = own_throw_lines();
    require(lines.size() == 1u);
    require(lines[0].find("own throw " + std::to_string(second.net_id) + ": blast 803") == 0u);
    require(lines[0].find(
        expect_ms(second.landed_client_us, game.client.client_local_time_us_) +
        ", 0.00 m from it") != std::string::npos);
    // Nor a second blast on this one.
    own_blast(game, 804, second.landed_position);
    require(own_throw_lines().empty());
}

}  // namespace

int main() {
    a_throw_is_drawn_on_the_throwers_timeline();
    a_throw_stops_where_the_client_sees_it_strike();
    a_refused_throw_is_taken_back();
    an_unanswered_throw_times_out();
    the_wait_after_a_predicted_landing_is_logged();
    std::printf("own_throw_prediction_test: PASS\n");
    return 0;
}
