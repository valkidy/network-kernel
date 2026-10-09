// A held burst fire -- the Projectile Spammer's shape: three pellets a commit,
// the trigger held -- from a dedicated server to a pure client, through the
// encoder, a transport and the decoder. A pure client has no weapon of its own
// in world_, so it predicts none of these shots: every one is built from the
// authority's spawn record. What this pins (docs/HELD_FIRE_PROJECTILE_
// PREDICTION_PLAN.md, P3):
//   - releasing the trigger -- a terminal action result for the one instance
//     id every shot shares -- leaves the shots in flight;
//   - each client shot is the authority's shot of the same net id, commit and
//     pellet, and stays so as snapshots correct it -- none is pulled onto
//     another's path, none drawn twice, none missing.
// Two engines and a shuttle standing in for the network, as
// suspension_end_to_end_test does it.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
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

constexpr ne::PeerId kPeer = 1;
constexpr std::uint8_t kWeaponId = 2;
constexpr std::uint32_t kProjectileTemplateId = 2;
constexpr std::uint32_t kFireActionId = 4098;
constexpr std::uint32_t kInstance = 7301;
constexpr std::uint32_t kCommitInterval = 2;

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

void shuttle(ne::LoopbackTransport* from, ne::LoopbackTransport* to) {
    ne::TransportEvent event;
    while (from->PollClientEvent(event)) {
        require(to->SendClient(
            event.peer,
            event.payload.data(),
            static_cast<std::uint32_t>(event.payload.size()),
            event.mode,
            event.channel));
    }
}

// The spammer's bullet: slow, straight, long-lived -- nothing in this arena
// for it to strike, so every shot is still flying when the checks run.
ne::RuntimeProjectileTemplate bullet(ne::ProjectileSyncMode sync_mode) {
    ne::RuntimeProjectileTemplate projectile{};
    projectile.projectile_template_id = kProjectileTemplateId;
    projectile.weapon_id = kWeaponId;
    projectile.projectile_type = ne::ProjectileType::kStandard;
    projectile.motion_model = ne::ProjectileMotionModel::kLinear;
    projectile.sync_mode = sync_mode;
    projectile.speed = 5.0f;
    projectile.lifetime_ticks = 120;
    return projectile;
}

// spammer_fire: held, cancelled on release.
ne::RuntimeActionTemplate fire_action() {
    return ne::RuntimeActionTemplate{
        kFireActionId,
        KernelActionTriggerMode_Hold,
        KernelActionTemplateFlag_CancelOnRelease |
            KernelActionTemplateFlag_CancelOnDeath |
            KernelActionTemplateFlag_CancelOnWeaponChange |
            KernelActionTemplateFlag_CancelBeforeFirstCommit,
        1,
        0,
        kCommitInterval,
        0,
        4,
        6,
    };
}

void arm(ne::KernelEngine* server, ne::NetId player) {
    ne::World& world = server->world_;
    const entt::entity entity = *world.find_entity(player);
    world.registry().get_or_emplace<ne::Health>(entity) = ne::Health{100, 100};
    ne::WeaponTuning& tuning = world.registry().get_or_emplace<ne::WeaponTuning>(entity);
    tuning.configured[kWeaponId] = true;
    ne::WeaponMechanicsDefinition& spammer = tuning.definitions[kWeaponId];
    spammer.id = kWeaponId;
    spammer.mode = ne::WeaponFireMode::kProjectile;
    spammer.magazine_size = 30;
    spammer.projectile_template_id = kProjectileTemplateId;
    spammer.fire_action_template_id = kFireActionId;
    spammer.pellet_count = 3;
    spammer.pellet_spread = 15.0f;
    ne::WeaponState& weapon = world.registry().get_or_emplace<ne::WeaponState>(entity);
    weapon.weapon_slot_count = ne::kWeaponSlotCount;
    weapon.active_weapon_slot = kWeaponId;
    weapon.weapon_ids[kWeaponId] = kWeaponId;
    weapon.ammo[kWeaponId] = 30;
    weapon.reserve_magazines[kWeaponId] = 3;
}

struct ServerShot {
    std::uint16_t commit_index = 0;
    std::uint8_t burst_index = 0;
    glm::vec3 initial_velocity{0.0f};
};

std::map<ne::NetId, ServerShot> server_shots(ne::KernelEngine& server) {
    std::map<ne::NetId, ServerShot> shots;
    auto view = server.world_.registry()
                    .view<const ne::NetworkIdentity, const ne::ProjectileState>();
    for (const auto [entity, identity, projectile] : view.each()) {
        (void)entity;
        if (projectile.action_instance_id == kInstance) {
            shots[identity.net_id] = ServerShot{
                projectile.commit_index, projectile.burst_index,
                projectile.initial_velocity};
        }
    }
    return shots;
}

