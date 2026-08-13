#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "kernel/src/kernel.h"
#include "simulation/public/action_graph.h"
#include "simulation/src/systems.h"

namespace {

using namespace network_example;

CompiledActionGraphBinding apply_status_binding(
    std::uint32_t status_effect_id = 1001u) {
    CompiledActionGraphBinding binding;
    binding.event_type = TriggerEventType::kActivated;
    binding.graph.id = "apply_status";
    binding.graph.parameters = {
        {"target", std::monostate{}},
        {"status", StatusEffectIdValue{status_effect_id}},
    };
    binding.graph.actions = {
        ActionApplyStatusDefinition{"target", "status"},
    };
    binding.parameters = {
        {"target", EntityRefExpression{EntityRefSource::kEventTarget}},
    };
    return binding;
}

CompiledActionGraphBinding lifecycle_damage_binding(EntityRefSource target_source) {
    CompiledActionGraphBinding binding;
    binding.event_type = TriggerEventType::kStatusTick;
    binding.graph.id = "lifecycle_damage";
    binding.graph.parameters = {
        {"target", std::monostate{}},
        {"amount", 5.0f},
    };
    binding.graph.actions = {
        ActionApplyDamageDefinition{"target", "amount"},
    };
    binding.parameters = {
        {"target", EntityRefExpression{target_source}},
    };
    return binding;
}

bool apply_status(
    KernelEngine& engine,
    NetId source,
    NetId target,
    std::uint32_t status_effect_id,
    std::uint64_t request_id) {
    TriggerEvent event{
        TriggerEventType::kActivated,
        source,
        source,
        target,
    };
    ActionExecutionProvenance provenance;
    provenance.request_id = request_id;
    provenance.server_tick = engine.current_tick();
    provenance.instigator = source;
    provenance.owner_peer = 1u;
    std::vector<ActionGraphCommand> commands;
    if (!evaluate_action_graph(
            apply_status_binding(status_effect_id),
            source,
            event,
            provenance,
            &commands,
            nullptr)) {
        return false;
    }
    return execute_action_graph_command_batch(
        engine,
        ActionGraphCommandBatch{
            event, provenance, static_cast<std::uint32_t>(request_id),
            std::move(commands)},
        0u);
}

CompiledActionGraphBinding speed_on_apply_binding(
    float operation = 0.0f,
    float value = 2.0f) {
    CompiledActionGraphBinding binding;
    binding.event_type = TriggerEventType::kStatusApplied;
    binding.graph.id = "speed_on_apply";
    binding.graph.parameters = {
        {"target", std::monostate{}},
        {"operation", operation},
        {"value", value},
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

void require_at(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d\n", line);
        std::abort();
    }
}

#define require(condition) require_at((condition), __LINE__)

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
    RuntimeStatusEffectTemplate multiplier_status = status;
    multiplier_status.status_effect_id = 1002u;
    multiplier_status.on_apply_binding = speed_on_apply_binding(1.0f, 0.5f);
    world.set_status_effect_templates({status, multiplier_status});

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
    const std::uint32_t revision_after_apply = state.revision;
    require(execute_action_graph_command_batch(engine, apply_batch, 0));
    require(state.active.size() == 1u);
    require(state.speed_modifiers.size() == 1u);
    require(state.revision == revision_after_apply);

    require(apply_status(engine, source, target, 1002u, 3u));
    require(state.active.size() == 1u);
    require(state.active[0].status_effect_id == 1002u);
    require(state.speed_modifiers.size() == 1u);
    require(movement.speed_meters_per_second == 5.0f);

    CompiledActionGraphBinding remove_binding = apply_status_binding(1002u);
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

void status_capacity_and_same_channel_replacement_are_atomic() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    KernelEngine engine(config);
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1u, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{0.0f});
    std::vector<RuntimeStatusEffectTemplate> templates;
    for (std::uint32_t index = 0u; index < 33u; ++index) {
        RuntimeStatusEffectTemplate status;
        status.status_effect_id = 1000u + index;
        status.channel_id = 1u + index;
        status.duration_ticks = 100u;
        templates.push_back(status);
    }
    RuntimeStatusEffectTemplate replacement;
    replacement.status_effect_id = 2000u;
    replacement.channel_id = 1u;
    replacement.duration_ticks = 100u;
    templates.push_back(replacement);
    world.set_status_effect_templates(templates);

