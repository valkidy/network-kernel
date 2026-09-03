#include "game_server/src/game_rule_director.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <spdlog/spdlog.h>

#include "game_server/src/agent_runtime.h"
#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {
namespace {

constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
// Kept exactly: changing it would move every spawn point in every authored
// wave.
constexpr float kGoldenAngleRadians = 2.39996323f;

bool entity_alive(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    return Kernel_ServerGetEntityState(kernel, net_id, &state) &&
        state.valid != 0u;
}

std::uint32_t live_player_count(KernelHandle* kernel) {
    std::vector<KernelServerEntityState> states(64);
    while (true) {
        for (KernelServerEntityState& state : states) {
            state.struct_size = sizeof(KernelServerEntityState);
        }
        const std::uint32_t count = Kernel_ServerQueryEntities(
            kernel,
            kEntityTypeActor,
            states.data(),
            static_cast<std::uint32_t>(states.size()));
        // The query reports what it wrote, never what it had, so a full buffer
        // is indistinguishable from a truncated one.
        if (count >= states.size() && states.size() < 4096) {
            states.resize(states.size() * 2);
            continue;
        }
        std::uint32_t players = 0;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (states[index].valid != 0u &&
                states[index].actor_type == kActorTypePlayer) {
                ++players;
            }
        }
        return players;
    }
}

}  // namespace

GameRuleDirector::GameRuleDirector(std::vector<GameRuleConfig> rules)
    : rules_(std::move(rules)), runtimes_(rules_.size()) {}

void GameRuleDirector::activate_node(
    KernelHandle* kernel,
    const GameRuleConfig& rule,
    RuleRuntime* runtime,
    std::size_t node_offset) {
    const GameRuleNodeConfig& node = rule.nodes[node_offset];
    runtime->node_states[node_offset] = GameRuleNodeState::kActive;
    if (!node.has_spawn_effect) {
        // A node with nothing to spawn is only meaningful if it is waiting on
        // something other than a group it never filled.
        if (node.condition_type != GameRuleConditionType::kPlayerCountAtLeast) {
            runtime->status = GameRuleStatus::kFailed;
        }
        return;
    }

    const auto group = std::find_if(
        runtime->groups.begin(),
        runtime->groups.end(),
        [&node](const GroupRuntime& candidate) {
            return candidate.group_id == node.spawn.group_id;
        });
    if (group == runtime->groups.end()) {
        runtime->status = GameRuleStatus::kFailed;
        return;
    }

    for (std::uint32_t index = 0; index < node.spawn.count; ++index) {
        const float angle =
            static_cast<float>(node.spawn.seed + index) * kGoldenAngleRadians;
        KernelServerEntityCreateInfo create_info{};
        create_info.struct_size = sizeof(create_info);
        create_info.owner_peer = 0;
        create_info.entity_template_id = node.spawn.entity_template_id;
        create_info.position = KernelVec3{
            node.spawn.position.x + std::cos(angle) * node.spawn.radius,
            node.spawn.position.y,
            node.spawn.position.z + std::sin(angle) * node.spawn.radius,
        };
        create_info.rotation = kIdentityRotation;
        std::uint32_t net_id = 0;
        if (!Kernel_ServerCreateEntity(kernel, &create_info, &net_id) ||
            net_id == 0) {
            // A half-spawned wave can never be cleared, so it fails the rule
            // rather than sitting there waiting for a group that will never
            // empty because it was never filled.
            group->failed = true;
            runtime->status = GameRuleStatus::kFailed;
            spdlog::error(
                "game rule spawn failed rule={} group={} template_id={}",
                rule.name,
                node.spawn.group_id,
                node.spawn.entity_template_id);
            return;
        }
        group->member_net_ids.push_back(net_id);
    }
    spdlog::info(
        "game rule spawned rule={} group={} count={}",
        rule.name,
        node.spawn.group_id,
        node.spawn.count);
}

