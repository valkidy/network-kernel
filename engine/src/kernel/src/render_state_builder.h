#ifndef KERNEL_SRC_RENDER_STATE_BUILDER_H_
#define KERNEL_SRC_RENDER_STATE_BUILDER_H_

#include <cstdint>

#include <entt/entt.hpp>

#include "kernel/public/kernel_types.h"
#include "sync/public/snapshot.h"
#include "world/public/world.h"

namespace network_example {

RenderEntityState render_state_from_world_entity(
    const World& world,
    entt::entity entity,
    std::uint64_t entity_id);

RenderEntityState render_state_from_snapshot_entity(
    const EntitySnapshot& entity,
    std::uint64_t entity_id);

EntitySnapshot interpolate_snapshot_entity(
    const EntitySnapshot& from,
    const EntitySnapshot& to,
    float alpha);

}  // namespace network_example

#endif  // KERNEL_SRC_RENDER_STATE_BUILDER_H_
