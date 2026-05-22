#include "ai/node_factory.h"

#include <cassert>
#include <cstdint>

int main() {
    network_example::ai::NodeFactory factory;
    factory.register_node_type("Action.Patrol", [](
        const network_example::ai::NodeConfig&,
        const network_example::ai::NodeFactory&) {
        return network_example::ai::make_action_patrol();
    });
    factory.register_score_function(
        "Score.AttackWhenHealthy",
        network_example::ai::score_attack_when_healthy);

    assert(factory.has_node_type("Action.Patrol"));
    assert(!factory.has_node_type("Action.Unknown"));
    network_example::ai::NodeBuildResult patrol =
        factory.create_node(network_example::ai::NodeConfig{"Action.Patrol"});
    assert(patrol.success());
    assert(patrol.node != nullptr);
    assert(factory.create_node(
               network_example::ai::NodeConfig{"Action.Unknown"}).node == nullptr);

    auto score = factory.score_function("Score.AttackWhenHealthy");
    assert(score.has_value());
    assert(!factory.score_function("Score.Unknown").has_value());

    network_example::ai::NodeFactory defaults =
        network_example::ai::make_default_node_factory();
    network_example::ai::NodeConfig combat;
    combat.type = "Composite.UtilitySelector";
    combat.utility_children.push_back(network_example::ai::UtilityChildConfig{
        "Attack",
        "Score.AttackWhenHealthy",
        network_example::ai::NodeConfig{"Action.AttackTarget",
                                        "",
                                        {{"target", "nearestEnemyId"}}},
    });
    combat.utility_children.push_back(network_example::ai::UtilityChildConfig{
        "Flee",
        "Score.FleeWhenCriticalHp",
        network_example::ai::NodeConfig{"Action.FleeFromTarget",
                                        "",
                                        {{"target", "nearestEnemyId"}}},
    });

    network_example::ai::NodeBuildResult built = defaults.create_node(combat);
    assert(built.success());
    assert(built.node != nullptr);
    assert(built.report.required_nodes.size() == 3);
    assert(built.report.required_scores.size() == 2);
    assert(built.report.required_features.size() == 2);

    network_example::ai::AIContext context;
    network_example::ai::AICommandBuffer commands;
    context.set_feature("hp01", 0.05f);
    context.set_feature("nearestEnemyId", static_cast<std::uint32_t>(7));
    assert(built.node->tick(context, &commands) ==
           network_example::ai::NodeStatus::kRunning);
    assert(commands.size() == 1);
    assert(commands.commands()[0].type == "FleeFromTarget");

    return 0;
}
