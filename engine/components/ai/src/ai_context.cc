#include "ai/ai_context.h"

#include <utility>

namespace network_example::ai {

void AIContext::set_feature(std::string name, AIValue value) {
    features_[std::move(name)] = std::move(value);
}

bool AIContext::has_feature(std::string_view name) const {
    return find(name) != nullptr;
}

std::optional<bool> AIContext::get_bool(std::string_view name) const {
    const AIValue* value = find(name);
    if (value == nullptr || !std::holds_alternative<bool>(*value)) {
        return std::nullopt;
    }
    return std::get<bool>(*value);
}

std::optional<float> AIContext::get_float(std::string_view name) const {
    const AIValue* value = find(name);
    if (value == nullptr || !std::holds_alternative<float>(*value)) {
        return std::nullopt;
    }
    return std::get<float>(*value);
}

std::optional<std::uint32_t> AIContext::get_uint32(std::string_view name) const {
    const AIValue* value = find(name);
    if (value == nullptr || !std::holds_alternative<std::uint32_t>(*value)) {
        return std::nullopt;
    }
    return std::get<std::uint32_t>(*value);
}

std::optional<std::string> AIContext::get_string(std::string_view name) const {
    const AIValue* value = find(name);
    if (value == nullptr || !std::holds_alternative<std::string>(*value)) {
        return std::nullopt;
    }
    return std::get<std::string>(*value);
}

void AIContext::clear() {
    features_.clear();
}

const AIValue* AIContext::find(std::string_view name) const {
    const auto iter = features_.find(std::string(name));
    if (iter == features_.end()) {
        return nullptr;
    }
    return &iter->second;
}

}  // namespace network_example::ai
