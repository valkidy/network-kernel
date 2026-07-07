#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_NODE_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_NODE_H_

#include "ai_context.h"
#include "ai_intent.h"
#include "node_status.h"

namespace network_example::ai {

class AINode {
public:
    virtual ~AINode() = default;

    virtual NodeStatus tick(const AIContext& context,
                            IntentBuffer* intents) = 0;
    virtual void halt(const AIContext& context, IntentBuffer* intents) = 0;
};

}  // namespace network_example::ai

#endif  // ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_NODE_H_
