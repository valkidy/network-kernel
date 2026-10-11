// A hit scheduled against a life ends with it. Whatever ends the life -- a
// death by damage through enter_death_state, or health zeroed directly by
// server_set_entity_health -- drops the hits still on their way, so a revive
// that comes before one would have landed is not struck by it.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// The same reach into the engine activation_system_test takes.
#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

namespace ne = network_example;

void require_impl(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d\n", line);
        std::abort();
    }
}

#define require(condition) require_impl((condition), __LINE__)

enum class Death { kByDamage, kBySetHealth, kNone };

// Schedules a 30-point hit for tick 20, ends the target's life by `death`,
// revives it at full health, and returns its health after tick 20 is drained.
std::uint16_t health_after_a_revive(Death death) {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    ne::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    const ne::NetId target = engine.world_.spawn_enemy(glm::vec3{0.0f});
    const auto entity = engine.world_.find_entity(target);
    require(entity.has_value());
    engine.world_.registry().replace<ne::Health>(*entity, ne::Health{100, 100});

    ne::DamageRequest missile = ne::damage_request_at(
        10, 0, 999, target, 0, 1, 30, 0, glm::vec3{0.0f});
    missile.apply_tick = 20;
    require(engine.damage_pipeline_.submit_damage_request(missile));
    // Moved from the queue to pending, as a tick between fire and death does.
    require(engine.damage_pipeline_
                .drain_ready_damage(engine.world_, 0, 12)
                .empty());

    if (death == Death::kByDamage) {
        engine.world_.registry().get<ne::Health>(*entity).hp = 0;
        ne::EntityLifecycleSystem lifecycle;
        lifecycle.enter_death_state(
            engine,
            {ne::ConfirmedDamage{14, 0, 998, target, 0, 1, 100, 0, glm::vec3{0.0f}}});
    } else if (death == Death::kBySetHealth) {
        require(engine.server_set_entity_health(target, 0));
    }
    engine.world_.registry().get<ne::Health>(*entity) = ne::Health{100, 100};

    const std::vector<ne::ConfirmedDamage> ready =
        engine.damage_pipeline_.drain_ready_damage(
            engine.world_, std::numeric_limits<std::uint64_t>::max(), 20);
    std::vector<KernelEvent> events;
    (void)ne::apply_damage_applications(engine.world_, ready, 20, &events);
    return engine.world_.registry().get<ne::Health>(*entity).hp;
}

}  // namespace

int main() {
    // The control: with no death in between, the hit lands.
    require(health_after_a_revive(Death::kNone) == 70);
    require(health_after_a_revive(Death::kByDamage) == 100);
    require(health_after_a_revive(Death::kBySetHealth) == 100);
    std::puts("scheduled_damage_death_test passed");
    return 0;
}