    for (std::uint32_t index = 0u; index < kMaxActiveStatusEffects; ++index) {
        require(apply_status(
            engine, source, target, 1000u + index, 10u + index));
    }
    const entt::entity target_entity = *world.find_entity(target);
    const StatusEffectState& state =
        world.registry().get<StatusEffectState>(target_entity);
    require(state.active.size() == kMaxActiveStatusEffects);
    require(state.revision == kMaxActiveStatusEffects);
    std::vector<KernelStatusEffectView> views(kMaxActiveStatusEffects);
    require(engine.query_status_effects(
                target,
                views.data(),
                static_cast<std::uint32_t>(views.size())) ==
            kMaxActiveStatusEffects);
    require(views.front().struct_size == sizeof(KernelStatusEffectView));
    require(views.front().instigator_net_id == source);

    const std::uint32_t revision_before_rejection = state.revision;
    require(!apply_status(engine, source, target, 1032u, 100u));
    require(state.active.size() == kMaxActiveStatusEffects);
    require(state.revision == revision_before_rejection);

    require(apply_status(engine, source, target, 2000u, 101u));
    require(state.active.size() == kMaxActiveStatusEffects);
    require(state.revision == revision_before_rejection + 1u);
    require(std::none_of(
        state.active.begin(), state.active.end(),
        [](const ActiveStatusEffect& active) {
            return active.status_effect_id == 1000u;
        }));
    require(std::any_of(
        state.active.begin(), state.active.end(),
        [](const ActiveStatusEffect& active) {
            return active.status_effect_id == 2000u;
        }));
}

void failed_lifecycle_prepare_leaves_status_state_unchanged() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1u, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{0.0f});
    RuntimeStatusEffectTemplate status;
    status.status_effect_id = 1001u;
    status.channel_id = 1u;
    status.duration_ticks = 10u;
    CompiledActionGraphBinding invalid = speed_on_apply_binding();
    invalid.parameters[0].expression =
        EntityRefExpression{EntityRefSource::kEventInstigator};
    status.on_apply_binding = invalid;
    world.set_status_effect_templates({status});

    TriggerEvent event{
        TriggerEventType::kActivated, source, source, target};
    ActionExecutionProvenance provenance;
    provenance.request_id = 1u;
    provenance.instigator = source;
    provenance.owner_peer = 1u;
    ActionGraphCommandBatch batch{event, provenance, 1u, {}};
    batch.commands.push_back(ActionApplyDamageCommand{
        source, target, 5u, provenance});
    batch.commands.push_back(ActionApplyStatusCommand{
        source, target, 1001u, provenance});
    require(!execute_action_graph_command_batch(engine, batch, 0u));
    const entt::entity target_entity = *world.find_entity(target);
    const StatusEffectState* state =
        world.registry().try_get<StatusEffectState>(target_entity);
    require(state == nullptr ||
            (state->active.empty() && state->speed_modifiers.empty() &&
             state->revision == 0u));
    require(engine.damage_pipeline().pending_count() == 0u);
}

void tick_keeps_instigator_attribution_after_despawn() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(7u, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{0.0f});
    RuntimeStatusEffectTemplate status;
    status.status_effect_id = 1001u;
    status.channel_id = 1u;
    status.duration_ticks = 10u;
    status.interval_ticks = 1u;
    status.on_tick_binding =
        lifecycle_damage_binding(EntityRefSource::kEventSubject);
    world.set_status_effect_templates({status});
    const entt::entity target_entity = *world.find_entity(target);
    StatusEffectState& state =
        world.registry().get_or_emplace<StatusEffectState>(target_entity);
    state.active.push_back(ActiveStatusEffect{
        1u, 1001u, 1u, source, 7u, 0u, 10u, 0u});
    state.revision = 1u;
    require(world.destroy(source));

    simulate_status_effects(engine, 0u);
    const std::vector<ConfirmedDamage> damage =
        engine.damage_pipeline().drain_ready_damage(world, 0u);
    require(damage.size() == 1u);
    require(damage[0].source_net_id == source);
    require(damage[0].source_peer == 7u);
    require(damage[0].target_net_id == target);
    require(damage[0].damage == 5u);
}

}  // namespace

int main() {
    apply_and_remove_status_owns_speed_modifier();
    status_capacity_and_same_channel_replacement_are_atomic();
    failed_lifecycle_prepare_leaves_status_state_unchanged();
    tick_keeps_instigator_attribution_after_despawn();
    return 0;
}
