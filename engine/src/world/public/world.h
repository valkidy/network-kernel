#ifndef WORLD_PUBLIC_WORLD_H_
#define WORLD_PUBLIC_WORLD_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <limits>
#include <optional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "world/public/components.h"

namespace network_example {

namespace physics {
class PhysicsWorld;
}

class World {
public:
    explicit World(bool allow_standalone_collision = true);
    ~World();
    World(World&&) noexcept;
    World& operator=(World&&) noexcept;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

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
    void set_action_templates(
        const std::vector<RuntimeActionTemplate>& action_templates);
    const RuntimeActionTemplate* find_action_template(
        std::uint32_t action_template_id) const;
    void set_status_effect_templates(
        const std::vector<RuntimeStatusEffectTemplate>& status_effect_templates);
    const RuntimeStatusEffectTemplate* find_status_effect_template(
        std::uint32_t status_effect_id) const;
    std::uint32_t allocate_status_instance_id();

    static constexpr std::size_t kActionGraphDedupCapacity = 4096u;

    struct ActionGraphDedupKey {
        PeerId requester_peer = 0;
        std::uint64_t request_id = 0;
        TriggerEventType event_type = TriggerEventType::kActivated;
        std::uint32_t sequence = 0;

        bool operator==(const ActionGraphDedupKey&) const = default;
    };

    enum class ActionGraphDedupReservationResult {
        kDuplicate,
        kReserved,
        kRejected,
    };

    ActionGraphDedupReservationResult reserve_action_graph_batch(
        const ActionGraphDedupKey& key);
    bool commit_action_graph_batch(
        const ActionGraphDedupKey& key,
        std::uint32_t committed_tick);
    void cancel_action_graph_batch(const ActionGraphDedupKey& key);
    bool reserve_action_graph_batch_capacity(std::size_t count);
    void release_action_graph_batch_capacity(std::size_t count);
    void prune_action_graph_batches(std::uint32_t current_tick);
    void clear_action_graph_batches_for_peer(PeerId requester_peer);
    void set_action_graph_dedup_retention_ticks(std::uint32_t retention_ticks);
    std::uint32_t action_graph_dedup_retention_ticks() const {
        return action_graph_dedup_retention_ticks_;
    }
    std::size_t action_graph_batch_count() const {
        return processed_action_graph_batches_.size();
    }
    std::size_t action_graph_batch_reserved_count() const {
        return reserved_action_graph_entry_count_;
    }

    bool action_graph_batch_processed(
        PeerId requester_peer,
        std::uint64_t request_id,
        TriggerEventType event_type,
        std::uint32_t sequence) const {
        const auto found = processed_action_graph_batches_.find(
            ActionGraphDedupKey{
                requester_peer, request_id, event_type, sequence});
        return found != processed_action_graph_batches_.end() &&
            !found->second->reserved;
    }

    entt::registry& registry();
    const entt::registry& registry() const;
    void set_collision_world(physics::PhysicsWorld* collision_world);
    physics::PhysicsWorld* collision_world();
    const physics::PhysicsWorld* collision_world() const;

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
    void synchronize_standalone_collision_world();
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
    std::vector<RuntimeActionTemplate> action_templates_;
    std::vector<RuntimeStatusEffectTemplate> status_effect_templates_;
    struct ActionGraphDedupKeyHash {
        std::size_t operator()(const ActionGraphDedupKey& key) const noexcept {
            std::size_t hash = std::hash<PeerId>{}(key.requester_peer);
            hash ^= std::hash<std::uint64_t>{}(key.request_id) +
                0x9e3779b9u + (hash << 6u) + (hash >> 2u);
            hash ^= std::hash<std::uint32_t>{}(
                        static_cast<std::uint32_t>(key.event_type)) +
                0x9e3779b9u + (hash << 6u) + (hash >> 2u);
            hash ^= std::hash<std::uint32_t>{}(key.sequence) +
                0x9e3779b9u + (hash << 6u) + (hash >> 2u);
            return hash;
        }
    };
    struct ActionGraphDedupEntry {
        ActionGraphDedupKey key;
        std::uint32_t committed_tick = 0;
        bool reserved = true;
    };
    using ActionGraphDedupOrder = std::list<ActionGraphDedupEntry>;
    using ActionGraphDedupIterator = ActionGraphDedupOrder::iterator;
    std::unordered_map<
        ActionGraphDedupKey,
        ActionGraphDedupIterator,
        ActionGraphDedupKeyHash>
        processed_action_graph_batches_;
    ActionGraphDedupOrder action_graph_dedup_order_;
    std::uint32_t action_graph_dedup_retention_ticks_ = 1u;
    std::size_t reserved_action_graph_capacity_ = 0u;
    std::size_t reserved_action_graph_entry_count_ = 0u;
    ColliderRegistry collider_registry_;
    physics::PhysicsWorld* collision_world_ = nullptr;
    bool allow_standalone_collision_ = true;
    std::unique_ptr<physics::PhysicsWorld> standalone_collision_world_;
    NetId next_net_id_ = 1;
    std::uint32_t next_status_instance_id_ = 1;
};

}  // namespace network_example

#endif  // WORLD_PUBLIC_WORLD_H_
