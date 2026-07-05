#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_INTENT_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_INTENT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ai_value.h"

namespace network_example::ai {

using EntityId = std::uint32_t;
using RuntimeId = std::uint32_t;

enum class IntentScope {
    kActor,
    kDirector,
    kWorld,
};

enum class IntentStatus {
    kRunning,
    kSucceeded,
    kFailed,
    kInterrupted,
};

struct ScopedIntent {
    IntentScope scope = IntentScope::kActor;
    std::string type;
    EntityId subject = 0;
    std::unordered_map<std::string, AIValue> params;
};

class IntentBuffer {
public:
    void push(ScopedIntent intent) {
        intents_.push_back(std::move(intent));
    }

    void clear() {
        intents_.clear();
    }

    bool empty() const {
        return intents_.empty();
    }

    std::size_t size() const {
        return intents_.size();
    }

    const std::vector<ScopedIntent>& intents() const {
        return intents_;
    }

private:
    std::vector<ScopedIntent> intents_;
};

}  // namespace network_example::ai

#endif  // ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_INTENT_H_
