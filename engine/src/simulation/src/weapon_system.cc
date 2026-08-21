#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>

#include "physics/public/physics_world.h"
#include "simulation/public/collision_filter.h"

namespace network_example {

glm::vec3 projectile_launch_position(const Transform& transform) {
    return transform.position + glm::vec3{0.0f, 1.0f, 0.0f};
}

namespace {

const WeaponMechanicsDefinition* weapon_definition_for_entity(
    const World& world,
    entt::entity entity,
    std::uint8_t weapon_id) {
    const std::size_t index = static_cast<std::size_t>(weapon_id);
    if (!world.registry().all_of<WeaponTuning>(entity)) {
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
    return weapon_definition_for_entity(
        world, entity, active_weapon_id(weapon));
}

glm::vec3 input_aim_to_world(const KernelPlayerInput& input) {
    glm::vec3 aim{input.aim_dir.x, input.aim_dir.y, input.aim_dir.z};
    if (glm::length(aim) <= 0.0001f) {
        return glm::vec3{1.0f, 0.0f, 0.0f};
    }
    return glm::normalize(aim);
}

glm::vec3 normalized_or(const glm::vec3& value, const glm::vec3& fallback) {
    if (glm::length(value) <= 0.0001f) {
        return fallback;
    }
    return glm::normalize(value);
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

std::vector<glm::vec3> projectile_burst_directions(
    const glm::vec3& direction,
    const WeaponMechanicsDefinition& definition) {
    const std::uint8_t burst_count =
        definition.pellet_count == 0 ? 1 : definition.pellet_count;
    std::vector<glm::vec3> directions;
    directions.reserve(burst_count);
    if (burst_count == 1 || definition.pellet_spread == 0.0f) {
        directions.push_back(direction);
        return directions;
    }

    constexpr float kPi = 3.14159265358979323846f;
    const float center = static_cast<float>(burst_count - 1) * 0.5f;
    for (std::uint8_t index = 0; index < burst_count; ++index) {
        const float degrees =
            (static_cast<float>(index) - center) * definition.pellet_spread;
        const float radians = degrees * kPi / 180.0f;
        const float cos_angle = std::cos(radians);
        const float sin_angle = std::sin(radians);
        directions.push_back(glm::normalize(glm::vec3{
            direction.x * cos_angle + direction.z * sin_angle,
            direction.y,
            -direction.x * sin_angle + direction.z * cos_angle}));
    }
    return directions;
}

// Limbs are out of reach here, and cannot be authored into it.
//
// The live path below runs on the default query filter, whose collision_mask is
// kCollisionMaskAll -- which excludes kActorLimb, so no limb hit ever arrives to
// classify. The rewound path never touches the physics world at all: it tests
// the history frame, which holds one volume per actor built from its Hitbox.
//
// Putting limbs in that frame is not a matter of pushing nine more volumes.
// raycast_history_frame ignores volume.rotation and tests an axis-aligned box,
// which is close enough for a hitbox and badly wrong for a 1.5 x 19 m leg lying
// at an angle -- its AABB is mostly empty air, and shots near a monster would
// register hits on nothing. Reaching limbs from a hitscan needs oriented-box
// intersection in the history path plus a way for a weapon to opt in, and both
// belong to their own change rather than being smuggled in here.
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

    const physics::PhysicsWorld* collision_world = world.collision_world();
    physics::CollisionHit hit{};
    physics::RayCastRequest request{};
    request.origin = origin;
    request.direction = direction;
    request.max_distance = max_range;
    request.filter.ignored_entity_net_id = shooter_net_id;
    if (collision_world == nullptr ||
        !collision_world->ray_cast_closest(request, &hit) ||
        !is_actor_hit(hit.identity.kind)) {
        if (target_net_id != nullptr) {
            *target_net_id = 0;
        }
        return false;
    }
    if (target_net_id != nullptr) {
        *target_net_id = hit.identity.entity_net_id;
    }
    return true;
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
    if (definition.segment_collider_template_id != 0) {
        ColliderInstance segment{};
        segment.collider_template_id = definition.segment_collider_template_id;
        segment.owner_net_id = shooter_net_id;
        segment.entity_net_id = shooter_net_id;
        segment.entity_type = EntityType::kUnknown;
        segment.shape_type = ColliderShapeType::kSegment;
        segment.purpose_flags = KernelColliderPurpose_Damage;
        segment.layer_mask = kCollisionLayerProjectile;
        segment.segment_start = origin;
        segment.segment_end = origin + direction * definition.max_range;
        segment.world_center = (segment.segment_start + segment.segment_end) * 0.5f;
        segment.lifetime_ticks = 3;
        segment.remaining_ticks = 3;
        segment.has_resolved_damage = true;
        world.collider_registry().add_ephemeral_collider(segment);
    }

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
    const RuntimeProjectileTemplate& projectile_template,
    std::uint32_t event_tick,
    std::uint32_t spawn_tick,
    NetId shooter_net_id,
    PeerId shooter_peer_id,
    std::uint32_t action_instance_id,
    const glm::vec3& origin,
    const glm::vec3& velocity,
    float fixed_delta_seconds,
    std::uint32_t age_ticks,
    std::vector<KernelEvent>* events) {
    const float age_duration =
        static_cast<float>(age_ticks) * fixed_delta_seconds;
    const glm::vec3 current_position = projectile_position_at(
        origin,
        velocity,
        projectile_template.motion_model,
        projectile_template.gravity,
        age_duration);
    const glm::vec3 current_velocity = projectile_velocity_at(
        velocity,
        projectile_template.motion_model,
        projectile_template.gravity,
        age_duration);
    const NetId projectile = world.spawn_projectile(
        shooter_peer_id,
        current_position,
        current_velocity);
    const auto projectile_entity = world.find_entity(projectile);
    if (projectile_entity.has_value()) {
        ProjectileState& projectile_state =
            world.registry().get<ProjectileState>(*projectile_entity);
        projectile_state.weapon_id = definition.id;
        projectile_state.projectile_template_id =
            projectile_template.projectile_template_id;
        projectile_state.damage = projectile_template.damage;
        projectile_state.spawn_tick = spawn_tick;
        projectile_state.action_instance_id = action_instance_id;
        projectile_state.shooter_net_id = shooter_net_id;
        projectile_state.motion_model = projectile_template.motion_model;
        projectile_state.hit_response = projectile_template.hit_response;
        projectile_state.damage_shape = projectile_template.damage_shape;
        projectile_state.collision_mask = projectile_template.collision_mask;
        // Without these a weapon-fired projectile falls back to a thin segment
        // between its previous and current position, ignoring the volume its
        // collider template authors -- a rocket with a 0.1 m box swept a ray.
        // spawn_projectile_from_template, the action-graph spawn path, has
        // always copied them, so the same template behaved differently
        // depending on which path created it.
        projectile_state.collision_geometry = projectile_template.collision_geometry;
        projectile_state.has_collision_geometry =
            projectile_template.has_collision_geometry;
        projectile_state.collision_query_mode =
            projectile_template.collision_query_mode;
        projectile_state.max_hit_count =
            std::max(1u, projectile_template.max_hit_count);
        projectile_state.hit_count = 0;
        projectile_state.max_lifetime_ticks = projectile_template.lifetime_ticks;
        projectile_state.age_ticks = age_ticks;
        projectile_state.spawn_position = origin;
        projectile_state.initial_velocity = velocity;
        projectile_state.gravity = projectile_template.gravity;
        projectile_state.previous_position = current_position;
        if (projectile_template.projectile_impact_binding.has_value()) {
            world.registry().emplace<OnProjectileImpactTriggerTag>(
                *projectile_entity);
        }
        if (projectile_template.expired_binding.has_value()) {
            world.registry().emplace<OnExpiredTriggerTag>(*projectile_entity);
        }
        if (projectile_template.motion_model == ProjectileMotionModel::kHoming) {
            world.registry().emplace<HomingState>(
                *projectile_entity,
                HomingState{
                    projectile_template.homing_mode,
                    projectile_template.sync_mode,
                    MissileGuidancePhase::kBoost,
                    0,
                    projectile_template.homing_boost_ticks,
                    spawn_tick + projectile_template.homing_boost_ticks,
                    projectile_template.homing_lock_on_range,
                    projectile_template.homing_lose_target_range,
                    projectile_template.homing_lock_cone_degrees,
                    projectile_template.homing_max_turn_degrees_per_tick,
                    projectile_template.homing_acceleration,
                    projectile_template.homing_max_speed});
        }
        if (projectile_template.projectile_type == ProjectileType::kAreaEffect) {
            world.registry().replace<Hitbox>(
                *projectile_entity,
                Hitbox{
                    {0.0f, 0.0f, 0.0f},
                    {projectile_template.area_radius,
                     projectile_template.area_radius,
                     projectile_template.area_radius},
                    projectile_template.collider_template_id});
            world.registry().emplace<ProjectileAreaEffectRuntime>(
                *projectile_entity,
                ProjectileAreaEffectRuntime{
                    projectile_template.area_radius,
                    projectile_template.damage,
                    projectile_template.damage_interval_ticks == 0
                        ? 1u
                        : projectile_template.damage_interval_ticks,
                    event_tick + std::max(1u, projectile_template.lifetime_ticks),
                    definition.id,
                    projectile_template.collision_mask,
                    projectile_template.damage_falloff,
                    {},
                    projectile_template.projectile_impact_binding,
                });
        }
        if (projectile_template.projectile_type == ProjectileType::kBeam) {
            world.registry().emplace<ProjectileBeamRuntime>(
                *projectile_entity,
                ProjectileBeamRuntime{
                    shooter_net_id,
                    current_position,
                    normalized_or(velocity, glm::vec3{1.0f, 0.0f, 0.0f}),
                    projectile_template.beam_length,
                    projectile_template.beam_radius,
                    projectile_template.damage,
                    event_tick + std::max(1u, projectile_template.lifetime_ticks),
                    definition.id,
                    projectile_template.collision_mask,
                    {},
                });
        }
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
    std::vector<ActionCommit> action_commits =
        simulate_actions(world, inputs, current_tick, context.action_outcomes);
    if (action_commits.empty()) {
        for (const QueuedInput& input : inputs) {
            if ((input.input.buttons & InputButton_Fire) == 0u) {
                continue;
            }
            const auto player_view =
                world.registry().view<NetworkIdentity, PlayerTag>();
            for (const entt::entity entity : player_view) {
                const NetworkIdentity& identity =
                    player_view.get<NetworkIdentity>(entity);
                if ((input.controlled_net_id != 0 &&
                     identity.net_id != input.controlled_net_id) ||
                    (input.controlled_net_id == 0 &&
                     identity.owner_peer != input.owner_peer)) {
                    continue;
                }
                const WeaponMechanicsDefinition* definition =
                    weapon_definition_for_entity(
                        world, entity, input.input.selected_weapon);
                if (definition != nullptr) {
                    action_commits.push_back(ActionCommit{
                        identity.net_id,
                        identity.owner_peer,
                        definition->id,
                        KernelActionBinding_PrimaryFire,
                        0,
                        input.input.input_seq == 0 ? 1u : input.input.input_seq,
                        1,
                        current_tick,
                        true,
                        input_aim_to_world(input.input),
                    });
                }
                break;
            }
        }
    }

    auto push_action_outcome = [&context](
                                   const ActionCommit& commit,
                                   ActionOutcomeType type,
                                   KernelLocalActionResultReason reason) {
        if (context.action_outcomes == nullptr) {
            return;
        }
        context.action_outcomes->push_back(ActionOutcome{
            commit.controlled_net_id,
            commit.owner_peer,
            commit.action_template_id,
            commit.action_instance_id,
            commit.binding_id,
            commit.commit_count,
            commit.authoritative_tick,
            type,
            reason,
        });
    };

    for (const ActionCommit& commit : action_commits) {
        QueuedInput queued_input{};
        queued_input.owner_peer = commit.owner_peer;
        queued_input.controlled_net_id = commit.controlled_net_id;
        queued_input.input.action_intent.action_instance_id =
            commit.action_instance_id;
        queued_input.input.aim_dir = KernelVec3{
            commit.aim_direction.x,
            commit.aim_direction.y,
            commit.aim_direction.z,
        };
        queued_input.input.selected_weapon = commit.weapon_id;
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
            const std::size_t slot = find_weapon_slot(weapon, definition->id);
            if (slot >= weapon.weapon_slot_count) {
                push_action_outcome(
                    commit,
                    ActionOutcomeType::Corrected,
                    KernelLocalActionResultReason_MissingTemplate);
                break;
            }
            weapon.active_weapon_slot = static_cast<std::uint8_t>(slot);
            if (commit.binding_id == KernelActionBinding_Reload) {
                if (weapon.reserve_magazines[slot] == 0u ||
                    weapon.ammo[slot] >= definition->magazine_size) {
                    weapon.is_reloading = false;
                    push_action_outcome(
                        commit,
                        ActionOutcomeType::Corrected,
                        KernelLocalActionResultReason_EffectFailed);
                    break;
                }
                weapon.ammo[slot] = definition->magazine_size;
                --weapon.reserve_magazines[slot];
                weapon.is_reloading = false;
                push_action_outcome(
                    commit,
                    ActionOutcomeType::Committed,
                    KernelLocalActionResultReason_None);
                if (commit.completes_action) {
                    push_action_outcome(
                        commit,
                        ActionOutcomeType::Completed,
                        KernelLocalActionResultReason_None);
                }
                break;
            }
            if (commit.binding_id != KernelActionBinding_PrimaryFire) {
                push_action_outcome(
                    commit,
                    ActionOutcomeType::Corrected,
                    KernelLocalActionResultReason_MissingTemplate);
                break;
            }
            const RuntimeActionTemplate* action_template =
                world.find_action_template(commit.action_template_id);
            const bool legacy_button_commit =
                action_template == nullptr && commit.action_template_id == 0;
            const std::uint16_t ammo_cost = legacy_button_commit
                ? 1u
                : action_template == nullptr
                    ? 0u
                    : action_template->ammo_cost_per_commit;
            if ((!legacy_button_commit && action_template == nullptr) ||
                weapon.ammo[slot] < ammo_cost) {
                push_action_outcome(
                    commit,
                    ActionOutcomeType::Corrected,
                    KernelLocalActionResultReason_EffectFailed);
                break;
            }
            if (definition->mode == WeaponFireMode::kProjectile &&
                world.find_projectile_template(
                    definition->projectile_template_id) == nullptr) {
                push_action_outcome(
                    commit,
                    ActionOutcomeType::Corrected,
                    KernelLocalActionResultReason_EffectFailed);
                break;
            }
            weapon.ammo[slot] = static_cast<std::uint16_t>(
                weapon.ammo[slot] - ammo_cost);
            weapon.next_primary_commit_tick[slot] =
                commit.authoritative_tick +
                (legacy_button_commit
                     ? 1u
                     : action_template->commit_interval_ticks);
            push_action_outcome(
                commit,
                ActionOutcomeType::Committed,
                KernelLocalActionResultReason_None);
            if (commit.completes_action) {
                push_action_outcome(
                    commit,
                    ActionOutcomeType::Completed,
                    KernelLocalActionResultReason_None);
            }
            push_event(
                events,
                KernelEventType_FireConfirmed,
                current_tick,
                player_identity.net_id,
                queued_input.owner_peer,
                definition->id);

            const Transform& player_transform =
                player_view.get<Transform>(player_entity);
            const glm::vec3 origin =
                projectile_launch_position(player_transform);
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
            } else if (definition->mode == WeaponFireMode::kProjectile) {
                const RuntimeProjectileTemplate* projectile_template =
                    world.find_projectile_template(definition->projectile_template_id);
                if (projectile_template == nullptr) {
                    continue;
                }
                const Hitbox& shooter_hitbox = player_view.get<Hitbox>(player_entity);
                const glm::vec3 compensated_origin =
                    compensated_projectile_origin(
                        context.rewind_frame,
                        player_identity.net_id,
                        shooter_hitbox,
                        origin);
                const std::uint32_t spawn_tick =
                    context.rewind_frame != nullptr ? context.rewind_tick : current_tick;
                const std::uint32_t elapsed_ticks =
                    current_tick > spawn_tick ? current_tick - spawn_tick : 0u;
                if (projectile_template->projectile_type == ProjectileType::kBeam &&
                    weapon.active_effect_net_id != 0u) {
                    const std::optional<entt::entity> beam_entity =
                        world.find_entity(weapon.active_effect_net_id);
                    if (beam_entity.has_value() &&
                        world.registry().all_of<ProjectileBeamRuntime>(
                            *beam_entity)) {
                        ProjectileBeamRuntime& beam =
                            world.registry().get<ProjectileBeamRuntime>(*beam_entity);
                        beam.origin = compensated_origin;
                        beam.direction = direction;
                        beam.expire_tick = current_tick +
                            std::max(1u, projectile_template->lifetime_ticks);
                        world.registry().get<Transform>(*beam_entity).position =
                            compensated_origin;
                        continue;
                    }
                    weapon.active_effect_net_id = 0u;
                }
                for (const glm::vec3& projectile_direction :
                     projectile_burst_directions(direction, *definition)) {
                    const glm::vec3 velocity =
                        projectile_direction * projectile_template->speed;
                    const NetId projectile = fire_projectile(
                        world,
                        *definition,
                        *projectile_template,
                        current_tick,
                        spawn_tick,
                        player_identity.net_id,
                        queued_input.owner_peer,
                        queued_input.input.action_intent.action_instance_id,
                        compensated_origin,
                        velocity,
                        context.fixed_delta_seconds,
                        elapsed_ticks,
                        events);
                    if (projectile_template->projectile_type ==
                        ProjectileType::kBeam) {
                        weapon.active_effect_net_id = projectile;
                    }
                    if (context.history_buffer != nullptr &&
                        context.rewind_frame != nullptr &&
                        context.fixed_delta_seconds > 0.0f &&
                        projectile_template->motion_model !=
                            ProjectileMotionModel::kHoming) {
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
