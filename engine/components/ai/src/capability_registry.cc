#include "capability_registry.h"

#include <utility>

namespace network_example::ai {
namespace {

bool contains(const std::unordered_set<std::string>& values,
              std::string_view name) {
    return values.find(std::string(name)) != values.end();
}

void add_missing_suggestions(const std::vector<std::string>& missing,
                             const char* prefix,
                             std::vector<std::string>* suggestions) {
    for (const std::string& value : missing) {
        suggestions->push_back(std::string(prefix) + value);
    }
}

}  // namespace

bool CapabilityReport::supported() const {
    return missing_features.empty() && missing_nodes.empty() &&
           missing_scores.empty() && missing_queries.empty() &&
           missing_gameplay_systems.empty() && missing_executors.empty() &&
           missing_data.empty() && missing_actions.empty();
}

void CapabilityRegistry::add_feature(std::string name) {
    features_.insert(std::move(name));
}

void CapabilityRegistry::add_node_type(std::string name) {
    node_types_.insert(std::move(name));
}

void CapabilityRegistry::add_score_function(std::string name) {
    score_functions_.insert(std::move(name));
}

void CapabilityRegistry::add_query(std::string name) {
    queries_.insert(std::move(name));
}

void CapabilityRegistry::add_gameplay_system(std::string name) {
    gameplay_systems_.insert(std::move(name));
}

void CapabilityRegistry::add_executor(std::string name) {
    executors_.insert(std::move(name));
}

void CapabilityRegistry::add_data(std::string name) {
    data_.insert(std::move(name));
}

void CapabilityRegistry::add_action(std::string name) {
    actions_.insert(std::move(name));
}

bool CapabilityRegistry::has_feature(std::string_view name) const {
    return contains(features_, name);
}

bool CapabilityRegistry::has_node_type(std::string_view name) const {
    return contains(node_types_, name);
}

bool CapabilityRegistry::has_score_function(std::string_view name) const {
    return contains(score_functions_, name);
}

bool CapabilityRegistry::has_query(std::string_view name) const {
    return contains(queries_, name);
}

bool CapabilityRegistry::has_gameplay_system(std::string_view name) const {
    return contains(gameplay_systems_, name);
}

bool CapabilityRegistry::has_executor(std::string_view name) const {
    return contains(executors_, name);
}

bool CapabilityRegistry::has_data(std::string_view name) const {
    return contains(data_, name);
}

bool CapabilityRegistry::has_action(std::string_view name) const {
    return contains(actions_, name);
}

CapabilityReport CapabilityRegistry::validate(
    const ScenarioRequirements& requirements) const {
    CapabilityReport report;
    for (const std::string& feature : requirements.required_features) {
        if (!has_feature(feature)) {
            report.missing_features.push_back(feature);
        }
    }
    for (const std::string& node : requirements.required_nodes) {
        if (!has_node_type(node)) {
            report.missing_nodes.push_back(node);
        }
    }
    for (const std::string& score : requirements.required_scores) {
        if (!has_score_function(score)) {
            report.missing_scores.push_back(score);
        }
    }
    for (const std::string& query : requirements.required_queries) {
        if (!has_query(query)) {
            report.missing_queries.push_back(query);
        }
    }
    for (const std::string& system : requirements.required_gameplay_systems) {
        if (!has_gameplay_system(system)) {
            report.missing_gameplay_systems.push_back(system);
        }
    }
    for (const std::string& executor : requirements.required_executors) {
        if (!has_executor(executor)) {
            report.missing_executors.push_back(executor);
        }
    }
    for (const std::string& data : requirements.required_data) {
        if (!has_data(data)) {
            report.missing_data.push_back(data);
        }
    }
    for (const std::string& action : requirements.required_actions) {
        if (!has_action(action)) {
            report.missing_actions.push_back(action);
        }
    }

    add_missing_suggestions(
        report.missing_features, "Add feature: ", &report.suggested_extensions);
    add_missing_suggestions(
        report.missing_nodes, "Add node: ", &report.suggested_extensions);
    add_missing_suggestions(
        report.missing_scores, "Add score: ", &report.suggested_extensions);
    add_missing_suggestions(
        report.missing_queries, "Add query: ", &report.suggested_extensions);
    add_missing_suggestions(
        report.missing_gameplay_systems,
        "Add gameplay system: ",
        &report.suggested_extensions);
    add_missing_suggestions(
        report.missing_executors, "Add executor: ", &report.suggested_extensions);
    add_missing_suggestions(
        report.missing_data, "Add data: ", &report.suggested_extensions);
    add_missing_suggestions(
        report.missing_actions, "Add action: ", &report.suggested_extensions);
    return report;
}

CapabilityRegistry make_default_capability_registry() {
    CapabilityRegistry registry;
    registry.add_feature("hp01");
    registry.add_feature("hasVisibleHostile");
    registry.add_feature("hasAmmo");
    registry.add_feature("isAtTarget");
    registry.add_feature("nearestHostileId");
    registry.add_feature("hostileDistance");
    registry.add_feature("allyNearbyCount");
    registry.add_feature("coverScore");
    registry.add_feature("dangerScore");

    registry.add_node_type("Composite.Selector");
    registry.add_node_type("Composite.Sequence");
    registry.add_node_type("Composite.UtilitySelector");
    registry.add_node_type("Condition.HasVisibleHostile");
    registry.add_node_type("Condition.HpAbove");
    registry.add_node_type("Condition.HpBelow");
    registry.add_node_type("Condition.HasAmmo");
    registry.add_node_type("Condition.IsAtTarget");
    registry.add_node_type("Action.Patrol");
    registry.add_node_type("Action.MoveTo");
    registry.add_node_type("Action.AttackTarget");
    registry.add_node_type("Action.FleeFromTarget");
    registry.add_node_type("Action.RequestHelp");
    registry.add_node_type("Action.Reload");
    registry.add_node_type("Action.StopMovement");

    registry.add_score_function("Score.AttackWhenHealthy");
    registry.add_score_function("Score.FleeWhenCriticalHp");
    registry.add_score_function("Score.RequestHelpWhenInjured");

    registry.add_query("Query.NearestHostile");
    registry.add_query("Query.VisibleHostiles");

    registry.add_gameplay_system("System.Vision");
    registry.add_gameplay_system("System.Weapon");
    registry.add_gameplay_system("System.Combat");

    registry.add_executor("Executor.ActorIntent");

    registry.add_data("Data.TargetPosition");
    registry.add_data("Data.WeaponStatus");

    registry.add_action("Action.AttackTarget");
    registry.add_action("Action.Reload");
    return registry;
}

}  // namespace network_example::ai
