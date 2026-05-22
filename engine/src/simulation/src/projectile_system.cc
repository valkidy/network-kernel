#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace network_example {
namespace {

void push_event(
    std::vector<KernelEvent>* events,
    KernelEventType type,
    std::uint32_t tick,
    NetId net_id,
    PeerId peer_id,
    std::uint32_t code = 0,
    std::uint64_t event_time_us = 0,
    std::uint64_t presentation_time_us = 0) {
    if (events == nullptr) {
        return;
    }
    events->push_back(KernelEvent{
        type,
        tick,
        net_id,
        peer_id,
        code,
        event_time_us,
        presentation_time_us});
}

float distance_to_hitbox(
    const glm::vec3& point,
    const Transform& transform,
    const Hitbox& hitbox) {
    const glm::vec3 center = transform.position + hitbox.center;
    const glm::vec3 min_corner = center - hitbox.half_extents;
    const glm::vec3 max_corner = center + hitbox.half_extents;
    const glm::vec3 closest = glm::clamp(point, min_corner, max_corner);
    return glm::length(point - closest);
}

float distance_to_historical_hitbox(
    const glm::vec3& point,
    const HitVolumeSnapshot& volume) {
    const glm::vec3 min_corner = volume.center - volume.half_extents;
    const glm::vec3 max_corner = volume.center + volume.half_extents;
    const glm::vec3 closest = glm::clamp(point, min_corner, max_corner);
    return glm::length(point - closest);
}

void apply_historical_explosion_damage(
    World& world,
    const HistoryFrame& frame,
    NetId projectile_net_id,
    PeerId owner_peer,
    const ProjectileState& projectile,
    const glm::vec3& explosion_center,
    std::uint32_t current_tick,
    std::uint64_t hit_time_us,
    std::vector<KernelEvent>* events) {
    if (projectile.explosion_radius <= 0.0f || projectile.damage == 0) {
        return;
    }

    std::vector<std::pair<NetId, std::uint16_t>> damage_results;
    for (const HitVolumeSnapshot& volume : frame.volumes) {
        if (volume.net_id == projectile_net_id || volume.alive == 0) {
            continue;
        }

        const float distance = distance_to_historical_hitbox(explosion_center, volume);
        if (distance > projectile.explosion_radius) {
            continue;
        }

        const float falloff = 1.0f - (distance / projectile.explosion_radius);
        const auto damage =
            static_cast<std::uint16_t>(std::max(1.0f, std::round(projectile.damage * falloff)));
        damage_results.push_back({volume.net_id, damage});
    }

    for (const auto& [target_net_id, damage] : damage_results) {
        if (!world.apply_damage(target_net_id, damage)) {
            continue;
        }
        push_event(
            events,
            KernelEventType_HitConfirmed,
            current_tick,
            target_net_id,
            owner_peer,
            projectile.weapon_id,
            hit_time_us,
            hit_time_us);
        push_event(
            events,
            KernelEventType_DamageApplied,
            current_tick,
            target_net_id,
            owner_peer,
            damage,
            hit_time_us,
            hit_time_us);
    }
}

void explode_projectile(
    World& world,
    NetId projectile_net_id,
    PeerId owner_peer,
    const ProjectileState& projectile,
    const glm::vec3& explosion_center,
    std::uint32_t current_tick,
    std::uint64_t hit_time_us,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    if (projectile.explosion_radius <= 0.0f || projectile.damage == 0) {
        return;
    }

    push_event(
        events,
        KernelEventType_Explosion,
        current_tick,
        projectile_net_id,
        owner_peer,
        static_cast<std::uint32_t>(projectile.explosion_radius * 100.0f),
        hit_time_us,
        hit_time_us);

    std::vector<std::pair<NetId, std::uint16_t>> damage_results;
    auto target_view = world.registry().view<NetworkIdentity, Transform, Health, Hitbox>();
    for (const entt::entity target_entity : target_view) {
        if (world.registry().all_of<ProjectileTag>(target_entity)) {
            continue;
        }

        const NetworkIdentity& target_identity =
            target_view.get<NetworkIdentity>(target_entity);
        if (target_identity.net_id == projectile_net_id) {
            continue;
        }

        const float distance = distance_to_hitbox(
            explosion_center,
            target_view.get<Transform>(target_entity),
            target_view.get<Hitbox>(target_entity));
        if (distance > projectile.explosion_radius) {
            continue;
        }

        const float falloff = 1.0f - (distance / projectile.explosion_radius);
        const auto damage =
            static_cast<std::uint16_t>(std::max(1.0f, std::round(projectile.damage * falloff)));
        damage_results.push_back({target_identity.net_id, damage});
    }

    for (const auto& [target_net_id, damage] : damage_results) {
        if (damage_pipeline != nullptr &&
            damage_pipeline->submit_hit(
                world,
                target_net_id,
                projectile_net_id,
                owner_peer,
                projectile.weapon_id,
                damage,
                hit_time_us)) {
            continue;
        }
        if (!world.apply_damage(target_net_id, damage)) {
            continue;
        }
        push_event(
            events,
            KernelEventType_HitConfirmed,
            current_tick,
            target_net_id,
            owner_peer,
            projectile.weapon_id,
            hit_time_us,
            hit_time_us);
        push_event(
            events,
            KernelEventType_DamageApplied,
            current_tick,
            target_net_id,
            owner_peer,
            damage,
            hit_time_us,
            hit_time_us);
    }
}

bool projectile_sweep_hits_target(
    World& world,
    entt::entity projectile_entity,
    PeerId owner_peer,
    NetId ignored_net_id,
    const glm::vec3& previous_position,
    const glm::vec3& current_position,
    glm::vec3* impact_position) {
    const glm::vec3 displacement = current_position - previous_position;
    const float length = glm::length(displacement);
    if (length <= 0.0001f) {
        return false;
    }
    const glm::vec3 direction = displacement / length;

    float best_distance = length;
    bool found_hit = false;
    auto target_view = world.registry().view<NetworkIdentity, Transform, Health, Hitbox>();
    for (const entt::entity target_entity : target_view) {
        if (target_entity == projectile_entity ||
            world.registry().all_of<ProjectileTag>(target_entity)) {
            continue;
        }

        const NetworkIdentity& target_identity =
            target_view.get<NetworkIdentity>(target_entity);
        if (target_identity.net_id == ignored_net_id) {
            continue;
        }
        if (owner_peer != 0 && target_identity.owner_peer == owner_peer) {
            continue;
        }

        const Transform& target_transform = target_view.get<Transform>(target_entity);
        const Hitbox& hitbox = target_view.get<Hitbox>(target_entity);
        float distance = 0.0f;
        if (ray_intersects_aabb(
                previous_position,
                direction,
                target_transform.position + hitbox.center,
                hitbox.half_extents,
                &distance) &&
            distance <= best_distance) {
            best_distance = distance;
            found_hit = true;
        }
    }

    if (found_hit && impact_position != nullptr) {
        *impact_position = previous_position + direction * best_distance;
    }
    return found_hit;
}

std::uint64_t tick_time_us(std::uint32_t tick, float fixed_delta_seconds) {
    return static_cast<std::uint64_t>(
        static_cast<double>(tick) * static_cast<double>(fixed_delta_seconds) *
        1000000.0);
}

}  // namespace

