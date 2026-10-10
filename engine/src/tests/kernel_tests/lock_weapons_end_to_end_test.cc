// Lock-on weapons from a dedicated server to pure clients, through the
// encoder, a transport and the decoder (docs/LOCK_ON_WEAPONS_PLAN.md, P4).
//
//   - lock_missile: the shooter's own client and an observer each receive one
//     record per missile -- shooter, target, commit, launch and arrival -- and
//     the server's hit lands on that arrival tick, not before.
//   - lock_instant: an observer resolves each of a remote shooter's commits on
//     its own view when its render clock reaches it, and locks the actors the
//     server hurt, filling the rest of the lock with the shot's own spray.
//
// Two or three engines and a shuttle standing in for the network, as
// held_fire_end_to_end_test does it.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "protocol/public/network_packets.h"
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

constexpr ne::PeerId kShooterPeer = 1;
constexpr ne::PeerId kObserverPeer = 2;
constexpr std::uint8_t kWeaponId = 9;
constexpr std::uint32_t kShotTemplateId = 61;
constexpr std::uint32_t kFireActionId = 4601;
constexpr std::uint32_t kInstance = 8801;
// Odd, so some commits fall between snapshots and their records carry a
// launch older than the batch that brings them.
constexpr std::uint32_t kCommitInterval = 3;
constexpr std::uint16_t kDamage = 10;
constexpr std::uint32_t kLockSize = 3;
constexpr float kRange = 20.0f;
constexpr std::uint32_t kLongestFlight = 20;

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
    projectile.weapon_id = kWeaponId;
    projectile.max_hit_count = kLockSize;
    projectile.lifetime_ticks = kLongestFlight;
    return projectile;
}

ne::RuntimeActionTemplate fire_action() {
    return ne::RuntimeActionTemplate{
        kFireActionId,
        KernelActionTriggerMode_Hold,
        KernelActionTemplateFlag_CancelOnRelease |
            KernelActionTemplateFlag_CancelOnDeath |
            KernelActionTemplateFlag_CancelOnWeaponChange,
        1,
        0,
        kCommitInterval,
        0,
        2,
        6,
    };
}

ne::WeaponMechanicsDefinition lock_weapon(ne::WeaponFireMode mode) {
    ne::WeaponMechanicsDefinition weapon;
    weapon.id = kWeaponId;
    weapon.mode = mode;
    weapon.magazine_size = 30;
    weapon.damage = kDamage;
    weapon.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    weapon.projectile_template_id = kShotTemplateId;
    weapon.fire_action_template_id = kFireActionId;
    weapon.pellet_count = 1;
    // What the kernel resolves off the cone collider.
    weapon.melee_range = kRange;
    weapon.melee_fov_degrees = 60.0f;
    return weapon;
}

constexpr std::uint32_t kPlayerActorTemplate = 71;
constexpr std::uint32_t kEnemyActorTemplate = 72;

// A client draws an actor only once it knows its actor template, and reads
// its side and hitbox centre off that template's collider.
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

void install_actor_templates(ne::KernelEngine& engine) {
    engine.collider_templates_.push_back(
        hit_collider(51, KERNEL_COLLISION_LAYER_PLAYER_SIDE, 0.9f));
    engine.collider_templates_.push_back(
        hit_collider(52, KERNEL_COLLISION_LAYER_HOSTILE_SIDE, 0.5f));
    engine.actor_templates_.push_back(
        actor_template(kPlayerActorTemplate, KernelActorType_Player, 51));
    engine.actor_templates_.push_back(
        actor_template(kEnemyActorTemplate, KernelActorType_Agent, 52));
}

void set_health(ne::KernelEngine& server, ne::NetId net_id) {
    server.world_.registry().get_or_emplace<ne::Health>(
        *server.world_.find_entity(net_id)) = ne::Health{100, 100};
}

std::uint16_t hp(ne::KernelEngine& server, ne::NetId net_id) {
    return server.world_.registry()
        .get<ne::Health>(*server.world_.find_entity(net_id))
        .hp;
}

