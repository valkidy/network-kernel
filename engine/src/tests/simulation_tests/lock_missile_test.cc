// A lock_missile weapon fires one missile per commit at the
// (commit_index % locked)-th nearest actor it locks. The hit is scheduled for
// the missile's arrival -- the cone's full reach takes the shot template's
// lifetime_ticks, nearer targets proportionally less -- and reported as a
// launch for clients to draw. A commit that locks nothing is Committed but
// spends nothing, fires nothing and keeps the hold running.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "simulation/public/simulation.h"
#include "world/public/world.h"

namespace {

namespace ne = network_example;

constexpr std::uint8_t kMissileWeapon = 0;
constexpr std::uint32_t kShotTemplate = 7;
constexpr std::uint32_t kFireAction = 1000;
constexpr std::uint32_t kReloadAction = 2000;
constexpr std::uint16_t kMagazine = 12;
constexpr std::uint16_t kDamage = 10;
// 20 m of reach crossed in 20 ticks: one metre a tick.
constexpr float kRange = 20.0f;
constexpr std::uint32_t kLongestFlight = 20;
constexpr float kTickSeconds = 1.0f / 30.0f;

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

struct Tick {
    std::vector<ne::ActionOutcome> outcomes;
    std::vector<ne::LockMissileLaunch> launches;
    std::vector<KernelEvent> events;

    bool has(ne::ActionOutcomeType type) const {
        for (const ne::ActionOutcome& outcome : outcomes) {
            if (outcome.type == type) return true;
        }
        return false;
    }
    bool fired() const {
        for (const KernelEvent& event : events) {
            if (event.type == KernelEventType_FireConfirmed) return true;
        }
        return false;
    }
};

// A shooter at the origin aiming down +x, holding a missile weapon that fires
// on every other tick and locks up to 3. Its launch point is 1 m up; targets
// are tested at their hitbox centre, 0.5 m up.
struct Scene {
    ne::World world;
    ne::DamagePipeline pipeline;
    ne::NetId shooter = 0;

    Scene() {
        ne::RuntimeProjectileTemplate shot{};
        shot.projectile_template_id = kShotTemplate;
        shot.max_hit_count = 3;
        shot.lifetime_ticks = kLongestFlight;
        world.set_projectile_templates({shot});
        world.set_action_templates({
            {kFireAction,
             KernelActionTriggerMode_Hold,
             KernelActionTemplateFlag_CancelOnRelease,
             1u,
             0u,
             2u,
             0u,
             2u,
             6u},
            {kReloadAction, KernelActionTriggerMode_Press, 0u, 0u, 30u, 0u, 1u, 0u, 0u},
        });
        shooter = world.spawn_player(1, glm::vec3{0.0f});
        const auto entity = world.find_entity(shooter);
        require(entity.has_value());
        world.registry().get<ne::Health>(*entity) = ne::Health{100, 100};
        ne::WeaponMechanicsDefinition missile;
        missile.id = kMissileWeapon;
        missile.mode = ne::WeaponFireMode::kLockMissile;
        missile.magazine_size = kMagazine;
        missile.damage = kDamage;
        missile.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
        missile.projectile_template_id = kShotTemplate;
        missile.fire_action_template_id = kFireAction;
        missile.reload_action_template_id = kReloadAction;
        missile.pellet_count = 1;
        missile.melee_range = kRange;
        missile.melee_fov_degrees = 60.0f;
        ne::WeaponTuning& tuning = world.registry().get<ne::WeaponTuning>(*entity);
        tuning.configured[kMissileWeapon] = true;
        tuning.definitions[kMissileWeapon] = missile;
        ne::WeaponState& weapon = world.registry().get<ne::WeaponState>(*entity);
        weapon.weapon_slot_count = 1;
        weapon.weapon_ids[0] = kMissileWeapon;
        weapon.ammo[0] = kMagazine;
        weapon.reserve_magazines[0] = 1;
    }

    ne::NetId spawn_target(const glm::vec3& position) {
        const ne::NetId target = world.spawn_enemy(position);
        const auto entity = world.find_entity(target);
        world.registry().get<ne::Health>(*entity) = ne::Health{100, 100};
        world.registry().get<ne::Hitbox>(*entity) =
            ne::Hitbox{{0.0f, 0.5f, 0.0f}, {0.3f, 0.5f, 0.3f}, 0};
        return target;
    }

    std::uint16_t hp(ne::NetId net_id) {
        return world.registry().get<ne::Health>(*world.find_entity(net_id)).hp;
    }

    std::uint16_t ammo() {
        return world.registry()
            .get<ne::WeaponState>(*world.find_entity(shooter))
            .ammo[0];
    }

    // One tick of the hold, then whatever damage is due on it, as the kernel
    // drains it after the weapon pass.
    Tick hold(std::uint32_t tick) {
        KernelPlayerInput input{};
        input.input_seq = tick + 1u;
        input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
        input.selected_weapon = kMissileWeapon;
        input.action_intent =
            KernelActionIntent{7101u, KernelActionBinding_PrimaryFire, 0u, 0u};
        input.action_input = KernelActionInput{7101u, 1u, 0u, 0u};
        Tick result;
        ne::simulate_weapons(
            world,
            {ne::QueuedInput{1, input}},
            ne::WeaponSimulationContext{
                nullptr,
                nullptr,
                &pipeline,
                tick,
                tick,
                kTickSeconds,
                0,
                &result.outcomes,
                nullptr,
                &result.launches},
            &result.events);
        drain(tick);
        return result;
    }

