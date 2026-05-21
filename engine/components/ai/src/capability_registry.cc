#include "ai/capability_registry.h"

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
           missing_scores.empty() && missing_queries.empty();
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

    add_missing_suggestions(
        report.missing_features, "Add feature: ", &report.suggested_extensions);
    add_missing_suggestions(
        report.missing_nodes, "Add node: ", &report.suggested_extensions);
    add_missing_suggestions(
        report.missing_scores, "Add score: ", &report.suggested_extensions);
    add_missing_suggestions(
        report.missing_queries, "Add query: ", &report.suggested_extensions);
    return report;
}

}  // namespace network_example::ai
