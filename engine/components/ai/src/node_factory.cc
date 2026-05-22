#include "ai/node_factory.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace network_example::ai {
namespace {

void add_unique(std::vector<std::string>* values, std::string value) {
    if (std::find(values->begin(), values->end(), value) == values->end()) {
        values->push_back(std::move(value));
    }
}

void merge_unique(std::vector<std::string>* target,
                  const std::vector<std::string>& source) {
    for (const std::string& value : source) {
        add_unique(target, value);
    }
}

void merge_report(NodeBuildReport* target, const NodeBuildReport& source) {
    merge_unique(&target->errors, source.errors);
    merge_unique(&target->required_nodes, source.required_nodes);
    merge_unique(&target->required_scores, source.required_scores);
    merge_unique(&target->required_features, source.required_features);
    merge_unique(&target->missing_nodes, source.missing_nodes);
    merge_unique(&target->missing_scores, source.missing_scores);
    merge_unique(&target->missing_features, source.missing_features);
}

std::string param_or_default(const NodeConfig& config,
                             const std::string& name,
                             std::string fallback) {
    const auto iter = config.params.find(name);
    if (iter == config.params.end()) {
        return fallback;
    }
    return iter->second;
}

std::optional<float> float_param(const NodeConfig& config,
                                 const std::string& name,
                                 NodeBuildReport* report) {
    const auto iter = config.params.find(name);
    if (iter == config.params.end()) {
        add_unique(&report->errors, "missing scalar field: " + name);
        return std::nullopt;
    }
    try {
        return std::stof(iter->second);
    } catch (...) {
        add_unique(&report->errors, "invalid float field: " + name);
        return std::nullopt;
    }
}

class SelectorNode final : public AINode {
public:
    explicit SelectorNode(std::vector<NodePtr> children)
        : children_(std::move(children)) {}

    NodeStatus tick(const AIContext& context, AICommandBuffer* commands) override {
        std::optional<std::size_t> selected;
        NodeStatus selected_status = NodeStatus::kFailure;
        for (std::size_t index = 0; index < children_.size(); ++index) {
            const NodeStatus status = children_[index]->tick(context, commands);
            if (status == NodeStatus::kSuccess || status == NodeStatus::kRunning) {
                selected = index;
                selected_status = status;
                break;
            }
        }

        if (running_child_.has_value() &&
            (!selected.has_value() || *running_child_ != *selected)) {
            children_[*running_child_]->halt(context, commands);
        }

        if (selected_status == NodeStatus::kRunning && selected.has_value()) {
            running_child_ = selected;
        } else {
            running_child_.reset();
        }
        return selected_status;
    }

    void halt(const AIContext& context, AICommandBuffer* commands) override {
        if (running_child_.has_value()) {
            children_[*running_child_]->halt(context, commands);
            running_child_.reset();
        }
    }

private:
    std::vector<NodePtr> children_;
    std::optional<std::size_t> running_child_;
};

class SequenceNode final : public AINode {
public:
    explicit SequenceNode(std::vector<NodePtr> children)
        : children_(std::move(children)) {}

    NodeStatus tick(const AIContext& context, AICommandBuffer* commands) override {
        std::size_t index = running_child_.value_or(0);
        while (index < children_.size()) {
            const NodeStatus status = children_[index]->tick(context, commands);
            if (status == NodeStatus::kRunning) {
                running_child_ = index;
                return NodeStatus::kRunning;
            }
            if (status == NodeStatus::kFailure) {
                running_child_.reset();
                return NodeStatus::kFailure;
            }
            ++index;
        }
        running_child_.reset();
        return NodeStatus::kSuccess;
    }

    void halt(const AIContext& context, AICommandBuffer* commands) override {
        if (running_child_.has_value()) {
            children_[*running_child_]->halt(context, commands);
            running_child_.reset();
        }
    }

private:
    std::vector<NodePtr> children_;
    std::optional<std::size_t> running_child_;
};

class UtilitySelectorNode final : public AINode {
public:
    explicit UtilitySelectorNode(std::vector<UtilityCandidate> candidates)
        : candidates_(std::move(candidates)) {}

    NodeStatus tick(const AIContext& context, AICommandBuffer* commands) override {
        std::optional<std::size_t> selected;
        float best_score = 0.0f;
        for (std::size_t index = 0; index < candidates_.size(); ++index) {
            const ScoreResult score = candidates_[index].score(context);
            if (!score.valid) {
                continue;
            }
            if (!selected.has_value() || score.value > best_score) {
                selected = index;
                best_score = score.value;
            }
        }

        if (!selected.has_value()) {
            halt_previous(context, commands, std::nullopt);
            return NodeStatus::kFailure;
        }

        halt_previous(context, commands, selected);
        const NodeStatus status = candidates_[*selected].node->tick(context, commands);
        if (status == NodeStatus::kRunning) {
            running_child_ = selected;
        } else {
            running_child_.reset();
        }
        return status;
    }

