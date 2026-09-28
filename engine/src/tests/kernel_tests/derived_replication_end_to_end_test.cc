// A meteor storm from the server's world to a client's drawing of it, where
// the client is sent only the storm's root marker and derives every fuse,
// meteor and blast under it by running the same simulation.
//
// What is proved, in order:
//   - no derived projectile is ever introduced to a client;
//   - the root is relevant but never written into a snapshot;
//   - every derived projectile the client draws is where the server has it on
//     the same tick, and the client draws as many of each as the server has;
//   - the root is drawn Active for its own lifetime and not after, though it
//     is held (and so still known to the client) until the chain is over;
//   - a client that comes into range mid-storm derives the same storm;
//   - a root whose chain the server could not start is removed early, and the
//     client then draws nothing under it.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/action_graph.h"
#include "simulation/public/simulation.h"
#include "transport/public/loopback_transport.h"
#include "world/public/components.h"
#include "world/public/world.h"

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

constexpr std::uint32_t kStormTemplate = 40;
constexpr std::uint32_t kMeteorTemplate = 41;
constexpr std::uint32_t kFuseTemplate = 42;
constexpr std::uint32_t kBlastTemplate = 43;
constexpr std::uint32_t kStormLifetime = 10;
constexpr ne::PeerId kPeer = 1;
constexpr ne::PeerId kObserverPeer = 2;
const glm::vec3 kTarget{6.0f, 0.0f, 0.0f};

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

void shuttle(
    ne::LoopbackTransport* from,
    ne::LoopbackTransport* client,
    ne::LoopbackTransport* observer) {
    ne::TransportEvent event;
    while (from->PollClientEvent(event)) {
        ne::LoopbackTransport* to = event.peer == kObserverPeer ? observer : client;
        require(to->SendClient(
            event.peer,
            event.payload.data(),
            static_cast<std::uint32_t>(event.payload.size()),
            event.mode,
            event.channel));
    }
}

ne::RuntimeProjectileTemplate marker(std::uint32_t id, std::uint32_t lifetime) {
    ne::RuntimeProjectileTemplate value{};
    value.projectile_template_id = id;
    value.projectile_type = ne::ProjectileType::kStandard;
    value.sync_mode = ne::ProjectileSyncMode::kServerSnapshotOnly;
    value.damage_shape = ne::ProjectileDamageShape::kNone;
    value.speed = 0.0f;
    value.lifetime_ticks = lifetime;
    value.collision_mask = KERNEL_COLLISION_MASK_NONE;
    value.collider_template_id = 7;
    return value;
}

