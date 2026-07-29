#ifndef SIMULATION_SRC_SYSTEMS_H_
#define SIMULATION_SRC_SYSTEMS_H_

#include <cstdint>
#include <vector>

#include "ai_intent.h"
#include "kernel/public/kernel_types.h"
#include "world/public/components.h"

namespace network_example {

class KernelEngine;
struct ActionGraphCommandBatch;

bool execute_action_graph_command_batch(
    KernelEngine& engine,
    const ActionGraphCommandBatch& batch,
    std::uint64_t server_time_us);
struct ConfirmedDamage;

class EntityLifecycleSystem {
public:
    bool create_entity(
        KernelEngine& engine,
        const KernelServerEntityCreateInfo& create_info,
        NetId* out_net_id,
        bool publish_snapshot = true) const;

    bool destroy_entity(
        KernelEngine& engine,
        NetId net_id,
        std::uint32_t reason) const;

    void process_health_depleted(
        KernelEngine& engine,
        const std::vector<ConfirmedDamage>& health_depleted,
        std::uint64_t server_time_us) const;

    void destroy_dead_entities(
        KernelEngine& engine,
        const std::vector<ConfirmedDamage>& health_depleted) const;

private:
    bool destroy_entity_with_context(
        KernelEngine& engine,
        NetId net_id,
        std::uint32_t reason,
        NetId instigator,
        std::uint8_t source_code,
        const glm::vec3* event_position) const;
};

class ActivationSystem {
public:
    bool activate_entity(
        KernelEngine& engine,
        const KernelServerEntityActivateInfo& activate_info) const;
};

class CollisionTriggerSystem {
public:
    void update(KernelEngine& engine, std::uint64_t server_time_us) const;
};

class EntityStateSystem {
public:
    bool set_actor_template(
        KernelEngine& engine,
        NetId net_id,
        std::uint32_t actor_template_id) const;
    bool set_transform(
        KernelEngine& engine,
        NetId net_id,
        const KernelVec3& position,
        const KernelQuat& rotation) const;
    bool set_velocity(
        KernelEngine& engine,
        NetId net_id,
        const KernelVec3& velocity) const;
    bool set_state(
        KernelEngine& engine,
        NetId net_id,
        std::uint16_t animation_state,
        std::uint32_t visual_flags) const;
};

class MovementSystem {
public:
    bool submit_player_input(
        KernelEngine& engine,
        NetId net_id,
        const KernelPlayerInput& input) const;
};

struct DirectorIntentExecutionResult {
    ai::IntentStatus status = ai::IntentStatus::kFailed;
    bool unsupported = false;
    std::uint32_t created_count = 0;
};

class DirectorIntentExecutor {
public:
    DirectorIntentExecutionResult execute(
        KernelEngine& engine,
        const ai::ScopedIntent& intent) const;

    void update(KernelEngine& engine) const;
};

class DirectorAISystem {
public:
    void update(KernelEngine& engine) const;
};

}  // namespace network_example

#endif  // SIMULATION_SRC_SYSTEMS_H_
