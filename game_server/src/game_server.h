#ifndef GAME_SERVER_GAME_SERVER_H_
#define GAME_SERVER_GAME_SERVER_H_

#include "game_server/src/agent_runtime_manager.h"
#include "game_server/src/gameplay_config.h"
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
    void configure_player(std::uint32_t net_id) const;
    bool configure_player_inventory(
        std::uint32_t net_id,
        const ActorTemplateConfig& actor_template) const;

    KernelHandle* kernel_ = nullptr;
    GameServerGameplayConfig config_;
    AgentRuntimeManager agent_runtime_manager_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_GAME_SERVER_H_
