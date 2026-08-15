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

CompiledActionGraphBinding lifecycle_damage_binding(
    EntityRefSource target_source,
    float damage_amount = 5.0f,
    float health_amount = -2.0f) {
    CompiledActionGraphBinding binding;
    binding.event_type = TriggerEventType::kStatusTick;
    binding.graph.id = "lifecycle_damage";
    binding.graph.parameters = {
        {"target", std::monostate{}},
        {"amount", damage_amount},
        {"health_amount", health_amount},
    };
    binding.graph.actions = {
        ActionApplyDamageDefinition{"target", "amount"},
        ActionApplyHealthChangeDefinition{"target", "health_amount"},
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

World::ActionGraphDedupKey dedup_key(
    PeerId requester_peer,
    std::uint64_t request_id,
    TriggerEventType event_type = TriggerEventType::kActivated,
    std::uint32_t sequence = 0u) {
    return World::ActionGraphDedupKey{
        requester_peer, request_id, event_type, sequence};
}

void commit_dedup_key(
    World& world,
    const World::ActionGraphDedupKey& key,
    std::uint32_t committed_tick) {
    require(
        world.reserve_action_graph_batch(key) ==
        World::ActionGraphDedupReservationResult::kReserved);
    require(world.commit_action_graph_batch(key, committed_tick));
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
    status.replacement_policy = KernelStatusEffectReplacementPolicy_Stack;
    status.max_stacks = 3u;
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

    StatusEffectState& active_state =
        world.registry().get_or_emplace<StatusEffectState>(target_entity);
    active_state.active.push_back(ActiveStatusEffect{
        77u, 1001u, 1u, source, 1u, 0u, 10u, 2u, 1u});
    active_state.revision = 4u;
    require(!apply_status(engine, source, target, 1001u, 2u));
    require(active_state.active.size() == 1u);
    require(active_state.active[0].stack_count == 1u);
    require(active_state.active[0].source == source);
    require(active_state.revision == 4u);
    require(active_state.speed_modifiers.empty());
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
    require(damage.size() == 2u);
    require(damage[0].source_net_id == source);
    require(damage[0].source_peer == 7u);
    require(damage[0].target_net_id == target);
    require(damage[0].damage == 5u);
    require(damage[1].damage == 2u);
}

void stack_scales_lifecycle_and_speed_and_keeps_latest_instigator() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    KernelEngine engine(config);
    World& world = engine.simulation_world();
    const NetId source1 = world.spawn_player(1u, glm::vec3{0.0f});
    const NetId source2 = world.spawn_player(2u, glm::vec3{0.0f});
    const NetId source3 = world.spawn_player(3u, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{0.0f});
    const entt::entity target_entity = *world.find_entity(target);
    MovementState& movement =
        world.registry().get_or_emplace<MovementState>(target_entity);
    movement.base_speed_meters_per_second = 10.0f;
    movement.speed_meters_per_second = 10.0f;

    RuntimeStatusEffectTemplate status;
    status.status_effect_id = 1001u;
    status.channel_id = 1u;
    status.duration_ticks = 10u;
    status.interval_ticks = 1u;
    status.replacement_policy = KernelStatusEffectReplacementPolicy_Stack;
    status.max_stacks = 3u;
    status.refresh_on_stack = false;
    status.on_apply_binding = speed_on_apply_binding(0.0f, 2.0f);
    status.on_tick_binding =
        lifecycle_damage_binding(EntityRefSource::kEventSubject);
    status.on_expire_binding =
        lifecycle_damage_binding(EntityRefSource::kEventSubject);
    status.on_expire_binding->event_type = TriggerEventType::kStatusExpired;
    RuntimeStatusEffectTemplate multiplier_status = status;
    multiplier_status.status_effect_id = 1002u;
    multiplier_status.channel_id = 2u;
    multiplier_status.max_stacks = 2u;
    multiplier_status.interval_ticks = 0u;
    multiplier_status.on_apply_binding = speed_on_apply_binding(1.0f, 0.5f);
    multiplier_status.on_tick_binding.reset();
    world.set_status_effect_templates({status, multiplier_status});

    require(apply_status(engine, source1, target, 1001u, 1u));
    StatusEffectState& state =
        world.registry().get<StatusEffectState>(target_entity);
    const std::uint32_t instance_id = state.active[0].instance_id;
    const std::uint32_t applied_tick = state.active[0].applied_tick;
    const std::uint32_t expire_tick = state.active[0].expire_tick;
    const std::uint32_t next_tick = state.active[0].next_tick;
    require(state.active[0].stack_count == 1u);
    require(movement.speed_meters_per_second == 12.0f);

    require(apply_status(engine, source2, target, 1001u, 2u));
    require(state.active[0].instance_id == instance_id);
    require(state.active[0].stack_count == 2u);
    require(state.active[0].applied_tick == applied_tick);
    require(state.active[0].expire_tick == expire_tick);
    require(state.active[0].next_tick == next_tick);
    require(movement.speed_meters_per_second == 14.0f);

    require(apply_status(engine, source3, target, 1001u, 3u));
    require(state.active[0].stack_count == 3u);
    require(state.active[0].source == source3);
    require(state.active[0].source_peer == 3u);
    require(movement.speed_meters_per_second == 16.0f);
    const std::uint32_t revision_at_max = state.revision;
    require(apply_status(engine, source1, target, 1001u, 4u));
    require(state.revision == revision_at_max);
    require(state.active[0].source == source3);

    KernelStatusEffectView view{};
    require(engine.query_status_effects(target, &view, 1u) == 1u);
    require(view.stack_count == 3u);
    require(view.max_stacks == 3u);

    state.active[0].next_tick = 0u;
    require(world.destroy(source3));
    simulate_status_effects(engine, 0u);
    const std::vector<ConfirmedDamage> damage =
        engine.damage_pipeline().drain_ready_damage(world, 0u);
    require(damage.size() == 2u);
    require(damage[0].damage == 15u);
    require(damage[1].damage == 6u);
    require(damage[0].source_net_id == source3);
    require(damage[0].source_peer == 3u);

    state.active[0].expire_tick = 0u;
    simulate_status_effects(engine, 0u);
    const std::vector<ConfirmedDamage> expire_damage =
        engine.damage_pipeline().drain_ready_damage(world, 0u);
    require(expire_damage.size() == 2u);
    require(expire_damage[0].damage == 15u);
    require(expire_damage[1].damage == 6u);
    require(state.active.empty());
    require(state.speed_modifiers.empty());
    require(world.registry()
                .get<MovementState>(target_entity)
                .speed_meters_per_second == 10.0f);

    const NetId multiplier_target = world.spawn_enemy(glm::vec3{0.0f});
    MovementState& multiplier_movement =
        world.registry().get_or_emplace<MovementState>(
            *world.find_entity(multiplier_target));
    multiplier_movement.base_speed_meters_per_second = 10.0f;
    multiplier_movement.speed_meters_per_second = 10.0f;
    require(apply_status(
        engine, source1, multiplier_target, 1002u, 10u));
    require(multiplier_movement.speed_meters_per_second == 5.0f);
    require(apply_status(
        engine, source2, multiplier_target, 1002u, 11u));
    require(multiplier_movement.speed_meters_per_second == 2.5f);
}

void refresh_preserves_instance_stack_and_tick_cadence() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source1 = world.spawn_player(1u, glm::vec3{0.0f});
    const NetId source2 = world.spawn_player(2u, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{0.0f});
    RuntimeStatusEffectTemplate status;
    status.status_effect_id = 1001u;
    status.channel_id = 1u;
    status.duration_ticks = 10u;
    status.interval_ticks = 2u;
    status.replacement_policy = KernelStatusEffectReplacementPolicy_Refresh;
    RuntimeStatusEffectTemplate stack_status = status;
    stack_status.status_effect_id = 1002u;
    stack_status.channel_id = 2u;
    stack_status.replacement_policy =
        KernelStatusEffectReplacementPolicy_Stack;
    stack_status.max_stacks = 2u;
    stack_status.refresh_on_stack = true;
    world.set_status_effect_templates({status, stack_status});

    require(apply_status(engine, source1, target, 1001u, 1u));
    StatusEffectState& state = world.registry().get<StatusEffectState>(
        *world.find_entity(target));
    const ActiveStatusEffect before = state.active[0];
    state.active[0].expire_tick = 1u;
    const std::uint32_t revision = state.revision;
    require(apply_status(engine, source2, target, 1001u, 2u));
    require(state.revision == revision + 1u);
    require(state.active[0].instance_id == before.instance_id);
    require(state.active[0].applied_tick == before.applied_tick);
    require(state.active[0].next_tick == before.next_tick);
    require(state.active[0].stack_count == before.stack_count);
    require(state.active[0].expire_tick == 10u);
    require(state.active[0].source == source2);
    require(state.active[0].source_peer == 2u);

    const NetId stack_target = world.spawn_enemy(glm::vec3{0.0f});
    require(apply_status(engine, source1, stack_target, 1002u, 3u));
    require(apply_status(engine, source2, stack_target, 1002u, 4u));
    StatusEffectState& stack_state = world.registry().get<StatusEffectState>(
        *world.find_entity(stack_target));
    stack_state.active[0].expire_tick = 1u;
    const std::uint32_t stack_next_tick = stack_state.active[0].next_tick;
    const std::uint32_t stack_revision = stack_state.revision;
    require(apply_status(engine, source1, stack_target, 1002u, 5u));
    require(stack_state.revision == stack_revision + 1u);
    require(stack_state.active[0].stack_count == 2u);
    require(stack_state.active[0].expire_tick == 10u);
    require(stack_state.active[0].next_tick == stack_next_tick);
    require(stack_state.active[0].source == source1);
}

