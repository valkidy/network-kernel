// lock_strike over the wire (docs/LOCK_STRIKE_ACTION_PLAN.md P4): a dedicated
// server and two pure clients over loopback. A bottle that breaks runs its
// lock_strike as nobody, for its thrower's peer, and every client that sees a
// target draws what it struck: an instant strike as a lock strike event --
// flagged as a graph's, its arcs to the targets struck and the rest into the
// volume -- and a missile from where it was launched, with no shooter. A
// repeat is run by the server's own tick. Who is sent what is decided by the
// targets (D13); a strike that locked nobody goes to whoever is near where it
// was taken. However many there are, the records stay inside their share of
// the first packet, and the rest follow (D14). The listen host's own client
// is sent them too.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "protocol/public/network_packets.h"
#include "transport/public/listen_server_transport.h"
#include "transport/public/loopback_transport.h"
#include "world/public/components.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

#include "simulation/public/action_graph.h"
#include "simulation/src/systems.h"

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

constexpr ne::PeerId kThrowerPeer = 1;
constexpr ne::PeerId kObserverPeer = 2;
constexpr std::uint32_t kSphere = 54;
constexpr std::uint32_t kShotTemplateId = 35;
constexpr std::uint16_t kDamage = 10;
constexpr std::uint32_t kLockSize = 2;
constexpr float kRadius = 10.0f;
// A missile crosses the whole radius in this: a metre a tick.
constexpr std::uint32_t kLongestFlight = 10;
constexpr std::uint32_t kPlayerActorTemplate = 71;
constexpr std::uint32_t kEnemyActorTemplate = 72;
// A packet's header on top of its payload budget.
constexpr std::size_t kPacketHeaderBytes = 28;

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

ne::RuntimeProjectileTemplate shot() {
    ne::RuntimeProjectileTemplate projectile{};
    projectile.projectile_template_id = kShotTemplateId;
    projectile.damage = kDamage;
    projectile.max_hit_count = kLockSize;
    projectile.lifetime_ticks = kLongestFlight;
    return projectile;
}

KernelColliderTemplateDefinition sphere() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = kSphere;
    collider.shape_type = KernelColliderShapeType_Sphere;
    collider.shape_params = KernelVec4{kRadius, 0.0f, 0.0f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Damage;
    collider.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;
    return collider;
}

