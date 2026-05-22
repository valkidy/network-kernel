#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_NODE_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_NODE_H_

#include "ai/ai_command.h"
#include "ai/ai_context.h"
#include "ai/node_status.h"

namespace network_example::ai {

class AINode {
public:
    virtual ~AINode() = default;

    virtual NodeStatus tick(const AIContext& context,
                            AICommandBuffer* commands) = 0;
    virtual void halt(const AIContext& context, AICommandBuffer* commands) = 0;
};

}  // namespace network_example::ai

#endif  // ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_NODE_H_