void replace_orders_callbacks_and_preserves_failed_state() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1u, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{0.0f});

    RuntimeStatusEffectTemplate old_status;
    old_status.status_effect_id = 1001u;
    old_status.channel_id = 1u;
    old_status.duration_ticks = 10u;
    old_status.on_apply_binding = speed_on_apply_binding(0.0f, 2.0f);
    old_status.on_expire_binding = lifecycle_damage_binding(
        EntityRefSource::kEventSubject, 5.0f, -2.0f);
    old_status.on_expire_binding->event_type = TriggerEventType::kStatusExpired;
    RuntimeStatusEffectTemplate new_status = old_status;
    new_status.status_effect_id = 1002u;
    new_status.on_expire_binding.reset();
    new_status.on_apply_binding = lifecycle_damage_binding(
        EntityRefSource::kEventSubject, 7.0f, -3.0f);
    new_status.on_apply_binding->event_type = TriggerEventType::kStatusApplied;
    world.set_status_effect_templates({old_status, new_status});

    const entt::entity target_entity = *world.find_entity(target);
    MovementState& movement =
        world.registry().get_or_emplace<MovementState>(target_entity);
    movement.base_speed_meters_per_second = 10.0f;
    movement.speed_meters_per_second = 10.0f;
    require(apply_status(engine, source, target, 1001u, 1u));
    require(engine.damage_pipeline().drain_ready_damage(world, 0u).empty());
    require(movement.speed_meters_per_second == 12.0f);
    require(apply_status(engine, source, target, 1002u, 2u));
    const std::vector<ConfirmedDamage> replacement_damage =
        engine.damage_pipeline().drain_ready_damage(world, 0u);
    require(replacement_damage.size() == 4u);
    require(replacement_damage[0].damage == 5u);
    require(replacement_damage[1].damage == 7u);
    require(replacement_damage[2].damage == 2u);
    require(replacement_damage[3].damage == 3u);

    const StatusEffectState& state =
        world.registry().get<StatusEffectState>(target_entity);
    require(state.active.size() == 1u);
    require(state.active[0].status_effect_id == 1002u);
    require(state.revision == 2u);

    KernelEngine failure_engine(KernelConfig{});
    World& failure_world = failure_engine.simulation_world();
    const NetId failure_source =
        failure_world.spawn_player(1u, glm::vec3{0.0f});
    const NetId failure_target =
        failure_world.spawn_enemy(glm::vec3{0.0f});
    const entt::entity failure_target_entity =
        *failure_world.find_entity(failure_target);
    MovementState& failure_movement =
        failure_world.registry().get_or_emplace<MovementState>(
            failure_target_entity);
    failure_movement.base_speed_meters_per_second = 10.0f;
    failure_movement.speed_meters_per_second = 10.0f;
    RuntimeStatusEffectTemplate failure_old = old_status;
    failure_old.status_effect_id = 2001u;
    RuntimeStatusEffectTemplate invalid_status = failure_old;
    invalid_status.status_effect_id = 2002u;
    invalid_status.on_expire_binding.reset();
    CompiledActionGraphBinding invalid_binding = speed_on_apply_binding();
    invalid_binding.parameters[0].expression =
        EntityRefExpression{EntityRefSource::kEventInstigator};
    invalid_status.on_apply_binding = invalid_binding;
    failure_world.set_status_effect_templates({failure_old, invalid_status});
    require(apply_status(
        failure_engine, failure_source, failure_target, 2001u, 10u));
    const StatusEffectState& failure_state =
        failure_world.registry().get<StatusEffectState>(
            failure_target_entity);
    const ActiveStatusEffect before_failure = failure_state.active[0];
    const std::uint32_t revision_before_failure = failure_state.revision;
    require(failure_movement.speed_meters_per_second == 12.0f);
    require(!apply_status(
        failure_engine, failure_source, failure_target, 2002u, 11u));
    require(failure_engine.damage_pipeline().drain_ready_damage(
                failure_world, 0u).empty());
    require(failure_movement.speed_meters_per_second == 12.0f);
    require(failure_state.speed_modifiers.size() == 1u);
    require(failure_state.active.size() == 1u);
    require(failure_state.active[0].status_effect_id ==
            before_failure.status_effect_id);
    require(failure_state.active[0].instance_id == before_failure.instance_id);
    require(failure_state.revision == revision_before_failure);
}

