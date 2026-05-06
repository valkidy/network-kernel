#include "world/public/world.h"

namespace network_example {

bool World::destroy(NetId net_id) {
    const std::optional<entt::entity> entity = find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
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
    const auto view = registry_.view<const NetworkIdentity>();
    for (const entt::entity entity : view) {
        if (view.get<const NetworkIdentity>(entity).net_id == net_id) {
            return entity;
        }
    }
    return std::nullopt;
}

std::vector<NetId> World::net_ids() const {
    std::vector<NetId> ids;
    const auto view = registry_.view<const NetworkIdentity>();
    for (const entt::entity entity : view) {
        ids.push_back(view.get<const NetworkIdentity>(entity).net_id);
    }
    return ids;
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
    registry_.emplace<NetworkIdentity>(entity, NetworkIdentity{allocate_net_id(), owner_peer});
    registry_.emplace<EntityKind>(entity, EntityKind{type});
    registry_.emplace<Transform>(entity, Transform{position, glm::quat{1.0f, 0.0f, 0.0f, 0.0f}});
    return entity;
}

}  // namespace network_example
