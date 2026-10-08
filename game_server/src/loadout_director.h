#ifndef GAME_SERVER_SRC_LOADOUT_DIRECTOR_H_
#define GAME_SERVER_SRC_LOADOUT_DIRECTOR_H_

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

// The initial camp's loadout (design D1-D6): what a player picks there fills
// their inventory at once and is what every respawn gives them, until they
// leave the session. The wire format is GAME_SERVER_MESSAGE_LOADOUT_* in
// game_server_types.h.
//
// A camp is any prop whose template has a `loadout:` block; activating it
// (KernelEventType_UiOpened) sends the player the offer, which is also the
// client's cue to open the UI. The pick is checked here, once -- it is replayed
// on every respawn, so a forged one would pay out forever: the camp must be a
// camp the player stands in reach of, the picks must name its options, and
// there may be no more of them than the player has inventory slots.
// What a player picked: items, each filling one inventory slot, and weapon
// items, at most one per category. A loadout of neither is the default.
struct PlayerLoadout {
    std::vector<InventorySlotConfig> items;
    std::vector<std::uint32_t> weapon_items;
};

class LoadoutDirector {
public:
    // Replaces `player`'s inventory with its loadout (or the default); false
    // if that failed. Called the moment a pick is accepted.
    using ApplyLoadout = std::function<bool(std::uint32_t player)>;

    LoadoutDirector(
        KernelHandle* kernel,
        const GameServerGameplayConfig& config,
        ApplyLoadout apply);

    void handle_event(const KernelEvent& event);
    void handle_message(const KernelGameMessage& message);

    // `player`'s picked loadout, or nullptr for the player template's default.
    const PlayerLoadout* loadout_of(std::uint32_t player) const;

private:
    const ActorTemplateConfig* camp_template(std::uint32_t camp_net_id) const;
    void send_offers(std::uint32_t peer, std::uint32_t player, std::uint32_t camp);
    void reply(std::uint32_t peer, std::uint32_t camp, std::uint8_t result,
               std::uint8_t pick_count, std::uint8_t weapon_pick_count = 0u) const;
    // A weapon item template's category, or KERNEL_WEAPON_CATEGORY_COUNT for
    // anything else.
    std::uint8_t weapon_category_of(std::uint32_t item_template_id) const;

    KernelHandle* kernel_ = nullptr;
    const GameServerGameplayConfig& config_;
    ApplyLoadout apply_;
    std::map<std::uint32_t, PlayerLoadout> loadouts_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_SRC_LOADOUT_DIRECTOR_H_
