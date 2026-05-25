#include "simulation/public/collision_query.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace network_example {
namespace {

std::uint32_t entity_collision_layer_for_entity(
    const World& world,
    entt::entity entity) {
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
    return entity_layer;
}

EntityType entity_type_for_entity(const World& world, entt::entity entity) {
    if (!world.registry().all_of<EntityKind>(entity)) {
        return EntityType::kUnknown;
    }
    return world.registry().get<EntityKind>(entity).type;
}

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
    if (world.registry().all_of<BeamTag>(entity)) {
        return false;
    }
    if (world.registry().all_of<Health>(entity) &&
        world.registry().get<Health>(entity).hp == 0) {
        return false;
    }
    const std::uint32_t entity_layer =
        entity_collision_layer_for_entity(world, entity);
    if (entity_layer != 0 && (filter.collision_mask & entity_layer) == 0) {
        return false;
    }
    return true;
}

glm::vec3 normalized_or_zero(const glm::vec3& value) {
    if (glm::length(value) <= 0.0001f) {
        return glm::vec3{0.0f, 1.0f, 0.0f};
    }
    return glm::normalize(value);
}

glm::vec3 normal_from_aabb_surface(
    const glm::vec3& point,
    const glm::vec3& box_center,
    const glm::vec3& box_half_extents,
    const glm::vec3& fallback_direction) {
    const glm::vec3 local = point - box_center;
    const glm::vec3 safe_extents = glm::max(box_half_extents, glm::vec3{0.0001f});
    const glm::vec3 face_delta = glm::abs(glm::abs(local) - safe_extents);
    int axis = 0;
    if (face_delta.y < face_delta.x && face_delta.y <= face_delta.z) {
        axis = 1;
    } else if (face_delta.z < face_delta.x && face_delta.z < face_delta.y) {
        axis = 2;
    }

    glm::vec3 normal{0.0f, 0.0f, 0.0f};
    normal[axis] = local[axis] >= 0.0f ? 1.0f : -1.0f;
    if (glm::length(normal) <= 0.0001f) {
        return normalized_or_zero(-fallback_direction);
    }
    return normal;
}

glm::vec3 normal_from_overlap_center(
    const glm::vec3& query_center,
    const glm::vec3& hitbox_center) {
    return normalized_or_zero(query_center - hitbox_center);
}

QueryHit make_query_hit(
    const World& world,
    entt::entity entity,
    NetId net_id,
    float distance,
    const glm::vec3& position,
    const glm::vec3& normal) {
    return QueryHit{
        net_id,
        distance,
        position,
        normalized_or_zero(normal),
        entity_type_for_entity(world, entity),
        entity_collision_layer_for_entity(world, entity),
    };
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

bool aabb_overlaps_aabb(
    const glm::vec3& lhs_center,
    const glm::vec3& lhs_half_extents,
    const glm::vec3& rhs_center,
    const glm::vec3& rhs_half_extents) {
    const glm::vec3 delta = glm::abs(lhs_center - rhs_center);
    return delta.x <= lhs_half_extents.x + rhs_half_extents.x &&
           delta.y <= lhs_half_extents.y + rhs_half_extents.y &&
           delta.z <= lhs_half_extents.z + rhs_half_extents.z;
}

}  // namespace

std::uint32_t entity_collision_layer(const World& world, NetId net_id) {
    const std::optional<entt::entity> entity = world.find_entity(net_id);
    if (!entity.has_value()) {
        return 0;
    }
    return entity_collision_layer_for_entity(world, *entity);
}

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

std::vector<QueryHit> collect_swept_sphere_hits(
    World& world,
    const glm::vec3& segment_start,
    const glm::vec3& segment_end,
    float radius,
    const QueryFilter& filter) {
    if (radius < 0.0f) {
        return {};
    }
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
                hitbox.half_extents + glm::vec3{radius},
                &distance) &&
            distance <= length) {
            const glm::vec3 position = segment_start + direction * distance;
            hits.push_back(make_query_hit(
                world,
                entity,
                identity.net_id,
                distance,
                position,
                normal_from_aabb_surface(
                    position,
                    transform.position + hitbox.center,
                    hitbox.half_extents + glm::vec3{radius},
                    direction)));
        }
    }
    sort_hits(&hits);
    return hits;
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
            const glm::vec3 position = segment_start + direction * distance;
            hits.push_back(make_query_hit(
                world,
                entity,
                identity.net_id,
                distance,
                position,
                normal_from_aabb_surface(
                    position,
                    transform.position + hitbox.center,
                    hitbox.half_extents,
                    direction)));
        }
    }
    sort_hits(&hits);
    return hits;
}

std::vector<QueryHit> collect_box_overlaps(
    World& world,
    const glm::vec3& center,
    const glm::vec3& half_extents,
    const QueryFilter& filter) {
    if (half_extents.x < 0.0f || half_extents.y < 0.0f || half_extents.z < 0.0f) {
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
        const glm::vec3 hitbox_center = transform.position + hitbox.center;
        if (aabb_overlaps_aabb(
                center,
                half_extents,
                hitbox_center,
                hitbox.half_extents)) {
            hits.push_back(make_query_hit(
                world,
                entity,
                identity.net_id,
                distance_to_aabb(center, hitbox_center, hitbox.half_extents),
                center,
                normal_from_overlap_center(center, hitbox_center)));
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
            const glm::vec3 hitbox_center = transform.position + hitbox.center;
            hits.push_back(make_query_hit(
                world,
                entity,
                identity.net_id,
                distance,
                center,
                normal_from_overlap_center(center, hitbox_center)));
        }
    }
    sort_hits(&hits);
    return hits;
}

}  // namespace network_example