// Storm marker -> 10-15 fuses (lifetime 10 + a 60-tick stagger, 6 m) -> a
// descending meteor each -> a two-tick blast where it lands. All but the
// marker derived, as the shipped Meteor Storm Staff has it.
std::vector<ne::RuntimeProjectileTemplate> storm_templates() {
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(trigger);
    trigger.action_count = 1;
    KernelActionDefinition& action = trigger.actions[0];
    action.action_type = KernelEntityTriggerActionType_SpawnProjectile;
    action.spawn_projectile_template_id = kFuseTemplate;
    action.position_source = KernelEventVec3Source_Position;
    action.direction_source = KernelEventVec3Source_Direction;
    action.spawn_lifetime_ticks = 10;
    action.repeat_count_min = 10;
    action.repeat_count_max = 15;
    action.repeat_scatter_radius = 6.0f;
    action.repeat_stagger_lifetime_ticks = 60;

    ne::RuntimeProjectileTemplate storm = marker(kStormTemplate, kStormLifetime);
    storm.expired_binding = *ne::compile_action_trigger_definition(
        ne::TriggerEventType::kExpired, trigger);

    ne::RuntimeProjectileTemplate fuse = marker(kFuseTemplate, 99);
    fuse.derived = true;
    fuse.expired_binding = ne::compile_spawn_projectile_binding(
        ne::TriggerEventType::kExpired, kMeteorTemplate);

    ne::RuntimeProjectileTemplate meteor{};
    meteor.projectile_template_id = kMeteorTemplate;
    meteor.projectile_type = ne::ProjectileType::kStandard;
    meteor.sync_mode = ne::ProjectileSyncMode::kServerSnapshotOnly;
    meteor.damage_shape = ne::ProjectileDamageShape::kNone;
    meteor.lifetime_ticks = 18;
    meteor.collision_mask =
        KERNEL_COLLISION_LAYER_TERRAIN | KERNEL_COLLISION_LAYER_STATIC_OBSTACLE;
    meteor.collider_template_id = 7;
    meteor.launch_type = ne::ProjectileLaunchType::kDescent;
    meteor.launch_elevation_min_degrees = 75.0f;
    meteor.launch_elevation_max_degrees = 85.0f;
    meteor.launch_height = 40.0f;
    meteor.launch_fall_ticks = 15;
    meteor.derived = true;
    meteor.projectile_impact_binding = ne::compile_spawn_projectile_binding(
        ne::TriggerEventType::kProjectileImpact, kBlastTemplate);
    meteor.expired_binding = ne::compile_spawn_projectile_binding(
        ne::TriggerEventType::kExpired, kBlastTemplate);

    ne::RuntimeProjectileTemplate blast{};
    blast.projectile_template_id = kBlastTemplate;
    blast.projectile_type = ne::ProjectileType::kAreaEffect;
    blast.sync_mode = ne::ProjectileSyncMode::kServerSnapshotOnly;
    blast.damage = 1;
    blast.damage_interval_ticks = 2;
    blast.lifetime_ticks = 2;
    blast.area_radius = 4.0f;
    blast.collision_mask = KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
    blast.collider_template_id = 4;
    blast.derived = true;
    return {storm, fuse, meteor, blast};
}

// Ground with its top at y = 0, as terrain -- the same slab on each side.
std::unique_ptr<ne::physics::PhysicsWorld> ground() {
    auto physics = std::make_unique<ne::physics::PhysicsWorld>();
    ne::physics::CollisionObjectDescriptor slab;
    slab.identity = ne::physics::CollisionObjectIdentity{
        0, 900, 0,
        ne::physics::CollisionObjectKind::kTerrain,
        ne::physics::CollisionLayer::kTerrain};
    slab.shape.type = ne::physics::CollisionShapeType::kBox;
    slab.shape.half_extents = glm::vec3{200.0f, 0.5f, 200.0f};
    slab.position = glm::vec3{0.0f, -0.5f, 0.0f};
    std::string error;
    require(physics->upsert_object(slab, &error));
    return physics;
}

void install(ne::KernelEngine* engine, ne::physics::PhysicsWorld* physics) {
    const std::vector<ne::RuntimeProjectileTemplate> templates = storm_templates();
    engine->world_.set_projectile_templates(templates);
    engine->catalog_runtime_.projectile_templates = templates;
    ne::compute_derived_chain_ticks(&engine->catalog_runtime_.projectile_templates);
    engine->world_.set_collision_world(physics);
}

bool is_derived_template(std::uint32_t id) {
    return id == kFuseTemplate || id == kMeteorTemplate || id == kBlastTemplate;
}

struct Sample {
    std::uint32_t template_id = 0;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
};

struct Rig {
    std::unique_ptr<ne::physics::PhysicsWorld> server_ground = ground();
    std::unique_ptr<ne::physics::PhysicsWorld> client_ground = ground();
    std::unique_ptr<ne::physics::PhysicsWorld> observer_ground = ground();
    std::unique_ptr<ne::KernelEngine> server;
    std::unique_ptr<ne::KernelEngine> client;
    std::unique_ptr<ne::KernelEngine> observer;
    ne::LoopbackTransport* server_link = nullptr;
    ne::LoopbackTransport* client_link = nullptr;
    ne::LoopbackTransport* observer_link = nullptr;
    ne::NetId player = 0;
    ne::NetId observer_player = 0;
    std::uint64_t tick_us = 0;
    // What the server holds of each derived template, per simulated tick.
    std::map<std::uint32_t, std::vector<Sample>> history;

