#ifndef GAME_SERVER_GAME_SERVER_H_
#define GAME_SERVER_GAME_SERVER_H_

#include "game_server/enemy_manager.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

class GameServer {
public:
    explicit GameServer(KernelHandle* kernel);

    void handle_event(const KernelEvent& event);
    void tick(float delta_seconds);

    EnemyManager& enemy_manager();
    const EnemyManager& enemy_manager() const;

private:
    EnemyManager enemy_manager_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_GAME_SERVER_H_