    void drain(std::uint32_t tick) {
        std::vector<KernelEvent> events;
        pipeline.confirm_ready(
            world,
            static_cast<std::uint64_t>(tick * kTickSeconds * 1000000.0f),
            tick,
            &events);
    }
};

std::uint32_t expected_flight(const glm::vec3& target_position) {
    const glm::vec3 offset =
        target_position + glm::vec3{0.0f, 0.5f, 0.0f} - glm::vec3{0.0f, 1.0f, 0.0f};
    return static_cast<std::uint32_t>(
        std::ceil(glm::length(offset) / (kRange / kLongestFlight)));
}

// Commits land on ticks 0, 2, 4, 6. The fourth comes back round to the
// nearest. Each missile flies for its own distance and hits on arrival, not
// before.
void each_commit_takes_the_next_locked_target_in_turn() {
    Scene scene;
    const glm::vec3 near_at{5.0f, 0.0f, 0.0f};
    const glm::vec3 mid_at{8.0f, 0.0f, 1.0f};
    const glm::vec3 far_at{12.0f, 0.0f, -1.0f};
    const ne::NetId near = scene.spawn_target(near_at);
    const ne::NetId mid = scene.spawn_target(mid_at);
    const ne::NetId far = scene.spawn_target(far_at);

    std::vector<ne::LockMissileLaunch> launches;
    for (std::uint32_t tick = 0; tick <= 6; ++tick) {
        const Tick result = scene.hold(tick);
        launches.insert(
            launches.end(), result.launches.begin(), result.launches.end());
    }
    require(launches.size() == 4u);
    const ne::NetId order[] = {near, mid, far, near};
    for (std::size_t index = 0; index < launches.size(); ++index) {
        require(launches[index].target_net_id == order[index]);
        require(launches[index].commit_index == index);
        require(launches[index].launch_tick == index * 2u);
        require(launches[index].shooter_net_id == scene.shooter);
        require(launches[index].action_instance_id == 7101u);
    }
    require(launches[0].flight_ticks == expected_flight(near_at));
    require(launches[1].flight_ticks == expected_flight(mid_at));
    require(launches[2].flight_ticks == expected_flight(far_at));
    require(scene.ammo() == kMagazine - 4);

    // The nearest was struck once by tick 6 (arrival 0 + 5), and its second
    // missile, launched on tick 6, has not landed yet.
    require(scene.hp(near) == 100 - kDamage);
    const std::uint32_t far_arrival = 4u + launches[2].flight_ticks;
    for (std::uint32_t tick = 7; tick < far_arrival; ++tick) {
        scene.drain(tick);
    }
    require(scene.hp(far) == 100);
    scene.drain(far_arrival);
    require(scene.hp(far) == 100 - kDamage);
    for (std::uint32_t tick = far_arrival + 1; tick <= 6u + launches[3].flight_ticks;
         ++tick) {
        scene.drain(tick);
    }
    require(scene.hp(near) == 100 - 2 * kDamage);
    require(scene.hp(mid) == 100 - kDamage);
}

// D9 of the plan: a commit with nothing in the cone is still Committed and the
// hold runs on, but no ammunition is spent and nothing is fired -- not even
// the FireConfirmed a client would play a cast from.
void a_commit_that_locks_nothing_spends_nothing_and_keeps_holding() {
    Scene scene;
    const Tick empty = scene.hold(0);
    require(empty.has(ne::ActionOutcomeType::Committed));
    require(!empty.has(ne::ActionOutcomeType::Corrected));
    require(empty.launches.empty());
    require(!empty.fired());
    require(scene.ammo() == kMagazine);

    const ne::NetId arrival = scene.spawn_target({3.0f, 0.0f, 0.0f});
    require(!scene.hold(1).has(ne::ActionOutcomeType::Corrected));
    const Tick second = scene.hold(2);
    require(second.has(ne::ActionOutcomeType::Committed));
    require(second.fired());
    require(second.launches.size() == 1u);
    require(second.launches[0].target_net_id == arrival);
    // The second commit, counted from zero.
    require(second.launches[0].commit_index == 1u);
    require(scene.ammo() == kMagazine - 1);
}

// The longest flight is the shot's lifetime, at the edge of the cone; a target
// all but touching the shooter still takes one tick.
void the_flight_is_bounded_by_the_shot_lifetime() {
    {
        Scene scene;
        scene.spawn_target({19.5f, 0.0f, 0.0f});
        const Tick result = scene.hold(0);
        require(result.launches.size() == 1u);
        require(result.launches[0].flight_ticks == kLongestFlight);
    }
    {
        Scene scene;
        scene.spawn_target({0.4f, 0.5f, 0.0f});
        const Tick result = scene.hold(0);
        require(result.launches.size() == 1u);
        require(result.launches[0].flight_ticks == 1u);
    }
}

}  // namespace

int main() {
    each_commit_takes_the_next_locked_target_in_turn();
    a_commit_that_locks_nothing_spends_nothing_and_keeps_holding();
    the_flight_is_bounded_by_the_shot_lifetime();
    std::puts("lock_missile_test passed");
    return 0;
}