    void halt(const AIContext& context, AICommandBuffer* commands) override {
        halt_previous(context, commands, std::nullopt);
    }

private:
    void halt_previous(const AIContext& context,
                       AICommandBuffer* commands,
                       std::optional<std::size_t> selected) {
        if (running_child_.has_value() &&
            (!selected.has_value() || *running_child_ != *selected)) {
            candidates_[*running_child_].node->halt(context, commands);
            running_child_.reset();
        }
    }

    std::vector<UtilityCandidate> candidates_;
    std::optional<std::size_t> running_child_;
};

class BoolConditionNode final : public AINode {
public:
    explicit BoolConditionNode(std::string feature) : feature_(std::move(feature)) {}

    NodeStatus tick(const AIContext& context, AICommandBuffer*) override {
        return context.get_bool(feature_).value_or(false) ? NodeStatus::kSuccess
                                                          : NodeStatus::kFailure;
    }

    void halt(const AIContext&, AICommandBuffer*) override {}

private:
    std::string feature_;
};

class FloatThresholdConditionNode final : public AINode {
public:
    FloatThresholdConditionNode(std::string feature, float threshold, bool above)
        : feature_(std::move(feature)), threshold_(threshold), above_(above) {}

    NodeStatus tick(const AIContext& context, AICommandBuffer*) override {
        const std::optional<float> value = context.get_float(feature_);
        if (!value.has_value()) {
            return NodeStatus::kFailure;
        }
        const bool passed = above_ ? *value > threshold_ : *value < threshold_;
        return passed ? NodeStatus::kSuccess : NodeStatus::kFailure;
    }

    void halt(const AIContext&, AICommandBuffer*) override {}

private:
    std::string feature_;
    float threshold_ = 0.0f;
    bool above_ = false;
};

class CommandActionNode final : public AINode {
public:
    CommandActionNode(std::string command_type,
                      std::string target_feature,
                      bool long_running)
        : command_type_(std::move(command_type)),
          target_feature_(std::move(target_feature)),
          long_running_(long_running) {}

    NodeStatus tick(const AIContext& context, AICommandBuffer* commands) override {
        if (long_running_ && context.get_bool("isAtTarget").value_or(false)) {
            running_ = false;
            return NodeStatus::kSuccess;
        }

        AICommand command;
        command.type = command_type_;
        if (!target_feature_.empty()) {
            const std::optional<std::uint32_t> target =
                context.get_uint32(target_feature_);
            if (!target.has_value()) {
                return NodeStatus::kFailure;
            }
            command.params["target"] = *target;
        }
        if (commands != nullptr) {
            commands->push(std::move(command));
        }
        running_ = long_running_;
        return long_running_ ? NodeStatus::kRunning : NodeStatus::kSuccess;
    }

    void halt(const AIContext&, AICommandBuffer* commands) override {
        if (!running_) {
            return;
        }
        running_ = false;
        if (commands != nullptr) {
            commands->push(AICommand{"StopMovement", {}});
        }
    }

private:
    std::string command_type_;
    std::string target_feature_;
    bool long_running_ = false;
    bool running_ = false;
};

ScoreResult score_from_hp(const AIContext& context,
                          float (*score_fn)(float)) {
    const std::optional<float> hp = context.get_float("hp01");
    if (!hp.has_value()) {
        return ScoreResult::invalid();
    }
    return ScoreResult::valid_score(std::clamp(score_fn(*hp), 0.0f, 1.0f));
}

}  // namespace

ScoreResult ScoreResult::invalid() {
    return ScoreResult{};
}

ScoreResult ScoreResult::valid_score(float score) {
    return ScoreResult{true, score};
}

UtilityChildConfig::UtilityChildConfig() = default;

UtilityChildConfig::UtilityChildConfig(std::string child_name,
                                       std::string score_function,
                                       NodeConfig child_node)
    : name(std::move(child_name)),
      score(std::move(score_function)),
      node(std::make_shared<NodeConfig>(std::move(child_node))) {}

NodeConfig::NodeConfig() = default;

NodeConfig::NodeConfig(std::string node_type) : type(std::move(node_type)) {}

NodeConfig::NodeConfig(
    std::string node_type,
    std::string node_name,
    std::unordered_map<std::string, std::string> node_params)
    : type(std::move(node_type)),
      name(std::move(node_name)),
      params(std::move(node_params)) {}

bool NodeBuildReport::supported() const {
    return errors.empty() && missing_nodes.empty() && missing_scores.empty() &&
           missing_features.empty();
}

bool NodeBuildResult::success() const {
    return node != nullptr && report.supported();
}

