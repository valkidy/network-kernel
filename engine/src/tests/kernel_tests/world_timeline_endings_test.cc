// A thrown bottle's flight and the blast it sets off both live on the world
// timeline -- drawn an interpolation delay behind the server -- but the
// reliable records that end the flight and start the blast used to take effect
// the moment they arrived. So a bottle vanished in mid-air, a delay's worth of
// flight short of the spot where its blast had already appeared. These pin
// both ends to the render instant instead.
//
// Same client-only setup as client_mode_test's thrown-prop tests: no clock
// sync, so the render instant is the newest snapshot minus the interpolation
// delay (two snapshot intervals, four ticks), and advancing it means feeding
// snapshots.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "protocol/public/network_packets.h"
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

constexpr std::uint32_t kTrajectoryTemplateId = 21;
constexpr std::uint32_t kBlastTemplateId = 22;
constexpr std::uint32_t kBlastColliderTemplateId = 9;
constexpr std::uint32_t kPropTemplateId = 7;
constexpr ne::NetId kBottle = 51;
constexpr ne::PeerId kLocalPeer = 2;

struct Client {
    Client() : engine(make_config()) {
        engine.reset_runtime_state(KernelMode_Client);
        engine.local_client_peer_id_ = kLocalPeer;

        ne::RuntimeProjectileTemplate trajectory{};
        trajectory.projectile_template_id = kTrajectoryTemplateId;
        trajectory.motion_model = ne::ProjectileMotionModel::kParabolic;
        trajectory.sync_mode = ne::ProjectileSyncMode::kLocalPredictedDeterministic;
        trajectory.speed = 24.0f;
        trajectory.gravity = glm::vec3{0.0f, -9.81f, 0.0f};
        engine.catalog_runtime_.projectile_templates.push_back(trajectory);

        ne::RuntimeProjectileTemplate blast{};
        blast.projectile_template_id = kBlastTemplateId;
        blast.collider_template_id = kBlastColliderTemplateId;
        blast.motion_model = ne::ProjectileMotionModel::kLinear;
        blast.sync_mode = ne::ProjectileSyncMode::kServerSnapshotOnly;
        engine.catalog_runtime_.projectile_templates.push_back(blast);

        KernelEntityTemplateDefinition prop_template{};
        prop_template.struct_size = sizeof(prop_template);
        prop_template.entity_template_id = kPropTemplateId;
        prop_template.entity_type = static_cast<std::uint16_t>(ne::EntityType::kProp);
        prop_template.prop.struct_size = sizeof(prop_template.prop);
        prop_template.prop.throw_trajectory_projectile_template_id =
            kTrajectoryTemplateId;
        engine.entity_templates_.push_back(prop_template);
    }

    static KernelConfig make_config() {
        KernelConfig config{};
        config.mode = KernelMode_Client;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        return config;
    }

    void snapshots_through(std::uint32_t last_tick) {
        for (; next_tick <= last_tick; ++next_tick) {
            ne::WorldSnapshot snapshot;
            snapshot.header.server_tick = next_tick;
            engine.handle_client_snapshot(snapshot);
        }
    }

    // A bottle thrown at tick 10, and the world advanced to a render instant of
    // tick 36 with no snapshot ever carrying it again.
    void throw_bottle() {
        ne::EntitySpawnPacket spawn{};
        spawn.net_id = kBottle;
        spawn.entity_type = ne::EntityType::kProp;
        spawn.server_tick = 10;
        spawn.position = glm::vec3{0.0f, 1.0f, 0.0f};
        spawn.entity_template_id = kPropTemplateId;
        spawn.item_template_id = 13;
        spawn.item_instance_id = 1001;
        spawn.world_item_mode = KernelWorldItemMode_InFlight;
        engine.handle_client_spawn(spawn);
        snapshots_through(10);

        ne::PropStateChangeBatchPacket batch{};
        batch.server_tick = 10;
        ne::PropStateChangeRecord record{};
        record.net_id = kBottle;
        record.changed_fields = ne::kPropStateChangeMode |
            ne::kPropStateChangeTransform | ne::kPropStateChangeVelocity;
        record.world_mode = KernelWorldItemMode_InFlight;
        record.position = glm::vec3{0.0f, 1.0f, 0.0f};
        record.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        record.velocity = glm::vec3{12.0f, 6.0f, 0.0f};
        batch.records.push_back(record);
        engine.handle_client_prop_state_change_batch(batch);
        snapshots_through(40);
        render();
    }

