#include "ai/ai_scheduler.h"

namespace network_example::ai {
namespace {

constexpr float kSchedulerEpsilonSeconds = 0.000001f;

}  // namespace

AIScheduler::AIScheduler(float interval_seconds)
    : interval_seconds_(interval_seconds) {}

bool AIScheduler::update(float dt_seconds) {
    if (interval_seconds_ <= 0.0f) {
        return true;
    }
    accumulated_seconds_ += dt_seconds;
    if (accumulated_seconds_ + kSchedulerEpsilonSeconds < interval_seconds_) {
        return false;
    }
    while (accumulated_seconds_ + kSchedulerEpsilonSeconds >=
           interval_seconds_) {
        accumulated_seconds_ -= interval_seconds_;
    }
    if (accumulated_seconds_ < 0.0f &&
        accumulated_seconds_ > -kSchedulerEpsilonSeconds) {
        accumulated_seconds_ = 0.0f;
    }
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
