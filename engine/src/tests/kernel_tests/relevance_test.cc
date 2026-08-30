#include <cstdio>
#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#define private public
#include "kernel/src/kernel.h"
#undef private

#include "protocol/public/network_packets.h"
#include "transport/public/listen_server_transport.h"
#include "transport/public/loopback_transport.h"

namespace {

bool contains_entity(
    const network_example::WorldSnapshot& snapshot,
    network_example::NetId net_id) {
    return std::any_of(
        snapshot.entities.begin(),
        snapshot.entities.end(),
        [net_id](const network_example::EntitySnapshot& entity) {
            return entity.net_id == net_id;
        });
}

// assert() is compiled out under -c opt, which is how this suite is normally
// run, so anything that must actually gate uses this instead.
void require_impl(bool condition, const char* expression, int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}

#define require(condition) require_impl((condition), #condition, __LINE__)

void set_position(
    network_example::World& world,
    network_example::NetId net_id,
    const glm::vec3& position) {
    const std::optional<entt::entity> entity = world.find_entity(net_id);
    assert(entity.has_value());
    world.registry().get<network_example::Transform>(*entity).position = position;
}

bool poll_despawn(
    network_example::KernelEngine& engine,
    network_example::NetId expected_net_id,
    std::uint32_t expected_reason) {
    network_example::TransportEvent event;
    while (engine.transport_->PollEvent(event)) {
        if (event.channel != network_example::ChannelId::kReliableEvent) {
            continue;
        }
        network_example::EntityDespawnPacket despawn{};
        if (!network_example::decode_entity_despawn_packet(
                event.payload.data(),
                event.payload.size(),
                &despawn)) {
            continue;
        }
        if (despawn.net_id == expected_net_id && despawn.reason == expected_reason) {
            return true;
        }
    }
    return false;
}

bool poll_prop_bootstrap(
    network_example::KernelEngine& engine,
    network_example::NetId expected_net_id,
    std::uint16_t expected_hp) {
    bool saw_spawn = false;
    network_example::TransportEvent event;
    while (engine.transport_->PollEvent(event)) {
        if (event.channel != network_example::ChannelId::kReliableEvent) {
            continue;
        }
        network_example::EntitySpawnPacket spawn{};
        if (network_example::decode_entity_spawn_packet(
                event.payload.data(), event.payload.size(), &spawn)) {
            if (spawn.net_id == expected_net_id) {
                saw_spawn = true;
                assert(spawn.entity_type == network_example::EntityType::kProp);
            }
            continue;
        }
        network_example::PropStateChangeBatchPacket prop_state{};
        if (!network_example::decode_prop_state_change_batch_packet(
                event.payload.data(), event.payload.size(), &prop_state)) {
            continue;
        }
        for (const network_example::PropStateChangeRecord& record :
             prop_state.records) {
            if (record.net_id == expected_net_id) {
                assert(saw_spawn);
                assert(record.world_mode == KernelWorldItemMode_Placed);
                assert((record.changed_fields &
                        network_example::kPropStateChangeHealth) != 0u);
                assert(record.hp == expected_hp);
                return true;
            }
        }
    }
    return false;
}

std::vector<network_example::EntityDespawnPacket> poll_client_despawns(
    network_example::LoopbackTransport* transport) {
    std::vector<network_example::EntityDespawnPacket> despawns;
    if (transport == nullptr) {
        return despawns;
    }
    network_example::TransportEvent event;
    while (transport->PollClientEvent(event)) {
        network_example::EntityDespawnPacket despawn{};
        if (event.channel == network_example::ChannelId::kReliableEvent &&
            network_example::decode_entity_despawn_packet(
                event.payload.data(),
                event.payload.size(),
                &despawn)) {
            despawns.push_back(despawn);
        }
    }
    return despawns;
}

std::vector<network_example::EntityDespawnPacket> poll_local_client_despawns(
    network_example::ListenServerTransport* transport) {
    std::vector<network_example::EntityDespawnPacket> despawns;
    if (transport == nullptr) {
        return despawns;
    }
    network_example::TransportEvent event;
    while (transport->PollLocalClientEvent(event)) {
        network_example::EntityDespawnPacket despawn{};
        if (event.channel == network_example::ChannelId::kReliableEvent &&
            network_example::decode_entity_despawn_packet(
                event.payload.data(),
                event.payload.size(),
                &despawn)) {
            despawns.push_back(despawn);
        }
    }
    return despawns;
}

std::size_t count_despawn(
    const std::vector<network_example::EntityDespawnPacket>& despawns,
    network_example::NetId net_id,
    std::uint32_t reason) {
    return static_cast<std::size_t>(std::count_if(
        despawns.begin(),
        despawns.end(),
        [net_id, reason](const network_example::EntityDespawnPacket& despawn) {
            return despawn.net_id == net_id && despawn.reason == reason;
        }));
}

void configure_expiring_projectiles(
    network_example::KernelEngine* engine,
    network_example::PeerId owner_peer,
    std::vector<network_example::NetId>* out_projectiles) {
    assert(engine != nullptr);
    assert(out_projectiles != nullptr);

    const network_example::NetId projectile = engine->world_.spawn_projectile(
        owner_peer,
        glm::vec3{1.0f, 0.0f, 0.0f},
        glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId area_effect = engine->world_.spawn_projectile(
        owner_peer,
        glm::vec3{2.0f, 0.0f, 0.0f},
        glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId beam = engine->world_.spawn_projectile(
        owner_peer,
        glm::vec3{3.0f, 0.0f, 0.0f},
        glm::vec3{0.0f, 0.0f, 0.0f});

    const std::optional<entt::entity> projectile_entity =
        engine->world_.find_entity(projectile);
    const std::optional<entt::entity> area_effect_entity =
        engine->world_.find_entity(area_effect);
    const std::optional<entt::entity> beam_entity =
        engine->world_.find_entity(beam);
    assert(projectile_entity.has_value());
    assert(area_effect_entity.has_value());
    assert(beam_entity.has_value());

    engine->world_.registry()
        .get<network_example::ProjectileState>(*projectile_entity)
        .max_lifetime_ticks = 1;
    engine->world_.registry()
        .emplace<network_example::ProjectileAreaEffectRuntime>(*area_effect_entity)
        .expire_tick = 1;
    engine->world_.registry()
        .emplace<network_example::ProjectileBeamRuntime>(*beam_entity)
        .expire_tick = 1;
    *out_projectiles = {projectile, area_effect, beam};
}

std::size_t poll_client_spawn_count(
    network_example::LoopbackTransport* transport) {
    std::size_t spawns = 0;
    if (transport == nullptr) {
        return spawns;
    }
    network_example::TransportEvent event;
    while (transport->PollClientEvent(event)) {
        network_example::EntitySpawnPacket spawn{};
        if (event.channel == network_example::ChannelId::kReliableEvent &&
            network_example::decode_entity_spawn_packet(
                event.payload.data(), event.payload.size(), &spawn)) {
            ++spawns;
        }
    }
    return spawns;
}

// A crowd is introduced over several snapshots rather than all at once.
//
// Every introduction is a reliable entity spawn plus a locomotion baseline, and
// none of it is charged to the snapshot byte budget -- so before the quota, a
// player who became relevant to sixty agents in one snapshot produced sixty
// reliable packets in that snapshot. Both halves matter here: the per-snapshot
// ceiling, and that the backlog still drains, because deferring is only
// acceptable if the deferred entities actually arrive.
void a_crowd_is_introduced_over_several_snapshots() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);

    auto transport = std::make_unique<network_example::LoopbackTransport>();
    network_example::LoopbackTransport* loopback = transport.get();
    engine.transport_ = std::move(transport);
    // Started after reset_runtime_state, not before: the reset stops the
    // transport, and a stopped LoopbackTransport drops every Send silently.
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    require(loopback->StartServer(7783));

    constexpr std::size_t kCrowd = 60;
    // The quota is 16; one more is allowed because the session's own player is
    // exempt from it.
    constexpr std::size_t kMaxSpawnsPerSnapshot = 17;
    const network_example::NetId player =
        engine.world_.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    for (std::size_t index = 0; index < kCrowd; ++index) {
        // Well inside the 40 m relevance radius, so all of them are relevant
        // from the very first snapshot.
        engine.world_.spawn_enemy(glm::vec3{
            1.0f + static_cast<float>(index) * 0.5f, 0.0f, 0.0f});
    }
    network_example::KernelEngine::PeerSession session{1, player, 0, true, {}};
    engine.peer_sessions_.push_back(std::move(session));

    const std::size_t expected_total = kCrowd + 1;
    std::size_t announced = 0;
    std::size_t snapshots = 0;
    while (announced < expected_total) {
        // Two ticks per snapshot at a 30 Hz tick and a 15 Hz snapshot rate.
        engine.simulate_tick();
        engine.simulate_tick();
        const std::size_t spawns = poll_client_spawn_count(loopback);
        require(spawns <= kMaxSpawnsPerSnapshot);
        announced += spawns;
        ++snapshots;
        require(snapshots < 32);
    }
    require(announced == expected_total);
    // 61 introductions cannot have fitted into three snapshots of 16.
    require(snapshots >= 4);
    require(
        engine.peer_sessions_[0].relevant_entities.size() == expected_total);
}

// The relevance band, on its own engine so that it actually runs: everything in
// main() sits below a long-standing abort and never executes under a debug
// build, and its assert()s are compiled out under -c opt.
void relevance_holds_until_a_wider_radius_than_it_entered() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);

    auto transport = std::make_unique<network_example::LoopbackTransport>();
    network_example::LoopbackTransport* loopback = transport.get();
    engine.transport_ = std::move(transport);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    require(loopback->StartServer(7784));

    const network_example::NetId player =
        engine.world_.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        engine.world_.spawn_enemy(glm::vec3{10.0f, 0.0f, 0.0f});
    // Relevance measures to an entity's near edge, so a hitbox of any size would
    // shift the boundaries this test pins by exactly its radius. Zeroed here so
    // that what is under test is the band and nothing else; the radius has its
    // own test.
    const std::optional<entt::entity> enemy_entity =
        engine.world_.find_entity(enemy);
    require(enemy_entity.has_value());
    engine.world_.registry().get<network_example::Hitbox>(*enemy_entity)
        .half_extents = glm::vec3{0.0f, 0.0f, 0.0f};
    network_example::KernelEngine::PeerSession session{1, player, 0, true, {}};
    engine.peer_sessions_.push_back(std::move(session));

    const auto relevant_at = [&](float x) {
        set_position(engine.world_, enemy, glm::vec3{x, 0.0f, 0.0f});
        network_example::KernelEngine::PeerSession& live =
            engine.peer_sessions_[0];
        const network_example::WorldSnapshot snapshot =
            engine.build_relevant_snapshot(live, 0);
        const bool contained = contains_entity(snapshot, enemy);
        engine.sync_session_relevance(&live, snapshot);
        return contained;
    };

    require(relevant_at(10.0f));
    // Past the entry radius, but leaving costs a reliable despawn and coming
    // back costs a reliable spawn, so it holds.
    require(relevant_at(40.01f));
    require(!relevant_at(44.01f));
    // And the band holds on the way back in, or it would not be a band.
    require(!relevant_at(42.0f));
    require(relevant_at(39.0f));
}

