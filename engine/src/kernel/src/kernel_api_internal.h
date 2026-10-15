#ifndef KERNEL_SRC_KERNEL_API_INTERNAL_H_
#define KERNEL_SRC_KERNEL_API_INTERNAL_H_

#include <stdbool.h>
#include <stdint.h>

#include "kernel/public/kernel_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum KernelCommandSource {
    KernelCommandSource_Internal = 0,
    KernelCommandSource_PlayerInput = 1,
    KernelCommandSource_AI = 2,
    KernelCommandSource_ControlPlane = 3,
    KernelCommandSource_Test = 4,
} KernelCommandSource;

typedef enum KernelEntityLifecycleCommandType {
    KernelEntityLifecycleCommandType_Destroy = 1,
    /*
     * Reserved for future async create support. Current enqueue lifecycle
     * implementation rejects Create because create needs result/net_id handling.
     */
    KernelEntityLifecycleCommandType_Create = 2,
} KernelEntityLifecycleCommandType;

typedef struct KernelEntityLifecycleCommand {
    uint32_t struct_size;
    uint32_t command_type;
    uint32_t net_id;
    uint32_t reason;
    /*
     * Reserved for future KernelEntityLifecycleCommandType_Create support.
     * Current version only supports Destroy and ignores create_info.
     */
    KernelServerEntityCreateInfo create_info;
} KernelEntityLifecycleCommand;

KERNEL_RPC_STRUCT(R"json({"type":"KernelRpcEntityStateView"})json")
typedef struct KernelRpcEntityStateView {
    uint32_t struct_size;
    uint32_t net_id;
    uint16_t entity_type;
    uint16_t actor_type;
    uint32_t owner_peer;
    KernelVec3 position;
    KernelQuat rotation;
    KernelVec3 velocity;
    uint16_t hp;
    uint16_t max_hp;
    uint16_t animation_state;
    uint32_t visual_flags;
    uint32_t valid;
    uint32_t actor_template_id;
} KernelRpcEntityStateView;

KERNEL_RPC_INTERNAL(R"json({
  "method":"dev.ping",
  "authority":"developer_read_only",
  "phase":"immediate_read_only"
})json")
bool KernelRpc_DevPing(KernelHandle* kernel);

KERNEL_RPC_INTERNAL(R"json({
  "method":"dev.list_methods",
  "authority":"developer_read_only",
  "phase":"immediate_read_only",
  "result_schema":{
    "type":"object",
    "properties":{"methods":{"type":"array","items":{"type":"object"}}},
    "required":["methods"],
    "additionalProperties":false
  }
})json")
bool KernelRpc_DevListMethods(KernelHandle* kernel);

KERNEL_RPC_INTERNAL(R"json({
  "method":"dev.describe_method",
  "authority":"developer_read_only",
  "phase":"immediate_read_only",
  "params_schema":{
    "type":"object",
    "properties":{"method":{"type":"string"}},
    "required":["method"],
    "additionalProperties":false
  },
  "result_schema":{"type":"object"}
})json")
bool KernelRpc_DevDescribeMethod(KernelHandle* kernel);

KERNEL_RPC_INTERNAL(R"json({
  "method":"dev.get_schema",
  "authority":"developer_read_only",
  "phase":"immediate_read_only",
  "result_schema":{
    "type":"object",
    "properties":{"schema":{"type":"object"}},
    "required":["schema"],
    "additionalProperties":false
  }
})json")
bool KernelRpc_DevGetSchema(KernelHandle* kernel);

KERNEL_RPC_INTERNAL(R"json({
  "method":"world.get_entity_state",
  "authority":"developer_read_only",
  "phase":"immediate_read_only",
  "params":[
    {"name":"net_id","type":"uint32_t","passing":"value"}
  ]
})json")
bool KernelRpc_GetEntityState(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelRpcEntityStateView* out_state);

/*
 * Internal/experimental game_server migration bridge. Enqueues a lifecycle
 * command for the next simulation tick. Current version supports Destroy only;
 * Create is reserved for future async result/net_id handling.
 */
bool Kernel_ServerEnqueueEntityLifecycle(
    KernelHandle* kernel,
    uint32_t command_source,
    const KernelEntityLifecycleCommand* command);

/*
 * Internal/experimental game_server migration bridge. Enqueues non-lifecycle
 * entity mutation commands for the next simulation tick.
 */
bool Kernel_ServerEnqueueEntityTransform(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    const KernelVec3* position,
    const KernelQuat* rotation);

bool Kernel_ServerEnqueueEntityVelocity(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    const KernelVec3* velocity);

bool Kernel_ServerEnqueueEntityState(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    uint16_t animation_state,
    uint32_t visual_flags);