    void spawn_blast(ne::NetId net_id, ne::PeerId owner, std::uint32_t tick) {
        ne::ProjectileSpawnBatchPacket packet{};
        packet.server_tick = tick;
        packet.catalog_hash = engine.catalog_hash_;
        ne::ProjectileSpawnGroup group{};
        group.projectile_template_id = kBlastTemplateId;
        ne::ProjectileSpawnRecord record{};
        record.projectile_net_id = net_id;
        record.owner_peer = owner;
        record.spawn_position = glm::vec3{8.0f, 0.0f, 0.0f};
        group.records.push_back(record);
        packet.groups.push_back(group);
        engine.handle_client_projectile_spawn_batch(packet);
    }

    void render() {
        count = engine.get_render_states_at_time(
            1000000, states.data(), static_cast<std::uint32_t>(states.size()));
    }

    const RenderEntityState* drawn(ne::NetId net_id) {
        render();
        for (std::uint32_t index = 0; index < count; ++index) {
            if (states[index].net_id == net_id) {
                return &states[index];
            }
        }
        return nullptr;
    }

    bool destroyed_event_for(ne::NetId net_id) const {
        for (const KernelEntityLifecycleEvent& event : engine.lifecycle_events_) {
            if (event.net_id == net_id) {
                return true;
            }
        }
        return false;
    }

    ne::KernelEngine engine;
    std::uint32_t next_tick = 1;
    std::array<RenderEntityState, 8> states{};
    std::uint32_t count = 0;
};

// Destroyed at tick 41 -- it hit something and detonated -- while the render
// instant is at 36. It keeps flying for those five ticks, and Unity is not told
// it is gone until the world timeline gets there.
void a_bottle_destroyed_ahead_of_the_render_instant_keeps_flying() {
    Client client;
    client.throw_bottle();
    require(client.drawn(kBottle) != nullptr);

    ne::EntityDespawnPacket despawn{};
    despawn.net_id = kBottle;
    despawn.server_tick = 41;
    despawn.reason = KernelDespawnReason_Destroyed;
    client.engine.handle_client_despawn(despawn);

    const RenderEntityState* still = client.drawn(kBottle);
    require(still != nullptr);
    require(still->world_item_mode == KernelWorldItemMode_InFlight);
    require(!client.destroyed_event_for(kBottle));

    client.snapshots_through(46);
    require(client.drawn(kBottle) == nullptr);
    require(client.destroyed_event_for(kBottle));
}

// Leaving relevance is not an ending: it is applied the moment it arrives, as
// it always was.
void leaving_relevance_is_not_held_back() {
    Client client;
    client.throw_bottle();
    ne::EntityDespawnPacket despawn{};
    despawn.net_id = kBottle;
    despawn.server_tick = 41;
    despawn.reason = KernelDespawnReason_OutOfRange;
    client.engine.handle_client_despawn(despawn);
    require(client.drawn(kBottle) == nullptr);
    require(client.destroyed_event_for(kBottle));
}