// Why the send order is keyed on net id rather than on a position in the
// relevant list: an index only survives as long as the list does.
void a_departure_does_not_cost_the_next_entity_its_turn() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);

    network_example::EntitySnapshot agent;
    agent.type = network_example::EntityType::kActor;
    agent.actor_type = network_example::ActorType::kAgent;
    network_example::EntitySnapshot first = agent;
    first.net_id = 211;
    network_example::EntitySnapshot second = agent;
    second.net_id = 212;
    network_example::EntitySnapshot third = agent;
    third.net_id = 213;

    network_example::WorldSnapshot all_three;
    all_three.entities = {first, second, third};
    network_example::WorldSnapshot without_first;
    without_first.entities = {second, third};
    const std::size_t one_agent_budget =
        network_example::estimate_snapshot_base_packet_size() +
        network_example::estimate_snapshot_entity_size(first);

    network_example::KernelEngine::PeerSession session{1, 0, 0, true, {}};
    const network_example::WorldSnapshot round_one =
        engine.build_snapshot_send_set(session, all_three, one_agent_budget);
    require(contains_entity(round_one, 211));
    // 211 has left. An index of 1 would now point past 212 to 213, and 212
    // would wait out another full cycle for a turn it had already earned.
    const network_example::WorldSnapshot round_two =
        engine.build_snapshot_send_set(session, without_first, one_agent_budget);
    require(contains_entity(round_two, 212));
    require(!contains_entity(round_two, 213));
    const network_example::WorldSnapshot round_three =
        engine.build_snapshot_send_set(session, without_first, one_agent_budget);
    require(contains_entity(round_three, 213));
    require(!contains_entity(round_three, 212));
}

