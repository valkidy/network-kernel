#include "game_server/src/ai_trace.h"

#include <cmath>
#include <exception>
#include <memory>
#include <utility>

#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "game_server/src/agent_steering.h"

namespace network_example::game_server {
namespace {

// Less than this in the time below is not getting anywhere. Well above the
// fraction of a centimetre an agent pressed against a wall jitters by.
constexpr float kProgressMeters = 0.5f;
constexpr std::uint64_t kStuckTicks = 90;
// A squad member further than this from its slot is on its way back to it,
// rather than settling into place.
constexpr float kOffSlotMeters = 3.0f;
// One server's worth of lines per second at most. A world in a mass fight must
// not spend its tick on this, and the count of what was dropped is itself a
// finding.
constexpr std::uint64_t kBudgetWindowTicks = 30;
constexpr std::uint32_t kBudgetLines = 300;
constexpr float kTicksPerSecond = 30.0f;

const char* state_name(AgentSentryState state) {
    switch (state) {
        case AgentSentryState::kIdle:
            return "Idle";
        case AgentSentryState::kAlert:
            return "Alert";
        case AgentSentryState::kAttack:
            return "Attack";
        case AgentSentryState::kReturn:
            return "Return";
        case AgentSentryState::kChasing:
            return "Chasing";
        case AgentSentryState::kSearch:
            return "Search";
        case AgentSentryState::kSeek:
            return "Seek";
    }
    return "?";
}

std::string format_position(const KernelVec3& value) {
    return fmt::format("({:.2f},{:.2f},{:.2f})", value.x, value.y, value.z);
}

const KernelServerEntityState* find_actor(
    const ActorStateView& actors,
    std::uint32_t net_id) {
    for (std::uint32_t index = 0; index < actors.count; ++index) {
        if (actors.states[index].net_id == net_id) {
            return &actors.states[index];
        }
    }
    return nullptr;
}

// Why it is in the state it just entered, from what it left behind: the
// controllers do not say, and asking them to would be a change to every
// transition for the sake of a log.
const char* transition_reason(
    const AgentRuntimeState& agent,
    AgentSentryState previous_state,
    std::uint32_t previous_lost_target_ticks,
    bool previous_has_attention_point) {
    switch (agent.sentry.state) {
        case AgentSentryState::kAlert:
            if (agent.sentry.alerted_by_damage) {
                return "damage";
            }
            // A squad alert carries a point to look at and nothing to look at
            // there yet.
            return agent.sentry.target != 0 ? "sight" : "squad";
        case AgentSentryState::kReturn:
            return previous_lost_target_ticks + 1 >= agent.sentry_config.forget_ticks ||
                    !previous_has_attention_point
                ? "forgot"
                : "leash";
        case AgentSentryState::kSearch:
            return "lost_sight";
        case AgentSentryState::kIdle:
            return previous_state == AgentSentryState::kReturn ? "back" : "ended";
        default:
            return "-";
    }
}

// Whether it means to be walking to a place it has not reached, so that not
// moving is news. Not a chase: stopping there is holding, or attacking.
bool walking_somewhere(const AgentRuntimeState& agent) {
    if (agent.entry.active) {
        return false;
    }
    if (agent.sentry.state == AgentSentryState::kReturn) {
        return true;
    }
    if (agent.sentry.state != AgentSentryState::kIdle) {
        return false;
    }
    return !agent.seek_path.empty() ||
        (agent.patrol.has_slot &&
         agent_steering::horizontal_distance(agent.position, agent.patrol.slot) >
             kOffSlotMeters);
}

std::shared_ptr<spdlog::logger>& file_logger() {
    static std::shared_ptr<spdlog::logger> logger;
    return logger;
}

}  // namespace

AiTrace::AiTrace(Sink sink) : sink_(std::move(sink)) {}

void AiTrace::emit(const std::string& line) {
    if (budget_used_ >= kBudgetLines) {
        ++budget_dropped_;
        return;
    }
    ++budget_used_;
    sink_(line);
}

void AiTrace::observe_agent(
    std::uint64_t tick,
    const AgentRuntimeState& agent,
    const ActorStateView& actors) {
    const auto [found, first_seen] = agents_.try_emplace(agent.net_id);
    AgentMemory& memory = found->second;
    const std::string who = fmt::format(
        "tick={} net={} tpl={} group={}",
        tick,
        agent.net_id,
        agent.actor_template_id,
        agent.patrol.group_id);

    if (first_seen) {
        memory.state = agent.sentry.state;
        emit(fmt::format(
            "{} spawned state={} pos={}",
            who,
            state_name(agent.sentry.state),
            format_position(agent.position)));
    }

    const AgentSentryState previous = memory.state;
    const AgentSentryState current = agent.sentry.state;
    // Flips at the attack range, and which of the two it is says nothing a
    // report needs.
    const bool same_pursuit = is_pursuing(previous) && is_pursuing(current);
    if (previous != current && !same_pursuit) {
        // The target is dropped on the tick an engagement breaks off, and that
        // tick is the one a report wants its distance on.
        const std::uint32_t target =
            agent.sentry.target != 0 ? agent.sentry.target : memory.target;
        std::string target_text = fmt::format("target={}", target);
        const KernelServerEntityState* actor =
            target != 0 ? find_actor(actors, target) : nullptr;
        if (actor != nullptr) {
            target_text += fmt::format(
                " target_d={:.1f} target_dy={:.1f} target_visible={}",
                agent_steering::horizontal_distance(agent.position, actor->position),
                actor->position.y - agent.position.y,
                // Now, or on the tick before if that is what was just dropped.
                (agent.sentry.target != 0 ? agent.sentry.lost_target_ticks
                                          : memory.lost_target_ticks) == 0 &&
                        (agent.sentry.target != 0 || memory.target != 0)
                    ? 1
                    : 0);
        }
        emit(fmt::format(
            "{} {}->{} reason={} pos={} anchor={} anchor_d={:.1f} slot={} slot_d={:.1f} {}",
            who,
            state_name(previous),
            state_name(current),
            transition_reason(
                agent, previous, memory.lost_target_ticks, memory.has_attention_point),
            format_position(agent.position),
            format_position(agent.patrol.leash_anchor),
            agent_steering::horizontal_distance(agent.position, agent.patrol.leash_anchor),
            agent.patrol.has_slot ? format_position(agent.patrol.slot) : std::string("-"),
            agent.patrol.has_slot
                ? agent_steering::horizontal_distance(agent.position, agent.patrol.slot)
                : 0.0f,
            target_text));
    }

    const bool walking = walking_somewhere(agent);
    const bool made_progress =
        agent_steering::horizontal_distance(agent.position, memory.progress_position) >=
        kProgressMeters;
    if (walking && (!memory.walking || made_progress)) {
        memory.progress_position = agent.position;
        memory.progress_tick = tick;
    }
    if (memory.stuck && (!walking || made_progress)) {
        memory.stuck = false;
        emit(fmt::format(
            "{} unstuck state={} after={:.1f}s pos={}",
            who,
            state_name(current),
            static_cast<float>(tick - memory.stuck_since_tick) / kTicksPerSecond,
            format_position(agent.position)));
    }
    if (walking && !memory.stuck && tick - memory.progress_tick >= kStuckTicks) {
        memory.stuck = true;
        memory.stuck_since_tick = memory.progress_tick;
        emit(fmt::format(
            "{} stuck state={} for={:.1f}s pos={} slot={} slot_d={:.1f}",
            who,
            state_name(current),
            static_cast<float>(tick - memory.progress_tick) / kTicksPerSecond,
            format_position(agent.position),
            agent.patrol.has_slot ? format_position(agent.patrol.slot) : std::string("-"),
            agent.patrol.has_slot
                ? agent_steering::horizontal_distance(agent.position, agent.patrol.slot)
                : 0.0f));
    }

    memory.walking = walking;
    memory.state = current;
    memory.target = agent.sentry.target;
    memory.lost_target_ticks = agent.sentry.lost_target_ticks;
    memory.has_attention_point = agent.sentry.has_attention_point;
    memory.actor_template_id = agent.actor_template_id;
    memory.group_id = agent.patrol.group_id;
    memory.position = agent.position;
    memory.seen_tick = tick;
}

void AiTrace::observe_group(
    std::uint64_t tick,
    const PatrolGroup& group) {
    const auto [found, first_seen] = groups_.try_emplace(group.group_id);
    GroupMemory& memory = found->second;

    // Which member is keeping it there, and on what. One that can see its target
    // if there is one: a squad-mate merely alerted by the fight also counts as
    // engaged, and naming it would read as a squad holding for nobody.
    const AgentRuntimeState* holder = nullptr;
    for (const std::uint32_t net_id : group.member_net_ids) {
        const auto member = by_net_id_.find(net_id);
        if (member == by_net_id_.end() || !is_engaged(member->second->sentry.state)) {
            continue;
        }
        if (holder == nullptr) {
            holder = member->second;
        }
        if (member->second->sentry.target != 0 &&
            member->second->sentry.lost_target_ticks == 0) {
            holder = member->second;
            break;
        }
    }
    const std::uint32_t holder_id = holder != nullptr ? holder->net_id : 0;

    if (first_seen) {
        emit(fmt::format(
            "tick={} group={} def={} spawned members={} cursor={} waypoints={}",
            tick,
            group.group_id,
            group.definition_id,
            group.member_net_ids.size(),
            format_position(group.cursor),
            group.waypoints.size()));
    }
    if (first_seen || memory.holding != group.holding || memory.holder != holder_id) {
        std::string by = fmt::format("by={}", holder_id);
        if (holder != nullptr) {
            by += fmt::format(
                " by_state={} by_target={} by_lost={}",
                state_name(holder->sentry.state),
                holder->sentry.target,
                holder->sentry.lost_target_ticks);
        }
        emit(fmt::format(
            "tick={} group={} holding={} {} cursor={}",
            tick,
            group.group_id,
            group.holding ? 1 : 0,
            by,
            format_position(group.cursor)));
    }
    if (group.route_complete && !memory.route_complete) {
        emit(fmt::format(
            "tick={} group={} route_complete cursor={}",
            tick,
            group.group_id,
            format_position(group.cursor)));
    }

    memory.holding = group.holding;
    memory.holder = holder_id;
    memory.route_complete = group.route_complete;
    memory.seen_tick = tick;
}

void AiTrace::observe(
    std::uint64_t tick,
    const std::vector<AgentRuntimeState>& agents,
    const std::vector<PatrolGroup>& groups,
    const ActorStateView& actors) {
    if (!sink_) {
        return;
    }
    // Here rather than on the next line written: a quiet world after a burst
    // may not write one for a long time.
    if (tick >= budget_window_tick_ + kBudgetWindowTicks) {
        if (budget_dropped_ > 0) {
            sink_(fmt::format("tick={} dropped_lines={}", tick, budget_dropped_));
        }
        budget_window_tick_ = tick;
        budget_used_ = 0;
        budget_dropped_ = 0;
    }
    by_net_id_.clear();
    for (const AgentRuntimeState& agent : agents) {
        by_net_id_[agent.net_id] = &agent;
        observe_agent(tick, agent, actors);
    }
    for (const PatrolGroup& group : groups) {
        observe_group(tick, group);
    }

    for (auto entry = agents_.begin(); entry != agents_.end();) {
        if (entry->second.seen_tick == tick) {
            ++entry;
            continue;
        }
        emit(fmt::format(
            "tick={} net={} tpl={} group={} gone state={} pos={}",
            tick,
            entry->first,
            entry->second.actor_template_id,
            entry->second.group_id,
            state_name(entry->second.state),
            format_position(entry->second.position)));
        entry = agents_.erase(entry);
    }
    for (auto entry = groups_.begin(); entry != groups_.end();) {
        if (entry->second.seen_tick == tick) {
            ++entry;
            continue;
        }
        emit(fmt::format("tick={} group={} gone", tick, entry->first));
        entry = groups_.erase(entry);
    }
}

bool open_ai_trace_file(
    const std::string& path,
    std::size_t max_bytes,
    std::size_t max_files,
    std::string* error) {
    try {
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            path, max_bytes, max_files, /*rotate_on_open=*/true);
        // Its own logger, out of spdlog's registry: this is a file a report
        // reads, not the server's log.
        auto logger = std::make_shared<spdlog::logger>("ai_trace", std::move(sink));
        logger->set_pattern("%Y-%m-%d %H:%M:%S.%e %v");
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        file_logger() = std::move(logger);
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = exception.what();
        }
        return false;
    }
}

AiTrace::Sink ai_trace_file_sink() {
    std::shared_ptr<spdlog::logger> logger = file_logger();
    if (logger == nullptr) {
        return {};
    }
    return [logger = std::move(logger)](const std::string& line) {
        logger->info("{}", line);
    };
}

}  // namespace network_example::game_server