    explicit Rig(std::uint16_t port) {
        KernelConfig server_config{};
        server_config.mode = KernelMode_DedicatedServer;
        server_config.tick.server_tick_rate = 30;
        server_config.tick.snapshot_rate = 15;
        server_config.max_events = 4096;
        server_config.max_render_states = 256;
        server = std::make_unique<ne::KernelEngine>(server_config);
        server_link = attach_loopback(server.get(), KernelMode_DedicatedServer, port);
        install(server.get(), server_ground.get());

        KernelConfig client_config = server_config;
        client_config.mode = KernelMode_Client;
        client = std::make_unique<ne::KernelEngine>(client_config);
        client_link = attach_loopback(client.get(), KernelMode_Client, port + 1);
        install(client.get(), client_ground.get());
        observer = std::make_unique<ne::KernelEngine>(client_config);
        observer_link = attach_loopback(observer.get(), KernelMode_Client, port + 2);
        install(observer.get(), observer_ground.get());

        player = server->world_.spawn_player(kPeer, glm::vec3{0.0f});
        server->peer_sessions_.push_back(
            ne::KernelEngine::PeerSession{kPeer, player, 0, true, {}});
        // Well past the relevance radius.
        observer_player = server->world_.spawn_player(
            kObserverPeer, glm::vec3{0.0f, 0.0f, -90.0f});
        server->peer_sessions_.push_back(
            ne::KernelEngine::PeerSession{kObserverPeer, observer_player, 0, true, {}});
        tick_us = static_cast<std::uint64_t>(
            server->tick_loop_.fixed_delta_seconds() * 1000000.0f);
        for (int index = 0; index < 8; ++index) {
            step();
        }
        client->local_player_net_id_ = player;
        client->local_client_peer_id_ = kPeer;
        observer->local_player_net_id_ = observer_player;
        observer->local_client_peer_id_ = kObserverPeer;
    }

    void step() {
        client->client_local_time_us_ += tick_us;
        observer->client_local_time_us_ += tick_us;
        server->simulate_tick();
        std::vector<Sample>& samples = history[server->current_tick() - 1u];
        auto view = server->world_.registry()
                        .view<ne::ProjectileState, ne::Transform, ne::Velocity>();
        for (const entt::entity entity : view) {
            const ne::ProjectileState& state = view.get<ne::ProjectileState>(entity);
            if (is_derived_template(state.projectile_template_id)) {
                samples.push_back(Sample{
                    state.projectile_template_id,
                    view.get<ne::Transform>(entity).position,
                    view.get<ne::Velocity>(entity).linear});
            }
        }
        shuttle(server_link, client_link, observer_link);
        client->poll_transport();
        observer->poll_transport();
    }

    // The root, as a targeted strike lands it: before the tick about to be
    // simulated, with no launch salt.
    ne::NetId land_storm() {
        const ne::RuntimeProjectileTemplate* storm =
            server->world_.find_projectile_template(kStormTemplate);
        require(storm != nullptr);
        require(storm->derived_chain_ticks > 0u);
        require(ne::spawn_projectile_at(
            server->world_, *storm, kPeer, player, 0, 7001, kTarget,
            glm::vec3{0.0f, 0.0f, 1.0f}, server->current_tick(),
            server->tick_loop_.fixed_delta_seconds(), nullptr));
        auto view = server->world_.registry()
                        .view<ne::NetworkIdentity, ne::ProjectileState>();
        for (const entt::entity entity : view) {
            if (view.get<ne::ProjectileState>(entity).projectile_template_id ==
                kStormTemplate) {
                return view.get<ne::NetworkIdentity>(entity).net_id;
            }
        }
        require(false);
        return 0;
    }
};

