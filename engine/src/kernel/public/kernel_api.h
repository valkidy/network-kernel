#ifndef KERNEL_PUBLIC_KERNEL_API_H_
#define KERNEL_PUBLIC_KERNEL_API_H_

#include <stdbool.h>
#include <stdint.h>

#include "kernel/public/kernel_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KernelHandle KernelHandle;

/*
 * C ABI ownership and lifetime rules:
 * - Kernel_Create returns an opaque handle owned by the caller.
 * - Kernel_Destroy releases that handle; passing NULL is allowed.
 * - Callers own all input and output buffers passed to this API.
 * - Output buffer contents are copied during the call; the kernel does not
 *   retain caller buffer pointers after a function returns.
 * - Handles are single-thread owned unless a future ABI revision states
 *   otherwise.
 * - No C++ exceptions cross this ABI. Failures return NULL, false, or 0.
 */

bool Kernel_GetAbiInfo(KernelAbiInfo* out_info, uint32_t out_info_size);
bool Kernel_GetBuildInfo(KernelBuildInfo* out_info, uint32_t out_info_size);
bool Kernel_GetLocalPlayerInfo(
    KernelHandle* kernel,
    KernelLocalPlayerInfo* out_info);

KernelHandle* Kernel_Create(const KernelConfig* config);
void Kernel_Destroy(KernelHandle* kernel);

bool Kernel_StartClient(KernelHandle* kernel, const char* address);
bool Kernel_StartListenServer(KernelHandle* kernel, uint16_t port);
bool Kernel_StartDedicatedServer(KernelHandle* kernel, uint16_t port);

void Kernel_Update(KernelHandle* kernel, float delta_seconds);

void Kernel_SubmitInput(
    KernelHandle* kernel,
    uint32_t local_player_id,
    const PlayerInput* input);

uint32_t Kernel_GetRenderStates(
    KernelHandle* kernel,
    RenderEntityState* out_states,
    uint32_t max_states);

uint32_t Kernel_GetRenderStatesAtTime(
    KernelHandle* kernel,
    uint64_t client_render_time_us,
    RenderEntityState* out_states,
    uint32_t max_states);

uint32_t Kernel_PollEvents(
    KernelHandle* kernel,
    KernelEvent* out_events,
    uint32_t max_events);

bool Kernel_ServerCreateEntity(
    KernelHandle* kernel,
    const KernelServerEntityCreateInfo* create_info,
    uint32_t* out_net_id);

bool Kernel_ServerDestroyEntity(
    KernelHandle* kernel,
    uint32_t net_id,
    uint32_t reason);

bool Kernel_ServerSetEntityTransform(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelVec3* position,
    const KernelQuat* rotation);

bool Kernel_ServerSetEntityVelocity(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelVec3* velocity);

bool Kernel_ServerSetEntityState(
    KernelHandle* kernel,
    uint32_t net_id,
    uint16_t animation_state,
    uint32_t visual_flags);

bool Kernel_ServerSubmitEntityInput(
    KernelHandle* kernel,
    uint32_t net_id,
    const PlayerInput* input);

bool Kernel_ServerSetEntityCombatState(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelCombatStateDefinition* combat_state);

bool Kernel_ServerSetEntityWeaponMechanics(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelWeaponMechanicsDefinition* weapon_mechanics);

bool Kernel_ServerClearEntityWeaponMechanics(
    KernelHandle* kernel,
    uint32_t net_id,
    uint8_t weapon_id);

bool Kernel_ServerGetEntityWeaponMechanics(
    KernelHandle* kernel,
    uint32_t net_id,
    uint8_t weapon_id,
    KernelWeaponMechanicsDefinition* out_weapon_mechanics);

bool Kernel_ServerGetAreaEffectState(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelAreaEffectState* out_state);

bool Kernel_ServerGetBeamState(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelBeamState* out_state);

bool Kernel_ServerGetHomingState(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelHomingState* out_state);

bool Kernel_ServerValidateMechanicsConfig(
    const KernelWeaponMechanicsDefinition* weapon_mechanics);

bool Kernel_ServerGetEntityState(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelServerEntityState* out_state);

uint32_t Kernel_ServerQueryEntities(
    KernelHandle* kernel,
    uint16_t entity_type_filter,
    KernelServerEntityState* out_states,
    uint32_t max_states);

#ifdef __cplusplus
}
#endif

#endif  // KERNEL_PUBLIC_KERNEL_API_H_
