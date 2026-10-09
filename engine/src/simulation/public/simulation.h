#ifndef SIMULATION_PUBLIC_SIMULATION_H_
#define SIMULATION_PUBLIC_SIMULATION_H_

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "kernel/public/kernel_types.h"
#include "physics/public/physics_world.h"
#include "simulation/public/ground_follow.h"
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
    // Where a melee swing's impact graphs go. Left null, a swing that names one
    // silently does nothing -- which is why apply_melee_damage falls back to
    // submitting plain damage when there is nowhere to put the batches.
    std::vector<ActionGraphCommandBatch>* action_graph_batches = nullptr;
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
    // The multiplier carried by the volume that was hit, in hundredths. Carried
    // here but not yet applied to `damage`; see kHitZoneUnscaled.
    std::uint16_t hit_zone = kHitZoneUnscaled;
    // Explicit stagger meter this hit adds; negative derives it from damage.
    float stagger = kStaggerDerivedFromDamage;
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
    std::uint16_t hit_zone = kHitZoneUnscaled;
    float stagger = kStaggerDerivedFromDamage;
};

// Applies a volume's multiplier to a damage amount.
//
// Integer throughout: hit_zone is hundredths, so this is (damage * zone + 50)
// / 100, which rounds half away from zero without going near a float. 45 at 0.5
// is 23 rather than 22 -- rounding down would make every halved hit quietly
// cheaper than the number says.
//
// A zone of zero yields zero, which is the authored way to say a volume is
// harmless to hit. That is also why nothing may reach here with an unset zone;
// see kHitZoneUnscaled.
std::uint16_t scale_damage_by_hit_zone(
    std::uint16_t damage,
    std::uint16_t hit_zone);

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
    const glm::vec3& hit_position,
    std::uint16_t hit_zone = kHitZoneUnscaled);

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

// A ground-following area effect's solver settings: its authored ride, its
// horizontal travel, and the static-world layers that stop it. Every caller --
// the spawn paths, the per-tick advance and a client predicting the same field
// -- builds them here, so they cannot disagree about what the field is.
ground_follow::Config area_ground_follow_config(
    const AreaEffectGroundFollow& ground_follow,
    const glm::vec3& initial_velocity,
    std::uint32_t motion_collision_mask);

// The horizontal launch velocity of a ground-following field: the heading's
// horizontal part at full speed. Its height comes from the ground, so aiming
// up or down changes where it goes, not how fast.
glm::vec3 ground_following_launch_velocity(
    const glm::vec3& direction,
    float speed);

// Where a ground-following field starts: settled onto the ground under its
// spawn point, then stepped catch_up_ticks ticks forward for a spawn that is
// already that old. Without a physics world it stays where it spawned.
ground_follow::State ground_following_spawn_state(
    const physics::PhysicsWorld* physics_world,
    const ground_follow::Config& config,
    const glm::vec3& spawn_position,
    std::uint32_t catch_up_ticks,
    float fixed_delta_seconds);

// Where an actor under an ImpulseLockout is `elapsed_seconds` after a sample
// of it at `origin` moving at `velocity`, until something stops it. While the
// lockout stands the movement step keeps the horizontal velocity it was given
// and an airborne actor adds gravity before it moves -- semi-implicit Euler --
// so the height after n ticks is vy*n*dt + g*dt^2*n(n+1)/2, not the textbook
// parabola. This is that sum with n = t/dt, exact on every tick boundary and
// continuous between them. It knows nothing about the ground; the caller
// floors it.
glm::vec3 knockback_flight_position_at(
    const glm::vec3& origin,
    const glm::vec3& velocity,
    float gravity_y,
    float fixed_delta_seconds,
    float elapsed_seconds);

glm::vec3 knockback_flight_velocity_at(
    const glm::vec3& velocity,
    float gravity_y,
    float elapsed_seconds);

// How many ticks after a sample the flight still belongs to the lockout: the
// tick it comes back down to floor_y if it lands first, since the authority
// releases on the first landing after the arming tick, else the last tick
// before the lockout expires. A flight that starts on the floor -- a flat
// knockback -- does not land and slides out the whole lockout.
std::uint32_t knockback_flight_ticks(
    const glm::vec3& position,
    const glm::vec3& velocity,
    float gravity_y,
    float floor_y,
    float fixed_delta_seconds,
    std::uint32_t lockout_ticks);

