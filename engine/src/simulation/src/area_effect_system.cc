#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <unordered_set>
#include <vector>

#include "physics/public/physics_world.h"
#include "simulation/public/action_graph.h"
#include "simulation/public/collision_filter.h"

namespace network_example {
namespace {

void push_event(
    std::vector<KernelEvent>* events,
    KernelEventType type,
    std::uint32_t tick,
    NetId net_id,
    PeerId peer_id,
    std::uint32_t code = 0) {
    if (events == nullptr) {
        return;
    }
    events->push_back(KernelEvent{type, tick, net_id, peer_id, code});
}

}  // namespace

void simulate_area_effects(
    World& world,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    simulate_area_effects(
        world, current_tick, current_tick, events, damage_pipeline, nullptr);
}

void simulate_area_effects(
    World& world,
    std::uint32_t current_tick,
    std::uint64_t server_time_us,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    simulate_area_effects(
        world,
        current_tick,
        server_time_us,
        events,
        damage_pipeline,
        nullptr);
}

void simulate_area_effects(
    World& world,
    std::uint32_t current_tick,
    std::uint64_t server_time_us,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline,
    std::vector<ActionGraphCommandBatch>* action_graph_batches) {
    DamagePipeline local_damage_pipeline;
    DamagePipeline* active_damage_pipeline = damage_pipeline;
    if (active_damage_pipeline == nullptr) {
        active_damage_pipeline = &local_damage_pipeline;
    }

    std::vector<NetId> area_effects_to_destroy;
    auto view = world.registry().view<
        NetworkIdentity,
        Transform,
        ProjectileState,
        ProjectileAreaEffectRuntime,
        ProjectileTag>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        const Transform& transform = view.get<Transform>(entity);
        ProjectileAreaEffectRuntime& area_effect =
            view.get<ProjectileAreaEffectRuntime>(entity);
        const ProjectileState& projectile = view.get<ProjectileState>(entity);

        if (area_effect.expire_tick != 0 && current_tick >= area_effect.expire_tick) {
            area_effects_to_destroy.push_back(identity.net_id);
            continue;
        }
        if (area_effect.radius <= 0.0f ||
            (area_effect.damage_per_interval == 0 &&
             !area_effect.action_graph_binding.has_value())) {
            continue;
        }

        physics::PhysicsWorld* collision_world = world.collision_world();
        if (collision_world == nullptr) {
            continue;
        }
        physics::OverlapRequest request{};
        request.shape.type = physics::CollisionShapeType::kSphere;
        request.shape.radius = area_effect.radius;
        request.position = transform.position;
        request.filter = collision_filter_from_mask(area_effect.collision_mask);
        // One query feeds both the damage and the impulse trigger, so opting
        // in to reaching the shooter opts in to self-damage as well. That is
        // the trade a self-knockback is normally paying for.
        request.filter.ignored_entity_net_id = area_effect.hit_instigator
            ? 0u
            : projectile.shooter_net_id;
        std::vector<physics::CollisionHit> hits =
            collision_world->overlap_all(request);
        std::sort(
            hits.begin(),
            hits.end(),
            [](const physics::CollisionHit& lhs,
               const physics::CollisionHit& rhs) {
                if (lhs.identity.entity_net_id != rhs.identity.entity_net_id) {
                    return lhs.identity.entity_net_id < rhs.identity.entity_net_id;
                }
                if (lhs.identity.collider_id != rhs.identity.collider_id) {
                    return lhs.identity.collider_id < rhs.identity.collider_id;
                }
                if (lhs.subshape_id != rhs.subshape_id) {
                    return lhs.subshape_id < rhs.subshape_id;
                }
                return lhs.distance < rhs.distance;
            });

        std::uint32_t sequence_id = 0;
        std::unordered_set<NetId> seen_targets;
        std::vector<ActionGraphQueuedTrigger> queued_triggers;
        for (const physics::CollisionHit& hit : hits) {
            const NetId target_net_id = hit.identity.entity_net_id;
            const std::optional<entt::entity> target_entity =
                world.find_entity(target_net_id);
            if (!target_entity.has_value() ||
                !seen_targets.insert(target_net_id).second) {
                continue;
            }
            const EntityKind& target_kind =
                world.registry().get<EntityKind>(*target_entity);
            if (target_kind.type != EntityType::kActor &&
                target_kind.type != EntityType::kProp) {
                continue;
            }
            if (target_kind.type == EntityType::kProp &&
                (!world.registry().all_of<PropWorldMode>(*target_entity) ||
                 world.registry().get<PropWorldMode>(*target_entity).mode ==
                     PropMode::kCarrying)) {
                continue;
            }
            if (target_kind.type == EntityType::kActor &&
                (area_effect.collision_mask & KERNEL_COLLISION_MASK_ACTOR) == 0u) {
                continue;
            }
            if (target_kind.type == EntityType::kProp &&
                (area_effect.collision_mask & KERNEL_COLLISION_MASK_PROP) == 0u) {
                continue;
            }
            // Splash used to level a deployable no matter whose it was, so a
            // rocket cleared the thrower's own cover while a beam could not.
            if (!damage_source_may_damage(
                    world, area_effect.collision_mask, target_net_id)) {
                continue;
            }
            const auto next_damage_tick =
                area_effect.next_damage_tick_by_target.find(target_net_id);
            if (area_effect.action_graph_binding.has_value() &&
                area_effect.damage_interval_ticks == 0u) {
                continue;
            }
            if (next_damage_tick != area_effect.next_damage_tick_by_target.end() &&
                current_tick < next_damage_tick->second) {
                continue;
            }

            std::uint16_t damage = area_effect.damage_per_interval;
            if (area_effect.damage_falloff == ProjectileDamageFalloff::kLinear &&
                area_effect.radius > 0.0f) {
                const float falloff =
                    1.0f - std::min(1.0f, hit.distance / area_effect.radius);
                damage = static_cast<std::uint16_t>(
                    std::max(1.0f, std::round(area_effect.damage_per_interval * falloff)));
            }

            const std::uint32_t target_sequence = sequence_id++;
            if (area_effect.action_graph_binding.has_value()) {
                const glm::vec3 radial = hit.position - transform.position;
                const glm::vec3 direction = glm::length(radial) > 0.0001f
                    ? glm::normalize(radial)
                    : glm::vec3{0.0f, 1.0f, 0.0f};
                queued_triggers.push_back(ActionGraphQueuedTrigger{
                    *area_effect.action_graph_binding,
                    identity.net_id,
                    TriggerEvent{
                        TriggerEventType::kProjectileImpact,
                        identity.net_id,
                        projectile.shooter_net_id,
                        target_net_id,
                        hit.position,
                        direction,
                        ProjectileImpactPayload{
                            projectile.projectile_template_id,
                            projectile.action_instance_id,
                            projectile.weapon_id,
                            false},
                        std::nullopt},
                    ActionExecutionProvenance{
                        (static_cast<std::uint64_t>(current_tick) << 32u) ^
                            (static_cast<std::uint64_t>(identity.net_id) << 1u) ^
                            target_sequence,
                        projectile.action_instance_id,
                        current_tick,
                        projectile.shooter_net_id,
                        identity.owner_peer,
                        projectile.weapon_id,
                        ActionAuthoritySource::kAuthoritativeSimulation,
                        0u},
                    target_sequence,
                });
            } else {
                active_damage_pipeline->submit_damage_request(
                    damage_request_from_hit(
                        current_tick,
                        target_sequence,
                        identity.net_id,
                        identity.owner_peer,
                        area_effect.source_code,
                        damage,
                        server_time_us,
                        hit));
            }
            area_effect.next_damage_tick_by_target[target_net_id] =
                current_tick + std::max(1u, area_effect.damage_interval_ticks);
        }
        if (action_graph_batches != nullptr && !queued_triggers.empty()) {
            std::vector<ActionGraphCommandBatch> batches;
            if (dispatch_action_graph_triggers(
                    &queued_triggers, &batches, nullptr)) {
                action_graph_batches->insert(
                    action_graph_batches->end(),
                    std::make_move_iterator(batches.begin()),
                    std::make_move_iterator(batches.end()));
            }
        }
    }

    for (NetId area_effect : area_effects_to_destroy) {
        if (world.destroy(area_effect)) {
            push_event(
                events,
                KernelEventType_EntityDestroyed,
                current_tick,
                area_effect,
                0,
                KernelDespawnReason_Destroyed);
        }
    }

    if (damage_pipeline == nullptr) {
        active_damage_pipeline->confirm_ready(
            world,
            server_time_us,
            current_tick,
            events);
    }
}

}  // namespace network_example
