#include "ai/ai_scheduler.h"

namespace network_example::ai {

AIScheduler::AIScheduler(float interval_seconds)
    : interval_seconds_(interval_seconds) {}

bool AIScheduler::update(float dt_seconds) {
    if (interval_seconds_ <= 0.0f) {
        return true;
    }
    accumulated_seconds_ += dt_seconds;
    if (accumulated_seconds_ < interval_seconds_) {
        return false;
    }
    accumulated_seconds_ -= interval_seconds_;
    return true;
}

void AIScheduler::reset() {
    accumulated_seconds_ = 0.0f;
}

float AIScheduler::interval_seconds() const {
    return interval_seconds_;
}

float AIScheduler::accumulated_seconds() const {
    return accumulated_seconds_;
}

}  // namespace network_example::ai
