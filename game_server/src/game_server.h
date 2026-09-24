#ifndef GAME_SERVER_GAME_SERVER_H_
#define GAME_SERVER_GAME_SERVER_H_

#include <cstdint>
#include <set>

#include "game_server/src/agent_runtime_manager.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/respawn_scheduler.h"
#include "game_server/public/game_server_types.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

class GameServer {
public:
    explicit GameServer(
        KernelHandle* kernel,
        GameServerGameplayConfig config = default_game_server_gameplay_config());

    void handle_event(const KernelEvent& event);
    void tick(float delta_seconds);
    bool preload_directors();

    AgentRuntimeManager& agent_runtime_manager();
    const AgentRuntimeManager& agent_runtime_manager() const;
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
    void revive_player(std::uint32_t net_id, float delta_seconds);

    KernelHandle* kernel_ = nullptr;
    GameServerGameplayConfig config_;
    AgentRuntimeManager agent_runtime_manager_;
    RespawnScheduler respawn_;
    std::set<std::uint32_t> players_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_GAME_SERVER_H_
