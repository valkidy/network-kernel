#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/public/kernel_api.h"

int main(void) {
    KernelAbiInfo abi_info;
    KernelBuildInfo build_info;
    KernelConfig config;
    KernelLocalPlayerInfo local_player_info;
    PlayerInput input;
    RenderEntityState state;
    KernelEvent event;
    KernelServerEntityCreateInfo create_info;
    KernelServerEntityState server_state;
    KernelCombatStateDefinition combat_state;
    KernelWeaponMechanicsDefinition weapon_mechanics;
    KernelProjectileMechanicsDefinition projectile_mechanics;
    KernelAreaEffectMechanicsDefinition area_effect_mechanics;
    KernelBeamMechanicsDefinition beam_mechanics;
    KernelHomingMechanicsDefinition homing_mechanics;
    KernelAreaEffectState area_effect_state;
    KernelBeamState beam_state;
    KernelHomingState homing_state;

    (void)config;
    (void)build_info;
    (void)local_player_info;
    (void)input;
    (void)state;
    (void)event;
    (void)create_info;
    (void)server_state;
    (void)combat_state;
    (void)weapon_mechanics;
    (void)projectile_mechanics;
    (void)area_effect_mechanics;
    (void)beam_mechanics;
    (void)homing_mechanics;
    (void)area_effect_state;
    (void)beam_state;
    (void)homing_state;

    assert(KERNEL_ABI_VERSION == 13u);
    assert(KERNEL_MAX_WEAPONS == 7u);
    assert(sizeof(KernelAbiInfo) > 0u);
    assert(sizeof(KernelBuildInfo) > 0u);
    assert(sizeof(KernelLocalPlayerInfo) > 0u);
    assert(sizeof(KernelConfig) > 0u);
    assert(sizeof(PlayerInput) > 0u);
    assert(sizeof(RenderEntityState) > 0u);
    assert(sizeof(KernelEvent) > 0u);
    assert(sizeof(KernelServerEntityCreateInfo) > 0u);
    assert(sizeof(KernelServerEntityState) > 0u);
    assert(sizeof(KernelCombatStateDefinition) > 0u);
    assert(sizeof(KernelWeaponMechanicsDefinition) > 0u);
    assert(sizeof(KernelProjectileMechanicsDefinition) > 0u);
    assert(sizeof(KernelAreaEffectMechanicsDefinition) > 0u);
    assert(sizeof(KernelBeamMechanicsDefinition) > 0u);
    assert(sizeof(KernelHomingMechanicsDefinition) > 0u);
    assert(sizeof(KernelAreaEffectState) > 0u);
    assert(sizeof(KernelBeamState) > 0u);
    assert(sizeof(KernelHomingState) > 0u);
    assert((KERNEL_CAPABILITY_CLIENT_MODE & KERNEL_CAPABILITY_RENDER_STATES) == 0u);
    assert((KERNEL_CAPABILITY_LAG_COMPENSATED_PROJECTILE &
            KERNEL_CAPABILITY_EVENT_PRESENTATION_TIME) == 0u);
    assert((KERNEL_CAPABILITY_RENDER_STATES_AT_TIME &
            KERNEL_CAPABILITY_RENDER_STATES) == 0u);
    assert((KERNEL_CAPABILITY_SERVER_MECHANICS_CONFIG &
            KERNEL_CAPABILITY_SERVER_ENTITY_CREATE) == 0u);
    assert((KERNEL_CAPABILITY_WEAPON_METADATA_QUERY &
            KERNEL_CAPABILITY_SERVER_MECHANICS_CONFIG) == 0u);
    assert((KERNEL_CAPABILITY_AREA_EFFECT_WEAPONS &
            KERNEL_CAPABILITY_PROJECTILE_RESPONSE_MASKS) == 0u);
    assert((KERNEL_CAPABILITY_BEAM_WEAPONS &
            KERNEL_CAPABILITY_AREA_EFFECT_WEAPONS) == 0u);
    assert((KERNEL_CAPABILITY_HOMING_PROJECTILES &
            KERNEL_CAPABILITY_BEAM_WEAPONS) == 0u);
    assert((KERNEL_COLLISION_LAYER_PLAYER & KERNEL_COLLISION_LAYER_ENEMY) == 0u);
    assert((KERNEL_COLLISION_LAYER_PROJECTILE & KERNEL_COLLISION_LAYER_AREA_EFFECT) == 0u);
    assert((KERNEL_COLLISION_MASK_DAMAGEABLE & KERNEL_COLLISION_LAYER_PLAYER) != 0u);
    assert((KERNEL_COLLISION_MASK_DAMAGEABLE & KERNEL_COLLISION_LAYER_ENEMY) != 0u);
    assert((KERNEL_VISUAL_FLAG_MOVING & KERNEL_VISUAL_FLAG_RELOADING) == 0u);
    assert((KERNEL_VISUAL_FLAG_MOVING & KERNEL_VISUAL_FLAG_DEAD) == 0u);
    assert((InputButton_Dodge & InputButton_Parry) == 0u);
    assert((InputButton_Dodge & InputButton_Reload) == 0u);
    assert(sizeof(abi_info.abi_version) == sizeof(uint32_t));
    assert(offsetof(PlayerInput, client_action_time_us) > offsetof(PlayerInput, input_seq));
    assert(offsetof(PlayerInput, client_action_id) > offsetof(PlayerInput, client_action_time_us));
    assert(offsetof(RenderEntityState, entity_id) == 0u);
    assert(offsetof(RenderEntityState, hp) > offsetof(RenderEntityState, velocity));
    assert(offsetof(RenderEntityState, max_hp) > offsetof(RenderEntityState, hp));
    assert(offsetof(KernelEvent, event_time_us) > offsetof(KernelEvent, code));
    assert(offsetof(KernelEvent, presentation_time_us) > offsetof(KernelEvent, event_time_us));
    assert(offsetof(KernelWeaponMechanicsDefinition, area_effect) >
           offsetof(KernelWeaponMechanicsDefinition, projectile));
    assert(offsetof(KernelWeaponMechanicsDefinition, beam) >
           offsetof(KernelWeaponMechanicsDefinition, area_effect));
    assert(sizeof(state.entity_id) == sizeof(uint64_t));
    assert(sizeof(build_info.module_name) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.module_file_name) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.module_version) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.git_commit) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.build_timestamp) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.build_platform) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.build_config) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.compiler_info) == KERNEL_BUILD_INFO_TEXT_SIZE);

    return 0;
}
