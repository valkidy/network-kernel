#include "ai/node_factory.h"

#include <cassert>

int main() {
    network_example::ai::NodeFactory factory;
    factory.register_node_type("Action.Patrol", []() {
        return network_example::ai::make_action_patrol();
    });
    factory.register_score_function(
        "Score.AttackWhenHealthy",
        network_example::ai::score_attack_when_healthy);

    assert(factory.has_node_type("Action.Patrol"));
    assert(!factory.has_node_type("Action.Unknown"));
    auto patrol = factory.create_node("Action.Patrol");
    assert(patrol != nullptr);
    assert(factory.create_node("Action.Unknown") == nullptr);

    auto score = factory.score_function("Score.AttackWhenHealthy");
    assert(score.has_value());
    assert(!factory.score_function("Score.Unknown").has_value());

    return 0;
}
