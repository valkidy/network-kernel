#include "ai/ai_command.h"

#include <utility>

namespace network_example::ai {

void AICommandBuffer::push(AICommand command) {
    commands_.push_back(std::move(command));
}

void AICommandBuffer::clear() {
    commands_.clear();
}

bool AICommandBuffer::empty() const {
    return commands_.empty();
}

std::size_t AICommandBuffer::size() const {
    return commands_.size();
}

const std::vector<AICommand>& AICommandBuffer::commands() const {
    return commands_;
}

}  // namespace network_example::ai
