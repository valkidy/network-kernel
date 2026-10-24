// The two gameplay request lists that used to grow for as long as a server
// ran. A disconnected peer's processed requests are dropped: peer ids are
// never reused within a server run, so they can never match a resend again,
// and kept they only lengthen every later duplicate scan. Everyone else's stay
// answerable. And a dedicated server, which nothing may ever poll, keeps only
// the newest outcomes for Kernel_PollGameplayRequestOutcomes; a listen server,
// whose host polls its own, keeps them all.

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

    // The dedicated server's outcome queue keeps the newest, in order.
    constexpr std::size_t kCap = network_example::KernelEngine::kMaxQueuedGameplayRequestOutcomes;
    constexpr std::uint64_t kQueued = kCap + 904u;
    for (std::uint64_t id = 1; id <= kQueued; ++id) {
        engine.queue_gameplay_request_outcome(outcome(2u, id));
    }
    require(engine.pending_gameplay_request_outcomes_.size() == kCap);
    require(engine.pending_gameplay_request_outcomes_.front().request_id == kQueued - kCap + 1u);
    require(engine.pending_gameplay_request_outcomes_.back().request_id == kQueued);
    // A host polling it as it goes still gets every one it has not read.
    KernelGameplayRequestOutcome polled[4]{};
    require(engine.poll_gameplay_request_outcomes(polled, 4u) == 4u);
    require(polled[0].request_id == kQueued - kCap + 1u);
    require(engine.pending_gameplay_request_outcomes_.size() == kCap - 4u);

    // A listen server's host polls its own: no cap.
    KernelConfig listen_config = config;
    listen_config.mode = KernelMode_ListenServer;
    network_example::KernelEngine listen(listen_config);
    listen.reset_runtime_state(KernelMode_ListenServer);
    for (std::uint64_t id = 1; id <= kQueued; ++id) {
        listen.queue_gameplay_request_outcome(outcome(2u, id));
    }
    require(listen.pending_gameplay_request_outcomes_.size() == kQueued);

    std::printf("gameplay_request_prune_test passed\n");
    return 0;
}
