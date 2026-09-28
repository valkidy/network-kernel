// A repeated spawn_projectile: one event spawns a counted, scattered and
// staggered set of projectiles, the same set every time for the same event.
// End to end here as the meteor storm uses it: a storm marker expires into
// fuses, and each fuse expires into a descending meteor.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <source_location>
#include <vector>

#include <glm/glm.hpp>

#include "simulation/public/action_graph.h"
#include "simulation/public/simulation.h"
#include "world/public/world.h"

namespace {

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr std::uint32_t kStormTemplate = 40;
constexpr std::uint32_t kMeteorTemplate = 41;
constexpr std::uint32_t kFuseTemplate = 42;
constexpr std::uint32_t kFuseLifetime = 10;
constexpr std::uint32_t kStaggerWindow = 60;
constexpr float kScatterRadius = 6.0f;
const glm::vec3 kCenter{5.0f, 0.0f, 5.0f};

void require(
    bool condition,
    const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(
            stderr,
            "require failed at %s:%u\n",
            location.file_name(),
            location.line());
        std::abort();
    }
}

network_example::RuntimeProjectileTemplate marker_template(
    std::uint32_t template_id,
    std::uint32_t lifetime_ticks) {
    network_example::RuntimeProjectileTemplate marker;
    marker.projectile_template_id = template_id;
    marker.projectile_type = network_example::ProjectileType::kStandard;
    marker.sync_mode = network_example::ProjectileSyncMode::kServerSnapshotOnly;
    marker.damage_shape = network_example::ProjectileDamageShape::kNone;
    marker.speed = 0.0f;
    marker.lifetime_ticks = lifetime_ticks;
    marker.collision_mask = KERNEL_COLLISION_MASK_NONE;
    return marker;
}

// on_expired -> spawn_projectile(fuse) repeated [10, 15] times.
network_example::CompiledActionGraphBinding storm_binding() {
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(KernelActionTriggerDefinition);
    trigger.action_count = 1;
    KernelActionDefinition& action = trigger.actions[0];
    action.action_type = KernelEntityTriggerActionType_SpawnProjectile;
    action.spawn_projectile_template_id = kFuseTemplate;
    action.position_source = KernelEventVec3Source_Position;
    action.direction_source = KernelEventVec3Source_Direction;
    action.spawn_lifetime_ticks = kFuseLifetime;
    action.repeat_count_min = 10;
    action.repeat_count_max = 15;
    action.repeat_scatter_radius = kScatterRadius;
    action.repeat_stagger_lifetime_ticks = kStaggerWindow;
    require(network_example::spawn_repeat_is_authorable(action));
    const std::optional<network_example::CompiledActionGraphBinding> binding =
        network_example::compile_action_trigger_definition(
            network_example::TriggerEventType::kExpired, trigger);
    require(binding.has_value());
    return *binding;
}

struct Fuse {
    glm::vec3 position{0.0f};
    std::uint32_t lifetime = 0;
    std::uint32_t salt = 0;
};

struct Storm {
    std::vector<Fuse> fuses;
    std::vector<glm::vec3> meteor_velocities;
};

// Storm marker -> repeated fuses -> descending meteors. `derived` marks the
// fuse and meteor as never sent, which is what makes the marker a root.
std::vector<network_example::RuntimeProjectileTemplate> storm_templates(
    bool derived) {
    network_example::RuntimeProjectileTemplate storm =
        marker_template(kStormTemplate, 1);
    storm.expired_binding = storm_binding();
    // The fuse's own lifetime is overridden by the action; 99 would show if not.
    network_example::RuntimeProjectileTemplate fuse =
        marker_template(kFuseTemplate, 99);
    fuse.expired_binding = network_example::compile_spawn_projectile_binding(
        network_example::TriggerEventType::kExpired, kMeteorTemplate);
    network_example::RuntimeProjectileTemplate meteor;
    meteor.projectile_template_id = kMeteorTemplate;
    meteor.projectile_type = network_example::ProjectileType::kStandard;
    meteor.sync_mode = network_example::ProjectileSyncMode::kServerSnapshotOnly;
    meteor.damage_shape = network_example::ProjectileDamageShape::kNone;
    meteor.lifetime_ticks = 20;
    meteor.collision_mask = KERNEL_COLLISION_LAYER_TERRAIN;
    meteor.launch_type = network_example::ProjectileLaunchType::kDescent;
    meteor.launch_elevation_min_degrees = 75.0f;
    meteor.launch_elevation_max_degrees = 85.0f;
    meteor.launch_height = 40.0f;
    meteor.launch_fall_ticks = 15;
    fuse.derived = derived;
    meteor.derived = derived;
    return {storm, fuse, meteor};
}