/*
 * Puts an actor inside a building, or takes it out (shelter_net_id 0), on the
 * next simulation tick. Inside, the actor stands at the building's origin
 * under a terrain-only movement mask, takes no damage, stagger, impulse or
 * pull, starts no action, drops what it carries, and may make one gameplay
 * request: activating that building. Leaving puts it on the nearest clear spot
 * around the building -- where it came in from if none is clear -- and only
 * then restores the mask it had.
 *
 * Entering is refused, and nothing changes, unless the actor is a live actor
 * with movement, outside every building and not in a knockback, and the
 * building is a prop with room: fewer actors in it than its template's
 * shelter_capacity, KERNEL_SHELTER_CAPACITY when that is 0.
 * Each change that happens emits KernelEventType_ShelterChanged.
 *
 * A building that is destroyed, for any reason, lets everyone out the same way
 * first, while it still stands -- so their ShelterChanged events come before
 * its EntityDestroyed, and its on_destroy graph finds them outside.
 */
bool Kernel_ServerEnqueueEntityShelter(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    uint32_t shelter_net_id);

/*
 * Launches an agent on a leap on the next simulation tick: a flight solved by
 * leap_launch_velocity to put it down on `destination` on its airtime_ticks-th
 * movement step. The arming tick's own step is the first, so an agent armed on
 * tick T lands on tick T + airtime_ticks - 1. Its destination height is the
 * ground found under `destination`, not destination.y, which only says where
 * to look -- within 4 m above and below it, terrain and static obstacles
 * only, so whoever stands there now is not taken for the floor.
 *
 * Until it lands the authority steers it, not its controller: movement input
 * and velocity writes do not change the flight. Landing hands it straight
 * back -- no knockdown recovery -- and nothing is refused meanwhile: it takes
 * damage, dies, and may start or carry on an action in the air. Anything that
 * moves the actor itself ends the leap where it stands: an impulse, a pull, a
 * suspension, a teleport, death or revive. Remote clients replay it from an
 * ActorImpulseRecord flagged as a leap, and see KERNEL_VISUAL_FLAG_LEAPING
 * while it lasts.
 *
 * Refused, and nothing changes, unless the actor is a live agent (not a player)
 * on the ground under a character controller with gravity, in no building, no
 * knockback, no suspension and no leap already; airtime_ticks is 2 ..
 * KERNEL_MAX_IMPULSE_LOCKOUT_TICKS; there is ground under the destination; and
 * the flight rises on its first step -- a drop off a ledge given too little
 * airtime would be caught by the ground it stands on.
 */
bool Kernel_ServerEnqueueEntityLeap(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    const KernelVec3* destination,
    uint32_t airtime_ticks);

/*
 * Starts an agent on a charge on the next simulation tick: a run along
 * `direction` (flattened to the ground plane) at speed_meters_per_second, for
 * at most duration_ticks movement steps, the arming tick's own step first.
 * Every tick of it, after movement, the loadout weapon `weapon_id` -- a melee
 * weapon -- tests its cone from where the charger now stands, facing the way
 * it runs, and strikes each actor in it once per charge: the shot template's
 * damage and stagger, and its impact graph (a knockback) on top.
 *
 * Until it ends the authority steers it, not its controller: movement input
 * and velocity writes do not change the run. It ends
 *   - after the step of the tick it struck someone: a charge stops on a hit;
 *   - when the static world stops it (it covers under half of a step's
 *     distance with no actor in the way): the charger is staggered, on its
 *     own template's stagger profile, as if its meter had filled;
 *   - when another actor's body stops it without a hit: it just stops;
 *   - when it is staggered: it stops dead, as a stagger roots anyone;
 *   - after duration_ticks steps;
 * and, like a leap, on anything that moves the actor itself: an impulse, a
 * pull, a suspension, a teleport, death or revive. Clients see it only in
 * snapshots, with KERNEL_VISUAL_FLAG_CHARGING while it lasts.
 *
 * Refused, and nothing changes, unless the actor is a live agent (not a player)
 * on the ground under a character controller, in no building, no knockback, no
 * stagger, no suspension, no leap and no charge already; `direction` has a
 * horizontal part; speed is finite and above zero; duration_ticks is 1 ..
 * KERNEL_MAX_IMPULSE_LOCKOUT_TICKS; and weapon_id is a melee weapon in its
 * loadout.
 */
bool Kernel_ServerEnqueueEntityCharge(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    const KernelVec3* direction,
    float speed_meters_per_second,
    uint32_t duration_ticks,
    uint8_t weapon_id);

/*
 * Internal/experimental game_server migration bridge. Enqueues authoritative
 * entity input for the next simulation tick.
 */
bool Kernel_ServerEnqueueEntityInput(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    const KernelPlayerInput* input);

float Kernel_GetFixedDeltaSeconds(KernelHandle* kernel);

bool Kernel_ServerGetProjectileLaunchPosition(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelVec3* out_position);

bool Kernel_ServerGetEntityAimPoint(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelVec3* out_position);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // KERNEL_SRC_KERNEL_API_INTERNAL_H_
