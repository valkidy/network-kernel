#include "sync/public/snapshot.h"

namespace network_example {

WorldSnapshot build_world_snapshot(
    const World& world,
    std::uint32_t server_tick,
    std::uint32_t server_time_ms,
    std::uint32_t last_processed_input_seq) {
    WorldSnapshot snapshot;
    snapshot.header = SnapshotHeader{
        server_tick,
        server_time_ms,
        last_processed_input_seq,
    };

    auto view = world.registry().view<
        const NetworkIdentity,
        const EntityKind,
        const Transform>();
    for (const entt::entity entity : view) {
        EntitySnapshot entity_snapshot;
        entity_snapshot.net_id = view.get<const NetworkIdentity>(entity).net_id;
        entity_snapshot.type = view.get<const EntityKind>(entity).type;
        entity_snapshot.position = view.get<const Transform>(entity).position;
        entity_snapshot.rotation = view.get<const Transform>(entity).rotation;
        if (world.registry().all_of<Velocity>(entity)) {
            entity_snapshot.velocity = world.registry().get<Velocity>(entity).linear;
        }
        if (world.registry().all_of<Health>(entity)) {
            entity_snapshot.hp = world.registry().get<Health>(entity).hp;
        }
        snapshot.entities.push_back(entity_snapshot);
    }

    return snapshot;
}

}  // namespace network_example
