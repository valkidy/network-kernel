// A beam's origin and aim stopped travelling on the wire when the beam record
// shrank to net_id + reach. The client rebuilds both from the shooter, and this
// test pins the rebuild to what the server actually simulated: the reconstructed
// far end has to land where the replicated endpoint used to, or the beam is
// drawn somewhere the damage did not happen.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <optional>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#define private public
#include "kernel/src/kernel.h"
#undef private

#include "kernel/src/render_state_builder.h"
#include "protocol/public/network_packets.h"
#include "simulation/public/simulation.h"
#include "sync/public/snapshot.h"

namespace ne = network_example;

namespace {

constexpr float kEpsilon = 0.01f;

bool near_vec3(const glm::vec3& lhs, const glm::vec3& rhs) {
    return std::fabs(lhs.x - rhs.x) < kEpsilon &&
        std::fabs(lhs.y - rhs.y) < kEpsilon &&
        std::fabs(lhs.z - rhs.z) < kEpsilon;
}

glm::vec3 from_kernel(const KernelVec3& value) {
    return glm::vec3{value.x, value.y, value.z};
}

// What render_state_from_snapshot_entity hands the presentation, which is the
// only thing that has to be right -- EntitySnapshot no longer stores an
// endpoint of its own.
glm::vec3 rendered_beam_end(const ne::EntitySnapshot& entity) {
    return from_kernel(ne::render_state_from_snapshot_entity(entity, 1).beam_end);
}

const ne::EntitySnapshot& find_entity(
    const ne::WorldSnapshot& snapshot,
    ne::NetId net_id) {
    for (const ne::EntitySnapshot& entity : snapshot.entities) {
        if (entity.net_id == net_id) {
            return entity;
        }
    }
    assert(false && "entity missing from snapshot");
    std::abort();
}

// The client's view of a beam and its shooter: an EntitySpawn plus the
// projectile spawn batch behind it, which is where owner_net_id comes from.
void register_client_entities(
    ne::KernelEngine* engine,
    ne::NetId shooter,
    ne::NetId beam) {
    ne::KernelEngine::ClientReplicatedEntity shooter_entry{};
    shooter_entry.net_id = shooter;
    shooter_entry.type = ne::EntityType::kActor;
    shooter_entry.actor_type = ne::ActorType::kPlayer;
    engine->client_replicated_entities_.push_back(shooter_entry);

    ne::KernelEngine::ClientReplicatedEntity beam_entry{};
    beam_entry.net_id = beam;
    beam_entry.type = ne::EntityType::kProjectile;
    beam_entry.owner_net_id = shooter;
    engine->client_replicated_entities_.push_back(beam_entry);
}

// Server side: a shooter aiming somewhere, and a beam set up the way
// weapon_system.cc does it -- origin at projectile_launch_position(shooter),
// direction at the shooter's aim -- then advanced by the real simulate_beams so
// the transform and effective_length are written by production code.
struct ServerBeam {
    ne::NetId shooter = 0;
    ne::NetId beam = 0;
};

ServerBeam build_server_beam(
    ne::World* world,
    const glm::vec3& shooter_position,
    const glm::vec3& aim) {
    const ne::NetId shooter = world->spawn_player(1, shooter_position);
    const std::optional<entt::entity> shooter_entity = world->find_entity(shooter);
    assert(shooter_entity.has_value());
    world->registry().get<ne::Transform>(*shooter_entity).position =
        shooter_position;
    world->registry().emplace_or_replace<ne::ActionInputState>(*shooter_entity)
        .aim_direction = aim;

    const glm::vec3 origin = ne::projectile_launch_position(
        world->registry().get<ne::Transform>(*shooter_entity));
    const ne::NetId beam = world->spawn_projectile(1, origin, glm::vec3{0.0f});
    const std::optional<entt::entity> beam_entity = world->find_entity(beam);
    assert(beam_entity.has_value());
    ne::ProjectileBeamRuntime& runtime =
        world->registry().emplace<ne::ProjectileBeamRuntime>(*beam_entity);
    runtime.shooter_net_id = shooter;
    runtime.origin = origin;
    runtime.direction = aim;
    runtime.length = 8.0f;
    runtime.radius = 0.25f;
    runtime.damage_per_tick = 1;
    runtime.expire_tick = 100;
    return ServerBeam{shooter, beam};
}

// Server simulates, encodes, the client decodes and rebuilds. Returns the
// client's snapshot with the beam resolved.
ne::WorldSnapshot replicate(ne::KernelEngine* client, const ne::World& world) {
    const ne::WorldSnapshot server_snapshot =
        ne::build_world_snapshot(world, 10, 333, 0);
    const std::vector<std::uint8_t> bytes =
        ne::encode_snapshot_packet(server_snapshot);
    ne::WorldSnapshot decoded;
    assert(ne::decode_snapshot_packet(bytes.data(), bytes.size(), &decoded));
    client->resolve_client_beam_geometry(&decoded);
    return decoded;
}

void reconstructed_endpoint_matches_the_server() {
    ne::World world;
    const glm::vec3 shooter_position{4.0f, 0.0f, -2.0f};
    const glm::vec3 aim = glm::normalize(glm::vec3{1.0f, 0.0f, 1.0f});
    const ServerBeam ids = build_server_beam(&world, shooter_position, aim);

    // No collision world here, so the beam reaches its authored length; the
    // blocked case is covered separately below.
    ne::simulate_beams(world, 11, 1.0f / 30.0f, 0, nullptr, nullptr);

    KernelConfig config{};
    config.mode = KernelMode_Client;
    ne::KernelEngine client(config);
    register_client_entities(&client, ids.shooter, ids.beam);

    const ne::WorldSnapshot decoded = replicate(&client, world);
    const ne::EntitySnapshot& beam = find_entity(decoded, ids.beam);
    assert((beam.state_flags & ne::kSnapshotStateFlagProjectileBeam) != 0);

    const std::optional<entt::entity> beam_entity = world.find_entity(ids.beam);
    const ne::ProjectileBeamRuntime& runtime =
        world.registry().get<ne::ProjectileBeamRuntime>(*beam_entity);
    const glm::vec3 server_end =
        runtime.origin + runtime.direction * runtime.effective_length;

    std::printf(
        "origin server=(%.3f,%.3f,%.3f) client=(%.3f,%.3f,%.3f)\n",
        runtime.origin.x, runtime.origin.y, runtime.origin.z,
        beam.position.x, beam.position.y, beam.position.z);
    assert(near_vec3(beam.position, runtime.origin));

    const glm::vec3 client_end = rendered_beam_end(beam);
    std::printf(
        "end server=(%.3f,%.3f,%.3f) client=(%.3f,%.3f,%.3f)\n",
        server_end.x, server_end.y, server_end.z,
        client_end.x, client_end.y, client_end.z);
    assert(near_vec3(client_end, server_end));
}

void a_blocked_beam_replicates_the_short_reach() {
    ne::World world;
    const glm::vec3 aim{0.0f, 0.0f, 1.0f};
    const ServerBeam ids =
        build_server_beam(&world, glm::vec3{0.0f, 0.0f, 0.0f}, aim);

    const std::optional<entt::entity> beam_entity = world.find_entity(ids.beam);
    ne::ProjectileBeamRuntime& runtime =
        world.registry().get<ne::ProjectileBeamRuntime>(*beam_entity);
    // What a cast that hit a wall 2.5 m out leaves behind, plus the transform
    // simulate_beams writes alongside it.
    runtime.effective_length = 2.5f;
    world.registry().get<ne::Transform>(*beam_entity).position = runtime.origin;
    world.registry().get<ne::Transform>(*beam_entity).rotation =
        ne::beam_rotation(runtime.direction);

    KernelConfig config{};
    config.mode = KernelMode_Client;
    ne::KernelEngine client(config);
    register_client_entities(&client, ids.shooter, ids.beam);

    const ne::WorldSnapshot decoded = replicate(&client, world);
    const ne::EntitySnapshot& beam = find_entity(decoded, ids.beam);
    std::printf("blocked reach=%.3f\n", beam.beam_effective_length);
    assert(std::fabs(beam.beam_effective_length - 2.5f) < kEpsilon);
    // Stops at the wall, not at the authored 8 m.
    assert(near_vec3(
        rendered_beam_end(beam),
        runtime.origin + aim * 2.5f));
}

// Relevance culls the beam and the shooter independently, so a beam can outlive
// its shooter's presence in a snapshot. The last replicated aim is what keeps it
// pointing somewhere sane instead of collapsing to the world origin.
void a_culled_shooter_falls_back_to_its_last_aim() {
    ne::World world;
    const glm::vec3 shooter_position{1.0f, 0.0f, 0.0f};
    const glm::vec3 aim{0.0f, 0.0f, 1.0f};
    const ServerBeam ids = build_server_beam(&world, shooter_position, aim);
    ne::simulate_beams(world, 11, 1.0f / 30.0f, 0, nullptr, nullptr);

    KernelConfig config{};
    config.mode = KernelMode_Client;
    ne::KernelEngine client(config);
    register_client_entities(&client, ids.shooter, ids.beam);
    client.client_replicated_entities_[0].position = shooter_position;
    client.client_replicated_entities_[0].aim_direction = aim;

    ne::WorldSnapshot server_snapshot = ne::build_world_snapshot(world, 10, 333, 0);
    // Drop the shooter the way relevance filtering would.
    std::vector<ne::EntitySnapshot> kept;
    for (const ne::EntitySnapshot& entity : server_snapshot.entities) {
        if (entity.net_id != ids.shooter) {
            kept.push_back(entity);
        }
    }
    server_snapshot.entities = kept;

    const std::vector<std::uint8_t> bytes =
        ne::encode_snapshot_packet(server_snapshot);
    ne::WorldSnapshot decoded;
    assert(ne::decode_snapshot_packet(bytes.data(), bytes.size(), &decoded));
    client.resolve_client_beam_geometry(&decoded);

    const ne::EntitySnapshot& beam = find_entity(decoded, ids.beam);
    const glm::vec3 expected_origin =
        shooter_position + glm::vec3{0.0f, 1.0f, 0.0f};
    std::printf(
        "fallback origin=(%.3f,%.3f,%.3f) reach=%.3f\n",
        beam.position.x, beam.position.y, beam.position.z,
        beam.beam_effective_length);
    assert(near_vec3(beam.position, expected_origin));
    assert(near_vec3(rendered_beam_end(beam), expected_origin + aim * 8.0f));
}

// A beam a snapshot mentions before its spawn batch has landed has no shooter to
// hang off. It must not be drawn full length out of the world origin.
void an_unattached_beam_collapses_instead_of_pointing_at_nothing() {
    ne::World world;
    const ServerBeam ids = build_server_beam(
        &world,
        glm::vec3{3.0f, 0.0f, 3.0f},
        glm::vec3{1.0f, 0.0f, 0.0f});
    ne::simulate_beams(world, 11, 1.0f / 30.0f, 0, nullptr, nullptr);

    KernelConfig config{};
    config.mode = KernelMode_Client;
    ne::KernelEngine client(config);
    // Deliberately nothing registered: no EntitySpawn, no spawn batch.

    const ne::WorldSnapshot decoded = replicate(&client, world);
    const ne::EntitySnapshot& beam = find_entity(decoded, ids.beam);
    std::printf("unattached reach=%.3f\n", beam.beam_effective_length);
    assert(beam.beam_effective_length == 0.0f);
    assert(near_vec3(rendered_beam_end(beam), glm::vec3{0.0f, 0.0f, 0.0f}));
}

// Interpolation moved from the endpoint to the reach. Halfway between a 2 m and
// a 6 m beam is a 4 m one, riding an origin and aim that are themselves blended.
void interpolation_blends_the_reach() {
    ne::EntitySnapshot from;
    from.net_id = 5;
    from.type = ne::EntityType::kProjectile;
    from.state_flags |= ne::kSnapshotStateFlagProjectileBeam;
    from.position = glm::vec3{0.0f, 1.0f, 0.0f};
    from.rotation = ne::beam_rotation(glm::vec3{0.0f, 0.0f, 1.0f});
    from.beam_effective_length = 2.0f;

    ne::EntitySnapshot to = from;
    to.beam_effective_length = 6.0f;

    const ne::EntitySnapshot mid = ne::interpolate_snapshot_entity(from, to, 0.5f);
    std::printf("interpolated reach=%.3f\n", mid.beam_effective_length);
    assert(std::fabs(mid.beam_effective_length - 4.0f) < kEpsilon);
    assert(near_vec3(rendered_beam_end(mid), glm::vec3{0.0f, 1.0f, 4.0f}));
}

}  // namespace

int main() {
    reconstructed_endpoint_matches_the_server();
    a_blocked_beam_replicates_the_short_reach();
    a_culled_shooter_falls_back_to_its_last_aim();
    an_unattached_beam_collapses_instead_of_pointing_at_nothing();
    interpolation_blends_the_reach();
    std::printf("OK\n");
    return 0;
}
