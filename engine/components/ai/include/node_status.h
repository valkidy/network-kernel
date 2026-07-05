#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_NODE_STATUS_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_NODE_STATUS_H_

namespace network_example::ai {

enum class NodeStatus {
    kSuccess,
    kFailure,
    kRunning,
};

}  // namespace network_example::ai

#endif  // ENGINE_COMPONENTS_AI_INCLUDE_AI_NODE_STATUS_H_
