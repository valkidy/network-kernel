#include "ai/yaml_loader.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "ai/node_factory.h"

namespace network_example::ai {
namespace {

void add_unique(std::vector<std::string>* values, std::string value) {
    if (std::find(values->begin(), values->end(), value) == values->end()) {
        values->push_back(std::move(value));
    }
}

bool expect_scalar(const YAML::Node& node,
                   const char* key,
                   std::string* value,
                   YamlLoadResult* result) {
    const YAML::Node field = node[key];
    if (!field || !field.IsScalar()) {
        result->errors.push_back(std::string("missing scalar field: ") + key);
        return false;
    }
    *value = field.as<std::string>();
    return true;
}

void finalize_result(YamlLoadResult* result) {
    if (!result->errors.empty() || !result->missing_nodes.empty() ||
        !result->missing_scores.empty() || !result->missing_features.empty()) {
        result->root.reset();
    }
}

NodeConfig parse_node_config(const YAML::Node& node, YamlLoadResult* result) {
    NodeConfig config;
    if (!node || !node.IsMap()) {
        result->errors.push_back("node must be a map");
        return config;
    }

    if (!expect_scalar(node, "type", &config.type, result)) {
        return config;
    }
    const YAML::Node name = node["name"];
    if (name && name.IsScalar()) {
        config.name = name.as<std::string>();
    }

    for (const std::string& key : {"target", "value"}) {
        const YAML::Node field = node[key];
        if (field && field.IsScalar()) {
            config.params[key] = field.as<std::string>();
        }
    }

    const YAML::Node children = node["children"];
    if (children && children.IsSequence()) {
        if (config.type == "Composite.UtilitySelector") {
            for (const YAML::Node& child : children) {
                std::string child_name;
                if (!expect_scalar(child, "name", &child_name, result)) {
                    continue;
                }
                std::string score_type;
                if (!expect_scalar(child, "score", &score_type, result)) {
                    continue;
                }
                const YAML::Node child_node = child["node"];
                if (!child_node || !child_node.IsMap()) {
                    result->errors.push_back("utility child requires map field: node");
                    continue;
                }
                config.utility_children.push_back(UtilityChildConfig{
                    std::move(child_name),
                    std::move(score_type),
                    parse_node_config(child_node, result),
                });
            }
        } else {
            for (const YAML::Node& child : children) {
                config.children.push_back(parse_node_config(child, result));
            }
        }
    }
    return config;
}

void copy_build_report(const NodeBuildReport& report, YamlLoadResult* result) {
    for (const std::string& value : report.errors) {
        add_unique(&result->errors, value);
    }
    for (const std::string& value : report.required_nodes) {
        add_unique(&result->required_nodes, value);
    }
    for (const std::string& value : report.required_scores) {
        add_unique(&result->required_scores, value);
    }
    for (const std::string& value : report.required_features) {
        add_unique(&result->required_features, value);
    }
    for (const std::string& value : report.missing_nodes) {
        add_unique(&result->missing_nodes, value);
    }
    for (const std::string& value : report.missing_scores) {
        add_unique(&result->missing_scores, value);
    }
    for (const std::string& value : report.missing_features) {
        add_unique(&result->missing_features, value);
    }
}

}  // namespace

YamlLoadResult load_tree_from_yaml(const std::string& yaml,
                                   const CapabilityRegistry& registry) {
    YamlLoadResult result;
    YAML::Node document;
    try {
        document = YAML::Load(yaml);
    } catch (const YAML::Exception& error) {
        result.errors.push_back(std::string("invalid yaml: ") + error.what());
        return result;
    }

    if (!document || !document.IsMap()) {
        result.errors.push_back("document must be a map");
        return result;
    }
    if (!document["tree"] || !document["tree"].IsScalar()) {
        result.errors.push_back("missing scalar field: tree");
    }
    const YAML::Node root = document["root"];
    if (!root || !root.IsMap()) {
        result.errors.push_back("missing map field: root");
        return result;
    }

    NodeConfig root_config = parse_node_config(root, &result);
    NodeBuildResult built = make_default_node_factory().create_node(root_config);
    result.root = std::move(built.node);
    copy_build_report(built.report, &result);
    CapabilityReport report = registry.validate(ScenarioRequirements{
        result.required_features,
        result.required_nodes,
        result.required_scores,
        {},
    });
    for (const std::string& feature : report.missing_features) {
        add_unique(&result.missing_features, feature);
    }
    for (const std::string& node : report.missing_nodes) {
        add_unique(&result.missing_nodes, node);
    }
    for (const std::string& score : report.missing_scores) {
        add_unique(&result.missing_scores, score);
    }
    finalize_result(&result);
    return result;
}

YamlLoadResult load_tree_from_yaml(const std::string& yaml) {
    return load_tree_from_yaml(yaml, make_default_capability_registry());
}

CapabilityReport validate_yaml_capabilities(
    const std::string& yaml,
    const CapabilityRegistry& registry) {
    YamlLoadResult result = load_tree_from_yaml(yaml);
    ScenarioRequirements requirements;
    requirements.required_features = result.required_features;
    requirements.required_nodes = result.required_nodes;
    requirements.required_scores = result.required_scores;
    CapabilityReport report = registry.validate(requirements);
    for (const std::string& node : result.missing_nodes) {
        add_unique(&report.missing_nodes, node);
    }
    for (const std::string& score : result.missing_scores) {
        add_unique(&report.missing_scores, score);
    }
    for (const std::string& feature : result.missing_features) {
        add_unique(&report.missing_features, feature);
    }
    return report;
}

}  // namespace network_example::ai
