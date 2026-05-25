#include "ai/ai_context.h"
#include "ai/ai_tree.h"
#include "ai/node_factory.h"
#include "ai/yaml_loader.h"

#include <cassert>
#include <cstdint>
#include <vector>
#include <string>

namespace {

const char* valid_tree_yaml() {
    return R"yaml(
tree: EnemySoldierAI
root:
  type: Composite.Selector
  children:
    - type: Composite.Sequence
      name: Combat
      children:
        - type: Condition.HasVisibleEnemy
        - type: Composite.UtilitySelector
          name: CombatDecision
          children:
            - name: Attack
              score: Score.AttackWhenHealthy
              node:
                type: Composite.Selector
                children:
                  - type: Composite.Sequence
                    name: FireWithAmmo
                    children:
                      - type: Condition.HasAmmo
                      - type: Action.AttackTarget
                        target: nearestEnemyId
                  - type: Action.Reload
            - name: Flee
              score: Score.FleeWhenCriticalHp
              node:
                type: Action.FleeFromTarget
                target: nearestEnemyId
            - name: Hold
              score: Score.RequestHelpWhenInjured
              node:
                type: Action.StopMovement
    - type: Action.Patrol
)yaml";
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    for (const std::string& candidate : values) {
        if (candidate == value) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    network_example::ai::CapabilityRegistry defaults =
        network_example::ai::make_default_capability_registry();
    assert(defaults.has_node_type("Composite.Selector"));
    assert(defaults.has_node_type("Action.Reload"));
    assert(defaults.has_node_type("Action.StopMovement"));
    assert(defaults.has_feature("hasAmmo"));
    assert(defaults.has_feature("nearestEnemyId"));

    network_example::ai::NodeFactory factory =
        network_example::ai::make_default_node_factory();
    assert(factory.has_node_type("Action.Patrol"));
    assert(factory.score_function("Score.AttackWhenHealthy").has_value());

    network_example::ai::YamlLoadResult result =
        network_example::ai::load_tree_from_yaml(valid_tree_yaml());
    assert(result.success());
    assert(result.root != nullptr);
    assert(result.required_nodes.size() == 10);
    assert(result.required_scores.size() == 3);
    assert(contains(result.required_nodes, "Condition.HasAmmo"));
    assert(contains(result.required_nodes, "Action.Reload"));
    assert(contains(result.required_nodes, "Action.StopMovement"));
    assert(contains(result.required_features, "hasVisibleEnemy"));
    assert(contains(result.required_features, "hp01"));
    assert(contains(result.required_features, "nearestEnemyId"));
    assert(contains(result.required_features, "hasAmmo"));

    network_example::ai::AITreeInstance tree(std::move(result.root));
    network_example::ai::AIContext context;
    network_example::ai::AICommandBuffer commands;
    context.set_feature("hasVisibleEnemy", true);
    context.set_feature("hp01", 0.9f);
    context.set_feature("hasAmmo", true);
    context.set_feature("nearestEnemyId", static_cast<std::uint32_t>(42));
    assert(tree.tick(context, &commands) ==
           network_example::ai::NodeStatus::kSuccess);
    assert(commands.size() == 1);
    assert(commands.commands()[0].type == "AttackTarget");

    network_example::ai::AICommandBuffer reload_commands;
    network_example::ai::YamlLoadResult reload_result =
        network_example::ai::load_tree_from_yaml(valid_tree_yaml());
    network_example::ai::AITreeInstance reload_tree(std::move(reload_result.root));
    network_example::ai::AIContext reload_context;
    reload_context.set_feature("hasVisibleEnemy", true);
    reload_context.set_feature("hp01", 0.9f);
    reload_context.set_feature("hasAmmo", false);
    reload_context.set_feature("nearestEnemyId", static_cast<std::uint32_t>(5));
    assert(reload_tree.tick(reload_context, &reload_commands) ==
           network_example::ai::NodeStatus::kSuccess);
    assert(reload_commands.size() == 1);
    assert(reload_commands.commands()[0].type == "Reload");

    network_example::ai::AICommandBuffer hold_commands;
    network_example::ai::YamlLoadResult hold_result =
        network_example::ai::load_tree_from_yaml(valid_tree_yaml());
    network_example::ai::AITreeInstance hold_tree(std::move(hold_result.root));
    network_example::ai::AIContext hold_context;
    hold_context.set_feature("hasVisibleEnemy", true);
    hold_context.set_feature("hp01", 0.35f);
    hold_context.set_feature("hasAmmo", true);
    hold_context.set_feature("nearestEnemyId", static_cast<std::uint32_t>(5));
    assert(hold_tree.tick(hold_context, &hold_commands) ==
           network_example::ai::NodeStatus::kSuccess);
    assert(hold_commands.size() == 1);
    assert(hold_commands.commands()[0].type == "StopMovement");

    network_example::ai::YamlLoadResult unknown_node =
        network_example::ai::load_tree_from_yaml(R"yaml(
tree: Bad
root:
  type: Action.DoesNotExist
)yaml");
    assert(!unknown_node.success());
    assert(unknown_node.root == nullptr);
    assert(unknown_node.missing_nodes.size() == 1);
    assert(unknown_node.missing_nodes[0] == "Action.DoesNotExist");

    network_example::ai::YamlLoadResult unknown_score =
        network_example::ai::load_tree_from_yaml(R"yaml(
tree: BadScore
root:
  type: Composite.UtilitySelector
  children:
    - name: Broken
      score: Score.DoesNotExist
      node:
        type: Action.Patrol
)yaml");
    assert(!unknown_score.success());
    assert(unknown_score.root == nullptr);
    assert(unknown_score.missing_scores.size() == 1);

    network_example::ai::YamlLoadResult bad_shape =
        network_example::ai::load_tree_from_yaml(R"yaml(
tree: BadShape
root:
  type: Composite.Sequence
)yaml");
    assert(!bad_shape.success());
    assert(bad_shape.root == nullptr);
    assert(!bad_shape.errors.empty());

    network_example::ai::YamlLoadResult missing_tree =
        network_example::ai::load_tree_from_yaml(R"yaml(
root:
  type: Action.Patrol
)yaml");
    assert(!missing_tree.success());
    assert(missing_tree.root == nullptr);
    assert(!missing_tree.errors.empty());

    network_example::ai::YamlLoadResult missing_root =
        network_example::ai::load_tree_from_yaml(R"yaml(
tree: MissingRoot
)yaml");
    assert(!missing_root.success());
    assert(missing_root.root == nullptr);
    assert(!missing_root.errors.empty());

    network_example::ai::YamlLoadResult invalid_root =
        network_example::ai::load_tree_from_yaml(R"yaml(
tree: InvalidRoot
root: Action.Patrol
)yaml");
    assert(!invalid_root.success());
    assert(invalid_root.root == nullptr);
    assert(!invalid_root.errors.empty());

    network_example::ai::YamlLoadResult utility_missing_score =
        network_example::ai::load_tree_from_yaml(R"yaml(
tree: MissingScore
root:
  type: Composite.UtilitySelector
  children:
    - name: Attack
      node:
        type: Action.AttackTarget
        target: nearestEnemyId
)yaml");
    assert(!utility_missing_score.success());
    assert(utility_missing_score.root == nullptr);
    assert(!utility_missing_score.errors.empty());

    network_example::ai::YamlLoadResult utility_missing_node =
        network_example::ai::load_tree_from_yaml(R"yaml(
tree: MissingNode
root:
  type: Composite.UtilitySelector
  children:
    - name: Attack
      score: Score.AttackWhenHealthy
)yaml");
    assert(!utility_missing_node.success());
    assert(utility_missing_node.root == nullptr);
    assert(!utility_missing_node.errors.empty());

    network_example::ai::YamlLoadResult invalid_condition_value =
        network_example::ai::load_tree_from_yaml(R"yaml(
tree: InvalidCondition
root:
  type: Condition.HpAbove
  value: healthy
)yaml");
    assert(!invalid_condition_value.success());
    assert(invalid_condition_value.root == nullptr);
    assert(!invalid_condition_value.errors.empty());

    network_example::ai::CapabilityRegistry custom_registry;
    custom_registry.add_node_type("Action.AttackTarget");
    custom_registry.add_feature("hasVisibleEnemy");
    network_example::ai::CapabilityReport capability_report =
        network_example::ai::validate_yaml_capabilities(R"yaml(
tree: UnsupportedTarget
root:
  type: Action.AttackTarget
  target: nearestEnemyId
)yaml",
                                                        custom_registry);
    assert(!capability_report.supported());
    assert(capability_report.missing_features.size() == 1);
    assert(capability_report.missing_features[0] == "nearestEnemyId");

    network_example::ai::CapabilityRegistry extended_registry =
        network_example::ai::make_default_capability_registry();
    extended_registry.add_node_type("Action.CustomAbility");
    network_example::ai::CapabilityReport extended_report =
        network_example::ai::validate_yaml_capabilities(R"yaml(
tree: CustomAbilityTree
root:
  type: Action.CustomAbility
)yaml",
                                                        extended_registry);
    assert(extended_report.supported());

    network_example::ai::YamlLoadResult custom_load =
        network_example::ai::load_tree_from_yaml(R"yaml(
tree: UnsupportedTarget
root:
  type: Action.AttackTarget
  target: nearestEnemyId
)yaml",
                                                custom_registry);
    assert(!custom_load.success());
    assert(custom_load.root == nullptr);
    assert(custom_load.missing_features.size() == 1);
    assert(custom_load.missing_features[0] == "nearestEnemyId");

    return 0;
}