// Every client shot is the authority's shot of its net id: same commit, same
// pellet, same path; one each, none missing.
void require_client_matches(
    const ne::KernelEngine& client,
    const std::map<ne::NetId, ServerShot>& shots) {
    std::map<ne::NetId, int> seen;
    for (const auto& projectile : client.predicted_projectiles_) {
        if (projectile.action_instance_id != kInstance) {
            continue;
        }
        // A pure client predicts nothing: every shot is the authority's.
        require(projectile.net_id != 0u);
        const auto shot = shots.find(projectile.net_id);
        require(shot != shots.end());
        require(projectile.commit_index == shot->second.commit_index);
        require(projectile.burst_index == shot->second.burst_index);
        require(glm::length(projectile.initial_velocity -
                            shot->second.initial_velocity) < 1e-3f);
        ++seen[projectile.net_id];
    }
    require(seen.size() == shots.size());
    for (const auto& [net_id, count] : seen) {
        (void)net_id;
        require(count == 1);
    }
}

// One hold, as the client sees it. Returns how many of the shots the fullest
// snapshot carried.
std::size_t hold_and_release(ne::ProjectileSyncMode sync_mode, std::uint16_t port) {
    KernelConfig server_config{};
    server_config.mode = KernelMode_DedicatedServer;
    server_config.tick.server_tick_rate = 30;
    server_config.tick.snapshot_rate = 15;
    server_config.max_events = 1024;
    server_config.max_render_states = 256;
    ne::KernelEngine server(server_config);
    ne::LoopbackTransport* server_link =
        attach_loopback(&server, KernelMode_DedicatedServer, port);

    KernelConfig client_config = server_config;
    client_config.mode = KernelMode_Client;
    ne::KernelEngine client(client_config);
    ne::LoopbackTransport* client_link =
        attach_loopback(&client, KernelMode_Client, static_cast<std::uint16_t>(port + 1u));

    for (ne::KernelEngine* engine : {&server, &client}) {
        engine->catalog_runtime_.projectile_templates.push_back(bullet(sync_mode));
        engine->catalog_runtime_.action_templates.push_back(fire_action());
    }
    const ne::NetId player = server.world_.spawn_player(kPeer, glm::vec3{0.0f});
    arm(&server, player);
    server.peer_sessions_.push_back(ne::KernelEngine::PeerSession{kPeer, player, 0, true, {}});
    client.local_client_peer_id_ = kPeer;
    client.local_player_net_id_ = player;
    // A pure client: its own player is not in its world, so it has no weapon
    // to predict with.
    require(!client.world_.find_entity(player).has_value());

    std::uint32_t input_seq = 1;
    const auto tick = [&](bool held) {
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
            ne::encode_player_input_packet(kPeer, input, input.input_seq);
        require(server_link->SendClient(
            kPeer, packet.data(), static_cast<std::uint32_t>(packet.size()),
            ne::SendMode::kUnreliable, ne::ChannelId::kInput));
        server.poll_transport();
        server.simulate_tick();
        shuttle(server_link, client_link);
        client.poll_transport();
    };

    for (int index = 0; index < 8; ++index) {
        tick(false);
    }
    // Held through three commits, three pellets each; then released.
    for (std::uint32_t index = 0; index < 2u * kCommitInterval + 1u; ++index) {
        tick(true);
    }
    tick(false);
    tick(false);

    const std::map<ne::NetId, ServerShot> shots = server_shots(server);
    std::fprintf(stderr, "held_fire: %zu shots in flight on the authority\n", shots.size());
    require(shots.size() == 9u);
    // The release reached the client as a terminal result.
    const auto released = client.applied_local_action_results_.find(kInstance);
    require(released != client.applied_local_action_results_.end());
    require(released->second.result == KernelLocalActionResultType_Corrected);
    require(released->second.reason == KernelLocalActionResultReason_Cancelled);
    require(released->second.confirmed_commit_count == 3u);
    require_client_matches(client, shots);

    // A second of flight, snapshots correcting it all the while: each shot
    // stays the one it is.
    std::size_t most_in_a_snapshot = 0;
    for (int index = 0; index < 30; ++index) {
        tick(false);
        std::size_t in_snapshot = 0;
        for (const ne::EntitySnapshot& entity : client.latest_client_snapshot_.entities) {
            if (entity.type == ne::EntityType::kProjectile &&
                entity.action_instance_id == kInstance) {
                ++in_snapshot;
            }
        }
        most_in_a_snapshot = std::max(most_in_a_snapshot, in_snapshot);
        require_client_matches(client, shots);
    }
    std::fprintf(stderr, "held_fire: up to %zu of the shots in one snapshot\n",
                 most_in_a_snapshot);
    return most_in_a_snapshot;
}

}  // namespace

int main() {
    // The spammer's and the staff's bullets: flown from their spawns alone,
    // never in a snapshot.
    require(hold_and_release(ne::ProjectileSyncMode::kLocalPredictedDeterministic, 7797) == 0u);
    // A bullet the snapshots keep correcting: each correction must find the
    // shot it names, not the action's first.
    require(hold_and_release(ne::ProjectileSyncMode::kHybridDeterministicThenSnapshot, 7799) > 1u);
    std::printf("held_fire_end_to_end_test: PASS\n");
    return 0;
}