void lifecycle_duplicate_is_a_noop() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1u, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{0.0f});
    const TriggerEvent event{
        TriggerEventType::kStatusTick,
        target,
        source,
        target,
    };
    ActionExecutionProvenance provenance;
    provenance.request_id = 77u;
    provenance.server_tick = engine.current_tick();
    provenance.instigator = source;
    provenance.owner_peer = 1u;
    std::vector<ActionGraphCommand> commands;
    require(evaluate_action_graph(
        lifecycle_damage_binding(EntityRefSource::kEventSubject),
        target,
        event,
        provenance,
        &commands,
        nullptr));
    const ActionGraphCommandBatch batch{
        event, provenance, 4u, std::move(commands)};
    require(execute_action_graph_command_batch(engine, batch, 0u));
    const std::vector<ConfirmedDamage> first =
        engine.damage_pipeline().drain_ready_damage(world, 0u);
    require(first.size() == 2u);
    require(execute_action_graph_command_batch(engine, batch, 0u));
    require(engine.damage_pipeline().drain_ready_damage(world, 0u).empty());
}

void cross_channel_modifiers_restore_independently() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1u, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{0.0f});
    const entt::entity target_entity = *world.find_entity(target);
    MovementState& movement =
        world.registry().get_or_emplace<MovementState>(target_entity);
    movement.base_speed_meters_per_second = 10.0f;
    movement.speed_meters_per_second = 10.0f;

    RuntimeStatusEffectTemplate additive;
    additive.status_effect_id = 1101u;
    additive.channel_id = 1u;
    additive.duration_ticks = 10u;
    additive.on_apply_binding = speed_on_apply_binding(0.0f, 2.0f);
    RuntimeStatusEffectTemplate multiplier = additive;
    multiplier.status_effect_id = 1102u;
    multiplier.channel_id = 2u;
    multiplier.on_apply_binding = speed_on_apply_binding(1.0f, 0.5f);
    world.set_status_effect_templates({additive, multiplier});

    require(apply_status(engine, source, target, 1101u, 1u));
    require(apply_status(engine, source, target, 1102u, 2u));
    require(movement.speed_meters_per_second == 7.0f);

    CompiledActionGraphBinding remove_binding = apply_status_binding(1101u);
    remove_binding.graph.actions = {
        ActionRemoveStatusDefinition{"target", "status"},
    };
    const TriggerEvent event{
        TriggerEventType::kActivated, source, source, target};
    std::vector<ActionGraphCommand> commands;
    require(evaluate_action_graph(
        remove_binding,
        source,
        event,
        ActionExecutionProvenance{3u, 0u, 0u, source},
        &commands,
        nullptr));
    require(execute_action_graph_command_batch(
        engine,
        ActionGraphCommandBatch{
            event, ActionExecutionProvenance{3u, 0u, 0u, source}, 3u,
            std::move(commands)},
        0u));
    require(movement.speed_meters_per_second == 5.0f);

    StatusEffectState& state =
        world.registry().get<StatusEffectState>(target_entity);
    auto multiplier_status = std::find_if(
        state.active.begin(), state.active.end(),
        [](const ActiveStatusEffect& active) {
            return active.status_effect_id == 1102u;
        });
    require(multiplier_status != state.active.end());
    multiplier_status->expire_tick = 0u;
    simulate_status_effects(engine, 0u);
    require(state.active.empty());
    require(state.speed_modifiers.empty());
    require(movement.speed_meters_per_second == 10.0f);
}

