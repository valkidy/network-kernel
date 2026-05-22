#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_COMMAND_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_COMMAND_H_

#include <string>
#include <unordered_map>
#include <vector>

#include "ai/ai_value.h"

namespace network_example::ai {

struct AICommand {
    std::string type;
    std::unordered_map<std::string, AIValue> params;
};

class AICommandBuffer {
public:
    void push(AICommand command);
    void clear();

    bool empty() const;
    std::size_t size() const;
    const std::vector<AICommand>& commands() const;

private:
    std::vector<AICommand> commands_;
};

}  // namespace network_example::ai

#endif  // ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_COMMAND_H_
