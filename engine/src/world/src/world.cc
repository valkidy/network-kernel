#include "world/public/world.h"

namespace network_example {

bool World::destroy(NetId net_id) {
    const std::optional<entt::entity> entity = find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    entities_by_net_id_.erase(net_id);
    registry_.destroy(*entity);
    return true;
}

bool World::apply_damage(NetId net_id, std::uint16_t amount) {
    const std::optional<entt::entity> entity = find_entity(net_id);
    if (!entity.has_value() || !registry_.all_of<Health>(*entity)) {
        return false;
    }
    Health& health = registry_.get<Health>(*entity);
    health.hp = amount >= health.hp ? 0 : static_cast<std::uint16_t>(health.hp - amount);
    return true;
}

std::optional<entt::entity> World::find_entity(NetId net_id) const {
    const auto found = entities_by_net_id_.find(net_id);
    if (found == entities_by_net_id_.end() || !registry_.valid(found->second)) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<NetId> World::net_ids() const {
    std::vector<NetId> ids;
    const auto view = registry_.view<const NetworkIdentity>();
    for (const entt::entity entity : view) {
        ids.push_back(view.get<const NetworkIdentity>(entity).net_id);
    }
    return ids;
}

void World::add_projectile_interaction_rule(
    const ProjectileInteractionRule& rule) {
    projectile_interaction_rules_.push_back(rule);
}

void World::clear_projectile_interaction_rules() {
    projectile_interaction_rules_.clear();
}

const std::vector<ProjectileInteractionRule>&
World::projectile_interaction_rules() const {
    return projectile_interaction_rules_;
}

entt::registry& World::registry() {
    return registry_;
}

const entt::registry& World::registry() const {
    return registry_;
}

NetId World::allocate_net_id() {
    return next_net_id_++;
}

entt::entity World::create_networked_entity(
    EntityType type,
    PeerId owner_peer,
    const glm::vec3& position) {
    const entt::entity entity = registry_.create();
    const NetId net_id = allocate_net_id();
    registry_.emplace<NetworkIdentity>(entity, NetworkIdentity{net_id, owner_peer});
    registry_.emplace<EntityKind>(entity, EntityKind{type});
    registry_.emplace<Transform>(entity, Transform{position, glm::quat{1.0f, 0.0f, 0.0f, 0.0f}});
    entities_by_net_id_[net_id] = entity;
    return entity;
}

}  // namespace network_example