void bounded_dedup_ledger_handles_retention_capacity_and_peers() {
    World world;
    world.set_action_graph_dedup_retention_ticks(4u);
    const World::ActionGraphDedupKey wrap_key = dedup_key(1u, 1u);
    commit_dedup_key(world, wrap_key, UINT32_MAX - 1u);
    require(
        world.reserve_action_graph_batch(wrap_key) ==
        World::ActionGraphDedupReservationResult::kDuplicate);
    world.prune_action_graph_batches(1u);
    require(world.action_graph_batch_count() == 1u);
    world.prune_action_graph_batches(2u);
    require(world.action_graph_batch_count() == 0u);

    commit_dedup_key(world, dedup_key(1u, 2u), 10u);
    commit_dedup_key(world, dedup_key(2u, 2u), 10u);
    require(world.action_graph_batch_count() == 2u);
    world.clear_action_graph_batches_for_peer(1u);
    require(world.action_graph_batch_count() == 1u);
    commit_dedup_key(world, dedup_key(1u, 2u), 11u);
    require(world.action_graph_batch_count() == 2u);

    const World::ActionGraphDedupKey cancelled = dedup_key(3u, 3u);
    require(
        world.reserve_action_graph_batch(cancelled) ==
        World::ActionGraphDedupReservationResult::kReserved);
    require(world.action_graph_batch_reserved_count() == 1u);
    world.cancel_action_graph_batch(cancelled);
    require(world.action_graph_batch_reserved_count() == 0u);
    require(world.action_graph_batch_count() == 2u);

}

