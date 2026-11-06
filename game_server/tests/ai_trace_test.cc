// The AI trace on hand-built agents: which lines a change writes and which
// reason they give. No kernel; the trace only reads what controllers left behind.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "game_server/src/ai_trace.h"

namespace {

using namespace network_example::game_server;

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "ai_trace_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

struct Rig {
    std::vector<std::string> lines;
    AiTrace trace{[this](const std::string& line) { lines.push_back(line); }};
    std::vector<AgentRuntimeState> agents;
    std::vector<PatrolGroup> groups;
    std::vector<KernelServerEntityState> actors;
    std::uint64_t tick = 0;

    void step() {
        ++tick;
        lines.clear();
        trace.observe(
            tick,
            agents,
            groups,
            ActorStateView{actors.data(), static_cast<std::uint32_t>(actors.size())});
    }

    bool wrote(const std::string& text) const {
        return count(text) > 0;
    }

    std::size_t count(const std::string& text) const {
        return static_cast<std::size_t>(std::count_if(
            lines.begin(), lines.end(), [&text](const std::string& line) {
                return line.find(text) != std::string::npos;
            }));
    }
};

AgentRuntimeState squad_member(std::uint32_t net_id) {
    AgentRuntimeState agent;
    agent.net_id = net_id;
    agent.actor_template_id = 28;
    agent.patrol.group_id = 3;
    agent.patrol.has_slot = true;
    agent.sentry_config.forget_ticks = 150;
    return agent;
}

KernelServerEntityState player_at(std::uint32_t net_id, float x) {
    KernelServerEntityState state{};
    state.net_id = net_id;
    state.position = KernelVec3{x, 0.0f, 0.0f};
    return state;
}

// The report that started this: it broke off with the player in plain sight,
// and the line has to say so even though the target is already dropped.
void a_leash_break_names_the_target_it_left() {
    Rig rig;
    rig.actors.push_back(player_at(2, 28.0f));
    rig.agents.push_back(squad_member(7));
    AgentRuntimeState& agent = rig.agents[0];
    agent.position = KernelVec3{18.0f, 0.0f, 0.0f};
    agent.sentry.state = AgentSentryState::kChasing;
    agent.sentry.target = 2;
    agent.sentry.has_attention_point = true;
    rig.step();

    agent.sentry.state = AgentSentryState::kReturn;
    agent.sentry.target = 0;
    agent.sentry.has_attention_point = false;
    agent.sentry.lost_target_ticks = 0;
    rig.step();

    require(rig.count("Chasing->Return") == 1);
    require(rig.wrote("reason=leash"));
    require(rig.wrote("target=2"));
    require(rig.wrote("target_d=10.0"));
    require(rig.wrote("target_visible=1"));
    require(rig.wrote("anchor_d=18.0"));
}

void a_return_after_losing_sight_is_forgetting() {
    Rig rig;
    rig.agents.push_back(squad_member(7));
    AgentRuntimeState& agent = rig.agents[0];
    agent.sentry.state = AgentSentryState::kSearch;
    agent.sentry.has_attention_point = true;
    agent.sentry.lost_target_ticks = 149;
    rig.step();

    agent.sentry.state = AgentSentryState::kReturn;
    agent.sentry.lost_target_ticks = 0;
    rig.step();

    require(rig.wrote("Search->Return"));
    require(rig.wrote("reason=forgot"));
    require(!rig.wrote("target_visible=1"));
}

void an_alert_says_where_it_came_from() {
    Rig rig;
    rig.actors.push_back(player_at(2, 30.0f));
    rig.agents.push_back(squad_member(7));
    AgentRuntimeState& agent = rig.agents[0];
    rig.step();
    require(rig.wrote("spawned"));

    agent.sentry.state = AgentSentryState::kAlert;
    agent.sentry.has_attention_point = true;
    rig.step();
    require(rig.wrote("Idle->Alert reason=squad"));

    agent.sentry.state = AgentSentryState::kIdle;
    rig.step();
    agent.sentry.state = AgentSentryState::kAlert;
    agent.sentry.target = 2;
    rig.step();
    require(rig.wrote("Idle->Alert reason=sight"));
    require(rig.wrote("target_d=30.0"));
    require(rig.wrote("target_visible=1"));

    agent.sentry.state = AgentSentryState::kIdle;
    agent.sentry.target = 0;
    rig.step();
    agent.sentry.state = AgentSentryState::kAlert;
    agent.sentry.alerted_by_damage = true;
    rig.step();
    require(rig.wrote("Idle->Alert reason=damage"));
}

// Flips at the attack range, many times a fight.
void chasing_and_attack_are_one_state() {
    Rig rig;
    rig.agents.push_back(squad_member(7));
    rig.agents[0].sentry.state = AgentSentryState::kChasing;
    rig.step();
    rig.agents[0].sentry.state = AgentSentryState::kAttack;
    rig.step();
    require(rig.lines.empty());
    rig.agents[0].sentry.state = AgentSentryState::kChasing;
    rig.step();
    require(rig.lines.empty());
}

// Pressed against a cliff: it jitters by a centimetre and never gets anywhere.
void a_return_going_nowhere_is_stuck_once_and_then_unstuck() {
    Rig rig;
    rig.agents.push_back(squad_member(7));
    AgentRuntimeState& agent = rig.agents[0];
    agent.position = KernelVec3{100.87f, 13.13f, 43.21f};
    agent.patrol.slot = KernelVec3{118.0f, 24.0f, 50.0f};
    agent.sentry.state = AgentSentryState::kReturn;

    std::size_t stuck_lines = 0;
    for (int tick = 0; tick < 300; ++tick) {
        agent.position.x = 100.87f + (tick % 2 == 0 ? 0.01f : 0.0f);
        rig.step();
        stuck_lines += rig.count(" stuck ");
        if (tick == 80) {
            require(stuck_lines == 0);
        }
    }
    require(stuck_lines == 1);

    agent.position.x = 102.0f;
    rig.step();
    require(rig.wrote(" unstuck "));
    require(rig.wrote("after=10.0s"));
}

void walking_steadily_is_not_stuck() {
    Rig rig;
    rig.agents.push_back(squad_member(7));
    AgentRuntimeState& agent = rig.agents[0];
    agent.patrol.slot = KernelVec3{500.0f, 0.0f, 0.0f};
    agent.sentry.state = AgentSentryState::kReturn;
    for (int tick = 0; tick < 300; ++tick) {
        agent.position.x += 0.075f;  // 2.25 m/s
        rig.step();
        require(!rig.wrote("stuck"));
    }
}

// Standing in a slot, or fighting, it is not trying to go anywhere.
void standing_in_its_slot_is_not_stuck() {
    Rig rig;
    rig.agents.push_back(squad_member(7));
    for (int tick = 0; tick < 300; ++tick) {
        rig.step();
        require(!rig.wrote("stuck"));
    }
}

// Whether a squad is held by somebody with a target or by nobody in sight is
// the question the video could not answer.
void a_squad_says_who_holds_it_and_on_what() {
    Rig rig;
    rig.agents.push_back(squad_member(7));
    rig.agents.push_back(squad_member(8));
    PatrolGroup group;
    group.group_id = 3;
    group.definition_id = 1;
    group.member_net_ids = {7, 8};
    group.member_offsets = {KernelVec3{}, KernelVec3{}};
    rig.groups.push_back(group);
    rig.step();
    require(rig.wrote("group=3 def=1 spawned members=2"));
    require(rig.wrote("group=3 holding=0 by=0"));

    rig.agents[1].sentry.state = AgentSentryState::kAlert;
    rig.agents[1].sentry.lost_target_ticks = 12;
    rig.groups[0].holding = true;
    rig.step();
    require(rig.wrote("group=3 holding=1 by=8 by_state=Alert by_target=0 by_lost=12"));

    rig.step();
    require(!rig.wrote("holding="));

    // The hold passes to another member: two that re-alert each other show up
    // as exactly this, again and again.
    rig.agents[1].sentry.state = AgentSentryState::kIdle;
    rig.agents[0].sentry.state = AgentSentryState::kAlert;
    rig.step();
    require(rig.wrote("holding=1 by=7"));

    // Somebody with a target in sight is who it is really holding for, whoever
    // comes first in the list.
    rig.agents[1].sentry.state = AgentSentryState::kChasing;
    rig.agents[1].sentry.target = 2;
    rig.agents[1].sentry.lost_target_ticks = 0;
    rig.step();
    require(rig.wrote("holding=1 by=8 by_state=Chasing by_target=2"));

    rig.groups.clear();
    rig.step();
    require(rig.wrote("group=3 gone"));
}

void a_dead_agent_is_gone() {
    Rig rig;
    rig.agents.push_back(squad_member(7));
    rig.step();
    rig.agents.clear();
    rig.step();
    require(rig.wrote("net=7 tpl=28 group=3 gone state=Idle"));
    rig.step();
    require(rig.lines.empty());
}

// A mass fight must not spend its tick here; the count says it happened.
void lines_past_the_budget_are_counted_not_written() {
    Rig rig;
    for (std::uint32_t net_id = 1; net_id <= 1000; ++net_id) {
        rig.agents.push_back(squad_member(net_id));
    }
    rig.tick = 100;
    rig.step();
    require(rig.lines.size() == 300);
    rig.agents.clear();
    for (int tick = 0; tick < 29; ++tick) {
        rig.step();
    }
    rig.step();
    require(rig.wrote("dropped_lines="));
}

void the_file_has_a_wall_clock_on_each_line() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "ai_trace_test_trace.log";
    std::filesystem::remove(path);
    std::string error;
    require(ai_trace_file_sink() == nullptr);
    require(open_ai_trace_file(path.string(), 1024 * 1024, 1, &error));
    AiTrace::Sink sink = ai_trace_file_sink();
    require(sink != nullptr);
    sink("tick=1 hello");

    std::ifstream file(path);
    std::string line;
    require(static_cast<bool>(std::getline(file, line)));
    // 2026-11-06 14:17:05.123 tick=1 hello
    require(line.size() == 23 + 1 + 12);
    require(line[4] == '-' && line[10] == ' ' && line[19] == '.');
    require(line.substr(24) == "tick=1 hello");
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    a_leash_break_names_the_target_it_left();
    a_return_after_losing_sight_is_forgetting();
    an_alert_says_where_it_came_from();
    chasing_and_attack_are_one_state();
    a_return_going_nowhere_is_stuck_once_and_then_unstuck();
    walking_steadily_is_not_stuck();
    standing_in_its_slot_is_not_stuck();
    a_squad_says_who_holds_it_and_on_what();
    a_dead_agent_is_gone();
    lines_past_the_budget_are_counted_not_written();
    the_file_has_a_wall_clock_on_each_line();
    std::puts("ai_trace_test: ok");
    return 0;
}
