#include <cassert>
#include <cstdlib>

#include "kernel/src/kernel.h"
#include "simulation/public/action_graph.h"
#include "simulation/src/systems.h"

namespace {

using namespace network_example;

CompiledActionGraphBinding apply_status_binding() {
    CompiledActionGraphBinding binding;
    binding.event_type = TriggerEventType::kActivated;
    binding.graph.id = "apply_status";
    binding.graph.parameters = {
        {"target", std::monostate{}},
        {"status", StatusEffectIdValue{1001}},
    };
    binding.graph.actions = {
        ActionApplyStatusDefinition{"target", "status"},
    };
    binding.parameters = {
        {"target", EntityRefExpression{EntityRefSource::kEventTarget}},
    };
    return binding;
}

CompiledActionGraphBinding speed_on_apply_binding() {
    CompiledActionGraphBinding binding;
    binding.event_type = TriggerEventType::kStatusApplied;
    binding.graph.id = "speed_on_apply";
    binding.graph.parameters = {
        {"target", std::monostate{}},
        {"operation", 0.0f},
        {"value", 2.0f},
    };
    binding.graph.actions = {
        ActionApplySpeedModifierDefinition{
            "target", "operation", "value"},
    };
    binding.parameters = {
        {"target", EntityRefExpression{EntityRefSource::kEventSubject}},
    };
    return binding;
}

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

void apply_and_remove_status_owns_speed_modifier() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{0.0f});
    const entt::entity target_entity = *world.find_entity(target);
    MovementState& movement =
        world.registry().get_or_emplace<MovementState>(target_entity);
    movement.base_speed_meters_per_second = 10.0f;
    movement.speed_meters_per_second = 10.0f;

    RuntimeStatusEffectTemplate status;
    status.status_effect_id = 1001;
    status.channel_id = 7;
    status.duration_ticks = 10;
    status.on_apply_binding = speed_on_apply_binding();
    world.set_status_effect_templates({status});

    const TriggerEvent event{
        TriggerEventType::kActivated,
        source,
        source,
        target,
    };
    ActionExecutionProvenance provenance;
    provenance.request_id = 1;
    provenance.server_tick = 0;
    provenance.instigator = source;
    std::vector<ActionGraphCommand> commands;
    require(evaluate_action_graph(
        apply_status_binding(),
        source,
        event,
        provenance,
        &commands,
        nullptr));
    require(commands.size() == 1);
    ActionGraphCommandBatch apply_batch{event, provenance, 1, std::move(commands)};
    require(execute_action_graph_command_batch(engine, apply_batch, 0));
    const StatusEffectState& state =
        world.registry().get<StatusEffectState>(target_entity);
    require(state.active.size() == 1);
    require(state.speed_modifiers.size() == 1);
    require(movement.speed_meters_per_second == 12.0f);

    CompiledActionGraphBinding remove_binding = apply_status_binding();
    remove_binding.graph.id = "remove_status";
    remove_binding.graph.actions = {
        ActionRemoveStatusDefinition{"target", "status"},
    };
    commands.clear();
    require(evaluate_action_graph(
        remove_binding,
        source,
        event,
        ActionExecutionProvenance{2, 0, 0, source},
        &commands,
        nullptr));
    ActionGraphCommandBatch remove_batch{
        event,
        ActionExecutionProvenance{2, 0, 0, source},
        2,
        std::move(commands),
    };
    require(execute_action_graph_command_batch(engine, remove_batch, 0));
    require(state.active.empty());
    require(state.speed_modifiers.empty());
    require(movement.speed_meters_per_second == 10.0f);
}

}  // namespace

int main() {
    apply_and_remove_status_owns_speed_modifier();
    return 0;
}
