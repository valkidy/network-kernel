#include "ai/ai_scheduler.h"

#include <cassert>

int main() {
    network_example::ai::AIScheduler scheduler(0.5f);
    assert(!scheduler.update(0.1f));
    assert(!scheduler.update(0.39f));
    assert(scheduler.update(0.01f));
    assert(!scheduler.update(0.0f));
    assert(scheduler.update(1.2f));
    assert(!scheduler.update(0.0f));
    scheduler.reset();
    assert(!scheduler.update(0.49f));
    return 0;
}
