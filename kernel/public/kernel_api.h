#ifndef KERNEL_PUBLIC_KERNEL_API_H_
#define KERNEL_PUBLIC_KERNEL_API_H_

#include <stdbool.h>
#include <stdint.h>

#include "kernel/public/kernel_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KernelHandle KernelHandle;

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
