#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>

#include "simulation/public/collision_query.h"

namespace network_example {
namespace {

const WeaponMechanicsDefinition* weapon_definition_for_entity(
    const World& world,
    entt::entity entity,
    std::uint8_t weapon_id) {
    const std::size_t index = static_cast<std::size_t>(weapon_id);
    if (index >= kWeaponCount ||
        !world.registry().all_of<WeaponTuning>(entity)) {
        return nullptr;
    }

    const WeaponTuning& tuning = world.registry().get<WeaponTuning>(entity);
    if (!tuning.configured[index]) {
        return nullptr;
    }
    return &tuning.definitions[index];
}

const WeaponMechanicsDefinition* current_weapon_definition_for_entity(
    const World& world,
    entt::entity entity,
    const WeaponState& weapon) {
    return weapon_definition_for_entity(world, entity, weapon.weapon_id);
}

std::size_t weapon_index(std::uint8_t weapon_id) {
    return static_cast<std::size_t>(weapon_id);
}

glm::vec3 input_aim_to_world(const PlayerInput& input) {
    glm::vec3 aim{input.aim_dir.x, input.aim_dir.y, input.aim_dir.z};
    if (glm::length(aim) <= 0.0001f) {
        return glm::vec3{1.0f, 0.0f, 0.0f};
    }
    return glm::normalize(aim);
}

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

void complete_ready_reload(
    const World& world,
    entt::entity entity,
    WeaponState& weapon,
    std::uint32_t current_tick) {
    if (!weapon.is_reloading || current_tick < weapon.reload_end_tick) {
        return;
    }

    const WeaponMechanicsDefinition* definition =
        current_weapon_definition_for_entity(world, entity, weapon);
    if (definition == nullptr) {
        return;
    }
    const std::size_t index = weapon_index(definition->id);
    const std::uint16_t missing_ammo =
        static_cast<std::uint16_t>(definition->magazine_size - weapon.ammo[index]);
    const std::uint16_t loaded_ammo = std::min(missing_ammo, weapon.reserve_ammo[index]);
    weapon.ammo[index] = static_cast<std::uint16_t>(weapon.ammo[index] + loaded_ammo);
    weapon.reserve_ammo[index] =
        static_cast<std::uint16_t>(weapon.reserve_ammo[index] - loaded_ammo);
    weapon.is_reloading = false;
    weapon.reload_end_tick = 0;
}

void begin_reload(
    WeaponState& weapon,
    const WeaponMechanicsDefinition& definition,
    std::uint32_t current_tick) {
    const std::size_t index = weapon_index(definition.id);
    if (weapon.is_reloading || weapon.ammo[index] >= definition.magazine_size ||
        weapon.reserve_ammo[index] == 0) {
        return;
    }
    weapon.is_reloading = true;
    weapon.reload_end_tick = current_tick + definition.reload_ticks;
}

bool can_fire(
    const WeaponState& weapon,
    const WeaponMechanicsDefinition& definition,
    std::uint32_t current_tick) {
    const std::size_t index = weapon_index(definition.id);
    return !weapon.is_reloading && current_tick >= weapon.next_fire_tick[index] &&
           weapon.ammo[index] > 0;
}

void consume_ammo(
    WeaponState& weapon,
    const WeaponMechanicsDefinition& definition,
    std::uint32_t current_tick) {
    const std::size_t index = weapon_index(definition.id);
    --weapon.ammo[index];
    weapon.next_fire_tick[index] = current_tick + definition.cooldown_ticks;
}

std::vector<glm::vec3> pellet_directions(
    const glm::vec3& direction,
    const WeaponMechanicsDefinition& definition) {
    std::vector<glm::vec3> directions;
    directions.reserve(definition.pellet_count);
    if (definition.pellet_count == 0) {
        return directions;
    }

    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    const glm::vec3 side{0.0f, 0.0f, 1.0f};
    for (std::uint8_t pellet = 0; pellet < definition.pellet_count; ++pellet) {
        const int offset = static_cast<int>(pellet) -
                           static_cast<int>(definition.pellet_count / 2);
        const float side_offset = static_cast<float>(offset) * definition.pellet_spread;
        const float up_offset =
            (pellet % 2 == 0 ? 0.5f : -0.5f) * definition.pellet_spread;
        directions.push_back(glm::normalize(direction + side * side_offset + up * up_offset));
    }
    return directions;
}

bool find_hitscan_target(
    World& world,
    const HistoryFrame* rewind_frame,
    NetId shooter_net_id,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float max_range,
    NetId* target_net_id) {
    if (rewind_frame != nullptr) {
        HistoricalHitResult hit;
        if (raycast_history_frame(
                *rewind_frame,
                origin,
                direction,
                max_range,
                shooter_net_id,
                &hit)) {
            if (target_net_id != nullptr) {
                *target_net_id = hit.net_id;
            }
            return true;
        }
        if (target_net_id != nullptr) {
            *target_net_id = 0;
        }
        return false;
    }

    float best_distance = max_range;
    NetId best_net_id = 0;
    auto target_view = world.registry().view<NetworkIdentity, Transform, Health, Hitbox>();
    for (const entt::entity target_entity : target_view) {
        const NetworkIdentity& target_identity =
            target_view.get<NetworkIdentity>(target_entity);
        if (target_identity.net_id == shooter_net_id) {
            continue;
        }

        const Transform& target_transform = target_view.get<Transform>(target_entity);
        const Hitbox& hitbox = target_view.get<Hitbox>(target_entity);
        float distance = 0.0f;
        if (ray_intersects_aabb(
                origin,
                direction,
                target_transform.position + hitbox.center,
                hitbox.half_extents,
                &distance) &&
            distance < best_distance) {
            best_distance = distance;
            best_net_id = target_identity.net_id;
        }
    }

    if (target_net_id != nullptr) {
        *target_net_id = best_net_id;
    }
    return best_net_id != 0;
}

const HitVolumeSnapshot* find_historical_volume(
    const HistoryFrame* frame,
    NetId net_id) {
    if (frame == nullptr || !frame->valid) {
        return nullptr;
    }
    for (const HitVolumeSnapshot& volume : frame->volumes) {
        if (volume.net_id == net_id && volume.alive != 0) {
            return &volume;
        }
    }
    return nullptr;
}

glm::vec3 compensated_projectile_origin(
    const HistoryFrame* rewind_frame,
    NetId shooter_net_id,
    const Hitbox& shooter_hitbox,
    const glm::vec3& current_origin) {
    const HitVolumeSnapshot* historical_shooter =
        find_historical_volume(rewind_frame, shooter_net_id);
    if (historical_shooter == nullptr) {
        return current_origin;
    }
    const glm::vec3 muzzle_offset = glm::vec3{0.0f, 1.0f, 0.0f} - shooter_hitbox.center;
    return historical_shooter->center + muzzle_offset;
}

void apply_hitscan_damage(
    World& world,
    const WeaponMechanicsDefinition& definition,
    const HistoryFrame* rewind_frame,
    std::uint32_t current_tick,
    NetId shooter_net_id,
    PeerId shooter_peer_id,
    const glm::vec3& origin,
    const glm::vec3& direction,
    std::uint64_t hit_time_us,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    NetId target_net_id = 0;
    if (!find_hitscan_target(
            world,
            rewind_frame,
            shooter_net_id,
            origin,
            direction,
            definition.max_range,
            &target_net_id)) {
        return;
    }

    if (damage_pipeline == nullptr ||
        !damage_pipeline->submit_damage_request(DamageRequest{
            current_tick,
            0,
            shooter_net_id,
            target_net_id,
            shooter_peer_id,
            definition.id,
            definition.damage,
            hit_time_us,
            origin,
        })) {
        return;
    }
}

NetId fire_projectile(
    World& world,
    const WeaponMechanicsDefinition& definition,
    std::uint32_t event_tick,
    std::uint32_t spawn_tick,
    NetId shooter_net_id,
    PeerId shooter_peer_id,
    std::uint32_t client_action_id,
    const glm::vec3& origin,
    const glm::vec3& velocity,
    float age_seconds,
    std::vector<KernelEvent>* events) {
    const glm::vec3 current_position = projectile_position_at(
        origin,
        velocity,
        definition.projectile_motion_model,
        definition.projectile_gravity,
        age_seconds);
    const glm::vec3 current_velocity = projectile_velocity_at(
        velocity,
        definition.projectile_motion_model,
        definition.projectile_gravity,
        age_seconds);
    const NetId projectile = world.spawn_projectile(
        shooter_peer_id,
        current_position,
        current_velocity);
    const auto projectile_entity = world.find_entity(projectile);
    if (projectile_entity.has_value()) {
        ProjectileState& projectile_state =
            world.registry().get<ProjectileState>(*projectile_entity);
        projectile_state.weapon_id = definition.id;
        projectile_state.damage = definition.damage;
        projectile_state.spawn_tick = spawn_tick;
        projectile_state.client_action_id = client_action_id;
        projectile_state.shooter_net_id = shooter_net_id;
        projectile_state.motion_model = definition.projectile_motion_model;
        projectile_state.explosion_radius = definition.explosion_radius;
        projectile_state.max_lifetime_seconds = definition.projectile_lifetime_seconds;
        projectile_state.age_seconds = age_seconds;
        projectile_state.spawn_position = origin;
        projectile_state.initial_velocity = velocity;
        projectile_state.gravity = definition.projectile_gravity;
        projectile_state.previous_position = current_position;
    }
    push_event(
        events,
        KernelEventType_EntitySpawned,
        event_tick,
        projectile,
        shooter_peer_id,
        static_cast<std::uint32_t>(EntityType::kProjectile));
    return projectile;
}

void complete_all_reloads(World& world, std::uint32_t current_tick) {
    auto view = world.registry().view<WeaponState>();
    for (const entt::entity entity : view) {
        complete_ready_reload(
            world,
            entity,
            view.get<WeaponState>(entity),
            current_tick);
    }
}

}  // namespace

void simulate_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    const WeaponSimulationContext& context,
    std::vector<KernelEvent>* events) {
    const std::uint32_t current_tick = context.current_tick;
    DamagePipeline local_damage_pipeline;
    DamagePipeline* damage_pipeline = context.damage_pipeline;
    if (damage_pipeline == nullptr) {
        damage_pipeline = &local_damage_pipeline;
    }
    complete_all_reloads(world, current_tick);

    for (const QueuedInput& queued_input : inputs) {
        auto player_view =
            world.registry()
                .view<NetworkIdentity, Transform, WeaponState, Hitbox>();
        for (const entt::entity player_entity : player_view) {
            const NetworkIdentity& player_identity =
                player_view.get<NetworkIdentity>(player_entity);
            if (queued_input.controlled_net_id != 0) {
                if (player_identity.net_id != queued_input.controlled_net_id) {
                    continue;
                }
            } else {
                if (!world.registry().all_of<PlayerTag>(player_entity) ||
                    player_identity.owner_peer != queued_input.owner_peer) {
                    continue;
                }
            }

            const WeaponMechanicsDefinition* definition =
                weapon_definition_for_entity(
                    world,
                    player_entity,
                    queued_input.input.selected_weapon);
            if (definition == nullptr) {
                continue;
            }
            WeaponState& weapon = player_view.get<WeaponState>(player_entity);
            weapon.weapon_id = definition->id;

            if ((queued_input.input.buttons & InputButton_Reload) != 0) {
                begin_reload(weapon, *definition, current_tick);
            }
            if ((queued_input.input.buttons & InputButton_Fire) == 0) {
                continue;
            }
            if (!can_fire(weapon, *definition, current_tick)) {
                continue;
            }

            consume_ammo(weapon, *definition, current_tick);
            push_event(
                events,
                KernelEventType_FireConfirmed,
                current_tick,
                player_identity.net_id,
                queued_input.owner_peer,
                definition->id);

            const Transform& player_transform = player_view.get<Transform>(player_entity);
            const glm::vec3 origin = player_transform.position + glm::vec3{0.0f, 1.0f, 0.0f};
            const glm::vec3 direction = input_aim_to_world(queued_input.input);
            const std::uint64_t hit_time_us = context.action_time_us;

            if (definition->mode == WeaponFireMode::kHitscan) {
                apply_hitscan_damage(
                    world,
                    *definition,
                    context.rewind_frame,
                    current_tick,
                    player_identity.net_id,
                    queued_input.owner_peer,
                    origin,
                    direction,
                    hit_time_us,
                    events,
                    damage_pipeline);
            } else if (definition->mode == WeaponFireMode::kShotgun) {
                for (const glm::vec3& pellet_direction :
                     pellet_directions(direction, *definition)) {
                    apply_hitscan_damage(
                        world,
                        *definition,
                        context.rewind_frame,
                        current_tick,
                        player_identity.net_id,
                        queued_input.owner_peer,
                        origin,
                        pellet_direction,
                        hit_time_us,
                        events,
                        damage_pipeline);
                }
            } else {
                const Hitbox& shooter_hitbox = player_view.get<Hitbox>(player_entity);
                const glm::vec3 compensated_origin =
                    compensated_projectile_origin(
                        context.rewind_frame,
                        player_identity.net_id,
                        shooter_hitbox,
                        origin);
                const std::uint32_t spawn_tick =
                    context.rewind_frame != nullptr ? context.rewind_tick : current_tick;
                const float elapsed_seconds =
                    context.fixed_delta_seconds > 0.0f && current_tick > spawn_tick
                        ? static_cast<float>(current_tick - spawn_tick) *
                              context.fixed_delta_seconds
                        : 0.0f;
                const glm::vec3 velocity = direction * definition->projectile_speed;
                const NetId projectile = fire_projectile(
                    world,
                    *definition,
                    current_tick,
                    spawn_tick,
                    player_identity.net_id,
                    queued_input.owner_peer,
                    queued_input.input.client_action_id,
                    compensated_origin,
                    velocity,
                    elapsed_seconds,
                    events);
                if (context.history_buffer != nullptr &&
                    context.rewind_frame != nullptr &&
                    context.fixed_delta_seconds > 0.0f) {
                    const auto projectile_entity = world.find_entity(projectile);
                    if (projectile_entity.has_value()) {
                        const ProjectileState projectile_state =
                            world.registry().get<ProjectileState>(*projectile_entity);
                        resolve_projectile_historical_hit(
                            world,
                            *context.history_buffer,
                            projectile,
                            player_identity.net_id,
                            queued_input.owner_peer,
                            projectile_state,
                            compensated_origin,
                            velocity,
                            spawn_tick,
                            current_tick,
                            context.fixed_delta_seconds,
                            events,
                            damage_pipeline);
                    }
                }
            }
        }
    }
    if (context.damage_pipeline == nullptr) {
        const std::uint64_t confirm_time_us =
            context.fixed_delta_seconds > 0.0f
                ? static_cast<std::uint64_t>(
                      static_cast<double>(current_tick) *
                      static_cast<double>(context.fixed_delta_seconds) *
                      1000000.0)
                : context.action_time_us;
        damage_pipeline->confirm_ready(world, confirm_time_us, current_tick, events);
    }
}

void simulate_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    const HistoryFrame* rewind_frame) {
    simulate_weapons(
        world,
        inputs,
        WeaponSimulationContext{
            nullptr,
            rewind_frame,
            nullptr,
            rewind_frame != nullptr ? rewind_frame->server_tick : current_tick,
            current_tick,
            0.0f,
            0},
        events);
}

void simulate_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events) {
    simulate_weapons(world, inputs, current_tick, events, nullptr);
}

void simulate_hitscan_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events) {
    simulate_weapons(world, inputs, current_tick, events);
}

void simulate_hitscan_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline) {
    simulate_weapons(
        world,
        inputs,
        WeaponSimulationContext{
            nullptr,
            nullptr,
            damage_pipeline,
            current_tick,
            current_tick,
            0.0f,
            0},
        events);
}

}  // namespace network_example