Storm run_storm(std::uint32_t action_instance_id) {
    network_example::World world;
    world.set_projectile_templates(storm_templates(false));

    require(network_example::spawn_action_graph_projectile(
        world, kStormTemplate, 1, 77, action_instance_id, kCenter,
        glm::vec3{0.0f, 0.0f, 1.0f}, 0, kTickSeconds));

    Storm result;
    std::set<network_example::NetId> seen_fuses;
    std::set<network_example::NetId> seen_meteors;
    std::vector<KernelEvent> events;
    for (std::uint32_t tick = 1;
         tick <= 1 + kFuseLifetime + kStaggerWindow + 2; ++tick) {
        network_example::simulate_projectiles(world, kTickSeconds, tick, &events);
        auto view = world.registry()
                        .view<network_example::NetworkIdentity,
                              network_example::ProjectileState>();
        for (const entt::entity entity : view) {
            const network_example::NetId net_id =
                view.get<network_example::NetworkIdentity>(entity).net_id;
            const network_example::ProjectileState& state =
                view.get<network_example::ProjectileState>(entity);
            if (state.projectile_template_id == kFuseTemplate &&
                seen_fuses.insert(net_id).second) {
                result.fuses.push_back(Fuse{
                    state.spawn_position,
                    state.max_lifetime_ticks,
                    state.launch_salt});
            }
            if (state.projectile_template_id == kMeteorTemplate &&
                seen_meteors.insert(net_id).second) {
                result.meteor_velocities.push_back(state.initial_velocity);
            }
        }
    }
    return result;
}

// Count in range, every fuse on the disc at the event's height, lifetimes
// the override plus a stagger that puts each expiry in its own slice.
void repeat_spawns_a_scattered_staggered_set() {
    const Storm storm = run_storm(5001);
    const std::uint32_t count =
        static_cast<std::uint32_t>(storm.fuses.size());
    require(count >= 10 && count <= 15);

    std::set<std::uint32_t> salts;
    std::vector<std::uint32_t> extras;
    float farthest = 0.0f;
    for (const Fuse& fuse : storm.fuses) {
        require(std::fabs(fuse.position.y - kCenter.y) < 1e-5f);
        const float distance = glm::length(
            glm::vec2{fuse.position.x - kCenter.x, fuse.position.z - kCenter.z});
        require(distance <= kScatterRadius + 1e-4f);
        farthest = std::max(farthest, distance);
        require(fuse.lifetime >= kFuseLifetime &&
                fuse.lifetime < kFuseLifetime + kStaggerWindow);
        extras.push_back(fuse.lifetime - kFuseLifetime);
        salts.insert(fuse.salt);
    }
    // Scattered for real, not all stacked on the centre.
    require(farthest > kScatterRadius * 0.5f);
    // Each iteration carries its own salt.
    require(salts.size() == count);
    // Stratified: sorted, the i-th expiry lies in the i-th slice.
    std::sort(extras.begin(), extras.end());
    for (std::uint32_t index = 0; index < count; ++index) {
        require(extras[index] * count >= index * kStaggerWindow);
        require(extras[index] * count < (index + 1) * kStaggerWindow);
    }
}

// Every fuse becomes a meteor, and the salt each fuse carries makes their
// elevations differ -- without it they would share one seed and all fall at
// the same angle.
void each_fuse_drops_its_own_meteor() {
    const Storm storm = run_storm(5001);
    require(storm.meteor_velocities.size() == storm.fuses.size());
    std::set<int> elevations;
    for (const glm::vec3& velocity : storm.meteor_velocities) {
        const float elevation = glm::degrees(
            std::asin(-velocity.y / glm::length(velocity)));
        require(elevation >= 75.0f - 1e-3f && elevation <= 85.0f + 1e-3f);
        elevations.insert(static_cast<int>(std::lround(elevation * 100.0f)));
    }
    require(elevations.size() > storm.meteor_velocities.size() / 2);
}

// The same event reproduces the same storm; another action instance does not.
void repeat_is_deterministic_per_event() {
    const Storm first = run_storm(5001);
    const Storm again = run_storm(5001);
    require(first.fuses.size() == again.fuses.size());
    for (std::size_t index = 0; index < first.fuses.size(); ++index) {
        require(first.fuses[index].position == again.fuses[index].position);
        require(first.fuses[index].lifetime == again.fuses[index].lifetime);
    }
    const Storm other = run_storm(5002);
    bool differs = other.fuses.size() != first.fuses.size();
    for (std::size_t index = 0;
         !differs && index < first.fuses.size(); ++index) {
        differs = first.fuses[index].position != other.fuses[index].position;
    }
    require(differs);
}