// Slots are weighted by distance, so a crowd the player is standing in front of
// refreshes faster than one at the edge of the relevance sphere.
void a_nearer_agent_is_served_more_often_than_a_distant_one() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);

    network_example::EntitySnapshot player;
    player.net_id = 1;
    player.type = network_example::EntityType::kActor;
    player.actor_type = network_example::ActorType::kPlayer;
    network_example::EntitySnapshot near_agent;
    near_agent.net_id = 2;
    near_agent.type = network_example::EntityType::kActor;
    near_agent.actor_type = network_example::ActorType::kAgent;
    near_agent.position = glm::vec3{5.0f, 0.0f, 0.0f};
    network_example::EntitySnapshot far_agent = near_agent;
    far_agent.net_id = 3;
    far_agent.position = glm::vec3{30.0f, 0.0f, 0.0f};

    network_example::WorldSnapshot relevant;
    relevant.entities = {player, near_agent, far_agent};
    // Room for the player record, which is always written, and exactly one
    // agent -- so every snapshot is a choice between the two.
    const std::size_t one_agent_budget =
        network_example::estimate_snapshot_base_packet_size() +
        network_example::estimate_snapshot_entity_size(player) +
        network_example::estimate_snapshot_entity_size(near_agent);

    network_example::KernelEngine::PeerSession session{1, player.net_id, 0, true, {}};
    constexpr std::size_t kSnapshots = 24;
    std::size_t near_sends = 0;
    std::size_t far_sends = 0;
    for (std::size_t index = 0; index < kSnapshots; ++index) {
        const network_example::WorldSnapshot send =
            engine.build_snapshot_send_set(session, relevant, one_agent_budget);
        if (contains_entity(send, near_agent.net_id)) {
            ++near_sends;
        }
        if (contains_entity(send, far_agent.net_id)) {
            ++far_sends;
        }
    }
    require(near_sends + far_sends == kSnapshots);
    // The near band is weighted four to one against the far band; asserted as
    // a factor of two so that tuning the constants does not have to come back
    // through this test to stay true.
    require(near_sends > far_sends * 2);
    // Weighted, not exclusive. Losing every turn forever is the failure this
    // guards against.
    require(far_sends > 0);
}

// The backstop under the weighting. Outvoted by a crowd of higher-weight
// neighbours, an agent still gets a turn inside the starvation window rather
// than waiting for a share it will never be given.
void an_outvoted_agent_is_still_served_within_the_starvation_window() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);

    network_example::EntitySnapshot player;
    player.net_id = 1;
    player.type = network_example::EntityType::kActor;
    player.actor_type = network_example::ActorType::kPlayer;
    network_example::EntitySnapshot agent;
    agent.type = network_example::EntityType::kActor;
    agent.actor_type = network_example::ActorType::kAgent;

    network_example::WorldSnapshot relevant;
    relevant.entities.push_back(player);
    // Five agents in the near band against one in the far band. Their combined
    // weight is 21 to 1 with a single slot to share, so the far one's weighted
    // share alone would leave it waiting longer than the window allows.
    for (std::uint32_t index = 0; index < 5u; ++index) {
        network_example::EntitySnapshot near_agent = agent;
        near_agent.net_id = 10u + index;
        near_agent.position = glm::vec3{2.0f + static_cast<float>(index), 0.0f, 0.0f};
        relevant.entities.push_back(near_agent);
    }
    network_example::EntitySnapshot far_agent = agent;
    far_agent.net_id = 99;
    far_agent.position = glm::vec3{35.0f, 0.0f, 0.0f};
    relevant.entities.push_back(far_agent);

    const std::size_t one_agent_budget =
        network_example::estimate_snapshot_base_packet_size() +
        network_example::estimate_snapshot_entity_size(player) +
        network_example::estimate_snapshot_entity_size(far_agent);

    network_example::KernelEngine::PeerSession session{1, player.net_id, 0, true, {}};
    constexpr std::size_t kSnapshots = 60;
    std::size_t worst_gap = 0;
    std::size_t gap = 0;
    std::size_t far_sends = 0;
    for (std::size_t index = 0; index < kSnapshots; ++index) {
        const network_example::WorldSnapshot send =
            engine.build_snapshot_send_set(session, relevant, one_agent_budget);
        if (contains_entity(send, far_agent.net_id)) {
            ++far_sends;
            gap = 0;
            continue;
        }
        ++gap;
        worst_gap = std::max(worst_gap, gap);
    }
    require(far_sends > 0);
    // kMaxSnapshotsWithoutSend is 15; one more for the snapshot the promotion
    // is decided on.
    require(worst_gap <= 16);
}

// A teammate's record is scheduled, not guaranteed -- except the receiving
// session's own, which is neither.
void a_distant_teammate_competes_and_the_own_player_does_not() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);

    network_example::EntitySnapshot own_player;
    own_player.net_id = 1;
    own_player.type = network_example::EntityType::kActor;
    own_player.actor_type = network_example::ActorType::kPlayer;
    network_example::EntitySnapshot teammate = own_player;
    teammate.net_id = 2;
    teammate.position = glm::vec3{35.0f, 0.0f, 0.0f};
    network_example::EntitySnapshot near_agent;
    near_agent.net_id = 3;
    near_agent.type = network_example::EntityType::kActor;
    near_agent.actor_type = network_example::ActorType::kAgent;
    near_agent.position = glm::vec3{5.0f, 0.0f, 0.0f};
    // Mid-action doubles its weight, so a near agent doing something outranks a
    // teammate at the edge of the sphere.
    near_agent.action_template_id = 1002;
    near_agent.action_phase = KernelActionPhase_Active;

    network_example::WorldSnapshot relevant;
    relevant.entities = {own_player, teammate, near_agent};
    // The own player, plus room for one of the other two -- the teammate record
    // is the larger of them, so whichever wins takes the whole remainder.
    const std::size_t budget =
        network_example::estimate_snapshot_base_packet_size() +
        network_example::estimate_snapshot_entity_size(own_player) +
        network_example::estimate_snapshot_entity_size(teammate);

    network_example::KernelEngine::PeerSession session{1, own_player.net_id, 0, true, {}};
    constexpr std::size_t kSnapshots = 24;
    std::size_t own_sends = 0;
    std::size_t teammate_sends = 0;
    std::size_t agent_sends = 0;
    for (std::size_t index = 0; index < kSnapshots; ++index) {
        const network_example::WorldSnapshot send =
            engine.build_snapshot_send_set(session, relevant, budget);
        if (contains_entity(send, own_player.net_id)) {
            ++own_sends;
        }
        if (contains_entity(send, teammate.net_id)) {
            ++teammate_sends;
        }
        if (contains_entity(send, near_agent.net_id)) {
            ++agent_sends;
        }
    }
    // Never scheduled, never skipped: local prediction is reconciled against it.
    require(own_sends == kSnapshots);
    // The change this pins. A teammate record used to be written before the
    // rotation ran at all, so this was kSnapshots too.
    require(teammate_sends < kSnapshots);
    require(teammate_sends > 0);
    // The agent gets turns it could not have had before, when the teammate was
    // written ahead of the rotation and took the whole remainder every time.
    // How the two divide those turns is the weighting's business and is pinned
    // by a_nearer_agent_is_served_more_often_than_a_distant_one; with only two
    // entities and one slot the integer arithmetic ties often enough that a
    // ratio asserted here would be measuring the tie-break, not the weights.
    require(agent_sends > 0);
}

