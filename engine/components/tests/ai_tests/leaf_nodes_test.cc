#include "ai/ai_context.h"
#include "ai/ai_node.h"
#include "ai/node_factory.h"

#include <cassert>
#include <cstdint>

int main() {
    using network_example::ai::NodeStatus;

    network_example::ai::AIContext context;
    network_example::ai::AICommandBuffer commands;

    auto visible = network_example::ai::make_condition_has_visible_enemy();
    assert(visible->tick(context, &commands) == NodeStatus::kFailure);
    context.set_feature("hasVisibleEnemy", true);
    assert(visible->tick(context, &commands) == NodeStatus::kSuccess);

    auto hp_above = network_example::ai::make_condition_hp_above(0.5f);
    auto hp_below = network_example::ai::make_condition_hp_below(0.25f);
    context.set_feature("hp01", 0.75f);
    assert(hp_above->tick(context, &commands) == NodeStatus::kSuccess);
    assert(hp_below->tick(context, &commands) == NodeStatus::kFailure);

    auto attack = network_example::ai::make_action_attack_target("nearestEnemyId");
    context.set_feature("nearestEnemyId", static_cast<std::uint32_t>(88));
    assert(attack->tick(context, &commands) == NodeStatus::kSuccess);
    assert(commands.commands().back().type == "AttackTarget");
    assert(std::get<std::uint32_t>(
               commands.commands().back().params.at("target")) == 88);

    commands.clear();
    auto move = network_example::ai::make_action_move_to("nearestEnemyId");
    context.set_feature("isAtTarget", false);
    assert(move->tick(context, &commands) == NodeStatus::kRunning);
    assert(commands.commands().back().type == "MoveTo");
    move->halt(context, &commands);
    assert(commands.commands().back().type == "StopMovement");

    commands.clear();
    context.set_feature("isAtTarget", true);
    assert(move->tick(context, &commands) == NodeStatus::kSuccess);
    assert(commands.empty());

    context.set_feature("hp01", 0.9f);
    assert(network_example::ai::score_attack_when_healthy(context).valid);
    assert(network_example::ai::score_attack_when_healthy(context).value > 0.8f);
    context.set_feature("hp01", 0.05f);
    assert(network_example::ai::score_flee_when_critical_hp(context).valid);
    assert(network_example::ai::score_flee_when_critical_hp(context).value > 0.9f);
    network_example::ai::AIContext missing;
    assert(!network_example::ai::score_request_help_when_injured(missing).valid);

    return 0;
}
