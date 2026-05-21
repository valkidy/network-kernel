#include "ai/yaml_loader.h"

#include <algorithm>
#include <optional>
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

std::optional<float> optional_float(const YAML::Node& node,
                                    const char* key,
                                    YamlLoadResult* result) {
    const YAML::Node field = node[key];
    if (!field || !field.IsScalar()) {
        result->errors.push_back(std::string("missing scalar field: ") + key);
        return std::nullopt;
    }
    try {
        return field.as<float>();
    } catch (const YAML::Exception& error) {
        result->errors.push_back(
            std::string("invalid float field: ") + key + ": " + error.what());
        return std::nullopt;
    }
}

std::string target_feature_or_default(const YAML::Node& node) {
    const YAML::Node target = node["target"];
    if (target && target.IsScalar()) {
        return target.as<std::string>();
    }
    return "nearestEnemyId";
}

ScoreFunction score_for_type(const std::string& type, YamlLoadResult* result) {
    add_unique(&result->required_scores, type);
    if (type == "Score.AttackWhenHealthy") {
        return score_attack_when_healthy;
    }
    if (type == "Score.FleeWhenCriticalHp") {
        return score_flee_when_critical_hp;
    }
    if (type == "Score.RequestHelpWhenInjured") {
        return score_request_help_when_injured;
    }
    add_unique(&result->missing_scores, type);
    return {};
}

NodePtr build_node(const YAML::Node& node, YamlLoadResult* result);

NodePtr build_children_composite(const YAML::Node& node,
                                 const std::string& type,
                                 YamlLoadResult* result) {
    const YAML::Node children = node["children"];
    if (!children || !children.IsSequence() || children.size() == 0) {
        result->errors.push_back(type + " requires non-empty children");
        return nullptr;
    }

    std::vector<NodePtr> child_nodes;
    for (const YAML::Node& child : children) {
        NodePtr built = build_node(child, result);
        if (built != nullptr) {
            child_nodes.push_back(std::move(built));
        }
    }

    if (!result->errors.empty() || !result->missing_nodes.empty() ||
        child_nodes.empty()) {
        return nullptr;
    }

    if (type == "Composite.Selector") {
        return make_selector(std::move(child_nodes));
    }
    return make_sequence(std::move(child_nodes));
}

NodePtr build_utility_selector(const YAML::Node& node, YamlLoadResult* result) {
    const YAML::Node children = node["children"];
    if (!children || !children.IsSequence() || children.size() == 0) {
        result->errors.push_back(
            "Composite.UtilitySelector requires non-empty children");
        return nullptr;
    }

    std::vector<UtilityCandidate> candidates;
    for (const YAML::Node& child : children) {
        std::string name;
        if (!expect_scalar(child, "name", &name, result)) {
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
        ScoreFunction score = score_for_type(score_type, result);
        NodePtr built = build_node(child_node, result);
        if (score && built != nullptr) {
            candidates.push_back(UtilityCandidate{
                std::move(name),
                std::move(score),
                std::move(built),
            });
        }
    }

    if (!result->errors.empty() || !result->missing_nodes.empty() ||
        !result->missing_scores.empty() || candidates.empty()) {
        return nullptr;
    }
    return make_utility_selector(std::move(candidates));
}

NodePtr build_condition(const YAML::Node& node,
                        const std::string& type,
                        YamlLoadResult* result) {
    if (type == "Condition.HasVisibleEnemy") {
        add_unique(&result->required_features, "hasVisibleEnemy");
        return make_condition_has_visible_enemy();
    }
    if (type == "Condition.HpAbove") {
        add_unique(&result->required_features, "hp01");
        const std::optional<float> value = optional_float(node, "value", result);
        return value.has_value() ? make_condition_hp_above(*value) : nullptr;
    }
    if (type == "Condition.HpBelow") {
        add_unique(&result->required_features, "hp01");
        const std::optional<float> value = optional_float(node, "value", result);
        return value.has_value() ? make_condition_hp_below(*value) : nullptr;
    }
    if (type == "Condition.HasAmmo") {
        add_unique(&result->required_features, "hasAmmo");
        return make_condition_has_ammo();
    }
    if (type == "Condition.IsAtTarget") {
        add_unique(&result->required_features, "isAtTarget");
        return make_condition_is_at_target();
    }
    return nullptr;
}

NodePtr build_action(const YAML::Node& node,
                     const std::string& type,
                     YamlLoadResult* result) {
    if (type == "Action.Patrol") {
        return make_action_patrol();
    }
    if (type == "Action.StopMovement") {
        return make_action_stop_movement();
    }

    const std::string target_feature = target_feature_or_default(node);
    add_unique(&result->required_features, target_feature);
    if (type == "Action.MoveTo") {
        return make_action_move_to(target_feature);
    }
    if (type == "Action.AttackTarget") {
        return make_action_attack_target(target_feature);
    }
    if (type == "Action.FleeFromTarget") {
        return make_action_flee_from_target(target_feature);
    }
    if (type == "Action.RequestHelp") {
        return make_action_request_help(target_feature);
    }
    return nullptr;
}

NodePtr build_node(const YAML::Node& node, YamlLoadResult* result) {
    if (!node || !node.IsMap()) {
        result->errors.push_back("node must be a map");
        return nullptr;
    }

    std::string type;
    if (!expect_scalar(node, "type", &type, result)) {
        return nullptr;
    }
    add_unique(&result->required_nodes, type);

    if (type == "Composite.Selector" || type == "Composite.Sequence") {
        return build_children_composite(node, type, result);
    }
    if (type == "Composite.UtilitySelector") {
        return build_utility_selector(node, result);
    }

    NodePtr condition = build_condition(node, type, result);
    if (condition != nullptr) {
        return condition;
    }
    NodePtr action = build_action(node, type, result);
    if (action != nullptr) {
        return action;
    }

    add_unique(&result->missing_nodes, type);
    return nullptr;
}

}  // namespace

YamlLoadResult load_tree_from_yaml(const std::string& yaml) {
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

    result.root = build_node(root, &result);
    if (!result.errors.empty() || !result.missing_nodes.empty() ||
        !result.missing_scores.empty() || !result.missing_features.empty()) {
        result.root.reset();
    }
    return result;
}

}  // namespace network_example::ai
