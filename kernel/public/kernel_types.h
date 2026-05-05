#ifndef KERNEL_PUBLIC_KERNEL_TYPES_H_
#define KERNEL_PUBLIC_KERNEL_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum KernelMode {
    KernelMode_Client = 0,
    KernelMode_ListenServer = 1,
    KernelMode_DedicatedServer = 2,
} KernelMode;

typedef enum KernelEventType {
    KernelEventType_Connected = 0,
    KernelEventType_Disconnected = 1,
    KernelEventType_PlayerJoined = 2,
    KernelEventType_PlayerLeft = 3,
    KernelEventType_EntitySpawned = 4,
    KernelEventType_EntityDestroyed = 5,
    KernelEventType_FireConfirmed = 6,
    KernelEventType_HitConfirmed = 7,
    KernelEventType_DamageApplied = 8,
    KernelEventType_Explosion = 9,
    KernelEventType_MissionStateChanged = 10,
    KernelEventType_Error = 11,
} KernelEventType;

typedef enum InputButton {
    InputButton_MoveJump = 1u << 0,
    InputButton_Fire = 1u << 1,
    InputButton_Reload = 1u << 2,
    InputButton_Sprint = 1u << 3,
    InputButton_Interact = 1u << 4,
    InputButton_Ability1 = 1u << 5,
} InputButton;

typedef struct KernelVec2 {
    float x;
    float y;
} KernelVec2;

typedef struct KernelVec3 {
    float x;
    float y;
    float z;
} KernelVec3;

typedef struct KernelQuat {
    float x;
    float y;
    float z;
    float w;
} KernelQuat;

typedef struct TickConfig {
    uint32_t server_tick_rate;
    uint32_t snapshot_rate;
    uint32_t history_ms;
    uint32_t max_ticks_per_update;
} TickConfig;

typedef struct KernelConfig {
    KernelMode mode;
    TickConfig tick;
    uint32_t max_render_states;
    uint32_t max_events;
} KernelConfig;

typedef struct PlayerInput {
    uint32_t input_seq;
    uint32_t client_tick;
    KernelVec2 move;
    KernelVec2 look_delta;
    KernelVec3 aim_dir;
    uint32_t buttons;
    uint8_t selected_weapon;
} PlayerInput;

typedef struct RenderEntityState {
    uint32_t net_id;
    uint16_t entity_type;
    KernelVec3 position;
    KernelQuat rotation;
    uint16_t animation_state;
    uint32_t visual_flags;
} RenderEntityState;

typedef struct KernelEvent {
    KernelEventType type;
    uint32_t tick;
    uint32_t net_id;
    uint32_t peer_id;
    uint32_t code;
} KernelEvent;

#ifdef __cplusplus
}
#endif

#endif  // KERNEL_PUBLIC_KERNEL_TYPES_H_