void long_running_status_ticks_bound_ledger() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30u;
    config.tick.snapshot_rate = 15u;
    config.tick.history_ms = 501u;
    KernelEngine engine(config);
    require(engine.start_dedicated_server(19001u));
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1u, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{0.0f});
    RuntimeStatusEffectTemplate status;
    status.status_effect_id = 1201u;
    status.channel_id = 1u;
    status.duration_ticks = 20000u;
    status.interval_ticks = 1u;
    status.on_tick_binding =
        lifecycle_damage_binding(EntityRefSource::kEventSubject);
    world.set_status_effect_templates({status});

    require(apply_status(engine, source, target, 1201u, 1u));
    require(world.action_graph_dedup_retention_ticks() == 17u);
    for (std::uint32_t tick = 0u; tick < 10000u; ++tick) {
        engine.update(1.0f / 30.0f);
        require(world.action_graph_batch_count() <= 17u);
    }
    require(world.action_graph_batch_count() <=
            world.action_graph_dedup_retention_ticks());
    require(world.action_graph_batch_count() <
            World::kActionGraphDedupCapacity);
}

void lifecycle_capacity_rejection_preserves_replace_state() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1u, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{0.0f});
    const entt::entity target_entity = *world.find_entity(target);
    MovementState& movement =
        world.registry().get_or_emplace<MovementState>(target_entity);
    movement.base_speed_meters_per_second = 10.0f;
    movement.speed_meters_per_second = 10.0f;

    RuntimeStatusEffectTemplate old_status;
    old_status.status_effect_id = 1301u;
    old_status.channel_id = 1u;
    old_status.duration_ticks = 100u;
    old_status.on_apply_binding = speed_on_apply_binding(0.0f, 2.0f);
    old_status.on_expire_binding = lifecycle_damage_binding(
        EntityRefSource::kEventSubject);
    old_status.on_expire_binding->event_type = TriggerEventType::kStatusExpired;
    RuntimeStatusEffectTemplate new_status = old_status;
    new_status.status_effect_id = 1302u;
    new_status.on_expire_binding.reset();
    new_status.on_apply_binding = lifecycle_damage_binding(
        EntityRefSource::kEventSubject, 7.0f, -3.0f);
    new_status.on_apply_binding->event_type = TriggerEventType::kStatusApplied;
    world.set_status_effect_templates({old_status, new_status});

    require(apply_status(engine, source, target, 1301u, 1u));
    require(movement.speed_meters_per_second == 12.0f);
    const StatusEffectState& state =
        world.registry().get<StatusEffectState>(target_entity);
    const std::uint32_t revision = state.revision;
    while (world.action_graph_batch_count() + 2u <
           World::kActionGraphDedupCapacity) {
        commit_dedup_key(
            world,
            dedup_key(
                9u,
                static_cast<std::uint64_t>(world.action_graph_batch_count()) +
                    10000u),
            0u);
    }
    require(!apply_status(engine, source, target, 1302u, 2u));
    require(engine.damage_pipeline().pending_count() == 0u);
    require(world.action_graph_batch_reserved_count() == 0u);
    require(movement.speed_meters_per_second == 12.0f);
    require(state.active.size() == 1u);
    require(state.active[0].status_effect_id == 1301u);
    require(state.revision == revision);
}