// One budget, one queue. Agents used to be served to exhaustion before the
// projectile pass was reached and before props were written at all, so a crowd
// of relevant agents meant a projectile reached the client at a rate of zero --
// measured, not hypothetical: agent_cpu_bench packed 0 projectiles at 196
// agents and 8 at 20.
void every_kind_of_entity_shares_one_budget() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);

    network_example::EntitySnapshot own_player;
    own_player.net_id = 1;
    own_player.type = network_example::EntityType::kActor;
    own_player.actor_type = network_example::ActorType::kPlayer;
    own_player.has_authoritative_movement_state = true;

    network_example::WorldSnapshot relevant;
    relevant.entities.push_back(own_player);
    // Enough agents to take every slot the old order would have given them.
    for (std::uint32_t index = 0; index < 20u; ++index) {
        network_example::EntitySnapshot agent;
        agent.net_id = 100u + index;
        agent.type = network_example::EntityType::kActor;
        agent.actor_type = network_example::ActorType::kAgent;
        agent.position = glm::vec3{2.0f + static_cast<float>(index), 0.0f, 0.0f};
        relevant.entities.push_back(agent);
    }
    for (std::uint32_t index = 0; index < 3u; ++index) {
        network_example::EntitySnapshot projectile;
        projectile.net_id = 200u + index;
        projectile.type = network_example::EntityType::kProjectile;
        projectile.position = glm::vec3{3.0f + static_cast<float>(index), 0.0f, 0.0f};
        projectile.velocity = glm::vec3{40.0f, 0.0f, 0.0f};
        relevant.entities.push_back(projectile);
    }
    for (std::uint32_t index = 0; index < 2u; ++index) {
        network_example::EntitySnapshot prop;
        prop.net_id = 300u + index;
        prop.type = network_example::EntityType::kProp;
        prop.position = glm::vec3{4.0f + static_cast<float>(index), 0.0f, 0.0f};
        relevant.entities.push_back(prop);
    }

    network_example::EntitySnapshot sizing_agent;
    sizing_agent.type = network_example::EntityType::kActor;
    sizing_agent.actor_type = network_example::ActorType::kAgent;
    // The player, which is never scheduled, plus room for four agent records --
    // far less than the twenty agents want.
    const std::size_t budget =
        network_example::estimate_snapshot_base_packet_size() +
        network_example::estimate_snapshot_entity_size(own_player) +
        4u * network_example::estimate_snapshot_entity_size(sizing_agent);

    network_example::KernelEngine::PeerSession session{1, own_player.net_id, 0, true, {}};
    constexpr std::size_t kSnapshots = 24;
    std::size_t agent_sends = 0;
    std::size_t projectile_sends = 0;
    std::size_t prop_sends = 0;
    std::size_t first_projectile_gap = 0;
    std::size_t worst_first_projectile_gap = 0;
    for (std::size_t index = 0; index < kSnapshots; ++index) {
        const network_example::WorldSnapshot send =
            engine.build_snapshot_send_set(session, relevant, budget);
        require(contains_entity(send, own_player.net_id));
        for (const network_example::EntitySnapshot& entity : send.entities) {
            if (entity.type == network_example::EntityType::kProjectile) {
                ++projectile_sends;
            } else if (entity.type == network_example::EntityType::kProp) {
                ++prop_sends;
            } else if (entity.actor_type ==
                       network_example::ActorType::kAgent) {
                ++agent_sends;
            }
        }
        if (contains_entity(send, 200u)) {
            first_projectile_gap = 0;
        } else {
            ++first_projectile_gap;
            worst_first_projectile_gap =
                std::max(worst_first_projectile_gap, first_projectile_gap);
        }
    }
    require(agent_sends > 0);
    // The bug. Both of these were zero for as long as the sections were served
    // in a fixed order.
    require(projectile_sends > 0);
    require(prop_sends > 0);
    // And the floor reaches across the whole queue now, rather than being
    // promised separately inside a section that never got any budget.
    require(worst_first_projectile_gap <= 16);
}

