#include "scenario_analysis.h"

#include <cassert>
#include <string>

int main() {
    network_example::ai::ScenarioAnalysisResult analysis;
    analysis.actor = "HostileSoldier";
    analysis.features = {"hasVisibleHostile", "hp01"};
    analysis.actions = {"Patrol", "AttackTarget", "FleeFromTarget"};
    analysis.rules = {
        "if no visible hostile -> Patrol",
        "if visible hostile and hp > 0.5 -> Attack",
    };
    analysis.requirements.required_features = analysis.features;
    analysis.requirements.required_nodes = {
        "Condition.HasVisibleHostile",
        "Action.AttackTarget",
    };

    assert(analysis.actor == "HostileSoldier");
    assert(analysis.requirements.required_features.size() == 2);
    assert(analysis.requirements.required_nodes[1] == "Action.AttackTarget");

    network_example::ai::YamlGenerationResult result;
    result.status = network_example::ai::YamlGenerationStatus::kUnsupported;
    result.report.missing_features = {"visibleHostiles", "hostileHp"};
    result.report.suggested_extensions = {
        "Add perception output: visibleHostiles[]",
    };

    assert(!result.success());
    assert(result.report.missing_features.size() == 2);
    assert(result.report.suggested_extensions[0].find("visibleHostiles") !=
           std::string::npos);

    return 0;
}
