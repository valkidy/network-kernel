#include "ai/ai_tree.h"

#include <utility>

namespace network_example::ai {

AITreeInstance::AITreeInstance(NodePtr root) : root_(std::move(root)) {}

NodeStatus AITreeInstance::tick(const AIContext& context,
                                AICommandBuffer* commands) {
    if (root_ == nullptr) {
        return NodeStatus::kFailure;
    }
    return root_->tick(context, commands);
}

void AITreeInstance::halt(const AIContext& context, AICommandBuffer* commands) {
    if (root_ != nullptr) {
        root_->halt(context, commands);
    }
}

bool AITreeInstance::has_root() const {
    return root_ != nullptr;
}

}  // namespace network_example::ai
