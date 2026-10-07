// apply_block_actions: a status on_apply action that holds its subject for as
// long as that status instance stands -- no new action, and the one under way
// ended. The water bubble's first part (docs/WATER_BUBBLE_PLAN.md, P1).
//
// Every refusal and interrupt here pairs with a control that runs the same
// thing without the block and watches it succeed, because "nothing happened"
// is also what a broken template or a missing input looks like.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "kernel/src/kernel.h"
#include "simulation/public/action_graph.h"
#include "simulation/public/simulation.h"
#include "simulation/src/systems.h"

namespace {

using namespace network_example;

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "status_action_block_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::uint32_t kBlockingStatusId = 1101u;
constexpr std::uint32_t kPlainStatusId = 1102u;

CompiledActionGraphBinding apply_status_binding(std::uint32_t status_effect_id) {
    CompiledActionGraphBinding binding;
    binding.event_type = TriggerEventType::kActivated;
    binding.graph.id = "apply_status";
    binding.graph.parameters = {
        {"target", std::monostate{}},
        {"status", StatusEffectIdValue{status_effect_id}},
    };
    binding.graph.actions = {ActionApplyStatusDefinition{"target", "status"}};
    binding.parameters = {
        {"target", EntityRefExpression{EntityRefSource::kEventTarget}},
    };
    return binding;
}

CompiledActionGraphBinding block_on_apply_binding() {
    CompiledActionGraphBinding binding;
    binding.event_type = TriggerEventType::kStatusApplied;
    binding.graph.id = "block_on_apply";
    binding.graph.parameters = {{"target", std::monostate{}}};
    binding.graph.actions = {ActionApplyBlockActionsDefinition{"target"}};
    binding.parameters = {
        {"target", EntityRefExpression{EntityRefSource::kEventSubject}},
    };
    return binding;
}

bool run_status_action(
    KernelEngine& engine,
    const CompiledActionGraphBinding& binding,
    NetId source,
    NetId target,
    std::uint64_t request_id) {
    const TriggerEvent event{TriggerEventType::kActivated, source, source, target};
    ActionExecutionProvenance provenance;
    provenance.request_id = request_id;
    provenance.server_tick = engine.current_tick();
    provenance.instigator = source;
    provenance.owner_peer = 1u;
    std::vector<ActionGraphCommand> commands;
    if (!evaluate_action_graph(binding, source, event, provenance, &commands, nullptr)) {
        return false;
    }
    return execute_action_graph_command_batch(
        engine,
        ActionGraphCommandBatch{
            event, provenance, static_cast<std::uint32_t>(request_id),
            std::move(commands)},
        0u);
}

struct StatusFixture {
    StatusFixture() {
        World& world = engine.simulation_world();
        source = world.spawn_player(1, glm::vec3{0.0f});
        target = world.spawn_enemy(glm::vec3{2.0f, 0.0f, 0.0f});
        world.registry().get_or_emplace<Health>(*world.find_entity(target)) =
            Health{50, 50};

        RuntimeStatusEffectTemplate blocking;
        blocking.status_effect_id = kBlockingStatusId;
        blocking.channel_id = 11u;
        blocking.duration_ticks = 30u;
        blocking.on_apply_binding = block_on_apply_binding();
        // The control: same lifetime, nothing on apply.
        RuntimeStatusEffectTemplate plain = blocking;
        plain.status_effect_id = kPlainStatusId;
        plain.channel_id = 12u;
        plain.on_apply_binding.reset();
        world.set_status_effect_templates({blocking, plain});
    }

    KernelLocalActionResultReason reason() {
        World& world = engine.simulation_world();
        return action_block_reason(
            world, *world.find_entity(target), engine.current_tick());
    }

