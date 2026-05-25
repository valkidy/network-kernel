#ifndef WORLD_PUBLIC_WORLD_H_
#define WORLD_PUBLIC_WORLD_H_

#include <optional>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "world/public/components.h"

namespace network_example {

class World {
public:
    World() = default;

    NetId spawn_player(PeerId owner_peer, const glm::vec3& position);
    NetId spawn_enemy(const glm::vec3& position);
    NetId spawn_projectile(
        PeerId owner_peer,
        const glm::vec3& position,
        const glm::vec3& velocity);
    NetId spawn_area_effect(
        PeerId owner_peer,
        const glm::vec3& position,
        float radius,
        std::uint32_t damage_interval_ticks,
        std::uint32_t expire_tick,
        std::uint16_t damage_per_interval,
        std::uint8_t source_code);

    bool destroy(NetId net_id);
    bool apply_damage(NetId net_id, std::uint16_t amount);

    std::optional<entt::entity> find_entity(NetId net_id) const;
    std::vector<NetId> net_ids() const;

    entt::registry& registry();
    const entt::registry& registry() const;

private:
    NetId allocate_net_id();
    entt::entity create_networked_entity(
        EntityType type,
        PeerId owner_peer,
        const glm::vec3& position);

    entt::registry registry_;
    std::unordered_map<NetId, entt::entity> entities_by_net_id_;
    NetId next_net_id_ = 1;
};

}  // namespace network_example

#endif  // WORLD_PUBLIC_WORLD_H_