void capacity_rejection_happens_before_damage_submission() {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    for (std::size_t index = 0u;
         index < World::kActionGraphDedupCapacity;
         ++index) {
        commit_dedup_key(world, dedup_key(9u, index + 1u), 0u);
    }
    const NetId source = world.spawn_player(1u, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{0.0f});
    const TriggerEvent event{
        TriggerEventType::kActivated, source, source, target};
    ActionExecutionProvenance provenance;
    provenance.request_id = 9000u;
    provenance.server_tick = 0u;
    provenance.instigator = source;
    provenance.owner_peer = 1u;
    const ActionGraphCommandBatch batch{
        event,
        provenance,
        0u,
        {ActionApplyDamageCommand{source, target, 10u, provenance}},
    };
    require(!execute_action_graph_command_batch(engine, batch, 0u));
    require(engine.damage_pipeline().pending_count() == 0u);
    require(world.action_graph_batch_reserved_count() == 0u);
}

}  // namespace

int main() {
    apply_and_remove_status_owns_speed_modifier();
    status_capacity_and_same_channel_replacement_are_atomic();
    failed_lifecycle_prepare_leaves_status_state_unchanged();
    tick_keeps_instigator_attribution_after_despawn();
    stack_scales_lifecycle_and_speed_and_keeps_latest_instigator();
    refresh_preserves_instance_stack_and_tick_cadence();
    replace_orders_callbacks_and_preserves_failed_state();
    lifecycle_duplicate_is_a_noop();
    cross_channel_modifiers_restore_independently();
    bounded_dedup_ledger_handles_retention_capacity_and_peers();
    long_running_status_ticks_bound_ledger();
    lifecycle_capacity_rejection_preserves_replace_state();
    capacity_rejection_happens_before_damage_submission();
    return 0;
}
