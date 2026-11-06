#ifndef GAME_SERVER_AI_TRACE_H_
#define GAME_SERVER_AI_TRACE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "game_server/src/agent_runtime.h"
#include "game_server/src/patrol_group_runtime.h"

namespace network_example::game_server {

// What the AI decided, for a bug report: a client sees where an agent stands
// but never its state, its target, its slot or why it turned round.
//
// It only watches. Once a tick it compares what the controllers left behind
// with the last tick and writes a line for each change, so turning it on cannot
// change a decision. Quiet by construction: an agent in one state writes
// nothing, and Chasing <-> Attack, which flips at the attack range, is one state
// here.
//
//   tick=90 net=7 tpl=28 group=3 Chasing->Return reason=leash pos=(..)
//       anchor=(..) anchor_d=18.1 slot=(..) slot_d=30.2 target=2 target_d=10.0
//       target_visible=1
//   tick=200 net=7 tpl=28 group=3 stuck state=Return for=3.0s pos=(..) slot_d=20
//   tick=900 net=7 tpl=28 group=3 unstuck state=Idle after=23.3s pos=(..)
//   tick=90 group=3 holding=1 by=7 by_state=Chasing by_target=2 by_lost=0
//
// A squad held by a member with by_target=0 is holding for nobody in sight: one
// with a target in sight is named ahead of any without.
class AiTrace {
public:
    using Sink = std::function<void(const std::string&)>;

    explicit AiTrace(Sink sink);

    void observe(
        std::uint64_t tick,
        const std::vector<AgentRuntimeState>& agents,
        const std::vector<PatrolGroup>& groups,
        const ActorStateView& actors);

private:
    struct AgentMemory {
        AgentSentryState state = AgentSentryState::kIdle;
        std::uint32_t target = 0;
        std::uint32_t lost_target_ticks = 0;
        bool has_attention_point = false;
        std::uint32_t actor_template_id = 0;
        std::uint32_t group_id = 0;
        KernelVec3 position{0.0f, 0.0f, 0.0f};
        // Where it last made headway, and when. Only meaningful while
        // `walking` says it is trying to get somewhere.
        bool walking = false;
        KernelVec3 progress_position{0.0f, 0.0f, 0.0f};
        std::uint64_t progress_tick = 0;
        std::uint64_t stuck_since_tick = 0;
        bool stuck = false;
        std::uint64_t seen_tick = 0;
    };
    struct GroupMemory {
        bool holding = false;
        std::uint32_t holder = 0;
        bool route_complete = false;
        std::uint64_t seen_tick = 0;
    };

    void observe_agent(
        std::uint64_t tick,
        const AgentRuntimeState& agent,
        const ActorStateView& actors);
    void observe_group(
        std::uint64_t tick,
        const PatrolGroup& group);
    void emit(const std::string& line);

    Sink sink_;
    std::unordered_map<std::uint32_t, AgentMemory> agents_;
    std::unordered_map<std::uint32_t, GroupMemory> groups_;
    std::unordered_map<std::uint32_t, const AgentRuntimeState*> by_net_id_;
    std::uint64_t budget_window_tick_ = 0;
    std::uint32_t budget_used_ = 0;
    std::uint32_t budget_dropped_ = 0;
};

// Sends the trace of every AgentRuntimeManager built from here on to a size-
// capped file, each line stamped with local wall-clock time to the millisecond,
// so it lines up with a video recorded on the same machine. Flushed per line:
// the file is read while the server runs. Returns false, with `error` set, when
// it cannot be opened.
bool open_ai_trace_file(
    const std::string& path,
    std::size_t max_bytes,
    std::size_t max_files,
    std::string* error);

// The sink open_ai_trace_file set up, or an empty one if it was not called.
AiTrace::Sink ai_trace_file_sink();

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AI_TRACE_H_
