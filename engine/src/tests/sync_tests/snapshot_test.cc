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

    const network_example::WorldSnapshot snapshot =
        network_example::build_world_snapshot(world, 7, 233, 3);
    assert(snapshot.header.server_tick == 7);
    assert(snapshot.header.server_time_ms == 233);
    assert(snapshot.header.last_processed_input_seq == 3);
    assert(snapshot.entities.size() == 2);

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
    const auto enemy_entity = world.find_entity(enemy);
    assert(enemy_entity.has_value());
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
    dead_world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId dead_enemy =
        dead_world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    assert(dead_world.apply_damage(dead_enemy, 50));
    network_example::HistoryBuffer dead_history(1);
    dead_history.write_frame(dead_world, 1);
    assert(!network_example::raycast_history_frame(
        *dead_history.find_frame(1),
        glm::vec3{0.0f, 1.0f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        10.0f,
        0,
        &hit));
    return 0;
}
