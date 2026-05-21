#include "ai/scenario_analysis.h"

#include <cassert>

int main() {
    network_example::ai::ScenarioAnalysisResult analysis;
    analysis.actor = "EnemySoldier";
    analysis.features = {"hasVisibleEnemy", "hp01"};
    analysis.actions = {"Patrol", "AttackTarget", "FleeFromTarget"};
    analysis.rules = {
        "if no visible enemy -> Patrol",
        "if visible enemy and hp > 0.5 -> Attack",
    };
    analysis.requirements.required_features = analysis.features;
    analysis.requirements.required_nodes = {
        "Condition.HasVisibleEnemy",
        "Action.AttackTarget",
    };

    assert(analysis.actor == "EnemySoldier");
    assert(analysis.requirements.required_features.size() == 2);
    assert(analysis.requirements.required_nodes[1] == "Action.AttackTarget");

    network_example::ai::YamlGenerationResult result;
    result.status = network_example::ai::YamlGenerationStatus::kUnsupported;
    result.report.missing_features = {"visibleEnemies", "enemyHp"};
    result.report.suggested_extensions = {
        "Add perception output: visibleEnemies[]",
    };

    assert(!result.success());
    assert(result.report.missing_features.size() == 2);
    assert(result.report.suggested_extensions[0].find("visibleEnemies") !=
           std::string::npos);

    return 0;
}