// No projectile the client knows of by net id is derived.
void require_no_derived_replicated(const ne::KernelEngine& engine) {
    for (const auto& entity : engine.client_replicated_entities_) {
        if (entity.type == ne::EntityType::kProjectile) {
            require(!is_derived_template(entity.projectile_template_id));
        }
    }
}

struct Drawn {
    std::vector<RenderEntityState> derived;
    const RenderEntityState* root = nullptr;
    std::array<RenderEntityState, 96> states{};
};

void draw(ne::KernelEngine& engine, ne::NetId root, Drawn* drawn) {
    const std::uint32_t count = engine.get_render_states_at_time(
        engine.client_local_time_us_,
        drawn->states.data(),
        static_cast<std::uint32_t>(drawn->states.size()));
    require(count < drawn->states.size());
    drawn->derived.clear();
    drawn->root = nullptr;
    for (std::uint32_t index = 0; index < count; ++index) {
        const RenderEntityState& state = drawn->states[index];
        if (state.net_id == root && root != 0u) {
            drawn->root = &state;
        }
        if (state.net_id == 0u && is_derived_template(state.template_id)) {
            drawn->derived.push_back(state);
        }
    }
}

// Every drawn derived projectile has a server twin of its template where the
// server had it on the drawn tick (extrapolated by the same slice of a tick),
// and the counts per template agree. Returns the templates seen.
std::set<std::uint32_t> require_matches_server(
    const Rig& rig,
    const ne::KernelEngine& engine,
    const std::vector<RenderEntityState>& derived) {
    std::set<std::uint32_t> seen;
    if (derived.empty()) {
        return seen;
    }
    const double dt = rig.server->tick_loop_.fixed_delta_seconds();
    const double render_seconds =
        static_cast<double>(engine.render_server_time_us_) / 1000000.0;
    const std::uint32_t tick =
        static_cast<std::uint32_t>(render_seconds / dt);
    const auto found = rig.history.find(tick);
    require(found != rig.history.end());
    const float since = static_cast<float>(
        std::clamp(render_seconds - tick * dt, 0.0, dt));
    std::map<std::uint32_t, int> server_counts;
    std::map<std::uint32_t, int> client_counts;
    for (const Sample& sample : found->second) {
        ++server_counts[sample.template_id];
    }
    for (const RenderEntityState& state : derived) {
        ++client_counts[state.template_id];
        seen.insert(state.template_id);
        const glm::vec3 drawn{state.position.x, state.position.y, state.position.z};
        float nearest = 1e9f;
        for (const Sample& sample : found->second) {
            if (sample.template_id == state.template_id) {
                nearest = std::min(
                    nearest,
                    glm::length(sample.position + sample.velocity * since - drawn));
            }
        }
        require(nearest < 0.01f);
        require(state.status == RenderEntityStatus_Active);
    }
    require(server_counts == client_counts);
    return seen;
}

