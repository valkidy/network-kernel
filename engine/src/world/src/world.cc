#include "world/public/world.h"

#include <algorithm>
#include <cstdlib>

#include "physics/public/physics_world.h"

namespace network_example {

World::World(bool allow_standalone_collision)
    : allow_standalone_collision_(allow_standalone_collision) {}

World::~World() = default;
World::World(World&&) noexcept = default;
World& World::operator=(World&&) noexcept = default;

bool World::destroy(NetId net_id) {
    const std::optional<entt::entity> entity = find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    entities_by_net_id_.erase(net_id);
    collider_registry_.remove_entity_colliders(net_id);
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

void World::set_action_templates(
    const std::vector<RuntimeActionTemplate>& action_templates) {
    action_templates_ = action_templates;
}

const RuntimeActionTemplate* World::find_action_template(
    std::uint32_t action_template_id) const {
    const auto found = std::find_if(
        action_templates_.begin(),
        action_templates_.end(),
        [action_template_id](const RuntimeActionTemplate& action_template) {
            return action_template.action_template_id == action_template_id;
        });
    return found == action_templates_.end() ? nullptr : &*found;
}

void World::set_status_effect_templates(
    const std::vector<RuntimeStatusEffectTemplate>& status_effect_templates) {
    status_effect_templates_ = status_effect_templates;
}

const RuntimeStatusEffectTemplate* World::find_status_effect_template(
    std::uint32_t status_effect_id) const {
    const auto found = std::find_if(
        status_effect_templates_.begin(), status_effect_templates_.end(),
        [status_effect_id](const RuntimeStatusEffectTemplate& status) {
            return status.status_effect_id == status_effect_id;
        });
    return found == status_effect_templates_.end() ? nullptr : &*found;
}

std::uint32_t World::allocate_status_instance_id() {
    const std::uint32_t instance_id = next_status_instance_id_++;
    return instance_id == 0u ? next_status_instance_id_++ : instance_id;
}

World::ActionGraphDedupReservationResult
World::reserve_action_graph_batch(const ActionGraphDedupKey& key) {
    if (processed_action_graph_batches_.find(key) !=
        processed_action_graph_batches_.end()) {
        return ActionGraphDedupReservationResult::kDuplicate;
    }
    if (reserved_action_graph_capacity_ == 0u &&
        processed_action_graph_batches_.size() >= kActionGraphDedupCapacity) {
        return ActionGraphDedupReservationResult::kRejected;
    }
    if (reserved_action_graph_capacity_ > 0u) {
        --reserved_action_graph_capacity_;
    }
    action_graph_dedup_order_.push_back(ActionGraphDedupEntry{key});
    auto entry = action_graph_dedup_order_.end();
    --entry;
    processed_action_graph_batches_.emplace(key, entry);
    ++reserved_action_graph_entry_count_;
    return ActionGraphDedupReservationResult::kReserved;
}

bool World::commit_action_graph_batch(
    const ActionGraphDedupKey& key,
    std::uint32_t committed_tick) {
    const auto found = processed_action_graph_batches_.find(key);
    if (found == processed_action_graph_batches_.end() ||
        !found->second->reserved) {
        return false;
    }
    found->second->committed_tick = committed_tick;
    found->second->reserved = false;
    --reserved_action_graph_entry_count_;
    return true;
}

void World::cancel_action_graph_batch(const ActionGraphDedupKey& key) {
    const auto found = processed_action_graph_batches_.find(key);
    if (found == processed_action_graph_batches_.end()) {
        return;
    }
    if (found->second->reserved) {
        --reserved_action_graph_entry_count_;
    }
    action_graph_dedup_order_.erase(found->second);
    processed_action_graph_batches_.erase(found);
}

bool World::reserve_action_graph_batch_capacity(std::size_t count) {
    const std::size_t used = processed_action_graph_batches_.size() +
        reserved_action_graph_capacity_;
    if (count > kActionGraphDedupCapacity - used) {
        return false;
    }
    reserved_action_graph_capacity_ += count;
    return true;
}

void World::release_action_graph_batch_capacity(std::size_t count) {
    reserved_action_graph_capacity_ =
        count >= reserved_action_graph_capacity_
        ? 0u
        : reserved_action_graph_capacity_ - count;
}

void World::prune_action_graph_batches(std::uint32_t current_tick) {
    while (!action_graph_dedup_order_.empty()) {
        const ActionGraphDedupEntry& entry = action_graph_dedup_order_.front();
        if (entry.reserved ||
            current_tick - entry.committed_tick <
                action_graph_dedup_retention_ticks_) {
            break;
        }
        processed_action_graph_batches_.erase(entry.key);
        action_graph_dedup_order_.pop_front();
    }
}

void World::clear_action_graph_batches_for_peer(PeerId requester_peer) {
    for (auto entry = action_graph_dedup_order_.begin();
         entry != action_graph_dedup_order_.end();) {
        if (entry->key.requester_peer != requester_peer) {
            ++entry;
            continue;
        }
        if (entry->reserved) {
            --reserved_action_graph_entry_count_;
        }
        processed_action_graph_batches_.erase(entry->key);
        entry = action_graph_dedup_order_.erase(entry);
    }
}

void World::set_action_graph_dedup_retention_ticks(
    std::uint32_t retention_ticks) {
    action_graph_dedup_retention_ticks_ = std::min(
        std::max(1u, retention_ticks),
        std::numeric_limits<std::uint32_t>::max() / 2u);
}

entt::registry& World::registry() {
    return registry_;
}

const entt::registry& World::registry() const {
    return registry_;
}

void World::set_collision_world(physics::PhysicsWorld* collision_world) {
    collision_world_ = collision_world;
}

physics::PhysicsWorld* World::collision_world() {
    if (collision_world_ == nullptr && allow_standalone_collision_) {
        synchronize_standalone_collision_world();
        return standalone_collision_world_.get();
    }
    return collision_world_;
}

const physics::PhysicsWorld* World::collision_world() const {
    if (collision_world_ == nullptr && allow_standalone_collision_) {
        World* mutable_world = const_cast<World*>(this);
        mutable_world->synchronize_standalone_collision_world();
        return mutable_world->standalone_collision_world_.get();
    }
    return collision_world_;
}

void World::synchronize_standalone_collision_world() {
    if (standalone_collision_world_ == nullptr) {
        standalone_collision_world_ =
            std::make_unique<physics::PhysicsWorld>();
    }
    auto view = registry_.view<NetworkIdentity, EntityKind, Transform, Hitbox>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        const EntityKind& kind = view.get<EntityKind>(entity);
        if (kind.type != EntityType::kActor) {
            continue;
        }
        const Transform& transform = view.get<Transform>(entity);
        const Hitbox& hitbox = view.get<Hitbox>(entity);
        physics::CollisionObjectDescriptor object{};
        object.identity.entity_net_id = identity.net_id;
        object.identity.collider_id = 0x40000000u | identity.net_id;
        object.identity.hit_zone = hitbox.hit_zone;
        object.identity.kind = physics::CollisionObjectKind::kActorHitbox;
        object.identity.layer = physics::CollisionLayer::kDamageable;
        object.identity.gameplay_category = kind.actor_type == ActorType::kPlayer
            ? kCollisionLayerPlayerSide
            : kCollisionLayerHostileSide;
        object.shape.type = physics::CollisionShapeType::kBox;
        object.shape.half_extents = hitbox.half_extents;
        object.position = transform.position + transform.rotation * hitbox.center;
        object.rotation = transform.rotation;
        object.enabled = !registry_.all_of<Health>(entity) ||
            registry_.get<Health>(entity).hp != 0;
        std::string error;
        (void)standalone_collision_world_->upsert_object(object, &error);
    }
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
                   instance.bone_index == UINT32_MAX &&
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

ColliderInstance& World::ColliderRegistry::upsert_bone_collider(
    NetId entity_net_id,
    std::uint32_t collider_template_id,
    std::uint32_t bone_index,
    const ColliderInstance& collider) {
    auto found = std::find_if(
        instances_.begin(),
        instances_.end(),
        [entity_net_id, collider_template_id, bone_index](
            const ColliderInstance& instance) {
            return instance.entity_net_id == entity_net_id &&
                   instance.collider_template_id == collider_template_id &&
                   instance.bone_index == bone_index &&
                   instance.lifetime_ticks == 0;
        });
    if (found != instances_.end()) {
        const std::uint32_t collider_id = found->collider_id;
        *found = collider;
        found->collider_id = collider_id;
        found->entity_net_id = entity_net_id;
        found->collider_template_id = collider_template_id;
        found->bone_index = bone_index;
        return *found;
    }

    ColliderInstance instance = collider;
    instance.collider_id = allocate_collider_id();
    instance.entity_net_id = entity_net_id;
    instance.collider_template_id = collider_template_id;
    instance.bone_index = bone_index;
    instances_.push_back(instance);
    return instances_.back();
}

void World::ColliderRegistry::remove_bone_colliders(NetId entity_net_id) {
    instances_.erase(
        std::remove_if(
            instances_.begin(),
            instances_.end(),
            [entity_net_id](const ColliderInstance& instance) {
                return instance.entity_net_id == entity_net_id &&
                       instance.bone_index != UINT32_MAX;
            }),
        instances_.end());
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

// Ids are never reused, which is what a client still holding a stale net_id
// needs and what a tombstone set would otherwise have to provide: this counter
// only ever advances, and every id in the world came out of it, so the next one
// is always past every id ever handed out.
//
// A destroyed id used to be recorded in a tombstone set that this loop then
// skipped. That set could never affect the outcome -- the loop cannot reach an
// id below the counter -- and nothing else read it, so it was one entry of
// permanent growth per destroyed entity and no protection. Retiring patrols is
// the first design here that destroys entities continuously rather than once
// per match, which is what made an unbounded write-only set worth removing
// rather than bounding.
//
// The remaining scan is the guard that would matter if an id ever entered the
// world from outside this counter, which today nothing does:
// create_networked_entity is the only thing that populates the map, and it
// always allocates here.
NetId World::allocate_net_id() {
    while (next_net_id_ != 0 &&
           entities_by_net_id_.find(next_net_id_) != entities_by_net_id_.end()) {
        ++next_net_id_;
    }
    if (next_net_id_ == 0) {
        std::abort();
    }
    return next_net_id_++;
}

entt::entity World::create_networked_entity(
    EntityType type,
    ActorType actor_type,
    PeerId owner_peer,
    const glm::vec3& position) {
    const entt::entity entity = registry_.create();
    const NetId net_id = allocate_net_id();
    registry_.emplace<NetworkIdentity>(entity, NetworkIdentity{net_id, owner_peer});
    registry_.emplace<EntityKind>(entity, EntityKind{type, actor_type});
    registry_.emplace<Transform>(entity, Transform{position, glm::quat{1.0f, 0.0f, 0.0f, 0.0f}});
    entities_by_net_id_[net_id] = entity;
    return entity;
}

}  // namespace network_example
