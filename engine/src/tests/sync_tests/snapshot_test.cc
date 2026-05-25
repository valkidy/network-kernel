#include <cassert>

#include <glm/glm.hpp>

#include "sync/public/history_buffer.h"
#include "sync/public/snapshot.h"
#include "world/public/world.h"

int main() {
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{1.0f, 2.0f, 3.0f});
    const network_example::NetId enemy =
        world.spawn_enemy(glm::vec3{4.0f, 0.0f, 0.0f});
    const network_example::NetId projectile =
        world.spawn_projectile(
            1,
            glm::vec3{2.0f, 1.0f, 0.0f},
            glm::vec3{10.0f, 0.0f, 0.0f});
    const network_example::NetId area =
        world.spawn_area_effect(0, glm::vec3{6.0f, 0.0f, 0.0f}, 3.0f, 3, 30, 10, 2);
    const auto player_entity = world.find_entity(player);
    assert(player_entity.has_value());
    world.registry().get<network_example::Velocity>(*player_entity).linear =
        glm::vec3{1.0f, 0.0f, 0.0f};
    world.registry().get<network_example::WeaponState>(*player_entity).is_reloading =
        true;
    world.registry().get<network_example::Health>(*player_entity).hp = 75;
    world.registry().get<network_example::Health>(*player_entity).max_hp = 100;
    world.registry().get<network_example::Hitbox>(*player_entity) =
        network_example::Hitbox{{0.0f, 0.9f, 0.0f}, {0.35f, 0.9f, 0.35f}, 0};
    const auto enemy_entity = world.find_entity(enemy);
    assert(enemy_entity.has_value());
    world.registry().get<network_example::Health>(*enemy_entity).hp = 25;
    world.registry().get<network_example::Health>(*enemy_entity).max_hp = 50;
    world.registry().get<network_example::Hitbox>(*enemy_entity) =
        network_example::Hitbox{{0.0f, 0.8f, 0.0f}, {0.4f, 0.8f, 0.4f}, 0};
    world.registry().emplace<network_example::ReplicationState>(
        *enemy_entity,
        network_example::ReplicationState{9, 0x01020300u});
    const auto projectile_entity = world.find_entity(projectile);
    assert(projectile_entity.has_value());
    network_example::ProjectileState& projectile_state =
        world.registry().get<network_example::ProjectileState>(*projectile_entity);
    projectile_state.spawn_tick = 7;
    projectile_state.client_action_id = 3456;

    const network_example::WorldSnapshot snapshot =
        network_example::build_world_snapshot(world, 7, 233, 3);
    assert(snapshot.header.server_tick == 7);
    assert(snapshot.header.server_time_ms == 233);
    assert(snapshot.header.last_processed_input_seq == 3);
    assert(snapshot.entities.size() == 4);
    bool saw_player_flags = false;
    bool saw_enemy_state = false;
    bool saw_projectile_metadata = false;
    bool saw_area_effect = false;
    for (const network_example::EntitySnapshot& entity : snapshot.entities) {
        if (entity.net_id == player) {
            saw_player_flags =
                entity.owner_peer == 1 &&
                entity.hp == 75 &&
                entity.max_hp == 100 &&
                (entity.flags & network_example::kVisualFlagMoving) != 0 &&
                (entity.flags & network_example::kVisualFlagReloading) != 0;
        }
        if (entity.net_id == enemy) {
            saw_enemy_state =
                entity.owner_peer == 0 && entity.state == 9 &&
                entity.hp == 25 &&
                entity.max_hp == 50 &&
                entity.flags == 0x01020300u;
        }
        if (entity.net_id == projectile) {
            saw_projectile_metadata =
                entity.owner_peer == 1 &&
                entity.spawn_tick == 7 &&
                entity.client_action_id == 3456 &&
                entity.velocity.x == 10.0f;
        }
        if (entity.net_id == area) {
            saw_area_effect =
                entity.owner_peer == 0 &&
                entity.type == network_example::EntityType::kAreaEffect &&
                entity.position.x == 6.0f;
        }
    }
    assert(saw_player_flags);
    assert(saw_enemy_state);
    assert(saw_projectile_metadata);
    assert(saw_area_effect);

    network_example::HistoryBuffer history(2);
    history.write_frame(world, 7);
    assert(history.find_frame(7) != nullptr);
    assert(history.find_frame(7)->volumes.size() == 2);
    assert(!history.empty());
    history.write_frame(world, 8);
    history.write_frame(world, 9);
    assert(history.find_frame(7) == nullptr);
    assert(history.find_frame(8) != nullptr);
    assert(history.find_frame(9) != nullptr);
    assert(history.oldest_tick() == 8);
    assert(history.newest_tick() == 9);
    assert(history.find_frame_clamped(0)->server_tick == 8);
    assert(history.find_frame_clamped(99)->server_tick == 9);

    network_example::HistoryBuffer raycast_history(4);
    raycast_history.write_frame(world, 10);
    world.registry().get<network_example::Transform>(*enemy_entity).position =
        glm::vec3{20.0f, 0.0f, 0.0f};
    raycast_history.write_frame(world, 11);

    network_example::HistoricalHitResult hit;
    assert(network_example::raycast_history_frame(
        *raycast_history.find_frame(10),
        glm::vec3{0.0f, 0.0f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        10.0f,
        player,
        &hit));
    assert(hit.net_id == enemy);

    assert(!network_example::raycast_history_frame(
        *raycast_history.find_frame(10),
        glm::vec3{0.0f, 0.0f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        10.0f,
        enemy,
        &hit));

    network_example::World dead_world;
    const network_example::NetId dead_player =
        dead_world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId dead_enemy =
        dead_world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    const auto dead_enemy_entity = dead_world.find_entity(dead_enemy);
    assert(dead_enemy_entity.has_value());
    dead_world.registry().get<network_example::Health>(*dead_enemy_entity) =
        network_example::Health{50, 50};
    dead_world.registry().get<network_example::Hitbox>(*dead_enemy_entity) =
        network_example::Hitbox{{0.0f, 0.8f, 0.0f}, {0.4f, 0.8f, 0.4f}, 0};
    assert(dead_world.apply_damage(dead_enemy, 50));
    const network_example::WorldSnapshot dead_snapshot =
        network_example::build_world_snapshot(dead_world, 1, 33, 0);
    bool saw_dead_flag = false;
    for (const network_example::EntitySnapshot& entity : dead_snapshot.entities) {
        if (entity.net_id == dead_enemy) {
            saw_dead_flag = (entity.flags & network_example::kVisualFlagDead) != 0;
        }
    }
    assert(saw_dead_flag);
    network_example::HistoryBuffer dead_history(1);
    dead_history.write_frame(dead_world, 1);
    assert(!network_example::raycast_history_frame(
        *dead_history.find_frame(1),
        glm::vec3{0.0f, 1.0f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        10.0f,
        dead_player,
        &hit));
    return 0;
}
