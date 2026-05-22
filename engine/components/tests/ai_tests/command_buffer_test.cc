#include "ai/ai_command.h"

#include <cassert>
#include <cstdint>
#include <string>

int main() {
    network_example::ai::AICommandBuffer commands;
    network_example::ai::AICommand attack;
    attack.type = "AttackTarget";
    attack.params["target"] = static_cast<std::uint32_t>(7);

    network_example::ai::AICommand help;
    help.type = "RequestHelp";
    help.params["urgent"] = true;

    commands.push(attack);
    commands.push(help);

    assert(commands.size() == 2);
    assert(commands.commands()[0].type == "AttackTarget");
    assert(std::get<std::uint32_t>(commands.commands()[0].params.at("target")) ==
           7);
    assert(commands.commands()[1].type == "RequestHelp");
    assert(std::get<bool>(commands.commands()[1].params.at("urgent")));

    commands.clear();
    assert(commands.empty());
    return 0;
}
