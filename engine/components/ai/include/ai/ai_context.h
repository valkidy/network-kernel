#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_CONTEXT_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_CONTEXT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ai/ai_value.h"

namespace network_example::ai {

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
