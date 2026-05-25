#ifndef SIMULATION_PUBLIC_COLLISION_QUERY_H_
#define SIMULATION_PUBLIC_COLLISION_QUERY_H_

#include <vector>

#include <glm/glm.hpp>

#include "world/public/world.h"

namespace network_example {

struct QueryFilter {
    NetId ignored_net_id = 0;
    PeerId ignored_owner_peer = 0;
    bool include_projectiles = false;
    bool include_area_effects = false;
};

struct QueryHit {
    NetId net_id = 0;
    float distance = 0.0f;
    glm::vec3 position{0.0f, 0.0f, 0.0f};
};

bool ray_intersects_aabb(
    const glm::vec3& ray_origin,
    const glm::vec3& ray_direction,
    const glm::vec3& box_center,
    const glm::vec3& box_half_extents,
    float* out_distance);

std::vector<QueryHit> collect_segment_hits(
    World& world,
    const glm::vec3& segment_start,
    const glm::vec3& segment_end,
    const QueryFilter& filter);

std::vector<QueryHit> collect_sphere_overlaps(
    World& world,
    const glm::vec3& center,
    float radius,
    const QueryFilter& filter);

}  // namespace network_example

#endif  // SIMULATION_PUBLIC_COLLISION_QUERY_H_