void NodeFactory::register_node_type(std::string type, NodeCreator creator) {
    node_creators_[std::move(type)] = std::move(creator);
}

void NodeFactory::register_node_type(std::string type) {
    node_creators_[std::move(type)] = nullptr;
}

void NodeFactory::register_score_function(std::string type, ScoreFunction score) {
    score_functions_[std::move(type)] = std::move(score);
}

bool NodeFactory::has_node_type(const std::string& type) const {
    return node_creators_.find(type) != node_creators_.end();
}

NodeBuildResult NodeFactory::create_node(const NodeConfig& config) const {
    NodeBuildResult result;
    add_unique(&result.report.required_nodes, config.type);

    const auto iter = node_creators_.find(config.type);
    if (iter == node_creators_.end()) {
        add_unique(&result.report.missing_nodes, config.type);
        return result;
    }

    if (config.type == "Composite.Selector" || config.type == "Composite.Sequence") {
        if (config.children.empty()) {
            add_unique(&result.report.errors,
                       config.type + " requires non-empty children");
            return result;
        }

        std::vector<NodePtr> children;
        for (const NodeConfig& child : config.children) {
            NodeBuildResult child_result = create_node(child);
            merge_report(&result.report, child_result.report);
            if (child_result.node != nullptr) {
                children.push_back(std::move(child_result.node));
            }
        }
        if (!result.report.supported() || children.empty()) {
            return result;
        }
        result.node = config.type == "Composite.Selector"
                          ? make_selector(std::move(children))
                          : make_sequence(std::move(children));
        return result;
    }

    if (config.type == "Composite.UtilitySelector") {
        if (config.utility_children.empty()) {
            add_unique(&result.report.errors,
                       "Composite.UtilitySelector requires non-empty children");
            return result;
        }

        std::vector<UtilityCandidate> candidates;
        for (const UtilityChildConfig& child : config.utility_children) {
            add_unique(&result.report.required_scores, child.score);
            const auto score = score_function(child.score);
            if (!score.has_value()) {
                add_unique(&result.report.missing_scores, child.score);
                continue;
            }
            if (child.node == nullptr) {
                add_unique(&result.report.errors,
                           "utility child requires map field: node");
                continue;
            }
            NodeBuildResult child_result = create_node(*child.node);
            merge_report(&result.report, child_result.report);
            if (child_result.node != nullptr) {
                candidates.push_back(UtilityCandidate{
                    child.name,
                    *score,
                    std::move(child_result.node),
                });
            }
        }
        if (!result.report.supported() || candidates.empty()) {
            return result;
        }
        result.node = make_utility_selector(std::move(candidates));
        return result;
    }

    if (config.type == "Condition.HasVisibleEnemy") {
        add_unique(&result.report.required_features, "hasVisibleEnemy");
    } else if (config.type == "Condition.HpAbove" ||
               config.type == "Condition.HpBelow") {
        add_unique(&result.report.required_features, "hp01");
    } else if (config.type == "Condition.HasAmmo") {
        add_unique(&result.report.required_features, "hasAmmo");
    } else if (config.type == "Condition.IsAtTarget") {
        add_unique(&result.report.required_features, "isAtTarget");
    } else if (config.type == "Action.MoveTo" ||
               config.type == "Action.AttackTarget" ||
               config.type == "Action.FleeFromTarget" ||
               config.type == "Action.RequestHelp") {
        add_unique(
            &result.report.required_features,
            param_or_default(config, "target", "nearestEnemyId"));
    }

    if (!iter->second) {
        add_unique(&result.report.missing_nodes, config.type);
        return result;
    }
    result.node = iter->second(config, *this);
    if (result.node == nullptr) {
        add_unique(&result.report.errors, "failed to create node: " + config.type);
    }
    return result;
}

NodePtr NodeFactory::create_node(const std::string& type) const {
    return create_node(NodeConfig{type}).node;
}

