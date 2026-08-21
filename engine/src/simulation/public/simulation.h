#ifndef SIMULATION_PUBLIC_SIMULATION_H_
#define SIMULATION_PUBLIC_SIMULATION_H_

#include <cstdint>
#include <vector>

#include "kernel/public/kernel_types.h"
#include "physics/public/physics_world.h"
#include "sync/public/history_buffer.h"
#include "world/public/world.h"

namespace network_example {

class DamagePipeline;
struct ActionGraphCommandBatch;

struct QueuedInput {
    PeerId owner_peer = 0;
    KernelPlayerInput input{};
    std::uint32_t received_server_tick = 0;
    std::uint64_t action_server_time_us = 0;
    bool has_action_server_time = false;
    NetId controlled_net_id = 0;
};

struct WeaponSimulationContext {
    const HistoryBuffer* history_buffer = nullptr;
    const HistoryFrame* rewind_frame = nullptr;
    DamagePipeline* damage_pipeline = nullptr;
    std::uint32_t rewind_tick = 0;
    std::uint32_t current_tick = 0;
    float fixed_delta_seconds = 0.0f;
    std::uint64_t action_time_us = 0;
    std::vector<struct ActionOutcome>* action_outcomes = nullptr;
};

struct DamageRequest {
    std::uint32_t server_tick = 0;
    std::uint32_t sequence_id = 0;
    NetId source_net_id = 0;
    NetId target_net_id = 0;
    PeerId source_peer = 0;
    std::uint8_t source_code = 0;
    std::uint16_t damage = 0;
    std::uint64_t hit_time_us = 0;
    glm::vec3 hit_position{0.0f, 0.0f, 0.0f};
};

struct ConfirmedDamage {
    std::uint32_t server_tick = 0;
    std::uint32_t sequence_id = 0;
    NetId source_net_id = 0;
    NetId target_net_id = 0;
    PeerId source_peer = 0;
    std::uint8_t source_code = 0;
    std::uint16_t damage = 0;
    std::uint64_t hit_time_us = 0;
    glm::vec3 hit_position{0.0f, 0.0f, 0.0f};
};

// The two ways a DamageRequest comes into being, so that its fields are set in
// one place each rather than at eleven aggregate initialisations across six
// files. Positional init of a ten-field struct is how a new field silently
// takes a zero at ten call sites and a real value at one.
//
// A hit volume caused this damage. The target and the impact point are read off
// the hit rather than restated, which is also what stops a request from naming
// one entity while pointing at another's geometry.
DamageRequest damage_request_from_hit(
    std::uint32_t server_tick,
    std::uint32_t sequence_id,
    NetId source_net_id,
    PeerId source_peer,
    std::uint8_t source_code,
    std::uint16_t damage,
    std::uint64_t hit_time_us,
    const physics::CollisionHit& hit);

// Damage with no collision result behind it: an action graph decided it, or a
// historical sweep resolved it against a rewound volume. The caller states the
// target and the position because there is nothing to read them from.
DamageRequest damage_request_at(
    std::uint32_t server_tick,
    std::uint32_t sequence_id,
    NetId source_net_id,
    NetId target_net_id,
    PeerId source_peer,
    std::uint8_t source_code,
    std::uint16_t damage,
    std::uint64_t hit_time_us,
    const glm::vec3& hit_position);

glm::vec3 projectile_launch_position(const Transform& transform);

// Maps a beam's aim onto the local +Z its collider template and presentation
// prefabs are both built along -- half_extents.z is where an oriented-box
// template states a beam's reach. Every consumer of a beam's rotation has to
// agree on this axis, including the client rebuilding a replicated beam from
// its shooter's aim, which is why it does not live inside beam_system.cc.
glm::quat beam_rotation(const glm::vec3& direction);

glm::vec3 projectile_position_at(
    const glm::vec3& origin,
    const glm::vec3& initial_velocity,
    ProjectileMotionModel motion_model,
    const glm::vec3& gravity,
    float elapsed_seconds);

glm::vec3 projectile_velocity_at(
    const glm::vec3& initial_velocity,
    ProjectileMotionModel motion_model,
    const glm::vec3& gravity,
    float elapsed_seconds);

bool spawn_action_graph_projectile(
    World& world,
    std::uint32_t projectile_template_id,
    PeerId owner_peer,
    NetId instigator,
    std::uint32_t action_instance_id,
    const glm::vec3& position,
    const glm::vec3& direction,
    std::uint32_t current_tick,
    float fixed_delta_seconds);

std::vector<physics::CollisionHit> query_projectile_collision_hits(
    const physics::PhysicsWorld& collision_world,
    const ProjectileState& projectile,
    const glm::vec3& previous_position,
    const glm::vec3& current_position,
    const physics::CollisionQueryFilter& filter);

class DamagePipeline {
public:
    static constexpr std::uint64_t kGraceWindowUs = 100000;
    static constexpr std::uint64_t kDefensiveActionWindowUs = 100000;

