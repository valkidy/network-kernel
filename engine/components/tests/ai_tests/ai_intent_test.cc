#include "ai_intent.h"

#include <cassert>
#include <string>

int main() {
    network_example::ai::ScopedIntent attack;
    attack.scope = network_example::ai::IntentScope::kActor;
    attack.type = "AttackTarget";
    attack.subject = 7;
    attack.params["target_id"] = std::uint32_t{42};

    assert(attack.scope == network_example::ai::IntentScope::kActor);
    assert(attack.type == "AttackTarget");
    assert(attack.subject == 7);
    assert(std::get<std::uint32_t>(attack.params["target_id"]) == 42);

    network_example::ai::ScopedIntent director;
    director.scope = network_example::ai::IntentScope::kDirector;
    director.type = "StartEncounter";
    director.subject = 3;
    assert(director.scope == network_example::ai::IntentScope::kDirector);

    network_example::ai::ScopedIntent world;
    world.scope = network_example::ai::IntentScope::kWorld;
    world.type = "SpawnAgent";
    assert(world.scope == network_example::ai::IntentScope::kWorld);

    assert(network_example::ai::IntentStatus::kRunning !=
           network_example::ai::IntentStatus::kSucceeded);
    assert(network_example::ai::IntentStatus::kFailed !=
           network_example::ai::IntentStatus::kInterrupted);

    network_example::ai::IntentBuffer intents;
    assert(intents.empty());
    intents.push(attack);
    intents.push(world);
    assert(!intents.empty());
    assert(intents.size() == 2);
    assert(intents.intents()[0].type == "AttackTarget");
    assert(intents.intents()[1].type == "SpawnAgent");
    intents.clear();
    assert(intents.empty());

    return 0;
}
