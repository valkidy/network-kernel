#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_NODE_FACTORY_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_NODE_FACTORY_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ai/ai_tree.h"

namespace network_example::ai {

struct ScoreResult {
    bool valid = false;
    float value = 0.0f;

    static ScoreResult invalid();
    static ScoreResult valid_score(float score);
};

using ScoreFunction = std::function<ScoreResult(const AIContext&)>;
using NodeCreator = std::function<NodePtr()>;

struct UtilityCandidate {
    std::string name;
    ScoreFunction score;
    NodePtr node;
};

class NodeFactory {
public:
    void register_node_type(std::string type, NodeCreator creator);
    void register_score_function(std::string type, ScoreFunction score);

    bool has_node_type(const std::string& type) const;
    NodePtr create_node(const std::string& type) const;
    std::optional<ScoreFunction> score_function(const std::string& type) const;

private:
    std::unordered_map<std::string, NodeCreator> node_creators_;
    std::unordered_map<std::string, ScoreFunction> score_functions_;
};

NodePtr make_selector(std::vector<NodePtr> children);
NodePtr make_sequence(std::vector<NodePtr> children);
NodePtr make_utility_selector(std::vector<UtilityCandidate> candidates);

NodePtr make_condition_has_visible_enemy();
NodePtr make_condition_hp_above(float value);
NodePtr make_condition_hp_below(float value);
NodePtr make_condition_has_ammo();
NodePtr make_condition_is_at_target();

NodePtr make_action_patrol();
NodePtr make_action_move_to(std::string target_feature);
NodePtr make_action_attack_target(std::string target_feature);
NodePtr make_action_flee_from_target(std::string target_feature);
NodePtr make_action_request_help(std::string target_feature);
NodePtr make_action_stop_movement();

ScoreResult score_attack_when_healthy(const AIContext& context);
ScoreResult score_flee_when_critical_hp(const AIContext& context);
ScoreResult score_request_help_when_injured(const AIContext& context);

}  // namespace network_example::ai

#endif  // ENGINE_COMPONENTS_AI_INCLUDE_AI_NODE_FACTORY_H_