KernelColliderTemplateDefinition hit_collider(
    std::uint32_t id, std::uint32_t layer, float center_y) {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = id;
    collider.shape_type = KernelColliderShapeType_Aabb;
    collider.center = KernelVec3{0.0f, center_y, 0.0f};
    collider.shape_params = KernelVec4{0.3f, center_y, 0.3f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Hit;
    collider.layer_mask = layer;
    return collider;
}

KernelActorTemplateDefinition actor_template(
    std::uint32_t id, std::uint16_t actor_type, std::uint32_t collider) {
    KernelActorTemplateDefinition definition{};
    definition.struct_size = sizeof(definition);
    definition.actor_template_id = id;
    definition.entity_type = KernelEntityType_Actor;
    definition.actor_type = actor_type;
    definition.collider_template_id = collider;
    definition.vision.struct_size = sizeof(KernelAgentVisionConfig);
    return definition;
}

// What server and client both have from the catalog: the actors' hit boxes,
// the lock's sphere and its shot.
void install_catalog(ne::KernelEngine& engine) {
    engine.collider_templates_.push_back(
        hit_collider(51, KERNEL_COLLISION_LAYER_PLAYER_SIDE, 0.9f));
    engine.collider_templates_.push_back(
        hit_collider(52, KERNEL_COLLISION_LAYER_HOSTILE_SIDE, 0.5f));
    engine.collider_templates_.push_back(sphere());
    engine.actor_templates_.push_back(
        actor_template(kPlayerActorTemplate, KernelActorType_Player, 51));
    engine.actor_templates_.push_back(
        actor_template(kEnemyActorTemplate, KernelActorType_Agent, 52));
    engine.catalog_runtime_.projectile_templates.push_back(shot());
}

std::uint16_t hp(ne::KernelEngine& server, ne::NetId net_id) {
    return server.world_.registry()
        .get<ne::Health>(*server.world_.find_entity(net_id))
        .hp;
}

KernelActionTriggerDefinition strike_trigger(
    std::uint8_t mode,
    std::uint16_t repeat_count,
    std::uint32_t repeat_interval) {
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(trigger);
    trigger.action_count = 1u;
    KernelActionDefinition& action = trigger.actions[0];
    action.action_type = KernelEntityTriggerActionType_LockStrike;
    action.lock_collider_template_id = kSphere;
    action.spawn_projectile_template_id = kShotTemplateId;
    action.lock_mode = mode;
    action.lock_origin_lift = 1.0f;
    action.lock_repeat_count = repeat_count;
    action.lock_repeat_interval_ticks = repeat_interval;
    action.position_source = KernelEventVec3Source_Position;
    action.direction_source = KernelEventVec3Source_Position;
    return trigger;
}

// A bottle breaking at `where`, as its collision trigger runs it: nobody
// instigates, the thrower's peer owns it, and the graph runs as `bottle`.
void break_bottle(
    ne::KernelEngine& server,
    const KernelActionTriggerDefinition& trigger,
    const glm::vec3& where,
    ne::NetId bottle = 0u) {
    static std::uint64_t request_id = 1000u;
    const std::optional<ne::CompiledActionGraphBinding> binding =
        ne::compile_action_trigger_definition(
            ne::TriggerEventType::kCollision, trigger);
    require(binding.has_value());
    const ne::TriggerEvent event{
        ne::TriggerEventType::kCollision,
        bottle,
        0u,
        0u,
        where,
        glm::vec3{1.0f, 0.0f, 0.0f},
        std::nullopt,
    };
    ne::ActionExecutionProvenance provenance;
    provenance.request_id = ++request_id;
    provenance.server_tick = server.current_tick();
    provenance.owner_peer = kThrowerPeer;
    std::vector<ne::ActionGraphCommand> commands;
    std::string error;
    require(ne::evaluate_action_graph(
        *binding, bottle, event, provenance, &commands, &error));
    require(ne::execute_action_graph_command_batch(
        server,
        ne::ActionGraphCommandBatch{event, provenance, 1u, std::move(commands)},
        static_cast<std::uint64_t>(
            server.current_tick() * server.fixed_delta_seconds() * 1000000.0f)));
}

struct Arena {
    ne::KernelEngine server;
    ne::LoopbackTransport* server_link = nullptr;
    struct Client {
        ne::PeerId peer = 0;
        std::unique_ptr<ne::KernelEngine> engine;
        ne::LoopbackTransport* link = nullptr;
        std::vector<KernelLockStrikeEvent> strikes;
        // The render clock's tick when each was polled.
        std::vector<std::uint32_t> polled_at;
    };
    std::vector<Client> clients;
    ne::NetId thrower = 0;
    ne::NetId observer = 0;
    std::uint64_t tick_us = 0;
    std::size_t largest_snapshot_packet = 0;

    explicit Arena(std::uint16_t port) : server(config(KernelMode_DedicatedServer)) {
        server_link = attach_loopback(&server, KernelMode_DedicatedServer, port);
        add_client(kThrowerPeer, static_cast<std::uint16_t>(port + 1u));
        add_client(kObserverPeer, static_cast<std::uint16_t>(port + 2u));
        install_catalog(server);
        for (Client& client : clients) {
            install_catalog(*client.engine);
        }
        // The thrower stands where the bottles break; the observer outside the
        // sphere, near enough to see what is in it.
        thrower = player(kThrowerPeer, glm::vec3{0.0f});
        observer = player(kObserverPeer, glm::vec3{-15.0f, 0.0f, 0.0f});
        clients[0].engine->local_client_peer_id_ = kThrowerPeer;
        clients[0].engine->local_player_net_id_ = thrower;
        clients[1].engine->local_client_peer_id_ = kObserverPeer;
        clients[1].engine->local_player_net_id_ = observer;
        tick_us = static_cast<std::uint64_t>(
            server.tick_loop_.fixed_delta_seconds() * 1000000.0f);
    }

    static KernelConfig config(KernelMode mode) {
        KernelConfig config{};
        config.mode = mode;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        config.max_events = 1024;
        config.max_render_states = 256;
        return config;
    }

    void add_client(ne::PeerId peer, std::uint16_t port) {
        Client client;
        client.peer = peer;
        client.engine = std::make_unique<ne::KernelEngine>(config(KernelMode_Client));
        client.link = attach_loopback(client.engine.get(), KernelMode_Client, port);
        clients.push_back(std::move(client));
    }

    ne::NetId player(ne::PeerId peer, const glm::vec3& position) {
        const ne::NetId net_id = server.world_.spawn_player(peer, position);
        server.world_.registry().get_or_emplace<ne::Health>(
            *server.world_.find_entity(net_id)) = ne::Health{100, 100};
        require(server.server_set_entity_actor_template(net_id, kPlayerActorTemplate));
        server.peer_sessions_.push_back(
            ne::KernelEngine::PeerSession{peer, net_id, 0, true, {}});
        return net_id;
    }

    ne::NetId enemy(const glm::vec3& position) {
        const ne::NetId net_id = server.world_.spawn_enemy(position);
        server.world_.registry().get_or_emplace<ne::Health>(
            *server.world_.find_entity(net_id)) = ne::Health{100, 100};
        require(server.server_set_entity_actor_template(net_id, kEnemyActorTemplate));
        server.world_.registry().get<ne::Hitbox>(*server.world_.find_entity(net_id)) =
            ne::Hitbox{{0.0f, 0.5f, 0.0f}, {0.3f, 0.5f, 0.3f}, 0};
        return net_id;
    }

    // One server tick, its output delivered to every client, every client
    // drawn one tick on and its lock strike events polled.
    void tick() {
        server.poll_transport();
        server.simulate_tick();
        ne::TransportEvent event;
        while (server_link->PollClientEvent(event)) {
            if (event.channel == ne::ChannelId::kSnapshot) {
                largest_snapshot_packet =
                    std::max(largest_snapshot_packet, event.payload.size());
            }
            for (Client& client : clients) {
                if (client.peer != event.peer) continue;
                require(client.link->SendClient(
                    event.peer,
                    event.payload.data(),
                    static_cast<std::uint32_t>(event.payload.size()),
                    event.mode,
                    event.channel));
            }
        }
        std::array<RenderEntityState, 64> states{};
        for (Client& client : clients) {
            client.engine->client_local_time_us_ += tick_us;
            client.engine->poll_transport();
            client.engine->get_render_states_at_time(
                client.engine->client_local_time_us_,
                states.data(),
                static_cast<std::uint32_t>(states.size()));
            std::array<KernelLockStrikeEvent, 16> polled{};
            const std::uint32_t count = client.engine->poll_lock_strike_events(
                polled.data(), static_cast<std::uint32_t>(polled.size()));
            client.strikes.insert(
                client.strikes.end(), polled.begin(), polled.begin() + count);
            client.polled_at.insert(
                client.polled_at.end(),
                count,
                static_cast<std::uint32_t>(
                    client.engine->current_render_time_us_ / tick_us));
            while (client.link->PollClientEvent(event)) {
                require(server_link->SendClient(
                    client.peer,
                    event.payload.data(),
                    static_cast<std::uint32_t>(event.payload.size()),
                    event.mode,
                    event.channel));
            }
        }
    }

    void settle() {
        for (int index = 0; index < 12; ++index) {
            tick();
        }
    }

    std::vector<KernelLockMissileView> missiles(std::size_t client) {
        std::array<KernelLockMissileView, 64> views{};
        const std::uint32_t count = clients[client].engine->get_lock_missiles(
            views.data(), static_cast<std::uint32_t>(views.size()));
        return {views.begin(), views.begin() + count};
    }
};

bool near(const KernelVec3& actual, const glm::vec3& expected, float tolerance) {
    return std::fabs(actual.x - expected.x) <= tolerance &&
           std::fabs(actual.y - expected.y) <= tolerance &&
           std::fabs(actual.z - expected.z) <= tolerance;
}

// Three times, six ticks apart, run by the server's own tick: both clients
// are handed each strike -- a graph's, by nobody, numbered in its run -- with
// its arcs to the two nearest it struck, and the server lands each.
void a_repeated_strike_reaches_every_client_that_sees_its_targets() {
    Arena arena(7951);
    const ne::NetId first = arena.enemy({3.0f, 0.0f, 0.0f});
    const ne::NetId second = arena.enemy({0.0f, 0.0f, 5.0f});
    const ne::NetId third = arena.enemy({0.0f, 0.0f, -8.0f});
    arena.settle();
    break_bottle(
        arena.server, strike_trigger(KernelLockStrikeMode_Instant, 3u, 6u),
        glm::vec3{0.0f});
    for (int index = 0; index < 40; ++index) {
        arena.tick();
    }
    require(hp(arena.server, first) == 100 - 3 * kDamage);
    require(hp(arena.server, second) == 100 - 3 * kDamage);
    require(hp(arena.server, third) == 100);
    require(arena.server.pending_lock_strikes_.empty());

    for (const Arena::Client& client : arena.clients) {
        require(client.strikes.size() == 3u);
        for (std::size_t index = 0; index < client.strikes.size(); ++index) {
            const KernelLockStrikeEvent& strike = client.strikes[index];
            require(strike.flags == KERNEL_LOCK_STRIKE_FLAG_GRAPH);
            require(strike.shooter_net_id == 0u);
            // No weapon to draw it as: the shot it struck with instead.
            require(strike.selection.weapon_id == 0u);
            require(strike.action_instance_id == kShotTemplateId);
            require(strike.commit_index == index);
            require(strike.server_tick ==
                    client.strikes[0].server_tick + 6u * index);
            // Handed over when the render clock reaches its launch, as a
            // weapon's strike is -- not as soon as its record arrives.
            require(client.polled_at[index] >= strike.server_tick);
            require(client.polled_at[index] <= strike.server_tick + 2u);
            require(strike.selection.arc_count == kLockSize);
            require(strike.selection.target_count == 2u);
            require(strike.selection.target_net_ids[0] == first);
            require(strike.selection.target_net_ids[1] == second);
            // Lifted a metre off where it broke; ending on the drawn centres.
            require(near(strike.selection.origin, {0.0f, 1.0f, 0.0f}, 0.001f));
            require(near(strike.selection.arc_ends[0], {3.0f, 0.5f, 0.0f}, 0.05f));
            require(near(strike.selection.arc_ends[1], {0.0f, 0.5f, 5.0f}, 0.05f));
        }
    }
}

// Missiles at both it locked, all at once: every client that sees them lists
// both -- from the lifted origin, by nobody -- and the server lands each on
// its arrival. Two from one strike are two missiles, not one sent twice.
void graph_missiles_leave_from_their_origin() {
    Arena arena(7955);
    const ne::NetId first = arena.enemy({3.0f, 0.0f, 0.0f});
    const ne::NetId second = arena.enemy({0.0f, 0.0f, 5.0f});
    arena.settle();
    break_bottle(
        arena.server, strike_trigger(KernelLockStrikeMode_Missile, 1u, 0u),
        glm::vec3{0.0f});
    const std::uint32_t launch = arena.server.current_tick();
    for (int index = 0; index < 3; ++index) {
        arena.tick();
    }
    for (std::size_t client = 0; client < arena.clients.size(); ++client) {
        const std::vector<KernelLockMissileView> views = arena.missiles(client);
        require(views.size() == 2u);
        std::set<std::uint32_t> targets;
        for (const KernelLockMissileView& view : views) {
            require(view.flags == KERNEL_LOCK_MISSILE_FLAG_FROM_ORIGIN);
            require(view.shooter_net_id == 0u);
            require(view.projectile_template_id == kShotTemplateId);
            require(view.commit_index == 0u);
            require(view.launch_tick == launch);
            require(near(view.origin, {0.0f, 1.0f, 0.0f}, 0.001f));
            targets.insert(view.target_net_id);
        }
        require((targets == std::set<std::uint32_t>{first, second}));
    }
    // From (0, 1, 0): 3.04 m and 5.02 m, four and six ticks at a metre a tick.
    for (int index = 0; index < 8; ++index) {
        arena.tick();
    }
    require(hp(arena.server, first) == 100 - kDamage);
    require(hp(arena.server, second) == 100 - kDamage);
}

// A bottle breaks with its own hit box still in the world: a prop's collider
// is a static obstacle, and a bottle's one-metre box sits on the very spot it
// broke. Its first strike runs inside its collision trigger, before the
// self-damage that spends it takes effect; the rest run after the bottle has
// left. Every strike is lifted the same metre and locks the same two.
void the_breaking_bottle_hides_nothing_from_its_first_strike() {
    Arena arena(7971);
    const ne::NetId first = arena.enemy({3.0f, 0.0f, 0.0f});
    const ne::NetId second = arena.enemy({0.0f, 0.0f, 5.0f});
    arena.settle();
    arena.server.physics_world_ = std::make_unique<ne::physics::PhysicsWorld>();
    arena.server.world_.set_collision_world(arena.server.physics_world_.get());
    constexpr ne::NetId kBottle = 9000u;
    constexpr std::uint32_t kBottleCollider = 9001u;
    ne::physics::CollisionObjectDescriptor bottle;
    bottle.identity = ne::physics::CollisionObjectIdentity{
        kBottle, kBottleCollider, ne::physics::kHitZoneUnscaled,
        ne::physics::CollisionObjectKind::kStaticObstacle,
        ne::physics::CollisionLayer::kStaticObstacle};
    bottle.shape.type = ne::physics::CollisionShapeType::kBox;
    bottle.shape.half_extents = glm::vec3{0.5f};
    // collision_damage_prop_hitbox: centre 0.5 m up, resting on the ground.
    bottle.position = glm::vec3{0.0f, 0.5f, 0.0f};
    bottle.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    std::string error;
    require(arena.server.physics_world_->upsert_object(bottle, &error));
    break_bottle(
        arena.server, strike_trigger(KernelLockStrikeMode_Instant, 3u, 6u),
        glm::vec3{0.0f},
        kBottle);
    require(arena.server.physics_world_->remove_object(kBottleCollider));
    for (int index = 0; index < 40; ++index) {
        arena.tick();
    }
    const Arena::Client& client = arena.clients[0];
    require(client.strikes.size() == 3u);
    for (const KernelLockStrikeEvent& strike : client.strikes) {
        require(near(strike.selection.origin, {0.0f, 1.0f, 0.0f}, 0.001f));
        require(strike.selection.target_count == 2u);
    }
    require(hp(arena.server, first) == 100 - 3 * kDamage);
    require(hp(arena.server, second) == 100 - 3 * kDamage);
}

// Who is sent what (D13). A strike far from both players strikes its target
// and is sent to nobody; one that locked nobody goes to whoever is within
// relevance of where it was taken -- here both -- and one far off to nobody.
// (The first two scenarios have the observer, 15 m off, sent strikes for
// targets it sees.)
void a_strike_goes_to_whoever_sees_it() {
    Arena arena(7959);
    const ne::NetId distant = arena.enemy({300.0f, 0.0f, 0.0f});
    arena.settle();
    break_bottle(
        arena.server, strike_trigger(KernelLockStrikeMode_Instant, 1u, 0u),
        glm::vec3{302.0f, 0.0f, 0.0f});
    break_bottle(
        arena.server, strike_trigger(KernelLockStrikeMode_Instant, 1u, 0u),
        glm::vec3{5.0f, 0.0f, 30.0f});
    break_bottle(
        arena.server, strike_trigger(KernelLockStrikeMode_Instant, 1u, 0u),
        glm::vec3{0.0f, 0.0f, 100.0f});
    arena.settle();
    require(hp(arena.server, distant) == 100 - kDamage);
    for (const Arena::Client& client : arena.clients) {
        require(client.strikes.size() == 1u);
        const KernelLockStrikeEvent& empty = client.strikes[0];
        require(empty.selection.target_count == 0u);
        require(empty.selection.arc_count == kLockSize);
        require(near(empty.selection.origin, {5.0f, 1.0f, 30.0f}, 0.001f));
        // Both arcs end inside the sphere, the same on every client.
        for (std::uint32_t arc = 0; arc < kLockSize; ++arc) {
            const KernelVec3 end = empty.selection.arc_ends[arc];
            const float reach = glm::length(
                glm::vec3{end.x, end.y, end.z} - glm::vec3{5.0f, 1.0f, 30.0f});
            require(reach >= kRadius * 0.4f - 0.01f && reach <= kRadius + 0.01f);
            const KernelVec3 other = arena.clients[0].strikes[0].selection.arc_ends[arc];
            require(end.x == other.x && end.y == other.y && end.z == other.z);
        }
    }
}

// More records than a snapshot's share (D14): each snapshot carries what fits
// in kSnapshotLockRecordBudgetBytes, its first packet stays inside the
// datagram budget, and the rest follow in later snapshots until every one
// has arrived -- even after the strikes ahead of them have killed both
// targets, since the players stand near where they were taken.
void records_past_the_budget_wait_for_the_next_snapshot() {
    Arena arena(7963);
    const ne::NetId first = arena.enemy({3.0f, 0.0f, 0.0f});
    const ne::NetId second = arena.enemy({0.0f, 0.0f, 5.0f});
    // A crowd out of the sphere's reach but in everyone's sight, so the
    // entities alone fill the first packet to its budget: the records only
    // fit because it left them room.
    for (int index = 0; index < 120; ++index) {
        const float angle = 0.0523f * static_cast<float>(index);
        const float radius = 14.0f + 0.1f * static_cast<float>(index);
        arena.enemy({radius * std::cos(angle), 0.0f, radius * std::sin(angle)});
    }
    arena.settle();
    // Twenty strikes on one tick, 43 B each: 860 B, over two snapshots' worth.
    constexpr std::size_t kStrikes = 20;
    for (std::size_t index = 0; index < kStrikes; ++index) {
        break_bottle(
            arena.server, strike_trigger(KernelLockStrikeMode_Instant, 1u, 0u),
            glm::vec3{0.0f, 0.0f, 0.01f * static_cast<float>(index)});
    }
    arena.server.choose_lock_records_to_send();
    const std::size_t first_share = arena.server.sending_lock_strike_records_.size();
    require(first_share == (ne::kSnapshotLockRecordBudgetBytes - 8u) / 43u);
    require(arena.server.pending_lock_strike_records_.size() == kStrikes - first_share);
    // Put back as it was, and let the server's own snapshots do it.
    for (auto& record : arena.server.sending_lock_strike_records_) {
        arena.server.pending_lock_strike_records_.insert(
            arena.server.pending_lock_strike_records_.begin(), record);
    }
    arena.server.sending_lock_strike_records_.clear();
    arena.largest_snapshot_packet = 0;
    for (int index = 0; index < 20; ++index) {
        arena.tick();
    }
    require(arena.largest_snapshot_packet <=
            ne::kSnapshotSendBudgetBytes + kPacketHeaderBytes);
    for (const Arena::Client& client : arena.clients) {
        require(client.strikes.size() == kStrikes);
    }
    require(arena.server.world_.find_entity(first) == std::nullopt ||
            hp(arena.server, first) == 0u);
    require(arena.server.world_.find_entity(second) == std::nullopt ||
            hp(arena.server, second) == 0u);
}

// The listen host's own client is sent the strikes its player's session sees,
// in the snapshot it sends itself.
void the_listen_host_sends_itself_the_strike() {
    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    ne::KernelEngine engine(config);
    auto transport = std::make_unique<ne::ListenServerTransport>(
        std::make_unique<ne::LoopbackTransport>());
    require(transport->StartServer(7967));
    ne::ListenServerTransport* listen_transport = transport.get();
    engine.listen_server_transport_ = listen_transport;
    engine.transport_ = std::move(transport);
    engine.reset_runtime_state(KernelMode_ListenServer);
    install_catalog(engine);

    const ne::NetId host = engine.world_.spawn_player(kThrowerPeer, glm::vec3{0.0f});
    engine.local_player_net_id_ = host;
    const ne::NetId target = engine.world_.spawn_enemy(glm::vec3{3.0f, 0.0f, 0.0f});
    engine.world_.registry().get_or_emplace<ne::Health>(
        *engine.world_.find_entity(target)) = ne::Health{100, 100};
    engine.local_listen_session_ =
        ne::KernelEngine::PeerSession{kThrowerPeer, host, 0, true, {}};
    engine.local_listen_session_.relevant_entities.insert(target);

    break_bottle(
        engine, strike_trigger(KernelLockStrikeMode_Instant, 1u, 0u),
        glm::vec3{0.0f});
    engine.publish_snapshot();
    std::vector<ne::SnapshotLockStrike> received;
    ne::TransportEvent event;
    while (listen_transport->PollLocalClientEvent(event)) {
        ne::WorldSnapshot snapshot;
        if (event.channel == ne::ChannelId::kSnapshot &&
            ne::decode_snapshot_packet(
                event.payload.data(), event.payload.size(), &snapshot)) {
            received.insert(
                received.end(),
                snapshot.lock_strikes.begin(),
                snapshot.lock_strikes.end());
        }
    }
    require(received.size() == 1u);
    require((received[0].target_net_ids == std::vector<ne::NetId>{target}));
}

}  // namespace

int main() {
    a_repeated_strike_reaches_every_client_that_sees_its_targets();
    graph_missiles_leave_from_their_origin();
    the_breaking_bottle_hides_nothing_from_its_first_strike();
    a_strike_goes_to_whoever_sees_it();
    records_past_the_budget_wait_for_the_next_snapshot();
    the_listen_host_sends_itself_the_strike();
    std::printf("lock_strike_end_to_end_test: PASS\n");
    return 0;
}
