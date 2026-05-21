#include "ai/ai_context.h"
#include "ai/ai_tree.h"
#include "ai/yaml_loader.h"

#include <cassert>
#include <cstdint>
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
                type: Action.AttackTarget
                target: nearestEnemyId
            - name: Flee
              score: Score.FleeWhenCriticalHp
              node:
                type: Action.FleeFromTarget
                target: nearestEnemyId
    - type: Action.Patrol
)yaml";
}

}  // namespace

int main() {
    network_example::ai::YamlLoadResult result =
        network_example::ai::load_tree_from_yaml(valid_tree_yaml());
    assert(result.success());
    assert(result.root != nullptr);
    assert(result.required_nodes.size() == 7);
    assert(result.required_scores.size() == 2);
    assert(result.required_features.size() >= 2);

    network_example::ai::AITreeInstance tree(std::move(result.root));
    network_example::ai::AIContext context;
    network_example::ai::AICommandBuffer commands;
    context.set_feature("hasVisibleEnemy", true);
    context.set_feature("hp01", 0.9f);
    context.set_feature("nearestEnemyId", static_cast<std::uint32_t>(42));
    assert(tree.tick(context, &commands) ==
           network_example::ai::NodeStatus::kSuccess);
    assert(commands.size() == 1);
    assert(commands.commands()[0].type == "AttackTarget");

    network_example::ai::YamlLoadResult unknown_node =
        network_example::ai::load_tree_from_yaml(R"yaml(
tree: Bad
root:
  type: Action.DoesNotExist
)yaml");
    assert(!unknown_node.success());
    assert(!unknown_node.errors.empty());
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
    assert(unknown_score.missing_scores.size() == 1);

    network_example::ai::YamlLoadResult bad_shape =
        network_example::ai::load_tree_from_yaml(R"yaml(
tree: BadShape
root:
  type: Composite.Sequence
)yaml");
    assert(!bad_shape.success());
    assert(!bad_shape.errors.empty());

    return 0;
}
