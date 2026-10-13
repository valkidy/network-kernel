// A projectile's weapon is the event's, not the template's (ABI 106).
//
// The catalog used to stamp weapon_id onto every projectile template a weapon
// named, and spawning preferred that stamp over the weapon the shot actually
// came from. So two weapons could not share a template -- the second stamp won
// -- and anything else that spawned a stamped template took the stamp: a
// thrown grenade item's shell was reported as fired by the grenade launcher.

#include <cstdlib>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "simulation/public/simulation.h"

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

network_example::RuntimeProjectileTemplate shell_template() {
    network_example::RuntimeProjectileTemplate shell{};
    shell.projectile_template_id = 3;
    shell.projectile_type = network_example::ProjectileType::kStandard;
    shell.motion_model = network_example::ProjectileMotionModel::kLinear;
    shell.speed = 10.0f;
    shell.damage = 20;
    shell.lifetime_ticks = 30;
    return shell;
}

std::uint8_t spawned_weapon_id(
    network_example::World& world,
    const std::vector<KernelEvent>& events) {
    for (const KernelEvent& event : events) {
        if (event.type != KernelEventType_EntitySpawned) {
            continue;
        }
        const std::optional<entt::entity> entity = world.find_entity(event.net_id);
        require(entity.has_value());
        return world.registry()
            .get<network_example::ProjectileState>(*entity)
            .weapon_id;
    }
    require(false);
    return 0;
}

// The same template, spawned for two weapons, carries each its own.
void one_template_serves_two_weapons() {
    network_example::World world;
    const network_example::RuntimeProjectileTemplate shell = shell_template();
    world.set_projectile_templates({shell});
    for (const std::uint8_t weapon_id : {std::uint8_t{3}, std::uint8_t{4}}) {
        std::vector<KernelEvent> events;
        require(network_example::spawn_projectile_at(
            world,
            shell,
            1,
            0,
            weapon_id,
            1,
            glm::vec3{0.0f, 1.0f, 0.0f},
            glm::vec3{1.0f, 0.0f, 0.0f},
            0,
            1.0f / 30.0f,
            &events));
        require(spawned_weapon_id(world, events) == weapon_id);
    }
}

// Spawned with no weapon behind it -- an item's graph, say -- it carries
// none, however many weapons fire the same template elsewhere.
void a_template_spawned_by_no_weapon_carries_none() {
    network_example::World world;
    const network_example::RuntimeProjectileTemplate shell = shell_template();
    world.set_projectile_templates({shell});
    std::vector<KernelEvent> events;
    require(network_example::spawn_projectile_at(
        world,
        shell,
        1,
        0,
        0,
        1,
        glm::vec3{0.0f, 1.0f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        0,
        1.0f / 30.0f,
        &events));
    require(spawned_weapon_id(world, events) == 0u);
}

}  // namespace

int main() {
    one_template_serves_two_weapons();
    a_template_spawned_by_no_weapon_carries_none();
    return 0;
}