void storm_is_derived_not_sent() {
    Rig rig(7811);
    const ne::NetId root = rig.land_storm();
    const std::uint32_t root_tick = rig.server->current_tick();
    const double dt = rig.server->tick_loop_.fixed_delta_seconds();

    // Relevant to the thrower, but no snapshot record is spent on it.
    rig.step();
    {
        ne::KernelEngine& server = *rig.server;
        ne::KernelEngine::PeerSession& session = server.peer_sessions_[0];
        const ne::WorldSnapshot relevant = server.build_relevant_snapshot(session, 0);
        const ne::WorldSnapshot sent = server.build_snapshot_send_set(
            session, relevant, 1u << 20);
        bool in_relevant = false;
        bool in_sent = false;
        for (const auto& entity : relevant.entities) in_relevant |= entity.net_id == root;
        for (const auto& entity : sent.entities) in_sent |= entity.net_id == root;
        require(in_relevant);
        require(!in_sent);
    }

    Drawn drawn;
    std::set<std::uint32_t> templates_seen;
    int frames_compared = 0;
    int marker_frames_active = 0;
    bool marker_hidden_after_life = false;
    bool observer_joined = false;
    int observer_frames_compared = 0;
    for (int frame = 0; frame < 140; ++frame) {
        rig.step();
        require_no_derived_replicated(*rig.client);
        require_no_derived_replicated(*rig.observer);

        draw(*rig.client, root, &drawn);
        const double render_seconds =
            static_cast<double>(rig.client->render_server_time_us_) / 1000000.0;
        const bool root_alive_on_client =
            std::any_of(rig.client->client_replicated_entities_.begin(),
                        rig.client->client_replicated_entities_.end(),
                        [root](const auto& entity) { return entity.net_id == root; });
        if (render_seconds >= root_tick * dt &&
            render_seconds < (root_tick + kStormLifetime) * dt) {
            require(drawn.root != nullptr);
            require(drawn.root->status == RenderEntityStatus_Active);
            ++marker_frames_active;
        } else if (render_seconds >= (root_tick + kStormLifetime) * dt &&
                   root_alive_on_client) {
            // Held for its chain, still known, no longer drawn.
            require(drawn.root == nullptr);
            marker_hidden_after_life = true;
        }
        const std::set<std::uint32_t> seen =
            require_matches_server(rig, *rig.client, drawn.derived);
        if (!drawn.derived.empty()) {
            ++frames_compared;
        }
        templates_seen.insert(seen.begin(), seen.end());

        // Mid-storm, the observer walks up and is handed the root only.
        if (!observer_joined && render_seconds > (root_tick + 40) * dt) {
            rig.server->world_.registry().get<ne::Transform>(
                *rig.server->world_.find_entity(rig.observer_player)).position =
                glm::vec3{kTarget.x, 0.0f, kTarget.z - 5.0f};
            observer_joined = true;
        }
        if (observer_joined) {
            Drawn seen_by_observer;
            draw(*rig.observer, root, &seen_by_observer);
            require_matches_server(rig, *rig.observer, seen_by_observer.derived);
            if (!seen_by_observer.derived.empty()) {
                ++observer_frames_compared;
            }
        }
    }
    std::printf(
        "derived storm: %d client frames compared, %d observer frames, "
        "marker active %d frames\n",
        frames_compared, observer_frames_compared, marker_frames_active);
    require(templates_seen.count(kFuseTemplate) == 1u);
    require(templates_seen.count(kMeteorTemplate) == 1u);
    require(templates_seen.count(kBlastTemplate) == 1u);
    require(frames_compared >= 60);
    require(marker_frames_active >= 8);
    require(marker_hidden_after_life);
    require(observer_frames_compared >= 20);
    // Over: the root is gone and nothing is left to draw.
    require(!rig.server->world_.find_entity(root).has_value());
    require(rig.client->derived_chains_.chain_count() == 0u);
}

// The dedup ledger is full, so the root's expiry batch cannot commit. The
// server removes the root on the spot, and the client -- which started
// deriving from the root it was handed -- drops the chain and never draws a
// fuse.
void called_off_storm_draws_nothing() {
    Rig rig(7821);
    const ne::NetId root = rig.land_storm();
    require(rig.server->world_.reserve_action_graph_batch_capacity(
        ne::World::kActionGraphDedupCapacity));
    Drawn drawn;
    bool root_seen = false;
    for (int frame = 0; frame < 60; ++frame) {
        rig.step();
        draw(*rig.client, root, &drawn);
        root_seen |= drawn.root != nullptr;
        require(drawn.derived.empty());
    }
    require(root_seen);
    require(!rig.server->world_.find_entity(root).has_value());
    require(rig.client->derived_chains_.chain_count() == 0u);
}

}  // namespace

int main() {
    storm_is_derived_not_sent();
    called_off_storm_draws_nothing();
    std::printf("derived_replication_end_to_end_test: PASS\n");
    return 0;
}
