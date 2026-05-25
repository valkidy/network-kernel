#include "simulation/public/collision_query.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace network_example {
namespace {

bool query_includes_entity(
    const World& world,
    entt::entity entity,
    const NetworkIdentity& identity,
    const QueryFilter& filter) {
    if (identity.net_id == filter.ignored_net_id) {
        return false;
    }
    if (filter.ignored_owner_peer != 0 &&
        identity.owner_peer == filter.ignored_owner_peer) {
        return false;
    }
    if (!filter.include_projectiles && world.registry().all_of<ProjectileTag>(entity)) {
        return false;
    }
    if (!filter.include_area_effects && world.registry().all_of<AreaEffectTag>(entity)) {
        return false;
    }
    if (world.registry().all_of<Health>(entity) &&
        world.registry().get<Health>(entity).hp == 0) {
        return false;
    }
    std::uint32_t entity_layer = 0;
    if (world.registry().all_of<PlayerTag>(entity)) {
        entity_layer |= kCollisionLayerPlayer;
    }
    if (world.registry().all_of<EnemyTag>(entity)) {
        entity_layer |= kCollisionLayerEnemy;
    }
    if (world.registry().all_of<ProjectileTag>(entity)) {
        entity_layer |= kCollisionLayerProjectile;
    }
    if (world.registry().all_of<AreaEffectTag>(entity)) {
        entity_layer |= kCollisionLayerAreaEffect;
    }
    if (entity_layer != 0 && (filter.collision_mask & entity_layer) == 0) {
        return false;
    }
    return true;
}

float distance_to_aabb(
    const glm::vec3& point,
    const glm::vec3& box_center,
    const glm::vec3& box_half_extents) {
    const glm::vec3 min_corner = box_center - box_half_extents;
    const glm::vec3 max_corner = box_center + box_half_extents;
    const glm::vec3 closest = glm::clamp(point, min_corner, max_corner);
    return glm::length(point - closest);
}

void sort_hits(std::vector<QueryHit>* hits) {
    std::sort(
        hits->begin(),
        hits->end(),
        [](const QueryHit& lhs, const QueryHit& rhs) {
            if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
            }
            return lhs.net_id < rhs.net_id;
        });
}

}  // namespace

bool ray_intersects_aabb(
    const glm::vec3& ray_origin,
    const glm::vec3& ray_direction,
    const glm::vec3& box_center,
    const glm::vec3& box_half_extents,
    float* out_distance) {
    float t_min = 0.0f;
    float t_max = std::numeric_limits<float>::max();
    const glm::vec3 min_corner = box_center - box_half_extents;
    const glm::vec3 max_corner = box_center + box_half_extents;

    for (int axis = 0; axis < 3; ++axis) {
        const float origin = ray_origin[axis];
        const float direction = ray_direction[axis];
        if (std::abs(direction) < 0.000001f) {
            if (origin < min_corner[axis] || origin > max_corner[axis]) {
                return false;
            }
            continue;
        }

        float t1 = (min_corner[axis] - origin) / direction;
        float t2 = (max_corner[axis] - origin) / direction;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        t_min = std::max(t_min, t1);
        t_max = std::min(t_max, t2);
        if (t_min > t_max) {
            return false;
        }
    }

    if (out_distance != nullptr) {
        *out_distance = t_min;
    }
    return true;
}

std::vector<QueryHit> collect_segment_hits(
    World& world,
    const glm::vec3& segment_start,
    const glm::vec3& segment_end,
    const QueryFilter& filter) {
    const glm::vec3 displacement = segment_end - segment_start;
    const float length = glm::length(displacement);
    if (length <= 0.0001f) {
        return {};
    }
    const glm::vec3 direction = displacement / length;

    std::vector<QueryHit> hits;
    auto view = world.registry().view<NetworkIdentity, Transform, Hitbox>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        if (!query_includes_entity(world, entity, identity, filter)) {
            continue;
        }

        const Transform& transform = view.get<Transform>(entity);
        const Hitbox& hitbox = view.get<Hitbox>(entity);
        float distance = 0.0f;
        if (ray_intersects_aabb(
                segment_start,
                direction,
                transform.position + hitbox.center,
                hitbox.half_extents,
                &distance) &&
            distance <= length) {
            hits.push_back(QueryHit{
                identity.net_id,
                distance,
                segment_start + direction * distance,
            });
        }
    }
    sort_hits(&hits);
    return hits;
}

std::vector<QueryHit> collect_sphere_overlaps(
    World& world,
    const glm::vec3& center,
    float radius,
    const QueryFilter& filter) {
    if (radius <= 0.0f) {
        return {};
    }

    std::vector<QueryHit> hits;
    auto view = world.registry().view<NetworkIdentity, Transform, Hitbox>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        if (!query_includes_entity(world, entity, identity, filter)) {
            continue;
        }

        const Transform& transform = view.get<Transform>(entity);
        const Hitbox& hitbox = view.get<Hitbox>(entity);
        const float distance = distance_to_aabb(
            center,
            transform.position + hitbox.center,
            hitbox.half_extents);
        if (distance <= radius) {
            hits.push_back(QueryHit{
                identity.net_id,
                distance,
                center,
            });
        }
    }
    sort_hits(&hits);
    return hits;
}

}  // namespace network_example
