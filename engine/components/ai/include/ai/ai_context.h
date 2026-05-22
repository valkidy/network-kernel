#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_CONTEXT_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_CONTEXT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ai/ai_value.h"

namespace network_example::ai {

// Blackboard v1: AIContext is the tick-local, read-only blackboard passed to
// tree nodes. Gameplay adapters rebuild it from perception and gameplay facts
// each tick; nodes should read these values but not retain them across ticks.
// Persistent memory such as cooldowns, patrol anchors, reload timers, and
// long-lived targets belongs in gameplay state or ECS components instead.
class AIContext {
public:
    void set_feature(std::string name, AIValue value);
    bool has_feature(std::string_view name) const;

    std::optional<bool> get_bool(std::string_view name) const;
    std::optional<float> get_float(std::string_view name) const;
    std::optional<std::uint32_t> get_uint32(std::string_view name) const;
    std::optional<std::string> get_string(std::string_view name) const;

    void clear();

private:
    const AIValue* find(std::string_view name) const;

    std::unordered_map<std::string, AIValue> features_;
};

}  // namespace network_example::ai

#endif  // ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_CONTEXT_H_
