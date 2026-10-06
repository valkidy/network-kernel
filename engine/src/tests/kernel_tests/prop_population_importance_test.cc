// Which building goes when a population group is over its cap (design D17,
// 2026-10-07).
//
// The member just spawned is never the one evicted. Of the rest, the lowest
// importance goes first, the oldest within it. So an important building (an
// initial camp) outlives ordinary ones, and a low-importance newcomer pushes
// out the oldest of the next tier up instead of removing itself.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <vector>

#include <entt/entt.hpp>

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::uint32_t kGroup = 2;
constexpr std::uint32_t kPlain = 300;      // importance 0
constexpr std::uint32_t kCamp = 301;       // importance 5
constexpr std::uint32_t kKeep = 302;       // importance 9

KernelEntityTemplateDefinition building(std::uint32_t id, std::uint8_t importance) {
    KernelEntityTemplateDefinition prop{};
    prop.struct_size = sizeof(prop);
    prop.entity_template_id = id;
    prop.entity_type = KernelEntityType_Prop;
    prop.component_flags = KERNEL_ENTITY_COMPONENT_TRANSFORM;
    prop.ai.struct_size = sizeof(prop.ai);
    prop.movement.struct_size = sizeof(prop.movement);
    prop.prop.struct_size = sizeof(prop.prop);
    prop.prop.interaction.struct_size = sizeof(prop.prop.interaction);
    prop.prop.population_group_id = kGroup;
    prop.prop.importance = importance;
    return prop;
}

struct World {
    explicit World(std::uint32_t max_alive) : engine(make_config()) {
        engine.reset_runtime_state(KernelMode_DedicatedServer);
        engine.entity_templates_.push_back(building(kPlain, 0));
        engine.entity_templates_.push_back(building(kCamp, 5));
        engine.entity_templates_.push_back(building(kKeep, 9));
        KernelPropPopulationRuleDefinition rule{};
        rule.struct_size = sizeof(rule);
        rule.population_group_id = kGroup;
        rule.max_alive = max_alive;
        engine.prop_population_rules_.push_back(rule);
    }

    static KernelConfig make_config() {
        KernelConfig config{};
        config.mode = KernelMode_DedicatedServer;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        return config;
    }

    // One per tick, so spawn ticks differ and "oldest" means something.
    std::uint32_t spawn(std::uint32_t template_id) {
        KernelServerEntityCreateInfo info{};
        info.struct_size = sizeof(info);
        info.entity_template_id = template_id;
        info.position = KernelVec3{static_cast<float>(next_x++) * 4.0f, 0.0f, 0.0f};
        info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t net_id = 0;
        require(engine.server_create_entity(info, &net_id));
        engine.tick_loop_.advance_tick();
        return net_id;
    }

    bool alive(std::uint32_t net_id) const {
        return engine.world_.find_entity(net_id).has_value();
    }

    void require_alive(std::initializer_list<std::uint32_t> expected,
                       std::initializer_list<std::uint32_t> gone) const {
        for (const std::uint32_t net_id : expected) require(alive(net_id));
        for (const std::uint32_t net_id : gone) require(!alive(net_id));
    }

    network_example::KernelEngine engine;
    int next_x = 0;
};

// All at importance 0: oldest first, as before.
void equal_importance_evicts_oldest() {
    World world(2);
    const std::uint32_t a = world.spawn(kPlain);
    const std::uint32_t b = world.spawn(kPlain);
    const std::uint32_t c = world.spawn(kPlain);
    world.require_alive({b, c}, {a});
}

// The camp is the oldest, but the plain building goes.
void lower_importance_goes_first() {
    World world(2);
    const std::uint32_t camp = world.spawn(kCamp);
    const std::uint32_t plain = world.spawn(kPlain);
    const std::uint32_t newcomer = world.spawn(kPlain);
    world.require_alive({camp, newcomer}, {plain});
}

// A full group of camps and a plain newcomer: the newcomer stays and the
// oldest camp goes -- the next tier up, not the building just put down.
void a_newcomer_never_evicts_itself() {
    World world(2);
    const std::uint32_t old_camp = world.spawn(kCamp);
    const std::uint32_t young_camp = world.spawn(kCamp);
    const std::uint32_t newcomer = world.spawn(kPlain);
    world.require_alive({young_camp, newcomer}, {old_camp});
}

// Tiers are walked lowest first: with a keep, a camp and a plain building in a
// group of three, a new keep pushes out the plain one, the next the camp, and
// only then the oldest keep.
void tiers_go_lowest_first() {
    World world(3);
    const std::uint32_t keep = world.spawn(kKeep);
    const std::uint32_t camp = world.spawn(kCamp);
    const std::uint32_t plain = world.spawn(kPlain);
    const std::uint32_t keep2 = world.spawn(kKeep);
    world.require_alive({keep, camp, keep2}, {plain});
    const std::uint32_t keep3 = world.spawn(kKeep);
    world.require_alive({keep, keep2, keep3}, {camp, plain});
    // Only keeps left: the oldest of them.
    const std::uint32_t keep4 = world.spawn(kKeep);
    world.require_alive({keep2, keep3, keep4}, {keep});
}

// A cap of one: the newcomer replaces whatever was there, whatever its rank.
void a_cap_of_one_keeps_the_newcomer() {
    World world(1);
    const std::uint32_t keep = world.spawn(kKeep);
    const std::uint32_t plain = world.spawn(kPlain);
    world.require_alive({plain}, {keep});
}

}  // namespace

int main() {
    equal_importance_evicts_oldest();
    lower_importance_goes_first();
    a_newcomer_never_evicts_itself();
    tiers_go_lowest_first();
    a_cap_of_one_keeps_the_newcomer();
    std::puts("prop_population_importance_test passed");
    return 0;
}