// Where a launch rule starts a projectile, and how fast.
struct ProjectileLaunch {
    glm::vec3 origin{0.0f};
    glm::vec3 velocity{0.0f};
};

// The directions a projectile weapon's one commit fires, in the order the
// authority spawns them: index i is that commit's burst_index i. A client
// predicting its own shots calls the same function, so its pellets leave on
// the very directions the authority's do.
std::vector<glm::vec3> projectile_burst_directions(
    const glm::vec3& direction,
    const WeaponMechanicsDefinition& definition);

// Mixes the facts that identify one launch into a seed. Integers only, so the
// same inputs give the same seed on every platform.
std::uint64_t projectile_launch_seed(
    NetId instigator,
    std::uint32_t action_instance_id,
    std::uint32_t projectile_template_id,
    std::uint32_t salt);

// The descent rule: falls onto `target` from `launch_height` above it, from an
// azimuth and at an elevation (within the template's range) both picked by
// `seed`, arriving after `launch_fall_ticks`. Nothing else steers it -- in
// particular not the spawn direction -- so anyone holding the target and the
// seed derives the same path. `target` is first dropped onto the ground under
// it when `ground` is given and has any (terrain or static obstacle) within
// the fall height; otherwise it is used as is.
ProjectileLaunch descent_launch(
    const RuntimeProjectileTemplate& projectile_template,
    const glm::vec3& target,
    std::uint64_t seed,
    float fixed_delta_seconds,
    const physics::PhysicsWorld* ground);

// Spawns `projectile_template` at `position` facing `direction`, applying its
// launch rule, and reports the spawn in `events`. For callers that decided the
// point themselves, such as a targeted strike's resolved landing point.
bool spawn_projectile_at(
    World& world,
    const RuntimeProjectileTemplate& projectile_template,
    PeerId owner_peer,
    NetId shooter_net_id,
    std::uint8_t weapon_id,
    std::uint32_t action_instance_id,
    const glm::vec3& position,
    const glm::vec3& direction,
    std::uint32_t current_tick,
    float fixed_delta_seconds,
    std::vector<KernelEvent>* events);

// lifetime_ticks zero keeps the template's; extra_lifetime_ticks is added
// either way. launch_salt is the command's provenance salt.
bool spawn_action_graph_projectile(
    World& world,
    std::uint32_t projectile_template_id,
    PeerId owner_peer,
    NetId instigator,
    std::uint32_t action_instance_id,
    const glm::vec3& position,
    const glm::vec3& direction,
    std::uint32_t current_tick,
    float fixed_delta_seconds,
    std::uint32_t lifetime_ticks = 0,
    std::uint32_t extra_lifetime_ticks = 0,
    std::uint32_t launch_salt = 0);

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
        std::uint16_t hit_zone = kHitZoneUnscaled;
        bool canceled = false;
        bool parry_applied = false;
        float stagger = kStaggerDerivedFromDamage;
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

// How much of `lift` metres straight up the entity's movement capsule can
// travel from `position` before its head meets something, less a small skin.
// Returns `lift` unchanged when there is no physics world or no movement
// capsule to sweep, and never less than zero.
// The height of whatever the entity's movement capsule would come to rest on
// straight below `position`, looking at most `max_distance` down, or nothing
// when it finds nothing (or there is no physics world or capsule to ask).
std::optional<float> ground_height_below(
    World& world,
    NetId net_id,
    const glm::vec3& position,
    const glm::quat& rotation,
    float max_distance);

// Arms the drop for every actor whose last status suspension has ended since
// the last call: velocity zeroed, straight down, out of its own control, until
// it lands -- a free-fall ImpulseLockout whose ceiling is the fall to the floor
// found below. Returns each one's net id and that floor height, for the
// knockback anchor that lets a client draw the drop.
std::vector<std::pair<NetId, float>> settle_status_suspensions(
    World& world,
    std::uint32_t current_tick,
    float fixed_delta_seconds);

float available_lift(
    World& world,
    NetId net_id,
    const glm::vec3& position,
    const glm::quat& rotation,
    float lift);