    KernelEngine engine{KernelConfig{}};
    NetId source = 0;
    NetId target = 0;
};

void a_status_without_the_action_blocks_nothing() {
    StatusFixture f;
    require(run_status_action(
        f.engine, apply_status_binding(kPlainStatusId), f.source, f.target, 1u));
    const StatusEffectState& state = f.engine.simulation_world().registry().get<
        StatusEffectState>(*f.engine.simulation_world().find_entity(f.target));
    require(state.active.size() == 1u);
    require(!state.active[0].blocks_actions);
    require(f.reason() == KernelLocalActionResultReason_None);
}

void the_block_stands_exactly_as_long_as_its_status() {
    StatusFixture f;
    require(f.reason() == KernelLocalActionResultReason_None);
    require(run_status_action(
        f.engine, apply_status_binding(kBlockingStatusId), f.source, f.target, 1u));
    World& world = f.engine.simulation_world();
    StatusEffectState& state =
        world.registry().get<StatusEffectState>(*world.find_entity(f.target));
    require(state.active.size() == 1u);
    require(state.active[0].blocks_actions);
    require(f.reason() == KernelLocalActionResultReason_StatusBlocked);

    // Removed: the block goes with the instance.
    CompiledActionGraphBinding remove = apply_status_binding(kBlockingStatusId);
    remove.graph.actions = {ActionRemoveStatusDefinition{"target", "status"}};
    require(run_status_action(f.engine, remove, f.source, f.target, 2u));
    require(state.active.empty());
    require(f.reason() == KernelLocalActionResultReason_None);

    // Expired: the same.
    require(run_status_action(
        f.engine, apply_status_binding(kBlockingStatusId), f.source, f.target, 3u));
    require(f.reason() == KernelLocalActionResultReason_StatusBlocked);
    state.active[0].expire_tick = 0u;
    simulate_status_effects(f.engine, 0u);
    require(state.active.empty());
    require(f.reason() == KernelLocalActionResultReason_None);
}

void the_action_is_refused_outside_a_status_on_apply() {
    // From the ABI: only kStatusApplied, and only onto the status's own
    // subject, compiles. The on_apply form is the control.
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(KernelActionTriggerDefinition);
    trigger.action_count = 1u;
    trigger.actions[0].action_type = KernelEntityTriggerActionType_ApplyBlockActions;
    trigger.actions[0].target_source = KernelEntityRefSource_EventSubject;
    require(compile_action_trigger_definition(
                TriggerEventType::kStatusApplied, trigger)
                .has_value());
    require(!compile_action_trigger_definition(TriggerEventType::kCollision, trigger)
                 .has_value());
    require(!compile_action_trigger_definition(TriggerEventType::kStatusTick, trigger)
                 .has_value());
    trigger.actions[0].target_source = KernelEntityRefSource_EventTarget;
    require(!compile_action_trigger_definition(
                 TriggerEventType::kStatusApplied, trigger)
                 .has_value());

    // A hand-built graph running it from an ordinary event has no status
    // instance to hold it on, and the whole batch is refused.
    StatusFixture f;
    CompiledActionGraphBinding direct;
    direct.event_type = TriggerEventType::kActivated;
    direct.graph.id = "direct_block";
    direct.graph.parameters = {{"target", std::monostate{}}};
    direct.graph.actions = {ActionApplyBlockActionsDefinition{"target"}};
    direct.parameters = {
        {"target", EntityRefExpression{EntityRefSource::kEventTarget}},
    };
    require(!run_status_action(f.engine, direct, f.source, f.target, 1u));
    require(f.reason() == KernelLocalActionResultReason_None);
}

// ---------------------------------------------------------------------------
// The action pass
// ---------------------------------------------------------------------------

constexpr std::uint32_t kSwingTemplateId = 3001u;
constexpr std::uint32_t kWindupTicks = 5u;
constexpr PeerId kPlayerPeer = 1u;

RuntimeActionTemplate swing_template() {
    return RuntimeActionTemplate{
        kSwingTemplateId,
        KernelActionTriggerMode_Press,
        static_cast<std::uint8_t>(KernelActionTemplateFlag_CancelOnDeath),
        0u,
        kWindupTicks,
        0u,
        1u,
        8u,
        0u,
    };
}

struct ActionFixture {
    ActionFixture() {
        world.set_action_templates({swing_template()});
        player = world.spawn_player(kPlayerPeer, glm::vec3{0.0f});
        const entt::entity entity = *world.find_entity(player);
        world.registry().get_or_emplace<Health>(entity) = Health{500, 500};
        WeaponTuning& tuning = world.registry().get_or_emplace<WeaponTuning>(entity);
        tuning.configured = {true, false, false, false, false, false, false};
        WeaponMechanicsDefinition weapon;
        weapon.id = kWeaponSlot0;
        weapon.mode = WeaponFireMode::kHitscan;
        weapon.magazine_size = 100;
        weapon.damage = 10;
        weapon.max_range = 10.0f;
        weapon.pellet_count = 1;
        weapon.fire_action_template_id = kSwingTemplateId;
        tuning.definitions[kWeaponSlot0] = weapon;
        WeaponState& state = world.registry().get_or_emplace<WeaponState>(entity);
        state.weapon_slot_count = 1;
        state.weapon_ids[0] = kWeaponSlot0;
        state.ammo[0] = weapon.magazine_size;
        state.reserve_magazines[0] = 1;
    }