glm::vec3 projectile_position_at(
    const glm::vec3& origin,
    const glm::vec3& initial_velocity,
    ProjectileMotionModel motion_model,
    const glm::vec3& gravity,
    float elapsed_seconds) {
    if (motion_model == ProjectileMotionModel::kParabolic) {
        return origin + initial_velocity * elapsed_seconds +
               0.5f * gravity * elapsed_seconds * elapsed_seconds;
    }
    return origin + initial_velocity * elapsed_seconds;
}

glm::vec3 projectile_velocity_at(
    const glm::vec3& initial_velocity,
    ProjectileMotionModel motion_model,
    const glm::vec3& gravity,
    float elapsed_seconds) {
    if (motion_model == ProjectileMotionModel::kParabolic) {
        return initial_velocity + gravity * elapsed_seconds;
    }
    return initial_velocity;
}

void simulate_projectiles(World& world, float fixed_delta_seconds) {
    simulate_projectiles(world, fixed_delta_seconds, 0, nullptr);
}

void simulate_projectiles(
    World& world,
    float fixed_delta_seconds,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events) {
    simulate_projectiles(world, fixed_delta_seconds, current_tick, events, nullptr);
}

void simulate_projectiles(
    World& world,
    float fixed_delta_seconds,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    std::vector<NetId> projectiles_to_destroy;
    auto view = world.registry().view<NetworkIdentity, Transform, Velocity, ProjectileState, ProjectileTag>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        Transform& transform = view.get<Transform>(entity);
        Velocity& velocity = view.get<Velocity>(entity);
        ProjectileState& projectile = view.get<ProjectileState>(entity);

        projectile.previous_position = transform.position;
        const float next_age_seconds = projectile.age_seconds + fixed_delta_seconds;
        transform.position = projectile_position_at(
            projectile.spawn_position,
            projectile.initial_velocity,
            projectile.motion_model,
            projectile.gravity,
            next_age_seconds);
        velocity.linear = projectile_velocity_at(
            projectile.initial_velocity,
            projectile.motion_model,
            projectile.gravity,
            next_age_seconds);
        projectile.age_seconds = next_age_seconds;

        glm::vec3 impact_position = transform.position;
        const bool hit_target = projectile_sweep_hits_target(
            world,
            entity,
            identity.owner_peer,
            projectile.shooter_net_id,
            projectile.previous_position,
            transform.position,
            &impact_position);
        const bool expired = projectile.max_lifetime_seconds > 0.0f &&
                             projectile.age_seconds >= projectile.max_lifetime_seconds;
        if (!hit_target && !expired) {
            continue;
        }

        explode_projectile(
            world,
            identity.net_id,
            identity.owner_peer,
            projectile,
            hit_target ? impact_position : transform.position,
            current_tick,
            tick_time_us(current_tick, fixed_delta_seconds),
            events,
            damage_pipeline);
        projectiles_to_destroy.push_back(identity.net_id);
    }

    for (NetId projectile : projectiles_to_destroy) {
        world.destroy(projectile);
    }
}

