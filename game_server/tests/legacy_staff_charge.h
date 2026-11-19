#pragma once

// The tests that exercise the charge trigger mode were written against the
// Meteor Staff (weapon 13): a 20-tick hold-to-charge cast, a 3-round magazine
// and a 60-tick reload. Weapon 13 is now Ice Spike, a press cast, and no
// shipped weapon charges any more (docs/PLAYER_WEAPON_REVAMP_PLAN.md D2). This
// puts the old staff's numbers back on Ice Spike in memory, so those tests keep
// testing the charge mode, not Ice Spike's tuning.

#include <cstdint>
#include <cstdlib>

#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_types.h"

namespace network_example::game_server::test {

inline constexpr std::uint8_t kLegacyStaffWeaponId = 13;

inline void restore_legacy_staff_charge(GameServerGameplayConfig& config) {
    KernelWeaponMechanicsDefinition& weapon =
        config.weapons.definitions[kLegacyStaffWeaponId];
    weapon.magazine_size = 3;
    weapon.reserve_magazines = 6;
    bool cast = false;
    bool reload = false;
    for (ActionTemplateConfig& action : config.action_templates) {
        KernelActionTemplateDefinition& definition = action.definition;
        if (definition.action_template_id == weapon.fire_action_template_id) {
            definition.trigger_mode = KernelActionTriggerMode_Charge;
            definition.commit_offset_ticks = 20u;
            definition.hold_input_timeout_ticks = 6u;
            definition.flags = static_cast<std::uint8_t>(
                definition.flags & ~KernelActionTemplateFlag_CancelBeforeFirstCommit);
            cast = true;
        } else if (definition.action_template_id == weapon.reload_action_template_id) {
            definition.commit_offset_ticks = 60u;
            reload = true;
        }
    }
    if (!cast || !reload) {
        std::abort();
    }
}

}  // namespace network_example::game_server::test
