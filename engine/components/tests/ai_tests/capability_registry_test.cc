#include "capability_registry.h"

#include <cassert>

int main() {
    network_example::ai::CapabilityRegistry registry;
    registry.add_feature("hasVisibleEnemy");
    registry.add_node_type("Action.AttackTarget");
    registry.add_score_function("Score.AttackWhenHealthy");
    registry.add_query("Query.NearestEnemy");
    registry.add_gameplay_system("System.Weapon");
    registry.add_executor("Executor.ActorIntent");
    registry.add_data("Data.WeaponStatus");
    registry.add_action("Action.Reload");

    network_example::ai::ScenarioRequirements supported;
    supported.required_features = {"hasVisibleEnemy"};
    supported.required_nodes = {"Action.AttackTarget"};
    supported.required_scores = {"Score.AttackWhenHealthy"};
    supported.required_queries = {"Query.NearestEnemy"};
    supported.required_gameplay_systems = {"System.Weapon"};
    supported.required_executors = {"Executor.ActorIntent"};
    supported.required_data = {"Data.WeaponStatus"};
    supported.required_actions = {"Action.Reload"};
    auto supported_report = registry.validate(supported);
    assert(supported_report.supported());
    assert(supported_report.missing_features.empty());
    assert(supported_report.missing_gameplay_systems.empty());
    assert(supported_report.missing_executors.empty());
    assert(supported_report.missing_data.empty());
    assert(supported_report.missing_actions.empty());

    network_example::ai::ScenarioRequirements unsupported;
    unsupported.required_features = {"visibleEnemies", "enemyHp"};
    unsupported.required_nodes = {
        "Query.VisibleEnemies",
        "Selector.LowestHpEnemy",
        "Blackboard.SetTarget",
    };
    unsupported.required_scores = {"Score.LowestHpTarget"};
    unsupported.required_queries = {"Query.FindCover"};
    unsupported.required_gameplay_systems = {"System.Cover"};
    unsupported.required_executors = {"Executor.DirectorIntent"};
    unsupported.required_data = {"Data.CoverSlots"};
    unsupported.required_actions = {"Action.FlankTarget"};
    auto report = registry.validate(unsupported);
    assert(!report.supported());
    assert(report.missing_features.size() == 2);
    assert(report.missing_nodes.size() == 3);
    assert(report.missing_scores.size() == 1);
    assert(report.missing_queries.size() == 1);
    assert(report.missing_gameplay_systems.size() == 1);
    assert(report.missing_executors.size() == 1);
    assert(report.missing_data.size() == 1);
    assert(report.missing_actions.size() == 1);
    assert(!report.suggested_extensions.empty());

    return 0;
}