// Relevance and the priority bands measure to an entity's near edge, not to its
// origin.
//
// Every actor used to be a point. That is fine for a 0.8 m grunt and wrong for
// the legged rigs the catalog ships: a quadruped's hitbox is 24 m across and
// 28 m tall, and a biped is 42 m tall -- taller than the 40 m radius that used to
// cull it. Both were disappearing while still filling a third of the screen.
void a_large_rig_is_measured_by_its_edge_not_its_origin() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);

    auto transport = std::make_unique<network_example::LoopbackTransport>();
    network_example::LoopbackTransport* loopback = transport.get();
    engine.transport_ = std::move(transport);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    require(loopback->StartServer(7795));

    const network_example::NetId player =
        engine.world_.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId grunt =
        engine.world_.spawn_enemy(glm::vec3{50.0f, 0.0f, 0.0f});
    const network_example::NetId rig =
        engine.world_.spawn_enemy(glm::vec3{50.0f, 0.0f, 0.0f});
    // The quadruped's authored hitbox: 12 m in x and z, so a horizontal radius
    // of about 17 m.
    const std::optional<entt::entity> rig_entity = engine.world_.find_entity(rig);
    require(rig_entity.has_value());
    engine.world_.registry().get<network_example::Hitbox>(*rig_entity)
        .half_extents = glm::vec3{12.0f, 14.0f, 12.0f};

    network_example::KernelEngine::PeerSession session{1, player, 0, true, {}};
    engine.peer_sessions_.push_back(std::move(session));
    network_example::KernelEngine::PeerSession& live = engine.peer_sessions_[0];

    require(std::fabs(engine.entity_bounding_radius(rig) - 16.97f) < 0.05f);
    // A grunt keeps the default half-metre box spawn_enemy leaves it, so its
    // radius is 0.71 m -- nothing about this moves the small units, which is
    // the property that makes measuring to an edge safe to apply to everything.
    require(engine.entity_bounding_radius(grunt) < 1.0f);

    const auto relevant_at = [&](network_example::NetId net_id, float x) {
        set_position(engine.world_, net_id, glm::vec3{x, 0.0f, 0.0f});
        const network_example::WorldSnapshot snapshot =
            engine.build_relevant_snapshot(live, 0);
        const bool contained = contains_entity(snapshot, net_id);
        engine.sync_session_relevance(&live, snapshot);
        return contained;
    };

    // A grunt at 50 m is gone, as it always was.
    require(!relevant_at(grunt, 50.0f));
    // 50 - 17 = 33 m of clear air between the player and the rig's near face,
    // which is well inside the 40 m entry radius. On the old point test this
    // was culled.
    require(relevant_at(rig, 50.0f));
    // And it still ends somewhere: past 40 + 17 with the exit band on top.
    require(!relevant_at(rig, 62.0f));

    // The bands move with it too. The rig's origin at 26 m would have been the
    // far band on a point measurement; its near face is 9 m away, so it is the
    // near band and outranks a grunt standing at the same distance.
    set_position(engine.world_, rig, glm::vec3{26.0f, 0.0f, 0.0f});
    set_position(engine.world_, grunt, glm::vec3{26.0f, 0.0f, 0.0f});
    const network_example::WorldSnapshot both =
        engine.build_relevant_snapshot(live, 0);
    engine.sync_session_relevance(&live, both);
    const auto record_for = [&](network_example::NetId net_id) {
        for (const network_example::EntitySnapshot& entity : both.entities) {
            if (entity.net_id == net_id) {
                return entity;
            }
        }
        require(false);
        return network_example::EntitySnapshot{};
    };
    // The player, which is written unconditionally, plus room for exactly one
    // of the two agents -- so every snapshot is a choice between them.
    const std::size_t one_agent_budget =
        network_example::estimate_snapshot_base_packet_size() +
        network_example::estimate_snapshot_entity_size(record_for(player)) +
        network_example::estimate_snapshot_entity_size(record_for(rig));
    std::size_t rig_sends = 0;
    std::size_t grunt_sends = 0;
    for (std::size_t index = 0; index < 24u; ++index) {
        const network_example::WorldSnapshot send =
            engine.build_snapshot_send_set(live, both, one_agent_budget);
        if (contains_entity(send, rig)) {
            ++rig_sends;
        }
        if (contains_entity(send, grunt)) {
            ++grunt_sends;
        }
    }
    require(rig_sends > grunt_sends);
}

void dedicated_server_projectile_destruction_uses_destroyed_reason() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);

    auto transport = std::make_unique<network_example::LoopbackTransport>();
    assert(transport->StartServer(7781));
    network_example::LoopbackTransport* loopback = transport.get();
    engine.transport_ = std::move(transport);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    const network_example::NetId player =
        engine.world_.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    std::vector<network_example::NetId> projectiles;
    configure_expiring_projectiles(&engine, 1, &projectiles);
    network_example::KernelEngine::PeerSession session{
        1,
        player,
        0,
        true,
        {},
    };
    session.relevant_entities.insert(projectiles.begin(), projectiles.end());
    engine.peer_sessions_.push_back(std::move(session));

    engine.simulate_tick();
    engine.simulate_tick();

    const std::vector<network_example::EntityDespawnPacket> despawns =
        poll_client_despawns(loopback);
    for (const network_example::NetId projectile : projectiles) {
        assert(!engine.world_.find_entity(projectile).has_value());
        assert(
            engine.peer_sessions_[0].relevant_entities.find(projectile) ==
            engine.peer_sessions_[0].relevant_entities.end());
        assert(count_despawn(
                   despawns,
                   projectile,
                   KernelDespawnReason_Destroyed) == 1);
        assert(count_despawn(
                   despawns,
                   projectile,
                   KernelDespawnReason_OutOfRange) == 0);
    }
    assert(engine.lifecycle_events_.size() == projectiles.size());
    for (const KernelEntityLifecycleEvent& event : engine.lifecycle_events_) {
        assert(event.type == KernelEntityLifecycleEventType_Destroyed);
        assert(event.reason == KernelDespawnReason_Destroyed);
        assert(event.entity_type == KernelEntityType_Projectile);
    }

    engine.publish_snapshot();
    const std::vector<network_example::EntityDespawnPacket> repeated_despawns =
        poll_client_despawns(loopback);
    for (const network_example::NetId projectile : projectiles) {
        assert(count_despawn(
                   repeated_despawns,
                   projectile,
                   KernelDespawnReason_OutOfRange) == 0);
    }

    const network_example::NetId departed_projectile =
        engine.world_.spawn_projectile(
            2,
            glm::vec3{100.0f, 0.0f, 0.0f},
            glm::vec3{0.0f, 0.0f, 0.0f});
    const std::optional<entt::entity> departed_entity =
        engine.world_.find_entity(departed_projectile);
    assert(departed_entity.has_value());
    engine.world_.registry()
        .get<network_example::ProjectileState>(*departed_entity)
        .max_lifetime_ticks = 1;
    engine.peer_sessions_[0].relevant_entities.insert(departed_projectile);

    engine.publish_snapshot();
    const std::vector<network_example::EntityDespawnPacket> range_despawns =
        poll_client_despawns(loopback);
    assert(count_despawn(
               range_despawns,
               departed_projectile,
               KernelDespawnReason_OutOfRange) == 1);
    assert(
        engine.peer_sessions_[0].out_of_range_projectiles.find(
            departed_projectile) !=
        engine.peer_sessions_[0].out_of_range_projectiles.end());

    engine.simulate_tick();
    const std::vector<network_example::EntityDespawnPacket> final_despawns =
        poll_client_despawns(loopback);
    assert(count_despawn(
               final_despawns,
               departed_projectile,
               KernelDespawnReason_Destroyed) == 1);
    assert(
        engine.peer_sessions_[0].out_of_range_projectiles.find(
            departed_projectile) ==
        engine.peer_sessions_[0].out_of_range_projectiles.end());
}