void repeat_limits_are_enforced() {
    KernelActionDefinition action{};
    action.action_type = KernelEntityTriggerActionType_SpawnProjectile;
    action.repeat_count_min = 1;
    action.repeat_count_max = KERNEL_MAX_ACTION_REPEAT;
    require(network_example::spawn_repeat_is_authorable(action));
    action.repeat_count_max = KERNEL_MAX_ACTION_REPEAT + 1;
    require(!network_example::spawn_repeat_is_authorable(action));
    action.repeat_count_max = 4;
    action.repeat_count_min = 5;
    require(!network_example::spawn_repeat_is_authorable(action));
    action.repeat_count_min = 0;
    require(!network_example::spawn_repeat_is_authorable(action));
    // A scatter without a repeat means nothing, so it is refused.
    KernelActionDefinition lone{};
    lone.action_type = KernelEntityTriggerActionType_SpawnProjectile;
    lone.repeat_scatter_radius = 2.0f;
    require(!network_example::spawn_repeat_is_authorable(lone));
    // And none of it belongs on another action.
    KernelActionDefinition damage{};
    damage.action_type = KernelEntityTriggerActionType_ApplyDamage;
    damage.spawn_lifetime_ticks = 5;
    require(!network_example::spawn_repeat_is_authorable(damage));
}

std::uint32_t storm_root(network_example::World& world) {
    require(network_example::spawn_action_graph_projectile(
        world, kStormTemplate, 1, 77, 5001, kCenter,
        glm::vec3{0.0f, 0.0f, 1.0f}, 0, kTickSeconds));
    auto view = world.registry()
                    .view<network_example::NetworkIdentity,
                          network_example::ProjectileState>();
    for (const entt::entity entity : view) {
        if (view.get<network_example::ProjectileState>(entity)
                .projectile_template_id == kStormTemplate) {
            return view.get<network_example::NetworkIdentity>(entity).net_id;
        }
    }
    require(false);
    return 0;
}

std::size_t count_template(
    network_example::World& world,
    std::uint32_t projectile_template_id) {
    std::size_t count = 0;
    auto view = world.registry().view<network_example::ProjectileState>();
    for (const entt::entity entity : view) {
        if (view.get<network_example::ProjectileState>(entity)
                .projectile_template_id == projectile_template_id) {
            ++count;
        }
    }
    return count;
}

// The chain a root starts is as long as its longest derived path: fuse 10 +
// stagger 60, then a meteor that lives 20. Replicated descendants add
// nothing, so the same templates without the flag give no chain at all.
void derived_chain_covers_the_longest_path() {
    network_example::World derived_world;
    derived_world.set_projectile_templates(storm_templates(true));
    require(derived_world.find_projectile_template(kStormTemplate)
                ->derived_chain_ticks == kFuseLifetime + kStaggerWindow + 20u);
    require(derived_world.find_projectile_template(kFuseTemplate)
                ->derived_chain_ticks == 20u);
    require(derived_world.find_projectile_template(kMeteorTemplate)
                ->derived_chain_ticks == 0u);

    network_example::World sent_world;
    sent_world.set_projectile_templates(storm_templates(false));
    require(sent_world.find_projectile_template(kStormTemplate)
                ->derived_chain_ticks == 0u);
}

// The root expires on tick 1 and starts its chain, but stays until the chain
// is over -- 90 ticks -- and is gone the tick after. Without derived
// descendants it goes as soon as it expires.
void root_is_held_until_its_chain_ends() {
    network_example::World world;
    world.set_projectile_templates(storm_templates(true));
    const std::uint32_t root = storm_root(world);
    std::vector<KernelEvent> events;
    std::uint32_t gone = 0;
    for (std::uint32_t tick = 1; tick <= 100 && gone == 0; ++tick) {
        network_example::simulate_projectiles(world, kTickSeconds, tick, &events);
        if (tick == 1) {
            require(count_template(world, kFuseTemplate) >= 10u);
        }
        if (!world.find_entity(root).has_value()) {
            gone = tick;
        }
    }
    require(gone == 1u + kFuseLifetime + kStaggerWindow + 20u + 1u);

    network_example::World control;
    control.set_projectile_templates(storm_templates(false));
    const std::uint32_t sent_root = storm_root(control);
    network_example::simulate_projectiles(control, kTickSeconds, 1, &events);
    require(!control.find_entity(sent_root).has_value());
}

// If the root's chain cannot start -- here the dedup ledger is full, so its
// batch is refused -- the root is removed on the spot rather than held, which
// is how a client learns the chain was called off.
void root_whose_chain_fails_is_not_held() {
    network_example::World world;
    world.set_projectile_templates(storm_templates(true));
    const std::uint32_t root = storm_root(world);
    require(world.reserve_action_graph_batch_capacity(
        network_example::World::kActionGraphDedupCapacity));
    std::vector<KernelEvent> events;
    network_example::simulate_projectiles(world, kTickSeconds, 1, &events);
    require(count_template(world, kFuseTemplate) == 0u);
    require(!world.find_entity(root).has_value());
}

}  // namespace

int main() {
    repeat_spawns_a_scattered_staggered_set();
    each_fuse_drops_its_own_meteor();
    repeat_is_deterministic_per_event();
    repeat_limits_are_enforced();
    derived_chain_covers_the_longest_path();
    root_is_held_until_its_chain_ends();
    root_whose_chain_fails_is_not_held();
    std::printf("action_repeat_test passed\n");
    return 0;
}
