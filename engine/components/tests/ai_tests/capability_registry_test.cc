#include "ai/capability_registry.h"

#include <cassert>

int main() {
    network_example::ai::CapabilityRegistry registry;
    registry.add_feature("hasVisibleEnemy");
    registry.add_node_type("Action.AttackTarget");
    registry.add_score_function("Score.AttackWhenHealthy");
    registry.add_query("Query.NearestEnemy");

    network_example::ai::ScenarioRequirements supported;
    supported.required_features = {"hasVisibleEnemy"};
    supported.required_nodes = {"Action.AttackTarget"};
    supported.required_scores = {"Score.AttackWhenHealthy"};
    supported.required_queries = {"Query.NearestEnemy"};
    auto supported_report = registry.validate(supported);
    assert(supported_report.supported());
    assert(supported_report.missing_features.empty());

    network_example::ai::ScenarioRequirements unsupported;
    unsupported.required_features = {"visibleEnemies", "enemyHp"};
    unsupported.required_nodes = {
        "Query.VisibleEnemies",
        "Selector.LowestHpEnemy",
        "Blackboard.SetTarget",
    };
    unsupported.required_scores = {"Score.LowestHpTarget"};
    auto report = registry.validate(unsupported);
    assert(!report.supported());
    assert(report.missing_features.size() == 2);
    assert(report.missing_nodes.size() == 3);
    assert(report.missing_scores.size() == 1);
    assert(!report.suggested_extensions.empty());

    return 0;
}
