#ifndef SIMULATION_SRC_SYSTEMS_H_
#define SIMULATION_SRC_SYSTEMS_H_

#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

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
void simulate_status_effects(KernelEngine& engine, std::uint64_t server_time_us);
// Arms the drop for every actor whose last status suspension has ended since
// the last call: straight down, out of its own control, until it lands (a
// free-fall ImpulseLockout), with the anchor that lets a client draw it. Runs
// ahead of movement each tick.
void settle_status_suspensions(KernelEngine& engine);
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

    void update_prop_lifetimes(KernelEngine& engine) const;

    // Everything that has fallen below the kill floor. A
    // living actor's health is zeroed and its death appended to
    // health_depleted, so the ordinary death flow runs on it this tick; a
    // prop, or an actor with no health to lose, is destroyed here. Runs after
    // movement, ahead of process_health_depleted.
    void reclaim_fallen_entities(
        KernelEngine& engine,
        std::uint64_t server_time_us,
        std::vector<ConfirmedDamage>* health_depleted) const;

    void process_health_depleted(
        KernelEngine& engine,
        const std::vector<ConfirmedDamage>& health_depleted,
        std::uint64_t server_time_us) const;

    // Every entity damage emptied this tick: EntityDied, and the corpse is
    // stopped. Runs before destroy_dead_entities, for both death policies.
    void enter_death_state(
        KernelEngine& engine,
        const std::vector<ConfirmedDamage>& health_depleted) const;

    void destroy_dead_entities(
        KernelEngine& engine,
        const std::vector<ConfirmedDamage>& health_depleted) const;

    static bool stays_dormant_on_death(
        const entt::registry& registry,
        entt::entity entity);

private:
    bool destroy_entity_with_context(
        KernelEngine& engine,
        NetId net_id,
        std::uint32_t reason,
        NetId instigator,
        std::uint8_t source_code,
        const glm::vec3* event_position,
        bool execute_destroy_graph = true) const;

    void enforce_prop_population_limit(
        KernelEngine& engine,
        std::uint32_t population_group_id,
        NetId spawned_net_id) const;
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

// Trigger volumes (ABI 103): which actors are in each, and the enter, overlap
// and exit graphs that follow from what changed since the last tick.
class OverlapTriggerSystem {
public:
    void update(KernelEngine& engine, std::uint64_t server_time_us) const;
};

class EntityStateSystem {
public:
    // A portal's teleport action (ABI 103); see
    // KernelEntityTriggerActionType_Teleport. Does nothing to a dead,
    // sheltered or non-actor target.
    void teleport_through_portal(
        KernelEngine& engine,
        NetId portal_net_id,
        NetId target_net_id) const;
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
    // Brings a dead entity back where its body lies: full health, lifted by as
    // much of `lift_meters` as its capsule has headroom for, with nothing of
    // the old life left on it, and immune to damage for `invulnerable_ticks`.
    // False, and nothing touched, unless the entity is actually dead.
    bool revive(
        KernelEngine& engine,
        NetId net_id,
        float lift_meters,
        std::uint32_t invulnerable_ticks) const;
    // Puts an actor inside a building, or (shelter_net_id 0) takes it out.
    // See Kernel_ServerEnqueueEntityShelter for every rule. False, and nothing
    // touched, when the change is refused.
    bool set_shelter(
        KernelEngine& engine,
        NetId net_id,
        NetId shelter_net_id) const;
    // Launches an agent on a leap. See Kernel_ServerEnqueueEntityLeap for
    // every rule. False, and nothing touched, when the leap is refused.
    bool start_leap(
        KernelEngine& engine,
        NetId net_id,
        const KernelVec3& destination,
        std::uint32_t airtime_ticks) const;
    // Starts an agent on a charge. See Kernel_ServerEnqueueEntityCharge for
    // every rule. False, and nothing touched, when the charge is refused.
    bool start_charge(
        KernelEngine& engine,
        NetId net_id,
        const KernelVec3& direction,
        float speed_meters_per_second,
        std::uint32_t duration_ticks,
        std::uint8_t weapon_id) const;
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

}  // namespace network_example

#endif  // SIMULATION_SRC_SYSTEMS_H_