    void clear();
    void ingest_defensive_input(
        PeerId owner_peer,
        const KernelPlayerInput& input,
        std::uint64_t received_server_time_us,
        std::uint64_t action_server_time_us = 0,
        bool has_action_server_time = false);
    bool submit_damage_request(const DamageRequest& request);
    bool submit_hit(
        const World& world,
        NetId target_net_id,
        NetId source_net_id,
        PeerId source_peer,
        std::uint8_t source_code,
        std::uint16_t damage,
        std::uint64_t hit_time_us);
    std::vector<ConfirmedDamage> drain_ready_damage(
        const World& world,
        std::uint64_t server_time_us);
    void confirm_ready(
        World& world,
        std::uint64_t server_time_us,
        std::uint32_t current_tick,
        std::vector<KernelEvent>* events);
    std::uint32_t pending_count() const;

private:
    enum class DefensiveActionType {
        kDodge,
        kParry,
    };

    struct DefensiveAction {
        PeerId owner_peer = 0;
        DefensiveActionType type = DefensiveActionType::kDodge;
        std::uint64_t action_time_us = 0;
    };

    struct PendingDamage {
        NetId target_net_id = 0;
        PeerId target_peer = 0;
        NetId source_net_id = 0;
        PeerId source_peer = 0;
        std::uint8_t source_code = 0;
        std::uint16_t damage = 0;
        std::uint64_t hit_time_us = 0;
        std::uint64_t confirm_time_us = 0;
        std::uint32_t server_tick = 0;
        std::uint32_t sequence_id = 0;
        glm::vec3 hit_position{0.0f, 0.0f, 0.0f};
        bool canceled = false;
        bool parry_applied = false;
    };

    void apply_defensive_actions(PendingDamage* pending);
    void prune_defensive_actions(std::uint64_t server_time_us);

