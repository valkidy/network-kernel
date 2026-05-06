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

uint32_t Kernel_PollEvents(
    KernelHandle* kernel,
    KernelEvent* out_events,
    uint32_t max_events);

#ifdef __cplusplus
}
#endif

#endif  // KERNEL_PUBLIC_KERNEL_API_H_