void listen_server_projectile_destruction_uses_destroyed_reason() {
    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);

    auto transport = std::make_unique<network_example::ListenServerTransport>(
        std::make_unique<network_example::LoopbackTransport>());
    assert(transport->StartServer(7782));
    network_example::ListenServerTransport* listen_transport = transport.get();
    engine.listen_server_transport_ = listen_transport;
    engine.transport_ = std::move(transport);
    engine.reset_runtime_state(KernelMode_ListenServer);

    const network_example::NetId player =
        engine.world_.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    engine.local_player_net_id_ = player;
    const network_example::NetId projectile = engine.world_.spawn_projectile(
        1,
        glm::vec3{1.0f, 0.0f, 0.0f},
        glm::vec3{0.0f, 0.0f, 0.0f});
    const std::optional<entt::entity> projectile_entity =
        engine.world_.find_entity(projectile);
    assert(projectile_entity.has_value());
    engine.world_.registry()
        .get<network_example::ProjectileState>(*projectile_entity)
        .max_lifetime_ticks = 1;
    engine.local_listen_session_ =
        network_example::KernelEngine::PeerSession{1, player, 0, true, {}};
    engine.local_listen_session_.relevant_entities.insert(projectile);

    engine.simulate_tick();

    const std::vector<network_example::EntityDespawnPacket> despawns =
        poll_local_client_despawns(listen_transport);
    assert(count_despawn(
               despawns,
               projectile,
               KernelDespawnReason_Destroyed) == 1);
    assert(count_despawn(
               despawns,
               projectile,
               KernelDespawnReason_OutOfRange) == 0);
    assert(
        engine.local_listen_session_.relevant_entities.find(projectile) ==
        engine.local_listen_session_.relevant_entities.end());

    engine.publish_snapshot();
    const std::vector<network_example::EntityDespawnPacket> repeated_despawns =
        poll_local_client_despawns(listen_transport);
    assert(count_despawn(
               repeated_despawns,
               projectile,
               KernelDespawnReason_OutOfRange) == 0);

    const network_example::NetId departed_projectile =
        engine.world_.spawn_projectile(
            2,
            glm::vec3{100.0f, 0.0f, 0.0f},
            glm::vec3{0.0f, 0.0f, 0.0f});
    const std::optional<entt::entity> departed_entity =
        engine.world_.find_entity(departed_projectile);
    assert(departed_entity.has_value());
    engine.world_.registry()
        .get<network_example::ProjectileState>(*departed_entity)
        .max_lifetime_ticks = 1;
    engine.local_listen_session_.relevant_entities.insert(departed_projectile);

    engine.publish_snapshot();
    const std::vector<network_example::EntityDespawnPacket> range_despawns =
        poll_local_client_despawns(listen_transport);
    assert(count_despawn(
               range_despawns,
               departed_projectile,
               KernelDespawnReason_OutOfRange) == 1);

    engine.simulate_tick();
    const std::vector<network_example::EntityDespawnPacket> final_despawns =
        poll_local_client_despawns(listen_transport);
    assert(count_despawn(
               final_despawns,
               departed_projectile,
               KernelDespawnReason_Destroyed) == 1);
    assert(
        engine.local_listen_session_.out_of_range_projectiles.find(
            departed_projectile) ==
        engine.local_listen_session_.out_of_range_projectiles.end());
}

}  // namespace

