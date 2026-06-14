#include "world/public/world.h"

#include <algorithm>
#include <cstdlib>

namespace network_example {

bool World::destroy(NetId net_id) {
    const std::optional<entt::entity> entity = find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    entities_by_net_id_.erase(net_id);
    collider_registry_.remove_entity_colliders(net_id);
    registry_.destroy(*entity);
    tombstoned_net_ids_.insert(net_id);
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

void World::set_projectile_templates(
    const std::vector<RuntimeProjectileTemplate>& projectile_templates) {
    projectile_templates_ = projectile_templates;
}

const RuntimeProjectileTemplate* World::find_projectile_template(
    std::uint32_t projectile_template_id) const {
    const auto found = std::find_if(
        projectile_templates_.begin(),
        projectile_templates_.end(),
        [projectile_template_id](const RuntimeProjectileTemplate& projectile_template) {
            return projectile_template.projectile_template_id == projectile_template_id;
        });
    return found == projectile_templates_.end() ? nullptr : &*found;
}

entt::registry& World::registry() {
    return registry_;
}

const entt::registry& World::registry() const {
    return registry_;
}

World::ColliderRegistry& World::collider_registry() {
    return collider_registry_;
}

const World::ColliderRegistry& World::collider_registry() const {
    return collider_registry_;
}

ColliderInstance& World::ColliderRegistry::upsert_entity_collider(
    NetId entity_net_id,
    std::uint32_t collider_template_id,
    const ColliderInstance& collider) {
    auto found = std::find_if(
        instances_.begin(),
        instances_.end(),
        [entity_net_id, collider_template_id](const ColliderInstance& instance) {
            return instance.entity_net_id == entity_net_id &&
                   instance.collider_template_id == collider_template_id &&
                   instance.lifetime_ticks == 0;
        });
    if (found != instances_.end()) {
        const std::uint32_t collider_id = found->collider_id;
        *found = collider;
        found->collider_id = collider_id;
        found->entity_net_id = entity_net_id;
        found->collider_template_id = collider_template_id;
        return *found;
    }

    ColliderInstance instance = collider;
    instance.collider_id = allocate_collider_id();
    instance.entity_net_id = entity_net_id;
    instance.collider_template_id = collider_template_id;
    instances_.push_back(instance);
    return instances_.back();
}

ColliderInstance& World::ColliderRegistry::add_ephemeral_collider(
    const ColliderInstance& collider) {
    ColliderInstance instance = collider;
    instance.collider_id = allocate_collider_id();
    if (instance.remaining_ticks == 0) {
        instance.remaining_ticks = instance.lifetime_ticks;
    }
    instances_.push_back(instance);
    return instances_.back();
}

void World::ColliderRegistry::remove_entity_colliders(NetId entity_net_id) {
    instances_.erase(
        std::remove_if(
            instances_.begin(),
            instances_.end(),
            [entity_net_id](const ColliderInstance& instance) {
                return instance.entity_net_id == entity_net_id;
            }),
        instances_.end());
}

void World::ColliderRegistry::expire_tick_lifetimes() {
    for (ColliderInstance& instance : instances_) {
        if (instance.lifetime_ticks == 0 || instance.remaining_ticks == 0) {
            continue;
        }
        --instance.remaining_ticks;
    }
    instances_.erase(
        std::remove_if(
            instances_.begin(),
            instances_.end(),
            [](const ColliderInstance& instance) {
                return instance.lifetime_ticks != 0 &&
                       instance.remaining_ticks == 0;
            }),
        instances_.end());
}

bool World::ColliderRegistry::has_persistent_entity_collider(
    NetId entity_net_id) const {
    return std::any_of(
        instances_.begin(),
        instances_.end(),
        [entity_net_id](const ColliderInstance& instance) {
            return instance.entity_net_id == entity_net_id &&
                   instance.lifetime_ticks == 0;
        });
}

std::vector<ColliderInstance>&
World::ColliderRegistry::mutable_instances() {
    return instances_;
}

const std::vector<ColliderInstance>&
World::ColliderRegistry::instances() const {
    return instances_;
}

std::uint32_t World::ColliderRegistry::allocate_collider_id() {
    return next_collider_id_++;
}

NetId World::allocate_net_id() {
    while (next_net_id_ != 0 &&
           (entities_by_net_id_.find(next_net_id_) != entities_by_net_id_.end() ||
            tombstoned_net_ids_.find(next_net_id_) != tombstoned_net_ids_.end())) {
        ++next_net_id_;
    }
    if (next_net_id_ == 0) {
        std::abort();
    }
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