// The blast a remote bottle set off at tick 41 is not drawn while the render
// instant is at 36; the local player's own server-only projectile is, because
// holding back your own action would only make it feel late. And the remote
// blast's despawn waits for the render instant too, so it is shown for its
// whole lifetime rather than one interpolation delay less.
void a_remote_blast_waits_for_its_spawn_tick_and_its_despawn() {
    Client client;
    client.throw_bottle();

    constexpr ne::NetId kRemoteBlast = 70;
    constexpr ne::NetId kOwnBlast = 71;
    client.spawn_blast(kRemoteBlast, 0u, 41u);
    client.spawn_blast(kOwnBlast, kLocalPeer, 41u);
    require(client.drawn(kRemoteBlast) == nullptr);
    require(client.drawn(kOwnBlast) != nullptr);

    client.snapshots_through(46);
    require(client.drawn(kRemoteBlast) != nullptr);

    ne::EntityDespawnPacket expired{};
    expired.net_id = kRemoteBlast;
    expired.server_tick = 60;
    expired.reason = KernelDespawnReason_Expired;
    client.engine.handle_client_despawn(expired);
    require(client.drawn(kRemoteBlast) != nullptr);
    client.snapshots_through(65);
    require(client.drawn(kRemoteBlast) == nullptr);
}

// Server side. A prop the client draws from its throw anchor gains nothing from
// a snapshot sample of it -- the render pass overrides it -- so it gives its
// slot back to the actors. One the client cannot anchor still needs the
// samples, and so does the same prop once it is no longer in the air.
void an_anchored_prop_in_flight_leaves_the_send_set() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    ne::KernelEngine server(config);
    server.reset_runtime_state(KernelMode_DedicatedServer);
    KernelEntityTemplateDefinition prop_template{};
    prop_template.struct_size = sizeof(prop_template);
    prop_template.entity_template_id = kPropTemplateId;
    prop_template.entity_type = static_cast<std::uint16_t>(ne::EntityType::kProp);
    prop_template.prop.struct_size = sizeof(prop_template.prop);
    prop_template.prop.throw_trajectory_projectile_template_id = kTrajectoryTemplateId;
    server.entity_templates_.push_back(prop_template);

    ne::World& world = server.simulation_world();
    const ne::NetId player = world.spawn_player(1, glm::vec3{0.0f});
    const ne::NetId prop = world.spawn_entity(
        ne::EntityType::kProp, ne::ActorType::kUnknown, 0, glm::vec3{3.0f, 1.0f, 0.0f});
    const entt::entity entity = *world.find_entity(prop);
    world.registry().emplace_or_replace<ne::EntityTemplateRef>(
        entity, ne::EntityTemplateRef{kPropTemplateId});
    world.registry().emplace_or_replace<ne::PropWorldMode>(
        entity, ne::PropWorldMode{ne::PropMode::kInFlight});
    world.registry().get_or_emplace<ne::Velocity>(entity).linear =
        glm::vec3{12.0f, 6.0f, 0.0f};

    ne::KernelEngine::PeerSession session{1, player, 0, true, {}};
    std::uint32_t time_ms = 0;
    const auto sent = [&]() {
        const ne::WorldSnapshot relevant =
            server.build_relevant_snapshot(session, time_ms += 66u);
        const ne::WorldSnapshot send =
            server.build_snapshot_send_set(session, relevant, 1200u);
        bool in_relevant = false;
        for (const ne::EntitySnapshot& candidate : relevant.entities) {
            in_relevant = in_relevant || candidate.net_id == prop;
        }
        require(in_relevant);
        for (const ne::EntitySnapshot& candidate : send.entities) {
            if (candidate.net_id == prop) return true;
        }
        return false;
    };
    require(!sent());

    world.registry().get<ne::PropWorldMode>(entity).mode = ne::PropMode::kCarrying;
    require(sent());

    world.registry().get<ne::PropWorldMode>(entity).mode = ne::PropMode::kInFlight;
    world.registry().get<ne::EntityTemplateRef>(entity).entity_template_id = 99u;
    require(sent());
}

}  // namespace

int main() {
    a_bottle_destroyed_ahead_of_the_render_instant_keeps_flying();
    leaving_relevance_is_not_held_back();
    a_remote_blast_waits_for_its_spawn_tick_and_its_despawn();
    an_anchored_prop_in_flight_leaves_the_send_set();
    std::printf("world_timeline_endings_test: PASS\n");
    return 0;
}
