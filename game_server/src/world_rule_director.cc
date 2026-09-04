#include "game_server/src/world_rule_director.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <spdlog/spdlog.h>

#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {
namespace {

constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
// The golden angle, which is what the kernel spread these on. Kept exactly:
// changing it would move every spawn point in every existing catalog.
constexpr float kGoldenAngleRadians = 2.39996323f;

}  // namespace

WorldRuleDirector::WorldRuleDirector(std::vector<WorldRuleSpawnConfig> rules)
    : rules_(std::move(rules)), runtimes_(rules_.size()) {}

void WorldRuleDirector::tick(
    KernelHandle* kernel,
    std::uint32_t live_agent_count) {
    if (kernel == nullptr) {
        return;
    }
    for (std::size_t index = 0; index < rules_.size(); ++index) {
        const WorldRuleSpawnConfig& rule = rules_[index];
        RuleRuntime& runtime = runtimes_[index];
        if (rule.target_count == 0 ||
            (rule.spawn_entity_template_id == 0u &&
             rule.spawn_actor_template_id == 0u)) {
            continue;
        }
        // Counted every tick, satisfied or not. The kernel held an absolute
        // next_tick, so its interval was wall clock rather than time spent
        // short -- a rule that has been at strength for longer than its
        // interval replaces a casualty on the tick it appears, instead of
        // waiting out an interval that only starts when something dies.
        if (runtime.ticks_until_spawn > 0) {
            --runtime.ticks_until_spawn;
        }
        if (rule.target_count <= live_agent_count ||
            runtime.ticks_until_spawn > 0) {
            continue;
        }

        const std::uint32_t missing = rule.target_count - live_agent_count;
        std::uint32_t created = 0;
        for (std::uint32_t offset = 0; offset < missing; ++offset) {
            const float angle =
                static_cast<float>(runtime.spawn_cursor + offset) *
                kGoldenAngleRadians;
            KernelServerEntityCreateInfo create_info{};
            create_info.struct_size = sizeof(create_info);
            create_info.owner_peer = 0;
            create_info.position = KernelVec3{
                rule.position.x + std::cos(angle) * rule.radius,
                rule.position.y,
                rule.position.z + std::sin(angle) * rule.radius,
            };
            create_info.rotation = kIdentityRotation;
            if (rule.spawn_entity_template_id != 0u) {
                create_info.entity_template_id = rule.spawn_entity_template_id;
            } else {
                create_info.entity_type = KernelEntityType_Actor;
                create_info.actor_type = KernelActorType_Agent;
                create_info.actor_template_id = rule.spawn_actor_template_id;
            }
            std::uint32_t net_id = 0;
            if (!Kernel_ServerCreateEntity(kernel, &create_info, &net_id) ||
                net_id == 0) {
                spdlog::warn(
                    "world rule spawn failed rule={} template_id={}",
                    rule.name,
                    rule.spawn_entity_template_id != 0u
                        ? rule.spawn_entity_template_id
                        : rule.spawn_actor_template_id);
                break;
            }
            ++created;
        }

        if (created == 0) {
            continue;
        }
        runtime.spawn_cursor += created;
        runtime.ticks_until_spawn = std::max<std::uint32_t>(1u, rule.tick_interval);
        spawned_agent_count_ += created;
        // Counted against the rules after this one in the same pass, so two
        // rules do not both fill the same shortfall.
        live_agent_count += created;
        spdlog::info(
            "world rule spawned rule={} count={} target={}",
            rule.name,
            created,
            rule.target_count);
    }
}

const std::vector<WorldRuleSpawnConfig>& WorldRuleDirector::rules() const {
    return rules_;
}

std::uint32_t WorldRuleDirector::spawned_agent_count() const {
    return spawned_agent_count_;
}

}  // namespace network_example::game_server