    // Stands in for a status instance whose on_apply ran apply_block_actions;
    // the status path itself is covered above.
    void hold(bool blocked) {
        StatusEffectState& state =
            world.registry().get_or_emplace<StatusEffectState>(*world.find_entity(player));
        state.active.clear();
        if (blocked) {
            ActiveStatusEffect active;
            active.instance_id = 1u;
            active.status_effect_id = kBlockingStatusId;
            active.channel_id = 11u;
            active.expire_tick = tick + 1000u;
            active.blocks_actions = true;
            state.active.push_back(active);
        }
    }

    KernelPlayerInput idle_input() const {
        KernelPlayerInput input{};
        input.input_seq = next_seq;
        input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
        input.selected_weapon = kWeaponSlot0;
        return input;
    }

    KernelPlayerInput press(std::uint32_t action_instance_id) const {
        KernelPlayerInput input = idle_input();
        input.action_intent = KernelActionIntent{
            action_instance_id, KernelActionBinding_PrimaryFire, 0u, 0u};
        return input;
    }

    void step(const KernelPlayerInput& input) {
        commits += static_cast<int>(
            simulate_actions(world, {QueuedInput{kPlayerPeer, input}}, tick, &outcomes)
                .size());
        ++tick;
        ++next_seq;
    }

    int outcomes_with(ActionOutcomeType type, KernelLocalActionResultReason reason) const {
        int count = 0;
        for (const ActionOutcome& outcome : outcomes) {
            if (outcome.type == type && outcome.reason == reason) {
                ++count;
            }
        }
        return count;
    }

    KernelActionPhase phase() const {
        return static_cast<KernelActionPhase>(
            world.registry().get<ActionRuntimeState>(*world.find_entity(player)).phase);
    }

