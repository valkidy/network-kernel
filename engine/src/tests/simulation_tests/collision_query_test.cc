#include <cassert>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

#include "simulation/public/collision_query.h"
#include "world/public/world.h"

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

network_example::NetId spawn_target(
    network_example::World& world,
    const glm::vec3& position) {
    const network_example::NetId target = world.spawn_enemy(position);
    const auto entity = world.find_entity(target);
    assert(entity.has_value());
    world.registry().get<network_example::Health>(*entity) =
        network_example::Health{50, 50};
    world.registry().get<network_example::Hitbox>(*entity) =
        network_example::Hitbox{{0.0f, 0.5f, 0.0f}, {0.25f, 0.5f, 0.25f}, 0};
    return target;
}

void ray_aabb_hit_and_miss() {
    float distance = 0.0f;
    require(network_example::ray_intersects_aabb(
        glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        glm::vec3{3.0f, 0.5f, 0.0f},
        glm::vec3{0.5f, 0.5f, 0.5f},
        &distance));
    require(distance > 2.49f && distance < 2.51f);
    require(!network_example::ray_intersects_aabb(
        glm::vec3{0.0f, 2.0f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        glm::vec3{3.0f, 0.5f, 0.0f},
        glm::vec3{0.5f, 0.5f, 0.5f},
        nullptr));
}

void segment_hits_are_sorted_by_distance_then_net_id() {
    network_example::World world;
    const network_example::NetId far = spawn_target(world, glm::vec3{5.0f, 0.0f, 0.0f});
    const network_example::NetId near = spawn_target(world, glm::vec3{2.0f, 0.0f, 0.0f});
    (void)far;
    (void)near;

    const std::vector<network_example::QueryHit> hits =
        network_example::collect_segment_hits(
            world,
            glm::vec3{0.0f, 0.5f, 0.0f},
            glm::vec3{6.0f, 0.5f, 0.0f},
            network_example::QueryFilter{});

    require(hits.size() == 2);
    require(hits[0].net_id == near);
    require(hits[1].net_id == far);
    require(hits[0].distance < hits[1].distance);
}

void sphere_overlap_respects_default_exclusions() {
    network_example::World world;
    const network_example::NetId source =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        spawn_target(world, glm::vec3{1.0f, 0.0f, 0.0f});
    const network_example::NetId friendly =
        world.spawn_player(1, glm::vec3{1.2f, 0.0f, 0.0f});
    const network_example::NetId projectile =
        world.spawn_projectile(0, glm::vec3{1.4f, 0.5f, 0.0f}, glm::vec3{0.0f});
    const network_example::NetId area =
        world.spawn_area_effect(0, glm::vec3{1.6f, 0.0f, 0.0f}, 2.0f, 1, 1, 10, 3);
    (void)friendly;
    (void)projectile;
    (void)area;

    network_example::QueryFilter filter;
    filter.ignored_net_id = source;
    filter.ignored_owner_peer = 1;
    const std::vector<network_example::QueryHit> hits =
        network_example::collect_sphere_overlaps(
            world,
            glm::vec3{0.0f, 0.5f, 0.0f},
            3.0f,
            filter);

    require(hits.size() == 1);
    require(hits[0].net_id == enemy);
}

}  // namespace

int main() {
    ray_aabb_hit_and_miss();
    segment_hits_are_sorted_by_distance_then_net_id();
    sphere_overlap_respects_default_exclusions();
    return 0;
}