struct Arena {
    ne::KernelEngine server;
    ne::LoopbackTransport* server_link = nullptr;
    struct Client {
        ne::PeerId peer = 0;
        std::unique_ptr<ne::KernelEngine> engine;
        ne::LoopbackTransport* link = nullptr;
    };
    std::vector<Client> clients;
    ne::NetId shooter = 0;
    std::uint32_t input_seq = 1;
    std::uint64_t tick_us = 0;

    Arena(ne::WeaponFireMode mode, std::uint16_t port, bool with_observer)
        : server(config(KernelMode_DedicatedServer)) {
        server_link = attach_loopback(&server, KernelMode_DedicatedServer, port);
        add_client(kShooterPeer, static_cast<std::uint16_t>(port + 1u));
        if (with_observer) {
            add_client(kObserverPeer, static_cast<std::uint16_t>(port + 2u));
        }
        server.catalog_runtime_.projectile_templates.push_back(shot());
        server.catalog_runtime_.action_templates.push_back(fire_action());
        install_actor_templates(server);
        for (Client& client : clients) {
            install_actor_templates(*client.engine);
            // What a client has from its catalog: the shot, the fire action and
            // the weapon itself (ABI 103) -- never a per-entity weapon.
            client.engine->catalog_runtime_.projectile_templates.push_back(shot());
            client.engine->catalog_runtime_.action_templates.push_back(fire_action());
            client.engine->catalog_weapons_.configured[kWeaponId] = true;
            client.engine->catalog_weapons_.definitions[kWeaponId] = lock_weapon(mode);
        }

        shooter = server.world_.spawn_player(kShooterPeer, glm::vec3{0.0f});
        set_health(server, shooter);
        require(server.server_set_entity_actor_template(shooter, kPlayerActorTemplate));
        const entt::entity entity = *server.world_.find_entity(shooter);
        ne::WeaponTuning& tuning =
            server.world_.registry().get_or_emplace<ne::WeaponTuning>(entity);
        tuning.configured[kWeaponId] = true;
        tuning.definitions[kWeaponId] = lock_weapon(mode);
        ne::WeaponState& weapon =
            server.world_.registry().get_or_emplace<ne::WeaponState>(entity);
        weapon.weapon_slot_count = ne::kWeaponSlotCount;
        weapon.active_weapon_slot = 0;
        weapon.weapon_ids[0] = kWeaponId;
        weapon.ammo[0] = 30;
        weapon.reserve_magazines[0] = 3;
        server.peer_sessions_.push_back(
            ne::KernelEngine::PeerSession{kShooterPeer, shooter, 0, true, {}});
        clients[0].engine->local_client_peer_id_ = kShooterPeer;
        clients[0].engine->local_player_net_id_ = shooter;

        if (with_observer) {
            // Behind the shooter, outside its cone.
            const ne::NetId observer =
                server.world_.spawn_player(kObserverPeer, glm::vec3{-4.0f, 0.0f, 0.0f});
            set_health(server, observer);
            require(server.server_set_entity_actor_template(
                observer, kPlayerActorTemplate));
            server.peer_sessions_.push_back(
                ne::KernelEngine::PeerSession{kObserverPeer, observer, 0, true, {}});
            clients[1].engine->local_client_peer_id_ = kObserverPeer;
            clients[1].engine->local_player_net_id_ = observer;
        }
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

    ne::NetId spawn_enemy(const glm::vec3& position) {
        const ne::NetId enemy = server.world_.spawn_enemy(position);
        set_health(server, enemy);
        require(server.server_set_entity_actor_template(enemy, kEnemyActorTemplate));
        server.world_.registry().get<ne::Hitbox>(*server.world_.find_entity(enemy)) =
            ne::Hitbox{{0.0f, 0.5f, 0.0f}, {0.3f, 0.5f, 0.3f}, 0};
        return enemy;
    }

    // One server tick with the shooter holding (or not), its output delivered
    // to every client, and every client drawn one tick on.
    void tick(bool held) {
        KernelPlayerInput input{};
        input.input_seq = input_seq++;
        input.selected_weapon = kWeaponId;
        input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
        if (held) {
            input.action_intent = KernelActionIntent{
                kInstance, KernelActionBinding_PrimaryFire, 0u, 0u};
            input.action_input = KernelActionInput{kInstance, 1u, 0u, 0u};
        } else {
            input.action_input = KernelActionInput{kInstance, 0u, 0u, 0u};
        }
        const std::vector<std::uint8_t> packet =
            ne::encode_player_input_packet(kShooterPeer, input, input.input_seq);
        require(server_link->SendClient(
            kShooterPeer, packet.data(), static_cast<std::uint32_t>(packet.size()),
            ne::SendMode::kUnreliable, ne::ChannelId::kInput));
        server.poll_transport();
        server.simulate_tick();
        ne::TransportEvent event;
        while (server_link->PollClientEvent(event)) {
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
            // And back: the clock-sync pings a client sends are what let it
            // place the server's timeline under its own clock and draw it.
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

    std::vector<KernelLockMissileView> missiles(std::size_t client) {
        std::array<KernelLockMissileView, 64> views{};
        const std::uint32_t count = clients[client].engine->get_lock_missiles(
            views.data(), static_cast<std::uint32_t>(views.size()));
        return {views.begin(), views.begin() + count};
    }
};

// Commits on the hold's ticks 0, 3, 6: each at the next of the three locked in
// turn. Both the shooter's own client and the observer draw every missile, and
// the server's hit on each target lands on the arrival the record states.
void lock_missiles_reach_every_client_and_land_on_their_arrival() {
    Arena arena(ne::WeaponFireMode::kLockMissile, 7841, true);
    const ne::NetId near = arena.spawn_enemy({5.0f, 0.0f, 0.0f});
    const ne::NetId mid = arena.spawn_enemy({9.0f, 0.0f, 1.0f});
    const ne::NetId far = arena.spawn_enemy({14.0f, 0.0f, -1.0f});

    for (int index = 0; index < 10; ++index) {
        arena.tick(false);
    }
    const std::uint32_t first_commit_tick = arena.server.tick_loop_.current_tick();
    for (std::uint32_t index = 0; index < 2u * kCommitInterval + 1u; ++index) {
        arena.tick(true);
    }
    arena.tick(false);
    // Long enough for every snapshot to carry its records out, and short of
    // the farthest arrival.
    arena.tick(false);

    for (std::size_t client = 0; client < arena.clients.size(); ++client) {
        std::vector<KernelLockMissileView> missiles = arena.missiles(client);
        std::sort(missiles.begin(), missiles.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.commit_index < rhs.commit_index;
        });
        require(missiles.size() == 3u);
        const ne::NetId order[] = {near, mid, far};
        for (std::size_t index = 0; index < missiles.size(); ++index) {
            require(missiles[index].shooter_net_id == arena.shooter);
            require(missiles[index].commit_index == index);
            require(missiles[index].target_net_id == order[index]);
            require(missiles[index].launch_tick ==
                    first_commit_tick + index * kCommitInterval);
            require(missiles[index].arrival_tick > missiles[index].launch_tick);
            require(missiles[index].arrival_tick - missiles[index].launch_tick <=
                    kLongestFlight);
        }
    }

    // The farthest missile's arrival, as its client was told it: the server
    // has not hurt that target a tick before, and has on the tick itself.
    const std::vector<KernelLockMissileView> observed = arena.missiles(1);
    const auto to_far = std::find_if(
        observed.begin(), observed.end(),
        [far](const KernelLockMissileView& view) { return view.target_net_id == far; });
    require(to_far != observed.end());
    require(hp(arena.server, near) == 100 - kDamage);
    while (arena.server.tick_loop_.current_tick() < to_far->arrival_tick) {
        require(hp(arena.server, far) == 100);
        arena.tick(false);
    }
    // The tick numbered arrival_tick has now been simulated.
    arena.tick(false);
    require(hp(arena.server, far) == 100 - kDamage);
    require(hp(arena.server, mid) == 100 - kDamage);
}

// An observer resolves each of the shooter's lock_instant commits when it is
// drawn, on its own view, and locks exactly the actors the server hurt. The
// lock has room for three; with two targets, the third arc is the shot's
// spray, the same for the same shot.
void an_observer_draws_each_lock_instant_commit_on_the_actors_hurt() {
    Arena arena(ne::WeaponFireMode::kLockInstant, 7851, true);
    const ne::NetId near = arena.spawn_enemy({5.0f, 0.0f, 0.0f});
    const ne::NetId far = arena.spawn_enemy({9.0f, 0.0f, 1.0f});
    const ne::NetId out_of_cone = arena.spawn_enemy({0.0f, 0.0f, 8.0f});

    for (int index = 0; index < 10; ++index) {
        arena.tick(false);
    }
    for (std::uint32_t index = 0; index < kCommitInterval + 1u; ++index) {
        arena.tick(true);
    }
    // The observer draws a snapshot interval or two behind.
    std::vector<KernelLockStrikeEvent> strikes;
    for (int index = 0; index < 12; ++index) {
        arena.tick(false);
        std::array<KernelLockStrikeEvent, 16> polled{};
        const std::uint32_t count = arena.clients[1].engine->poll_lock_strike_events(
            polled.data(), static_cast<std::uint32_t>(polled.size()));
        strikes.insert(strikes.end(), polled.begin(), polled.begin() + count);
    }
    // Two commits, two hits each.
    require(hp(arena.server, near) == 100 - 2 * kDamage);
    require(hp(arena.server, far) == 100 - 2 * kDamage);
    require(hp(arena.server, out_of_cone) == 100);

    require(strikes.size() == 2u);
    for (std::size_t index = 0; index < strikes.size(); ++index) {
        const KernelLockStrikeEvent& strike = strikes[index];
        require(strike.shooter_net_id == arena.shooter);
        require(strike.action_instance_id == kInstance);
        require(strike.commit_index == index);
        require(strike.selection.weapon_id == kWeaponId);
        require(strike.selection.arc_count == kLockSize);
        require(strike.selection.target_count == 2u);
        require(strike.selection.target_net_ids[0] == near);
        require(strike.selection.target_net_ids[1] == far);
    }
    // The unfilled arc is the shot's: the two commits spray differently.
    const KernelVec3 first = strikes[0].selection.arc_ends[2];
    const KernelVec3 second = strikes[1].selection.arc_ends[2];
    require(first.x != second.x || first.y != second.y || first.z != second.z);

    // The shooter's own client is not handed its own strikes -- it draws them
    // from the query at once -- and the query locks the same two.
    std::array<KernelLockStrikeEvent, 4> own{};
    require(arena.clients[0].engine->poll_lock_strike_events(own.data(), 4u) == 0u);
    KernelLockSelection query{};
    query.struct_size = sizeof(query);
    require(arena.clients[0].engine->client_query_lock_targets(
        kWeaponId,
        KernelVec3{0.0f, 1.0f, 0.0f},
        KernelVec3{1.0f, 0.0f, 0.0f},
        kInstance,
        0u,
        &query));
    require(query.target_count == 2u);
    require(query.target_net_ids[0] == near);
    require(query.target_net_ids[1] == far);
    // The shot's spray is the same wherever it is asked for.
    require(query.arc_ends[2].x == first.x && query.arc_ends[2].z == first.z);
}

}  // namespace

int main() {
    lock_missiles_reach_every_client_and_land_on_their_arrival();
    an_observer_draws_each_lock_instant_commit_on_the_actors_hurt();
    std::printf("lock_weapons_end_to_end_test: PASS\n");
    return 0;
}
