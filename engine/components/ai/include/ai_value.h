#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_VALUE_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_VALUE_H_

#include <cstdint>
#include <string>
#include <variant>

namespace network_example::ai {

using AIValue = std::variant<bool, int, float, std::uint32_t, std::string>;

}  // namespace network_example::ai

#endif  // ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_VALUE_H_
