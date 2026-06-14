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

}  // namespace

int main() {
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
    assert(contains_entity(player_one_snapshot, owned_projectile));
    assert(contains_entity(player_one_snapshot, toward_projectile));
    assert(!contains_entity(player_one_snapshot, player_two));
    assert(!contains_entity(player_one_snapshot, far_enemy));
    assert(!contains_entity(player_one_snapshot, away_projectile));

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
    set_position(engine.world_, near_enemy, glm::vec3{90.0f, 0.0f, 0.0f});
    const network_example::WorldSnapshot after_range_change =
        engine.build_relevant_snapshot(session_one, 200);
    assert(!contains_entity(after_range_change, near_enemy));
    engine.sync_session_relevance(&session_one, after_range_change);
    assert(poll_despawn(
        engine,
        near_enemy,
        KernelDespawnReason_OutOfRange));

    network_example::WorldSnapshot crowded;
    crowded.header.server_tick = 30;
    crowded.header.server_time_ms = 1000;
    network_example::EntitySnapshot player_entity;
    player_entity.net_id = 100;
    player_entity.type = network_example::EntityType::kPlayer;
    crowded.entities.push_back(player_entity);
    network_example::EntitySnapshot enemy_entity;
    enemy_entity.net_id = 101;
    enemy_entity.type = network_example::EntityType::kEnemy;
    crowded.entities.push_back(enemy_entity);
    network_example::EntitySnapshot projectile_entity;
    projectile_entity.net_id = 102;
    projectile_entity.type = network_example::EntityType::kProjectile;
    crowded.entities.push_back(projectile_entity);
    const std::size_t player_budget =
        network_example::estimate_snapshot_base_packet_size() +
        network_example::estimate_snapshot_entity_size(network_example::EntityType::kPlayer);
    const network_example::WorldSnapshot byte_budgeted =
        engine.build_snapshot_send_set(session_one, crowded, player_budget);
    assert(contains_entity(byte_budgeted, 100));
    assert(!contains_entity(byte_budgeted, 101));
    assert(!contains_entity(byte_budgeted, 102));
    assert(network_example::estimate_snapshot_packet_size(byte_budgeted) <= player_budget);

    const std::size_t player_enemy_budget =
        network_example::estimate_snapshot_base_packet_size() +
        network_example::estimate_snapshot_entity_size(network_example::EntityType::kPlayer) +
        network_example::estimate_snapshot_entity_size(network_example::EntityType::kEnemy);
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

    return 0;
}
