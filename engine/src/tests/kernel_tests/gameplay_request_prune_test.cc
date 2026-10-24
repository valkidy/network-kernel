// A disconnected peer's processed gameplay requests are dropped: peer ids are
// never reused within a server run, so they can never match a resend again,
// and kept they only lengthen every later duplicate scan. Everyone else's stay
// answerable.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <ranges>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "kernel/public/kernel_api.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}
#define require(condition) require_impl(static_cast<bool>(condition), #condition, __LINE__)

KernelGameplayRequestOutcome outcome(std::uint32_t peer, std::uint64_t request_id) {
    KernelGameplayRequestOutcome result{};
    result.struct_size = sizeof(result);
    result.requester_peer = peer;
    result.request_id = request_id;
    result.status = KernelGameplayRequestStatus_Committed;
    return result;
}

}  // namespace

int main() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    for (std::uint64_t id = 1; id <= 3; ++id) {
        engine.processed_gameplay_requests_.push_back(outcome(2u, id));
        engine.processed_gameplay_requests_.push_back(outcome(3u, id));
    }
    KernelGameplayRequestOutcome found{};
    require(engine.get_gameplay_request_outcome(2u, 2u, &found));

    engine.forget_gameplay_requests_of(2u);
    require(engine.processed_gameplay_requests_.size() == 3u);
    for (const KernelGameplayRequestOutcome& kept : engine.processed_gameplay_requests_) {
        require(kept.requester_peer == 3u);
    }
    require(!engine.get_gameplay_request_outcome(2u, 2u, &found));
    require(engine.get_gameplay_request_outcome(3u, 2u, &found));
    require(found.request_id == 2u);

    // A peer with nothing recorded changes nothing.
    engine.forget_gameplay_requests_of(9u);
    require(engine.processed_gameplay_requests_.size() == 3u);

    std::printf("gameplay_request_prune_test passed\n");
    return 0;
}