void GameRuleDirector::tick(KernelHandle* kernel) {
    if (kernel == nullptr) {
        return;
    }
    for (std::size_t rule_index = 0; rule_index < rules_.size(); ++rule_index) {
        const GameRuleConfig& rule = rules_[rule_index];
        RuleRuntime& runtime = runtimes_[rule_index];
        if (runtime.status != GameRuleStatus::kRunning) {
            continue;
        }
        if (runtime.ticks_until_update > 0) {
            --runtime.ticks_until_update;
            continue;
        }
        runtime.ticks_until_update = std::max<std::uint32_t>(1u, rule.tick_interval);

        if (!runtime.initialized) {
            runtime.node_states.assign(
                rule.nodes.size(), GameRuleNodeState::kInactive);
            runtime.groups.clear();
            // A group exists because some node waits on it being cleared.
            for (const GameRuleNodeConfig& node : rule.nodes) {
                if (node.condition_type ==
                    GameRuleConditionType::kGroupEliminated) {
                    runtime.groups.push_back(
                        GroupRuntime{node.condition_group_id, {}, false});
                }
            }
            runtime.initialized = true;
            // The roots: nodes nothing points at.
            for (std::size_t offset = 0; offset < rule.nodes.size(); ++offset) {
                const std::uint32_t node_id = rule.nodes[offset].node_id;
                const bool has_predecessor = std::any_of(
                    rule.nodes.begin(),
                    rule.nodes.end(),
                    [node_id](const GameRuleNodeConfig& candidate) {
                        return std::find(
                                   candidate.next_node_ids.begin(),
                                   candidate.next_node_ids.end(),
                                   node_id) != candidate.next_node_ids.end();
                    });
                if (!has_predecessor) {
                    activate_node(kernel, rule, &runtime, offset);
                }
            }
            continue;
        }

        const std::uint32_t player_count = live_player_count(kernel);
        for (std::size_t offset = 0; offset < rule.nodes.size(); ++offset) {
            if (runtime.node_states[offset] != GameRuleNodeState::kActive) {
                continue;
            }
            const GameRuleNodeConfig& node = rule.nodes[offset];
            bool completed = false;
            if (node.condition_type ==
                GameRuleConditionType::kGroupEliminated) {
                const auto group = std::find_if(
                    runtime.groups.begin(),
                    runtime.groups.end(),
                    [&node](const GroupRuntime& candidate) {
                        return candidate.group_id == node.condition_group_id;
                    });
                completed = group != runtime.groups.end() && !group->failed &&
                    std::none_of(
                        group->member_net_ids.begin(),
                        group->member_net_ids.end(),
                        [kernel](std::uint32_t net_id) {
                            return entity_alive(kernel, net_id);
                        });
            } else {
                completed = player_count >= node.condition_count;
            }
            if (completed) {
                runtime.node_states[offset] = GameRuleNodeState::kCompleted;
            }
        }

        // A node opens once every node pointing at it has completed.
        for (std::size_t offset = 0; offset < rule.nodes.size(); ++offset) {
            if (runtime.node_states[offset] != GameRuleNodeState::kInactive) {
                continue;
            }
            const std::uint32_t node_id = rule.nodes[offset].node_id;
            bool has_predecessor = false;
            bool all_completed = true;
            for (std::size_t source = 0; source < rule.nodes.size(); ++source) {
                const GameRuleNodeConfig& candidate = rule.nodes[source];
                if (std::find(
                        candidate.next_node_ids.begin(),
                        candidate.next_node_ids.end(),
                        node_id) == candidate.next_node_ids.end()) {
                    continue;
                }
                has_predecessor = true;
                if (runtime.node_states[source] != GameRuleNodeState::kCompleted) {
                    all_completed = false;
                }
            }
            if (has_predecessor && all_completed) {
                activate_node(kernel, rule, &runtime, offset);
            }
        }

        if (runtime.status == GameRuleStatus::kRunning &&
            std::all_of(
                runtime.node_states.begin(),
                runtime.node_states.end(),
                [](GameRuleNodeState state) {
                    return state == GameRuleNodeState::kCompleted;
                })) {
            runtime.status = GameRuleStatus::kCompleted;
            spdlog::info("game rule completed rule={}", rule.name);
        }
    }
}

const std::vector<GameRuleConfig>& GameRuleDirector::rules() const {
    return rules_;
}

const std::vector<GameRuleDirector::RuleRuntime>&
GameRuleDirector::runtimes() const {
    return runtimes_;
}

}  // namespace network_example::game_server
