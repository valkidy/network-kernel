// apply_status's strength: when authored, the status lands only on a target
// whose impulse_resistance it strictly exceeds -- the hive airship
// (resistance 10) takes a water balloon's damage but never its bubble. The
// rest of the hit is unaffected. The water bubble's fourth part
// (docs/WATER_BUBBLE_PLAN.md, P4).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#define private public
#include "kernel/src/kernel.h"
#undef private
#include "simulation/public/action_graph.h"
#include "simulation/public/simulation.h"
#include "simulation/src/systems.h"

namespace {

using namespace network_example;

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "status_strength_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::uint32_t kStatusId = 1501u;

struct Outcome {
    bool status = false;
    std::size_t damage_requests = 0;
};

// One hit: damage and the status together, as the balloon's graph is.
Outcome hit(float strength, std::optional<float> resistance) {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1, glm::vec3{0.0f, 0.0f, -10.0f});
    const NetId target = world.spawn_enemy(glm::vec3{0.0f});
    const entt::entity entity = *world.find_entity(target);
    world.registry().get_or_emplace<Health>(entity) = Health{50, 50};
    if (resistance.has_value()) {
        world.registry().emplace_or_replace<ImpulseResistance>(
            entity, ImpulseResistance{*resistance});
    }
    RuntimeStatusEffectTemplate status;
    status.status_effect_id = kStatusId;
    status.channel_id = 51u;
    status.duration_ticks = 30u;
    world.set_status_effect_templates({status});

    const TriggerEvent event{TriggerEventType::kCollision, source, source, target};
    ActionExecutionProvenance provenance;
    provenance.request_id = 1u;
    provenance.server_tick = engine.current_tick();
    provenance.instigator = source;
    ActionApplyDamageCommand damage{source, target, 10, provenance};
    ActionApplyStatusCommand apply{
        source, target, kStatusId, provenance, glm::vec3{0.0f}, strength};
    require(execute_action_graph_command_batch(
        engine, ActionGraphCommandBatch{event, provenance, 1u, {damage, apply}}, 0u));

    Outcome outcome;
    const StatusEffectState* state = world.registry().try_get<StatusEffectState>(entity);
    outcome.status = state != nullptr &&
        std::any_of(state->active.begin(), state->active.end(),
                    [](const ActiveStatusEffect& active) {
                        return active.status_effect_id == kStatusId;
                    });
    outcome.damage_requests = engine.damage_pipeline_.pending_count();
    return outcome;
}

void the_status_lands_only_past_the_resistance_and_the_damage_always() {
    // The airship: resistance 10 against the balloon's 10 -- strictly greater
    // is the rule, so equal is spared.
    Outcome airship = hit(10.0f, 10.0f);
    require(!airship.status);
    require(airship.damage_requests == 1u);
    // Just past it, it lands.
    require(hit(10.0f, 9.5f).status);
    // No resistance authored reads as zero.
    require(hit(10.0f, std::nullopt).status);
    // A resistance that is not a number spares everything, as an impulse's does.
    require(!hit(10.0f, std::numeric_limits<float>::infinity()).status);
}

void a_status_that_authors_no_strength_lands_on_anyone() {
    // As before ABI 102: a nest-strength resistance does not stop it.
    const Outcome outcome = hit(0.0f, 1000.0f);
    require(outcome.status);
    require(outcome.damage_requests == 1u);
}

void a_strength_that_is_not_a_finite_non_negative_number_does_not_compile() {
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(trigger);
    trigger.action_count = 1u;
    trigger.actions[0].action_type = KernelEntityTriggerActionType_ApplyStatus;
    trigger.actions[0].target_source = KernelEntityRefSource_EventTarget;
    trigger.actions[0].status_effect_id = kStatusId;
    trigger.actions[0].status_strength = 10.0f;
    require(compile_action_trigger_definition(TriggerEventType::kCollision, trigger)
                .has_value());
    trigger.actions[0].status_strength = -1.0f;
    require(!compile_action_trigger_definition(TriggerEventType::kCollision, trigger)
                 .has_value());
    trigger.actions[0].status_strength = std::numeric_limits<float>::quiet_NaN();
    require(!compile_action_trigger_definition(TriggerEventType::kCollision, trigger)
                 .has_value());
}

}  // namespace

int main() {
    the_status_lands_only_past_the_resistance_and_the_damage_always();
    a_status_that_authors_no_strength_lands_on_anyone();
    a_strength_that_is_not_a_finite_non_negative_number_does_not_compile();
    std::puts("status_strength_test passed");
    return 0;
}
