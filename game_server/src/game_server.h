#ifndef GAME_SERVER_GAME_SERVER_H_
#define GAME_SERVER_GAME_SERVER_H_

#include <cstdint>
#include <set>
#include <vector>

#include "game_server/src/agent_runtime_manager.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/respawn_scheduler.h"
#include "game_server/src/loadout_director.h"
#include "game_server/src/shelter_director.h"
#include "game_server/public/game_server_types.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

class GameServer {
public:
    explicit GameServer(
        KernelHandle* kernel,
        GameServerGameplayConfig config = default_game_server_gameplay_config());

    // The loadout director holds this object's config and a callback into it,
    // so it never moves.
    GameServer(const GameServer&) = delete;
    GameServer& operator=(const GameServer&) = delete;

    void handle_event(const KernelEvent& event);
    void tick(float delta_seconds);
    bool preload_directors();

    AgentRuntimeManager& agent_runtime_manager();
    const AgentRuntimeManager& agent_runtime_manager() const;
    const ShelterDirector& shelter_director() const { return shelter_; }
    const LoadoutDirector& loadout_director() const { return loadout_; }
    bool query_weapon_template(
        std::uint8_t weapon_id,
        GameServerWeaponTemplateInfo* out_info) const;

private:
    // Health, weapons, ammo and starting inventory from the player template.
    // On join an existing inventory is kept; a revive (reset_inventory) empties
    // it and hands out the starting items again.
    void configure_player(std::uint32_t net_id, bool reset_inventory = false) const;
    bool configure_player_inventory(
        std::uint32_t net_id,
        const ActorTemplateConfig& actor_template,
        bool reset_inventory) const;
    // The player's weapon container, filled from its loadout's weapons or the
    // template default's weapon items; refilled fresh when `reset`.
    bool configure_player_weapons(std::uint32_t net_id, bool reset) const;
    void revive_player(std::uint32_t net_id, float delta_seconds);
    // A dead player's tagged items -- quest items, map weapons -- go to the
    // ground around them (D19, K11); the untagged stay for the respawn to
    // replace.
    void drop_tagged_items(std::uint32_t net_id) const;
    // A temporary camp that just appeared gets its stock (D8, K9): a
    // container it owns, one slot per camp.stock entry. Anything else
    // spawned is left alone.
    void stock_camp(std::uint32_t net_id) const;
    // Puts the catalog's scene_props in the world, once the kernel takes
    // them; tried every tick until it has.
    void place_scene_props();

    KernelHandle* kernel_ = nullptr;
    GameServerGameplayConfig config_;
    AgentRuntimeManager agent_runtime_manager_;
    RespawnScheduler respawn_;
    ShelterDirector shelter_;
    LoadoutDirector loadout_;
    std::set<std::uint32_t> players_;
    bool scene_props_placed_ = false;
    // Players carry their weapons as items when every weapon of the player
    // template's default loadout has a weapon item (P3); these are those
    // items, in loadout order. Otherwise players keep the template loadout.
    bool weapons_are_items_ = false;
    std::vector<std::uint32_t> default_weapon_items_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_GAME_SERVER_H_