bool resolve_projectile_historical_hit(
    World& world,
    const HistoryBuffer& history_buffer,
    NetId projectile_net_id,
    NetId ignored_net_id,
    PeerId owner_peer,
    const ProjectileState& projectile,
    const glm::vec3& origin,
    const glm::vec3& velocity,
    std::uint32_t rewind_tick,
    std::uint32_t current_tick,
    float fixed_delta_seconds,
    std::vector<KernelEvent>* events) {
    if (fixed_delta_seconds <= 0.0f || current_tick <= rewind_tick) {
        return false;
    }

    // Only query historical hitboxes along the deterministic catch-up path; the
    // surviving projectile's current transform remains the fast-forwarded state.
    glm::vec3 previous_position = origin;
    for (std::uint32_t tick = rewind_tick + 1; tick <= current_tick; ++tick) {
        const float elapsed_seconds =
            static_cast<float>(tick - rewind_tick) * fixed_delta_seconds;
        const glm::vec3 current_position = projectile_position_at(
            origin,
            velocity,
            projectile.motion_model,
            projectile.gravity,
            elapsed_seconds);
        const HistoryFrame* frame = history_buffer.find_frame(tick);
        if (frame != nullptr) {
            HistoricalHitResult hit;
            if (sweep_history_frame(
                    *frame,
                    previous_position,
                    current_position,
                    ignored_net_id,
                    &hit)) {
                const std::uint64_t hit_time_us =
                    tick_time_us(frame->server_tick, fixed_delta_seconds);
                push_event(
                    events,
                    KernelEventType_Explosion,
                    current_tick,
                    projectile_net_id,
                    owner_peer,
                    static_cast<std::uint32_t>(
                        projectile.explosion_radius * 100.0f),
                    hit_time_us,
                    hit_time_us);
                apply_historical_explosion_damage(
                    world,
                    *frame,
                    projectile_net_id,
                    owner_peer,
                    projectile,
                    hit.impact_position,
                    current_tick,
                    hit_time_us,
                    events);
                world.destroy(projectile_net_id);
                return true;
            }
        }
        previous_position = current_position;
    }

    return false;
}

}  // namespace network_example
