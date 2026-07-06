#ifndef WORLD_PUBLIC_WORLD_H_
#define WORLD_PUBLIC_WORLD_H_

#include <optional>
#include <unordered_map>
#include <unordered_set>
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
    NetId spawn_entity(
        EntityType type,
        ActorType actor_type,
        PeerId owner_peer,
        const glm::vec3& position);
    NetId spawn_projectile(
        PeerId owner_peer,
        const glm::vec3& position,
        const glm::vec3& velocity);

    bool destroy(NetId net_id);
    bool apply_damage(NetId net_id, std::uint16_t amount);

    std::optional<entt::entity> find_entity(NetId net_id) const;
    std::vector<NetId> net_ids() const;
    void add_projectile_interaction_rule(const ProjectileInteractionRule& rule);
    void clear_projectile_interaction_rules();
    const std::vector<ProjectileInteractionRule>& projectile_interaction_rules() const;
    void set_projectile_templates(
        const std::vector<RuntimeProjectileTemplate>& projectile_templates);
    const RuntimeProjectileTemplate* find_projectile_template(
        std::uint32_t projectile_template_id) const;

    entt::registry& registry();
    const entt::registry& registry() const;

    class ColliderRegistry {
    public:
        ColliderInstance& upsert_entity_collider(
            NetId entity_net_id,
            std::uint32_t collider_template_id,
            const ColliderInstance& collider);
        ColliderInstance& add_ephemeral_collider(const ColliderInstance& collider);
        void remove_entity_colliders(NetId entity_net_id);
        void expire_tick_lifetimes();
        bool has_persistent_entity_collider(NetId entity_net_id) const;
        std::vector<ColliderInstance>& mutable_instances();
        const std::vector<ColliderInstance>& instances() const;

    private:
        std::uint32_t allocate_collider_id();

        std::vector<ColliderInstance> instances_;
        std::uint32_t next_collider_id_ = 1;
    };

    ColliderRegistry& collider_registry();
    const ColliderRegistry& collider_registry() const;

private:
    NetId allocate_net_id();
    entt::entity create_networked_entity(
        EntityType type,
        ActorType actor_type,
        PeerId owner_peer,
        const glm::vec3& position);

    entt::registry registry_;
    std::unordered_map<NetId, entt::entity> entities_by_net_id_;
    std::unordered_set<NetId> tombstoned_net_ids_;
    std::vector<ProjectileInteractionRule> projectile_interaction_rules_;
    std::vector<RuntimeProjectileTemplate> projectile_templates_;
    ColliderRegistry collider_registry_;
    NetId next_net_id_ = 1;
};

}  // namespace network_example

#endif  // WORLD_PUBLIC_WORLD_H_
