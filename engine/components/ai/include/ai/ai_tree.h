#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_TREE_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_TREE_H_

#include <memory>

#include "ai/ai_node.h"

namespace network_example::ai {

using NodePtr = std::unique_ptr<AINode>;

class AITreeInstance {
public:
    explicit AITreeInstance(NodePtr root);

    NodeStatus tick(const AIContext& context, AICommandBuffer* commands);
    void halt(const AIContext& context, AICommandBuffer* commands);

    bool has_root() const;

private:
    NodePtr root_;
};

}  // namespace network_example::ai

#endif  // ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_TREE_H_