    World world;
    NetId player = 0;
    std::uint32_t tick = 100u;
    std::uint32_t next_seq = 1u;
    int commits = 0;
    std::vector<ActionOutcome> outcomes;
};

void a_block_during_the_windup_ends_it_without_swinging() {
    // Control: unblocked, the swing lands.
    {
        ActionFixture f;
        f.step(f.press(1u));
        for (std::uint32_t i = 0; i < kWindupTicks + 2u; ++i) {
            f.step(f.idle_input());
        }
        require(f.commits == 1);
    }
    ActionFixture f;
    f.step(f.press(1u));
    require(f.phase() == KernelActionPhase_Windup);
    f.hold(true);
    for (std::uint32_t i = 0; i < kWindupTicks + 2u; ++i) {
        f.step(f.idle_input());
    }
    require(f.commits == 0);
    require(
        f.outcomes_with(
            ActionOutcomeType::Corrected, KernelLocalActionResultReason_StatusBlocked) ==
        1);
    // Recovery skipped: the action is gone, not recovering.
    require(f.phase() == KernelActionPhase_None);
}

void a_new_action_is_refused_while_held_and_admitted_after() {
    ActionFixture f;
    f.hold(true);
    f.step(f.press(1u));
    require(
        f.outcomes_with(
            ActionOutcomeType::Rejected, KernelLocalActionResultReason_StatusBlocked) ==
        1);
    require(f.phase() == KernelActionPhase_None);

    f.hold(false);
    f.step(f.press(2u));
    require(
        f.outcomes_with(ActionOutcomeType::Admitted, KernelLocalActionResultReason_None) ==
        1);
    for (std::uint32_t i = 0; i < kWindupTicks + 2u; ++i) {
        f.step(f.idle_input());
    }
    require(f.commits == 1);
}

// ---------------------------------------------------------------------------
// A held beam (plan R6: the drone's weapon is one)
// ---------------------------------------------------------------------------

constexpr std::uint32_t kBeamActionId = 1003u;
constexpr std::uint32_t kBeamProjectileId = 5u;

std::uint32_t live_beams(const World& world) {
    return static_cast<std::uint32_t>(
        world.registry().view<const ProjectileBeamRuntime>().size());
}

struct BeamFixture {
    BeamFixture() {
        world.set_action_templates({RuntimeActionTemplate{
            kBeamActionId,
            KernelActionTriggerMode_Hold,
            KernelActionTemplateFlag_CancelOnRelease |
                KernelActionTemplateFlag_CancelOnDeath |
                KernelActionTemplateFlag_CancelOnWeaponChange |
                KernelActionTemplateFlag_CancelBeforeFirstCommit,
            1,
            5,
            1,
            0,
            2,
            6,
        }});
        RuntimeProjectileTemplate beam;
        beam.projectile_template_id = kBeamProjectileId;
        beam.weapon_id = kWeaponId5;
        beam.projectile_type = ProjectileType::kBeam;
        beam.damage = 1;
        beam.lifetime_ticks = 30;
        beam.beam_length = 10.0f;
        beam.beam_radius = 0.25f;
        world.set_projectile_templates({beam});

        player = world.spawn_player(kPlayerPeer, glm::vec3{0.0f});
        const entt::entity entity = *world.find_entity(player);
        world.registry().get_or_emplace<Health>(entity) = Health{100, 100};
        WeaponTuning& tuning = world.registry().get_or_emplace<WeaponTuning>(entity);
        tuning.configured[kWeaponId5] = true;
        WeaponMechanicsDefinition weapon;
        weapon.id = kWeaponId5;
        weapon.mode = WeaponFireMode::kProjectile;
        weapon.magazine_size = 60;
        weapon.damage = 1;
        weapon.pellet_count = 1;
        weapon.projectile_template_id = kBeamProjectileId;
        weapon.fire_action_template_id = kBeamActionId;
        tuning.definitions[kWeaponId5] = weapon;
        WeaponState& state = world.registry().get_or_emplace<WeaponState>(entity);
        state.weapon_slot_count = 1;
        state.weapon_ids[0] = kWeaponId5;
        state.ammo[0] = 60;
    }

    KernelPlayerInput held(bool press) const {
        KernelPlayerInput input{};
        input.input_seq = 1;
        input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
        input.selected_weapon = kWeaponId5;
        if (press) {
            input.action_intent = KernelActionIntent{
                8001u, KernelActionBinding_PrimaryFire, 0u, 0u};
        }
        input.action_input = KernelActionInput{8001u, 1u, 0u, 0u};
        return input;
    }

    void step(bool press = false) {
        std::vector<KernelEvent> events;
        simulate_weapons(world, {QueuedInput{kPlayerPeer, held(press)}}, tick, &events);
        ++tick;
    }

    World world;
    NetId player = 0;
    std::uint32_t tick = 0u;
};

void a_block_ends_a_held_beam() {
    // Both fire and keep holding the trigger; only one is then held.
    BeamFixture control;
    BeamFixture blocked;
    for (BeamFixture* f : {&control, &blocked}) {
        f->step(true);
        for (int i = 0; i < 8; ++i) {
            f->step();
        }
        require(live_beams(f->world) == 1u);
    }

    StatusEffectState& state = blocked.world.registry().get_or_emplace<StatusEffectState>(
        *blocked.world.find_entity(blocked.player));
    ActiveStatusEffect active;
    active.instance_id = 1u;
    active.status_effect_id = kBlockingStatusId;
    active.channel_id = 11u;
    active.expire_tick = 1000u;
    active.blocks_actions = true;
    state.active.push_back(active);

    for (int i = 0; i < 3; ++i) {
        control.step();
        blocked.step();
        require(live_beams(control.world) == 1u);
        require(live_beams(blocked.world) == 0u);
    }
    require(
        blocked.world.registry()
            .get<WeaponState>(*blocked.world.find_entity(blocked.player))
            .active_effect_net_id == 0u);
}

}  // namespace

int main() {
    a_status_without_the_action_blocks_nothing();
    the_block_stands_exactly_as_long_as_its_status();
    the_action_is_refused_outside_a_status_on_apply();
    a_block_during_the_windup_ends_it_without_swinging();
    a_new_action_is_refused_while_held_and_admitted_after();
    a_block_ends_a_held_beam();
    std::puts("status_action_block_test passed");
    return 0;
}