// Somewhere just outside a footprint of `footprint_radius` metres around
// `center` that the entity's movement capsule can stand: on terrain or a
// static obstacle, touching no terrain, static obstacle or other actor. The
// first ring clears the footprint by the capsule's radius and a margin; a
// second ring lies a metre further out. Headings are tried nearest
// `preferred_direction` first. nullopt when there is no physics world, no
// movement capsule, or nothing clear -- the caller picks the fallback.
// Where an entity just spawned at `position` can stand clear, for a spawn
// that asked for clear placement (KERNEL_SPAWN_PLACEMENT_CLEAR). Its own box
// hit volume is looked for on terrain or a static obstacle, at `position` and
// then backing off against `direction` -- the way it came -- a step at a time,
// touching no terrain or static obstacle. A bottle that struck a wall's face
// is set down in front of the wall rather than half inside it. nullopt when
// there is no physics world, no box hit volume, or nothing clear within reach:
// the caller keeps `position`. Best effort, not a guarantee. `spawner`'s own
// volumes are looked through: the bottle that spawned it is still there.
std::optional<glm::vec3> find_clear_spawn_spot(
    World& world,
    NetId net_id,
    const glm::vec3& position,
    const glm::vec3& direction,
    NetId spawner);

std::optional<glm::vec3> find_clear_standing_spot(
    World& world,
    NetId net_id,
    const glm::vec3& center,
    float footprint_radius,
    const glm::vec3& preferred_direction);

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
// The same, handing a trigger batch that does more than spawn projectiles --
// an apply_status on what a bolt struck -- to `forwarded_batches` for the
// engine to execute, instead of dropping it.
void simulate_projectiles(
    World& world,
    float fixed_delta_seconds,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline,
    std::vector<ActionGraphCommandBatch>* forwarded_batches);
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
    DamagePipeline* damage_pipeline,
    std::vector<ActionGraphCommandBatch>* forwarded_batches = nullptr);

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

// Feeds one landed hit into the target's stagger meter. `explicit_stagger` is
// the hit's authored contribution; a negative value derives it from `damage`
// through the target's StaggerProfile. Returns true when this hit triggered a
// stagger. Targets without a StaggerProfile, dead targets, targets already
// staggered and targets inside their post-stagger immunity are left alone.
bool apply_stagger(
    World& world,
    NetId target_net_id,
    std::uint16_t damage,
    float explicit_stagger,
    PeerId source_peer,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events);

bool is_staggered(const World& world, entt::entity entity, std::uint32_t current_tick);

// Some active status instance on the actor ran apply_block_actions.
bool status_blocks_actions(const World& world, entt::entity entity);


// Gives the actor its template's StaggerProfile, or takes it away when the
// template authors none. Both ways an actor gets a template call this: the
// entity-create path and set_actor_template, which is the only one a player
// ever goes through.
// Puts the template's knockdown recovery on an actor, or takes it off.
void apply_knockdown_profile(
    World& world,
    entt::entity entity,
    const KernelEntityTemplateDefinition& entity_template);
void apply_stagger_profile(
    World& world,
    entt::entity entity,
    const KernelEntityTemplateDefinition& entity_template);

// Ends any stagger outright: the state and the replicated flag with it. The
// flag is only ever cleared by the action pass walking StaggerState, so
// removing the state without this leaves the flag raised for good.
void clear_stagger(World& world, entt::entity entity);

// Why this actor may not start a new action right now, or
// KernelLocalActionResultReason_None. Sheltered outranks the rest: inside a
// building nothing else can be happening to it. A status block comes next: it
// is what a bubble or a stun holds the actor with for its whole duration, so
// it is the answer while it stands. Staggered outranks KnockedBack so a hit
// that does both reports the one that also interrupted.
KernelLocalActionResultReason action_block_reason(
    const World& world,
    entt::entity entity,
    std::uint32_t current_tick);

std::vector<ConfirmedDamage> apply_damage_applications(
    World& world,
    const std::vector<ConfirmedDamage>& damage_applications,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events);

}  // namespace network_example

#endif  // SIMULATION_PUBLIC_SIMULATION_H_