    std::vector<DefensiveAction> defensive_actions_;
    std::vector<DamageRequest> queued_damage_;
    std::vector<PendingDamage> pending_damage_;
};

void simulate_player_movement(
    World& world,
    const std::vector<QueuedInput>& inputs,
    float fixed_delta_seconds);

struct MovementSimulationStats {
    std::uint64_t grounded_query_count = 0;
    std::uint64_t grounded_query_cost_us = 0;
    std::uint64_t kinematic_move_count = 0;
    std::uint64_t kinematic_move_cost_us = 0;
    std::uint64_t character_move_count = 0;
    std::uint64_t character_move_cost_us = 0;
};

void simulate_actor_movement(
    World& world,
    const std::vector<QueuedInput>& inputs,
    float fixed_delta_seconds,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    MovementSimulationStats* stats = nullptr,
    std::uint32_t actor_blocking_mode =
        KernelActorBlockingMode_Predicted,
    std::vector<NetId>* physics_finalized_actor_net_ids = nullptr);

void simulate_velocity_movement(World& world, float fixed_delta_seconds);

void simulate_projectiles(World& world, float fixed_delta_seconds);
void simulate_projectiles(
    World& world,
    float fixed_delta_seconds,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events);
void simulate_projectiles(
    World& world,
    float fixed_delta_seconds,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline);
void simulate_area_effects(
    World& world,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline);
void simulate_area_effects(
    World& world,
    std::uint32_t current_tick,
    std::uint64_t server_time_us,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline,
    std::vector<ActionGraphCommandBatch>* action_graph_batches);
void simulate_area_effects(
    World& world,
    std::uint32_t current_tick,
    std::uint64_t server_time_us,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline);
void simulate_beams(
    World& world,
    std::uint32_t current_tick,
    float fixed_delta_seconds,
    std::uint64_t server_time_us,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline);
bool resolve_projectile_historical_hit(
    World& world,
    const HistoryBuffer& history_buffer,
    NetId projectile_net_id,
    NetId ignored_net_id,
    PeerId owner_peer,
    const ProjectileState& projectile,
    const glm::vec3& origin,
    const glm::vec3& velocity,
    std::uint32_t rewind_tick,
    std::uint32_t current_tick,
    float fixed_delta_seconds,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline);

void simulate_hitscan_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events);
void simulate_hitscan_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline);

struct ActionCommit {
    NetId controlled_net_id = 0;
    PeerId owner_peer = 0;
    std::uint8_t weapon_id = 0;
    std::uint16_t binding_id = 0;
    std::uint32_t action_template_id = 0;
    std::uint32_t action_instance_id = 0;
    std::uint16_t commit_count = 0;
    std::uint32_t authoritative_tick = 0;
    bool completes_action = false;
    glm::vec3 aim_direction{1.0f, 0.0f, 0.0f};
};

enum class ActionOutcomeType {
    Admitted,
    Committed,
    Completed,
    Corrected,
    Rejected,
};

struct ActionOutcome {
    NetId actor_net_id = 0;
    PeerId owner_peer = 0;
    std::uint32_t action_template_id = 0;
    std::uint32_t action_instance_id = 0;
    std::uint16_t binding_id = 0;
    std::uint16_t confirmed_commit_count = 0;
    std::uint32_t authoritative_tick = 0;
    ActionOutcomeType type = ActionOutcomeType::Admitted;
    KernelLocalActionResultReason reason = KernelLocalActionResultReason_None;
};

std::vector<ActionCommit> simulate_actions(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<ActionOutcome>* outcomes = nullptr);

void simulate_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events);
void simulate_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    const HistoryFrame* rewind_frame);
void simulate_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    const WeaponSimulationContext& context,
    std::vector<KernelEvent>* events);

// Whether a damage source authored to attack `attacker_collision_mask` is
// allowed to hurt `target_net_id`.
//
// This is deliberately a separate question from whether the source can *reach*
// the target. Collision filtering decides reach, and a deployable is solid to
// everyone regardless of who put it there; this decides only whether the damage
// lands. Both sides deploy the same cover, so neither can cut down its own --
// the prop's own lifetime is what removes it.
//
// A target carrying no GameplaySide may be damaged by anything. That polarity
// matches how the engine reads an absent category elsewhere (filter_accepts
// treats gameplay_category 0 as visible to every query; homing_target_is_valid
// treats it as lockable by every missile) and it is the only polarity this can
// hold: actors carry no GameplaySide at all, so reading absence as immunity
// would make every player and agent invulnerable. Indestructible is spelled by
// carrying no Health, the way interaction_terminal does.
bool damage_source_may_damage(
    const World& world,
    std::uint32_t attacker_collision_mask,
    NetId target_net_id);

std::vector<ConfirmedDamage> apply_damage_applications(
    World& world,
    const std::vector<ConfirmedDamage>& damage_applications,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events);

}  // namespace network_example

#endif  // SIMULATION_PUBLIC_SIMULATION_H_
