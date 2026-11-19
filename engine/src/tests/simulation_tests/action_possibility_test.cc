// An action's possibility (ABI 110, docs/PLAYER_WEAPON_REVAMP_PLAN.md I1): an
// action authored at 30 runs for about 30 events in 100, the same events every
// time, and rolls separately for every target of one event. 0 and 100 always
// run. A spawn_projectile or lock_strike may not roll, and above 100 is
// refused.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <source_location>
#include <string>
#include <variant>
#include <vector>

#include "simulation/public/action_graph.h"
#include "simulation/public/deterministic_random.h"
#include "simulation/public/simulation.h"

namespace {

namespace ne = network_example;

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

KernelActionTriggerDefinition damage_trigger(std::uint8_t possibility_percent) {
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(KernelActionTriggerDefinition);
    trigger.action_count = 1;
    KernelActionDefinition& action = trigger.actions[0];
    action.action_type = KernelEntityTriggerActionType_ApplyDamage;
    action.target_source = KernelEntityRefSource_EventTarget;
    action.damage_amount = 10;
    action.possibility_percent = possibility_percent;
    return trigger;
}

std::optional<ne::CompiledActionGraphBinding> compile(
    const KernelActionTriggerDefinition& trigger) {
    return ne::compile_action_trigger_definition(
        ne::TriggerEventType::kCollision, trigger);
}

// Whether the one damage action ran for this target in this action instance.
bool ran(const ne::CompiledActionGraphBinding& binding,
         ne::NetId target,
         std::uint32_t action_instance_id) {
    ne::TriggerEvent event;
    event.type = ne::TriggerEventType::kCollision;
    event.subject = 1u;
    event.instigator = 7u;
    event.target = target;
    ne::ActionExecutionProvenance provenance;
    provenance.instigator = 7u;
    provenance.action_instance_id = action_instance_id;
    provenance.server_tick = 300u;
    std::vector<ne::ActionGraphCommand> commands;
    std::string error;
    require(ne::evaluate_action_graph(binding, 1u, event, provenance, &commands, &error));
    std::size_t damage = 0;
    for (const ne::ActionGraphCommand& command : commands) {
        if (std::holds_alternative<ne::ActionApplyDamageCommand>(command)) {
            ++damage;
        }
    }
    require(damage <= 1u);
    return damage == 1u;
}

}  // namespace

int main() {
    // The roll itself.
    require(ne::deterministic_roll_percent(12345u, 0u));
    require(ne::deterministic_roll_percent(12345u, 100u));
    require(ne::deterministic_roll_percent(0u, 1u));     // 0 % 100 < 1
    require(!ne::deterministic_roll_percent(99u, 99u));  // 99 % 100 == 99

    const std::optional<ne::CompiledActionGraphBinding> thirty = compile(damage_trigger(30));
    require(thirty.has_value());
    require(thirty->graph.possibility_percent.size() == thirty->graph.actions.size());

    // About 30 in 100 over many targets of one event, and the same ones again.
    constexpr int kTargets = 4000;
    int hits = 0;
    std::vector<bool> first;
    for (int index = 0; index < kTargets; ++index) {
        const bool hit = ran(*thirty, static_cast<ne::NetId>(100 + index), 55u);
        first.push_back(hit);
        hits += hit ? 1 : 0;
    }
    std::printf("possibility 30: %d of %d\n", hits, kTargets);
    // 30% of 4000 is 1200 with a standard deviation of ~29; six of them.
    require(hits > 1025 && hits < 1375);
    for (int index = 0; index < kTargets; ++index) {
        require(ran(*thirty, static_cast<ne::NetId>(100 + index), 55u) == first[index]);
    }

    // Another action instance rolls afresh for the same target.
    int differ = 0;
    for (int index = 0; index < kTargets; ++index) {
        if (ran(*thirty, static_cast<ne::NetId>(100 + index), 56u) != first[index]) ++differ;
    }
    require(differ > 0);

    // 0 (unauthored) and 100: always.
    for (const std::uint8_t always : {std::uint8_t{0}, std::uint8_t{100}}) {
        const std::optional<ne::CompiledActionGraphBinding> binding =
            compile(damage_trigger(always));
        require(binding.has_value());
        for (int index = 0; index < 200; ++index) {
            require(ran(*binding, static_cast<ne::NetId>(100 + index), 55u));
        }
    }

    // A graph built in code carries no possibilities and runs every action.
    {
        ne::CompiledActionGraphBinding binding = *compile(damage_trigger(0));
        binding.graph.possibility_percent.clear();
        require(ran(binding, 100u, 55u));
    }

    // Refused: above 100, and any roll on a spawn or a lock strike.
    require(!compile(damage_trigger(101)).has_value());
    {
        KernelActionTriggerDefinition spawn{};
        spawn.struct_size = sizeof(KernelActionTriggerDefinition);
        spawn.action_count = 1;
        spawn.actions[0].action_type = KernelEntityTriggerActionType_SpawnProjectile;
        spawn.actions[0].spawn_projectile_template_id = 9;
        spawn.actions[0].position_source = KernelEventVec3Source_Position;
        spawn.actions[0].direction_source = KernelEventVec3Source_Direction;
        require(compile(spawn).has_value());
        spawn.actions[0].possibility_percent = 50;
        require(!compile(spawn).has_value());
        spawn.actions[0].possibility_percent = 100;
        require(compile(spawn).has_value());
    }

    std::puts("action_possibility_test passed");
    return 0;
}
