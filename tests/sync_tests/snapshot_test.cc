#include <cassert>

#include <glm/glm.hpp>

#include "sync/public/history_buffer.h"
#include "sync/public/snapshot.h"
#include "world/public/world.h"

int main() {
    network_example::World world;
    world.spawn_player(1, glm::vec3{1.0f, 2.0f, 3.0f});
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
    return 0;
}