std::optional<ScoreFunction> NodeFactory::score_function(
    const std::string& type) const {
    const auto iter = score_functions_.find(type);
    if (iter == score_functions_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

NodeFactory make_default_node_factory() {
    NodeFactory factory;
    factory.register_node_type("Composite.Selector");
    factory.register_node_type("Composite.Sequence");
    factory.register_node_type("Composite.UtilitySelector");
    factory.register_node_type("Condition.HasVisibleEnemy", [](
        const NodeConfig&, const NodeFactory&) {
        return make_condition_has_visible_enemy();
    });
    factory.register_node_type("Condition.HpAbove", [](
        const NodeConfig& config, const NodeFactory&) {
        NodeBuildReport report;
        const std::optional<float> value = float_param(config, "value", &report);
        return value.has_value() ? make_condition_hp_above(*value) : nullptr;
    });
    factory.register_node_type("Condition.HpBelow", [](
        const NodeConfig& config, const NodeFactory&) {
        NodeBuildReport report;
        const std::optional<float> value = float_param(config, "value", &report);
        return value.has_value() ? make_condition_hp_below(*value) : nullptr;
    });
    factory.register_node_type("Condition.HasAmmo", [](
        const NodeConfig&, const NodeFactory&) {
        return make_condition_has_ammo();
    });
    factory.register_node_type("Condition.IsAtTarget", [](
        const NodeConfig&, const NodeFactory&) {
        return make_condition_is_at_target();
    });
    factory.register_node_type("Action.Patrol", [](
        const NodeConfig&, const NodeFactory&) {
        return make_action_patrol();
    });
    factory.register_node_type("Action.MoveTo", [](
        const NodeConfig& config, const NodeFactory&) {
        return make_action_move_to(
            param_or_default(config, "target", "nearestEnemyId"));
    });
    factory.register_node_type("Action.AttackTarget", [](
        const NodeConfig& config, const NodeFactory&) {
        return make_action_attack_target(
            param_or_default(config, "target", "nearestEnemyId"));
    });
    factory.register_node_type("Action.FleeFromTarget", [](
        const NodeConfig& config, const NodeFactory&) {
        return make_action_flee_from_target(
            param_or_default(config, "target", "nearestEnemyId"));
    });
    factory.register_node_type("Action.RequestHelp", [](
        const NodeConfig& config, const NodeFactory&) {
        return make_action_request_help(
            param_or_default(config, "target", "nearestEnemyId"));
    });
    factory.register_node_type("Action.Reload", [](
        const NodeConfig&, const NodeFactory&) {
        return make_action_reload();
    });
    factory.register_node_type("Action.StopMovement", [](
        const NodeConfig&, const NodeFactory&) {
        return make_action_stop_movement();
    });
    factory.register_score_function(
        "Score.AttackWhenHealthy",
        score_attack_when_healthy);
    factory.register_score_function(
        "Score.FleeWhenCriticalHp",
        score_flee_when_critical_hp);
    factory.register_score_function(
        "Score.RequestHelpWhenInjured",
        score_request_help_when_injured);
    return factory;
}

NodePtr make_selector(std::vector<NodePtr> children) {
    return std::make_unique<SelectorNode>(std::move(children));
}

NodePtr make_sequence(std::vector<NodePtr> children) {
    return std::make_unique<SequenceNode>(std::move(children));
}

NodePtr make_utility_selector(std::vector<UtilityCandidate> candidates) {
    return std::make_unique<UtilitySelectorNode>(std::move(candidates));
}

NodePtr make_condition_has_visible_enemy() {
    return std::make_unique<BoolConditionNode>("hasVisibleEnemy");
}

NodePtr make_condition_hp_above(float value) {
    return std::make_unique<FloatThresholdConditionNode>("hp01", value, true);
}

NodePtr make_condition_hp_below(float value) {
    return std::make_unique<FloatThresholdConditionNode>("hp01", value, false);
}

NodePtr make_condition_has_ammo() {
    return std::make_unique<BoolConditionNode>("hasAmmo");
}

NodePtr make_condition_is_at_target() {
    return std::make_unique<BoolConditionNode>("isAtTarget");
}

NodePtr make_action_patrol() {
    return std::make_unique<CommandActionNode>("Patrol", "", false);
}

NodePtr make_action_move_to(std::string target_feature) {
    return std::make_unique<CommandActionNode>(
        "MoveTo", std::move(target_feature), true);
}

NodePtr make_action_attack_target(std::string target_feature) {
    return std::make_unique<CommandActionNode>(
        "AttackTarget", std::move(target_feature), false);
}

NodePtr make_action_flee_from_target(std::string target_feature) {
    return std::make_unique<CommandActionNode>(
        "FleeFromTarget", std::move(target_feature), true);
}

NodePtr make_action_request_help(std::string target_feature) {
    return std::make_unique<CommandActionNode>(
        "RequestHelp", std::move(target_feature), false);
}

NodePtr make_action_reload() {
    return std::make_unique<CommandActionNode>("Reload", "", false);
}

NodePtr make_action_stop_movement() {
    return std::make_unique<CommandActionNode>("StopMovement", "", false);
}

ScoreResult score_attack_when_healthy(const AIContext& context) {
    return score_from_hp(context, [](float hp) { return hp; });
}

ScoreResult score_flee_when_critical_hp(const AIContext& context) {
    return score_from_hp(context, [](float hp) {
        return hp < 0.1f ? 1.0f : 0.0f;
    });
}

ScoreResult score_request_help_when_injured(const AIContext& context) {
    return score_from_hp(context, [](float hp) {
        return hp >= 0.1f && hp <= 0.5f ? 1.0f : 0.0f;
    });
}

}  // namespace network_example::ai