int main() {
    // Ahead of everything else: main() carries a long-standing abort part way
    // down, and anything below it never runs in a build where assert() is live.
    a_departure_does_not_cost_the_next_entity_its_turn();
    relevance_holds_until_a_wider_radius_than_it_entered();
    a_crowd_is_introduced_over_several_snapshots();
    a_nearer_agent_is_served_more_often_than_a_distant_one();
    an_outvoted_agent_is_still_served_within_the_starvation_window();
    a_distant_teammate_competes_and_the_own_player_does_not();
    every_kind_of_entity_shares_one_budget();
    a_large_rig_is_measured_by_its_edge_not_its_origin();

    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    network_example::KernelEngine engine(config);
    assert(engine.transport_->StartServer(7777));

    const network_example::NetId player_one =
        engine.world_.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId player_two =
        engine.world_.spawn_player(2, glm::vec3{120.0f, 0.0f, 0.0f});
    const network_example::NetId near_enemy =
        engine.world_.spawn_enemy(glm::vec3{10.0f, 0.0f, 0.0f});
    const network_example::NetId far_enemy =
        engine.world_.spawn_enemy(glm::vec3{70.0f, 0.0f, 0.0f});
    const network_example::NetId dormant_prop =
        engine.world_.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    const std::optional<entt::entity> dormant_prop_entity =
        engine.world_.find_entity(dormant_prop);
    assert(dormant_prop_entity.has_value());
    engine.world_.registry().replace<network_example::EntityKind>(
        *dormant_prop_entity,
        network_example::EntityKind{
            network_example::EntityType::kProp,
            network_example::ActorType::kUnknown});
    engine.world_.registry().emplace<network_example::PropWorldMode>(
        *dormant_prop_entity,
        network_example::PropWorldMode{network_example::PropMode::kPlaced});
    engine.world_.registry().get<network_example::Velocity>(
        *dormant_prop_entity).linear = glm::vec3{0.0f};
    const network_example::NetId owned_projectile = engine.world_.spawn_projectile(
        1,
        glm::vec3{100.0f, 0.0f, 0.0f},
        glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId toward_projectile = engine.world_.spawn_projectile(
        0,
        glm::vec3{70.0f, 0.0f, 0.0f},
        glm::vec3{-10.0f, 0.0f, 0.0f});
    const network_example::NetId away_projectile = engine.world_.spawn_projectile(
        0,
        glm::vec3{70.0f, 0.0f, 0.0f},
        glm::vec3{10.0f, 0.0f, 0.0f});
    const std::optional<entt::entity> owned_projectile_entity =
        engine.world_.find_entity(owned_projectile);
    assert(owned_projectile_entity.has_value());
    engine.world_.registry()
        .get<network_example::ProjectileState>(*owned_projectile_entity)
        .projectile_template_id = 77;
    KernelProjectileTemplateDefinition predicted_template{};
    predicted_template.struct_size = sizeof(predicted_template);
    predicted_template.projectile_template_id = 77;
    predicted_template.mechanics.sync_mode =
        KernelProjectileSyncMode_LocalPredictedDeterministic;
    engine.projectile_templates_.push_back(predicted_template);

    network_example::KernelEngine::PeerSession session_one{
        1,
        player_one,
        7,
        true,
        {},
    };
    network_example::KernelEngine::PeerSession session_two{
        2,
        player_two,
        11,
        true,
        {},
    };

    const network_example::WorldSnapshot player_one_snapshot =
        engine.build_relevant_snapshot(session_one, 100);
    assert(player_one_snapshot.header.last_processed_input_seq == 7);
    assert(contains_entity(player_one_snapshot, player_one));
    assert(contains_entity(player_one_snapshot, near_enemy));
    assert(contains_entity(player_one_snapshot, dormant_prop));
    assert(contains_entity(player_one_snapshot, owned_projectile));
    assert(contains_entity(player_one_snapshot, toward_projectile));
    assert(!contains_entity(player_one_snapshot, player_two));
    assert(!contains_entity(player_one_snapshot, far_enemy));
    assert(!contains_entity(player_one_snapshot, away_projectile));
    const network_example::WorldSnapshot player_one_send_set =
        engine.build_snapshot_send_set(
            session_one,
            player_one_snapshot,
            network_example::estimate_snapshot_packet_size(player_one_snapshot));
    assert(!contains_entity(player_one_send_set, owned_projectile));
    assert(!contains_entity(player_one_send_set, dormant_prop));
    const auto dormant_snapshot = std::find_if(
        player_one_snapshot.entities.begin(),
        player_one_snapshot.entities.end(),
        [dormant_prop](const network_example::EntitySnapshot& entity) {
            return entity.net_id == dormant_prop;
        });
    assert(dormant_snapshot != player_one_snapshot.entities.end());
    assert(network_example::estimate_snapshot_entity_size(*dormant_snapshot) == 48u);
    assert(network_example::estimate_snapshot_entity_size(*dormant_snapshot) * 15u ==
           720u);

    engine.sync_session_relevance(&session_one, player_one_snapshot);
    assert(poll_prop_bootstrap(
        engine,
        dormant_prop,
        engine.world_.registry().get<network_example::Health>(
            *dormant_prop_entity).hp));

    engine.world_.registry().get<network_example::PropWorldMode>(
        *dormant_prop_entity).mode = network_example::PropMode::kInFlight;
    const network_example::WorldSnapshot in_flight_relevant =
        engine.build_relevant_snapshot(session_one, 110);
    const network_example::WorldSnapshot in_flight_send_set =
        engine.build_snapshot_send_set(session_one, in_flight_relevant, 4096);
    assert(contains_entity(in_flight_send_set, dormant_prop));

    engine.world_.registry().get<network_example::PropWorldMode>(
        *dormant_prop_entity).mode = network_example::PropMode::kPlaced;
    engine.world_.registry().get<network_example::Velocity>(
        *dormant_prop_entity).linear = glm::vec3{1.0f, 0.0f, 0.0f};
    const network_example::WorldSnapshot moving_placed_relevant =
        engine.build_relevant_snapshot(session_one, 120);
    const network_example::WorldSnapshot moving_placed_send_set =
        engine.build_snapshot_send_set(session_one, moving_placed_relevant, 4096);
    assert(contains_entity(moving_placed_send_set, dormant_prop));

    engine.world_.registry().get<network_example::Velocity>(
        *dormant_prop_entity).linear = glm::vec3{0.0f};
    set_position(engine.world_, dormant_prop, glm::vec3{40.01f, 0.0f, 0.0f});
    const network_example::WorldSnapshot dormant_out_of_range =
        engine.build_relevant_snapshot(session_one, 130);
    engine.sync_session_relevance(&session_one, dormant_out_of_range);
    assert(poll_despawn(
        engine, dormant_prop, KernelDespawnReason_OutOfRange));
    set_position(engine.world_, dormant_prop, glm::vec3{5.0f, 0.0f, 0.0f});
    const network_example::WorldSnapshot dormant_reentered =
        engine.build_relevant_snapshot(session_one, 140);
    engine.sync_session_relevance(&session_one, dormant_reentered);
    assert(poll_prop_bootstrap(
        engine,
        dormant_prop,
        engine.world_.registry().get<network_example::Health>(
            *dormant_prop_entity).hp));

    const network_example::WorldSnapshot player_two_snapshot =
        engine.build_relevant_snapshot(session_two, 100);
    assert(player_two_snapshot.header.last_processed_input_seq == 11);
    assert(contains_entity(player_two_snapshot, player_two));
    assert(contains_entity(player_two_snapshot, owned_projectile));
    assert(contains_entity(player_two_snapshot, away_projectile));
    assert(!contains_entity(player_two_snapshot, player_one));
    assert(!contains_entity(player_two_snapshot, near_enemy));
    assert(!contains_entity(player_two_snapshot, far_enemy));
    assert(!contains_entity(player_two_snapshot, toward_projectile));

    session_one.relevant_entities.insert(near_enemy);
    const network_example::WorldSnapshot capped_relevant =
        engine.build_relevant_snapshot(session_one, 150);
    const network_example::WorldSnapshot send_set =
        engine.build_snapshot_send_set(session_one, capped_relevant, 96);
    assert(!contains_entity(send_set, near_enemy));
    assert(contains_entity(capped_relevant, near_enemy));
    engine.sync_session_relevance(&session_one, capped_relevant);
    assert(!poll_despawn(
        engine,
        near_enemy,
        KernelDespawnReason_OutOfRange));
    set_position(engine.world_, near_enemy, glm::vec3{40.0f, 0.0f, 0.0f});
    const network_example::WorldSnapshot at_relevance_boundary =
        engine.build_relevant_snapshot(session_one, 175);
    assert(contains_entity(at_relevance_boundary, near_enemy));
    engine.sync_session_relevance(&session_one, at_relevance_boundary);
    assert(!poll_despawn(
        engine,
        near_enemy,
        KernelDespawnReason_OutOfRange));
    // Past the entry radius, but near_enemy is already relevant and leaving
    // costs more than staying: it holds until the exit radius.
    set_position(engine.world_, near_enemy, glm::vec3{40.01f, 0.0f, 0.0f});
    const network_example::WorldSnapshot inside_hysteresis_band =
        engine.build_relevant_snapshot(session_one, 200);
    assert(contains_entity(inside_hysteresis_band, near_enemy));
    engine.sync_session_relevance(&session_one, inside_hysteresis_band);
    assert(!poll_despawn(
        engine,
        near_enemy,
        KernelDespawnReason_OutOfRange));
    set_position(engine.world_, near_enemy, glm::vec3{44.01f, 0.0f, 0.0f});
    const network_example::WorldSnapshot after_range_change =
        engine.build_relevant_snapshot(session_one, 225);
    assert(!contains_entity(after_range_change, near_enemy));
    engine.sync_session_relevance(&session_one, after_range_change);
    assert(poll_despawn(
        engine,
        near_enemy,
        KernelDespawnReason_OutOfRange));
    // The band holds on the way back in too, or it would not be a band: inside
    // the exit radius is not yet inside the entry radius.
    set_position(engine.world_, near_enemy, glm::vec3{42.0f, 0.0f, 0.0f});
    const network_example::WorldSnapshot inside_exit_radius =
        engine.build_relevant_snapshot(session_one, 250);
    assert(!contains_entity(inside_exit_radius, near_enemy));
    engine.sync_session_relevance(&session_one, inside_exit_radius);
    set_position(engine.world_, near_enemy, glm::vec3{39.0f, 0.0f, 0.0f});
    const network_example::WorldSnapshot back_inside_entry_radius =
        engine.build_relevant_snapshot(session_one, 275);
    assert(contains_entity(back_inside_entry_radius, near_enemy));
    engine.sync_session_relevance(&session_one, back_inside_entry_radius);

    network_example::WorldSnapshot crowded;
    crowded.header.server_tick = 30;
    crowded.header.server_time_ms = 1000;
    network_example::EntitySnapshot player_entity;
    player_entity.net_id = 100;
    player_entity.type = network_example::EntityType::kActor;
    player_entity.actor_type = network_example::ActorType::kPlayer;
    crowded.entities.push_back(player_entity);
    network_example::EntitySnapshot enemy_entity;
    enemy_entity.net_id = 101;
    enemy_entity.type = network_example::EntityType::kActor;
    enemy_entity.actor_type = network_example::ActorType::kAgent;
    crowded.entities.push_back(enemy_entity);
    network_example::EntitySnapshot projectile_entity;
    projectile_entity.net_id = 102;
    projectile_entity.type = network_example::EntityType::kProjectile;
    crowded.entities.push_back(projectile_entity);
    const std::size_t player_budget =
        network_example::estimate_snapshot_base_packet_size() +
        network_example::estimate_snapshot_entity_size(player_entity);
    const network_example::WorldSnapshot byte_budgeted =
        engine.build_snapshot_send_set(session_one, crowded, player_budget);
    assert(contains_entity(byte_budgeted, 100));
    assert(!contains_entity(byte_budgeted, 101));
    assert(!contains_entity(byte_budgeted, 102));
    assert(network_example::estimate_snapshot_packet_size(byte_budgeted) <= player_budget);

    const std::size_t player_enemy_budget =
        network_example::estimate_snapshot_base_packet_size() +
        network_example::estimate_snapshot_entity_size(player_entity) +
        network_example::estimate_snapshot_entity_size(enemy_entity);
    network_example::WorldSnapshot round_robin_relevant;
    round_robin_relevant.header = crowded.header;
    round_robin_relevant.entities.push_back(player_entity);
    network_example::EntitySnapshot first_enemy = enemy_entity;
    first_enemy.net_id = 201;
    network_example::EntitySnapshot second_enemy = enemy_entity;
    second_enemy.net_id = 202;
    round_robin_relevant.entities.push_back(first_enemy);
    round_robin_relevant.entities.push_back(second_enemy);
    const network_example::WorldSnapshot first_round =
        engine.build_snapshot_send_set(
            session_one,
            round_robin_relevant,
            player_enemy_budget);
    const network_example::WorldSnapshot second_round =
        engine.build_snapshot_send_set(
            session_one,
            round_robin_relevant,
            player_enemy_budget);
    assert(contains_entity(first_round, 201));
    assert(!contains_entity(first_round, 202));
    assert(!contains_entity(second_round, 201));
    assert(contains_entity(second_round, 202));

    // The reason the send order is keyed on net id rather than on a position
    // in the relevant list. An index survives only as long as the list does:
    // serve the first of three agents, let it leave, and an index of 1 now
    // points past the second agent to the third, so the second waits out
    // another full cycle for a turn it had already earned.
    network_example::EntitySnapshot churn_first = enemy_entity;
    churn_first.net_id = 211;
    network_example::EntitySnapshot churn_second = enemy_entity;
    churn_second.net_id = 212;
    network_example::EntitySnapshot churn_third = enemy_entity;
    churn_third.net_id = 213;
    network_example::WorldSnapshot churn_relevant;
    churn_relevant.header = crowded.header;
    churn_relevant.entities.push_back(churn_first);
    churn_relevant.entities.push_back(churn_second);
    churn_relevant.entities.push_back(churn_third);
    const std::size_t one_enemy_budget =
        network_example::estimate_snapshot_base_packet_size() +
        network_example::estimate_snapshot_entity_size(churn_first);
    const network_example::WorldSnapshot churn_round_one =
        engine.build_snapshot_send_set(
            session_one,
            churn_relevant,
            one_enemy_budget);
    assert(contains_entity(churn_round_one, 211));
    network_example::WorldSnapshot churn_departed;
    churn_departed.header = crowded.header;
    churn_departed.entities.push_back(churn_second);
    churn_departed.entities.push_back(churn_third);
    const network_example::WorldSnapshot churn_round_two =
        engine.build_snapshot_send_set(
            session_one,
            churn_departed,
            one_enemy_budget);
    assert(contains_entity(churn_round_two, 212));
    assert(!contains_entity(churn_round_two, 213));
    const network_example::WorldSnapshot churn_round_three =
        engine.build_snapshot_send_set(
            session_one,
            churn_departed,
            one_enemy_budget);
    assert(contains_entity(churn_round_three, 213));
    assert(!contains_entity(churn_round_three, 212));

    network_example::EntitySnapshot compact_projectile = projectile_entity;
    compact_projectile.net_id = 301;
    network_example::EntitySnapshot hybrid_projectile = projectile_entity;
    hybrid_projectile.net_id = 302;
    hybrid_projectile.owner_peer = 1;
    hybrid_projectile.spawn_tick = 3;
    hybrid_projectile.action_instance_id = 99;
    hybrid_projectile.state_flags |=
        network_example::kSnapshotStateFlagProjectileHybridCorrection;
    network_example::WorldSnapshot projectile_budget_relevant;
    projectile_budget_relevant.header = crowded.header;
    projectile_budget_relevant.entities.push_back(compact_projectile);
    projectile_budget_relevant.entities.push_back(hybrid_projectile);
    const std::size_t compact_only_budget =
        network_example::estimate_snapshot_base_packet_size() +
        network_example::estimate_snapshot_entity_size(compact_projectile);
    const network_example::WorldSnapshot projectile_budgeted =
        engine.build_snapshot_send_set(
            session_one,
            projectile_budget_relevant,
            compact_only_budget);
    assert(contains_entity(projectile_budgeted, 301));
    assert(!contains_entity(projectile_budgeted, 302));
    assert(network_example::estimate_snapshot_packet_size(projectile_budgeted) <=
           compact_only_budget);

    dedicated_server_projectile_destruction_uses_destroyed_reason();
    listen_server_projectile_destruction_uses_destroyed_reason();
    return 0;
}
