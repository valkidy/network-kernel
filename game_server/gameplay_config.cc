#include "game_server/gameplay_config.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>
#include <zip.h>

#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

DataLoadError::DataLoadError(
    std::uint32_t error_code,
    std::string diagnostic,
    std::string path,
    std::string field,
    std::uint32_t source_kind,
    std::uint32_t template_kind,
    std::uint32_t template_id,
    std::uint32_t field_id,
    std::int32_t line,
    std::int32_t column)
    : std::runtime_error(diagnostic),
      error_code(error_code),
      path(std::move(path)),
      field(std::move(field)),
      source_kind(source_kind),
      template_kind(template_kind),
      template_id(template_id),
      field_id(field_id),
      line(line),
      column(column) {}

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr const char* kDefaultGameplayCatalogPath =
    "game_server/gameplay_catalog.yaml";
constexpr std::uint64_t kMaxYamlEntryBytes = 1024ull * 1024ull;
constexpr std::uint64_t kMaxTotalYamlBytes = 8ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxSkeletonAssetBytes = 4ull * 1024ull * 1024ull;
constexpr std::uint16_t kDefaultReserveMagazines = 6;
void hash_bytes(std::uint64_t* hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        *hash ^= bytes[index];
        *hash *= kFnvPrime;
    }
}

template <typename T>
void hash_scalar(std::uint64_t* hash, const T& value) {
    hash_bytes(hash, &value, sizeof(T));
}

void hash_float(std::uint64_t* hash, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    hash_scalar(hash, bits);
}

void hash_vec3(std::uint64_t* hash, const KernelVec3& value) {
    hash_float(hash, value.x);
    hash_float(hash, value.y);
    hash_float(hash, value.z);
}

void hash_vec2(std::uint64_t* hash, const KernelVec2& value) {
    hash_float(hash, value.x);
    hash_float(hash, value.y);
}

void hash_vec4(std::uint64_t* hash, const KernelVec4& value) {
    hash_float(hash, value.x);
    hash_float(hash, value.y);
    hash_float(hash, value.z);
    hash_float(hash, value.w);
}

void hash_string(std::uint64_t* hash, const std::string& value) {
    const auto size = static_cast<std::uint32_t>(value.size());
    hash_scalar(hash, size);
    hash_bytes(hash, value.data(), value.size());
}

std::uint32_t stable_channel_id(std::string_view value) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash == 0u ? 1u : hash;
}

void hash_weapon(std::uint64_t* hash, const KernelWeaponMechanicsDefinition& weapon) {
    hash_scalar(hash, weapon.weapon_id);
    hash_scalar(hash, weapon.fire_mode);
    hash_scalar(hash, weapon.magazine_size);
    hash_scalar(hash, weapon.reserve_magazines);
    hash_scalar(hash, weapon.damage);
    hash_float(hash, weapon.max_range);
    hash_scalar(hash, weapon.pellet_count);
    hash_float(hash, weapon.pellet_spread);
    hash_scalar(hash, weapon.segment_collider_template_id);
    hash_scalar(hash, weapon.projectile_template_id);
    hash_scalar(hash, weapon.fire_action_template_id);
    hash_scalar(hash, weapon.reload_action_template_id);
    hash_scalar(hash, weapon.collision_mask);
}

void hash_action_template(
    std::uint64_t* hash,
    const KernelActionTemplateDefinition& action) {
    hash_scalar(hash, action.action_template_id);
    hash_scalar(hash, action.trigger_mode);
    hash_scalar(hash, action.flags);
    hash_scalar(hash, action.ammo_cost_per_commit);
    hash_scalar(hash, action.commit_offset_ticks);
    hash_scalar(hash, action.commit_interval_ticks);
    hash_scalar(hash, action.max_commit_count);
    hash_scalar(hash, action.recovery_ticks);
    hash_scalar(hash, action.hold_input_timeout_ticks);
}

void hash_collider_template(
    std::uint64_t* hash,
    const ColliderTemplateConfig& collider_template) {
    hash_string(hash, collider_template.name);
    const KernelColliderTemplateDefinition& definition =
        collider_template.definition;
    hash_scalar(hash, definition.template_id);
    hash_scalar(hash, definition.shape_type);
    hash_vec3(hash, definition.center);
    hash_vec4(hash, definition.shape_params);
    hash_scalar(hash, definition.lifetime_ticks);
    hash_scalar(hash, definition.purpose_flags);
    hash_scalar(hash, definition.layer_mask);
}

void hash_projectile_template(
    std::uint64_t* hash,
    const ProjectileTemplateConfig& projectile_template) {
    hash_string(hash, projectile_template.name);
    const KernelProjectileTemplateDefinition& definition =
        projectile_template.definition;
    const KernelProjectileMechanicsDefinition& mechanics = definition.mechanics;
    hash_scalar(hash, definition.projectile_template_id);
    hash_scalar(hash, definition.weapon_id);
    hash_scalar(hash, mechanics.projectile_type);
    hash_scalar(hash, mechanics.motion_model);
    hash_scalar(hash, mechanics.sync_mode);
    hash_scalar(hash, mechanics.hit_response);
    hash_scalar(hash, mechanics.damage_shape);
    hash_scalar(hash, mechanics.damage_falloff);
    hash_scalar(hash, mechanics.collision_query_mode);
    hash_scalar(hash, mechanics.damage);
    hash_float(hash, mechanics.speed);
    hash_scalar(hash, mechanics.lifetime_ticks);
    hash_vec3(hash, mechanics.gravity);
    hash_scalar(hash, mechanics.collider_template_id);
    hash_scalar(hash, mechanics.collision_mask);
    hash_scalar(hash, mechanics.max_hit_count);
    hash_scalar(hash, mechanics.homing.homing_mode);
    hash_scalar(hash, mechanics.homing.sync_mode);
    hash_scalar(hash, mechanics.homing.boost_ticks);
    hash_float(hash, mechanics.homing.lock_on_range);
    hash_float(hash, mechanics.homing.lose_target_range);
    hash_float(hash, mechanics.homing.lock_cone_degrees);
    hash_float(hash, mechanics.homing.max_turn_degrees_per_tick);
    hash_float(hash, mechanics.homing.acceleration);
    hash_float(hash, mechanics.homing.max_speed);
    hash_float(hash, mechanics.area_effect.radius);
    hash_scalar(hash, mechanics.area_effect.damage_per_interval);
    hash_scalar(hash, mechanics.area_effect.damage_interval_ticks);
    hash_scalar(hash, mechanics.area_effect.lifetime_ticks);
    hash_scalar(hash, mechanics.area_effect.collision_mask);
    hash_scalar(hash, mechanics.area_effect.hit_instigator);
    hash_float(hash, mechanics.beam.length);
    hash_float(hash, mechanics.beam.radius);
    hash_scalar(hash, mechanics.beam.damage_per_tick);
    hash_scalar(hash, mechanics.beam.lifetime_ticks);
    hash_scalar(hash, mechanics.beam.collision_mask);
    for (const KernelActionTriggerDefinition* trigger : {
             &mechanics.projectile_impact_trigger,
             &mechanics.expired_trigger,
         }) {
        hash_scalar(hash, trigger->action_count);
        for (std::uint32_t index = 0; index < trigger->action_count; ++index) {
            const KernelActionDefinition& action = trigger->actions[index];
            hash_scalar(hash, action.action_type);
            hash_scalar(hash, action.spawn_projectile_template_id);
            hash_scalar(hash, action.position_source);
            hash_scalar(hash, action.direction_source);
            hash_scalar(hash, action.target_source);
            hash_scalar(hash, action.damage_amount);
            hash_scalar(hash, action.health_change_amount);
            hash_scalar(hash, action.impulse_strength);
            hash_scalar(hash, action.impulse_collision_mask);
            hash_scalar(hash, action.condition_type);
        }
    }
}

void reject_unknown_keys(
    const YAML::Node& node,
    std::initializer_list<const char*> keys,
    const std::string& path,
    std::uint32_t source_kind,
    std::uint32_t template_kind,
    std::uint32_t template_id = 0);

void hash_actor_template(
    std::uint64_t* hash,
    const ActorTemplateConfig& actor_template) {
    hash_scalar(hash, actor_template.actor_template_id);
    hash_string(hash, actor_template.name);
    hash_scalar(hash, actor_template.entity_type);
    hash_scalar(hash, actor_template.actor_type);
    hash_scalar(hash, actor_template.server_only);
    hash_vec3(hash, actor_template.transform_position);
    hash_scalar(hash, actor_template.collider_template_id);
    hash_scalar(hash, actor_template.health.hp);
    hash_scalar(hash, actor_template.health.max_hp);
    hash_vec3(hash, actor_template.hitbox_center);
    hash_vec3(hash, actor_template.hitbox_half_extents);
    hash_float(hash, actor_template.move_speed_meters_per_second);
    hash_scalar(hash, actor_template.movement_controller_type);
    hash_scalar(hash, actor_template.movement_collider_template_id);
    hash_vec3(hash, actor_template.movement_gravity);
    hash_float(hash, actor_template.movement_max_slope_degrees);
    hash_float(hash, actor_template.movement_step_height);
    hash_float(hash, actor_template.movement_ground_probe_distance);
    hash_float(hash, actor_template.movement_ground_snap_distance);
    hash_float(hash, actor_template.movement_max_yaw_degrees_per_second);
    hash_float(hash, actor_template.impulse_resistance);
    hash_scalar(hash, actor_template.movement_collision_mask);
    hash_scalar(hash, actor_template.weapon_slot_count);
    for (std::uint8_t index = 0; index < actor_template.weapon_slot_count; ++index) {
        hash_scalar(hash, actor_template.weapon_ids[index]);
    }
    hash_scalar(hash, actor_template.active_weapon_slot);
    hash_scalar(hash, actor_template.inventory_slot_capacity);
    hash_scalar(
        hash,
        static_cast<std::uint32_t>(actor_template.inventory_slots.size()));
    for (const InventorySlotConfig& slot : actor_template.inventory_slots) {
        hash_scalar(hash, slot.item_template_id);
        hash_scalar(hash, slot.quantity);
    }
    hash_scalar(hash, actor_template.animation_idle);
    hash_scalar(hash, actor_template.animation_chasing);
    hash_scalar(hash, actor_template.sentry.alert_ticks);
    hash_scalar(hash, actor_template.sentry.forget_ticks);
    hash_scalar(hash, actor_template.sentry.ballistic_retry_cooldown_ticks);
    hash_scalar(hash, actor_template.sentry.patrol_rotation_interval_ticks);
    hash_float(hash, actor_template.sentry.patrol_rotation_min_degrees);
    hash_float(hash, actor_template.sentry.patrol_rotation_max_degrees);
    hash_scalar(hash, actor_template.sentry.passive_patrol);
    hash_float(hash, actor_template.sentry.patrol_extent_x_meters);
    hash_float(hash, actor_template.sentry.patrol_input_magnitude);
    hash_scalar(hash, actor_template.sentry.weapon_id);
    hash_float(hash, actor_template.chaser.stop_distance_meters);
    hash_float(hash, actor_template.chaser.resume_distance_meters);
    hash_float(hash, actor_template.chaser.input_magnitude);
    hash_scalar(hash, actor_template.vision.camp);
    hash_scalar(hash, actor_template.vision.vision_collider_template_id);
    hash_scalar(hash, actor_template.vision.max_visible_hostiles);
    hash_scalar(hash, actor_template.vision.max_visible_allies);
    hash_scalar(hash, actor_template.vision.max_visible_neutrals);
    hash_vec3(hash, actor_template.vision.local_origin);
    hash_vec3(hash, actor_template.vision.local_forward);
    hash_scalar(hash, actor_template.ai_controller_type);
    hash_scalar(hash, actor_template.ai_tick_interval);
    hash_scalar(hash, actor_template.director_kind);
    hash_scalar(hash, actor_template.director_spawn_target_count);
    hash_scalar(hash, actor_template.director_spawn_entity_template_id);
    hash_scalar(hash, actor_template.director_spawn_actor_template_id);
    hash_string(hash, actor_template.director_spawn_entity_template_ref);
    hash_vec3(hash, actor_template.director_spawn_position);
    hash_float(hash, actor_template.director_spawn_radius);
    hash_scalar(hash, actor_template.director_spawn_seed);
    for (const ActorTemplateConfig::GameRuleNodeConfig& node :
         actor_template.game_rule_nodes) {
        hash_scalar(hash, node.node_id);
        hash_string(hash, node.id);
        hash_scalar(hash, node.condition_type);
        hash_scalar(hash, node.condition_count);
        hash_scalar(hash, node.group_id);
        hash_string(hash, node.group);
        hash_scalar(hash, node.has_spawn_effect);
        hash_scalar(hash, node.spawn_count);
        hash_string(hash, node.spawn_entity_template_ref);
        hash_scalar(hash, node.spawn_entity_template_id);
        hash_vec3(hash, node.spawn_position);
        hash_float(hash, node.spawn_radius);
        hash_scalar(hash, node.spawn_seed);
        for (const std::uint32_t next_node_id : node.next_node_ids) {
            hash_scalar(hash, next_node_id);
        }
    }
    hash_string(hash, actor_template.activated_trigger.action_graph_ref);
    for (const auto& parameter : actor_template.activated_trigger.parameters) {
        hash_string(hash, parameter.first);
        hash_string(hash, parameter.second);
    }
    hash_string(hash, actor_template.collision_trigger.action_graph_ref);
    hash_scalar(hash, actor_template.collision_trigger_mask);
    for (const auto& parameter : actor_template.collision_trigger.parameters) {
        hash_string(hash, parameter.first);
        hash_string(hash, parameter.second);
    }
    hash_string(hash, actor_template.health_depleted_trigger.action_graph_ref);
    for (const auto& parameter :
         actor_template.health_depleted_trigger.parameters) {
        hash_string(hash, parameter.first);
        hash_string(hash, parameter.second);
    }
    hash_string(hash, actor_template.destroy_entity_trigger.action_graph_ref);
    for (const auto& parameter : actor_template.destroy_entity_trigger.parameters) {
        hash_string(hash, parameter.first);
        hash_string(hash, parameter.second);
    }
    hash_scalar(hash, actor_template.prop.interaction.capability_flags);
    hash_scalar(hash, actor_template.prop.interaction.line_of_sight_required);
    hash_float(hash, actor_template.prop.interaction.interaction_range);
    hash_scalar(hash, actor_template.prop.interaction.line_of_sight_blocking_mask);
    hash_float(hash, actor_template.prop.carry_offset_x);
    hash_float(hash, actor_template.prop.carry_offset_y);
    hash_float(hash, actor_template.prop.carry_offset_z);
    hash_scalar(
        hash,
        actor_template.prop.throw_trajectory_projectile_template_id);
    hash_scalar(hash, actor_template.prop.lifetime_ticks);
    hash_scalar(hash, actor_template.prop.population_group_id);
    hash_scalar(hash, actor_template.skeleton.enabled);
    if (actor_template.skeleton.enabled) {
        hash_scalar(hash, actor_template.skeleton.skeleton_asset_id);
        hash_scalar(hash, actor_template.skeleton.content_hash);
        hash_scalar(hash, actor_template.skeleton.root_bone_index);
        hash_scalar(hash, actor_template.skeleton.body_bone_index);
        hash_scalar(
            hash,
            static_cast<std::uint32_t>(
                actor_template.skeleton.legs.size()));
        // Tuning only: the legs' bones and bend geometry belong to the rig and
        // are hashed with the skeleton asset instead.
        for (const SkeletonLegConfig& leg : actor_template.skeleton.legs) {
            hash_string(hash, leg.id);
            hash_scalar(hash, leg.gait_group);
            hash_float(hash, leg.step_height_meters);
            hash_float(hash, leg.max_reach_ratio);
        }
        hash_float(hash, actor_template.skeleton.input_deadzone);
        hash_float(hash, actor_template.skeleton.step_threshold_meters);
        hash_scalar(hash, actor_template.skeleton.step_duration_ticks);
        hash_scalar(hash, actor_template.skeleton.max_swinging_legs);
        hash_float(hash, actor_template.skeleton.body_follow_speed);
        hash_float(hash, actor_template.skeleton.slope_alignment);
        hash_float(hash, actor_template.skeleton.stance_crouch_meters);
        hash_scalar(hash, actor_template.skeleton.foothold_query_type);
        hash_float(
            hash,
            actor_template.skeleton.foothold_query_start_height_meters);
        hash_float(
            hash,
            actor_template.skeleton.foothold_query_distance_meters);
        hash_scalar(
            hash,
            static_cast<std::uint32_t>(
                actor_template.skeleton.foothold_candidate_offsets.size()));
        for (const KernelVec2& offset :
             actor_template.skeleton.foothold_candidate_offsets) {
            hash_vec2(hash, offset);
        }
        for (const std::uint32_t leg_index :
             actor_template.skeleton.processing_order) {
            hash_scalar(hash, leg_index);
        }
        // Only the meaning: the geometry these apply to belongs to the rig and
        // is hashed with the skeleton asset instead.
        hash_scalar(
            hash,
            static_cast<std::uint32_t>(
                actor_template.skeleton.has_collision_flags ? 1u : 0u));
        hash_scalar(hash, actor_template.skeleton.collision_flags.hit_zone);
        hash_scalar(
            hash, actor_template.skeleton.collision_flags.purpose_flags);
        hash_scalar(hash, actor_template.skeleton.collision_flags.layer_mask);
    }
}

std::vector<PropPopulationRuleConfig> prop_population_rules_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind) {
    if (!node || !node.IsSequence()) {
        throw std::runtime_error(
            "prop_population_rules must be a sequence: " + path);
    }
    std::vector<PropPopulationRuleConfig> rules;
    for (const YAML::Node& entry : node) {
        reject_unknown_keys(
            entry,
            {"id", "name", "max_alive"},
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG);
        if (!entry["id"] || !entry["name"] || !entry["max_alive"]) {
            throw std::runtime_error(
                "prop population rule requires id, name, and max_alive: " +
                path);
        }
        PropPopulationRuleConfig rule;
        rule.name = entry["name"].as<std::string>();
        rule.definition.struct_size = sizeof(rule.definition);
        rule.definition.population_group_id = entry["id"].as<std::uint32_t>();
        rule.definition.max_alive = entry["max_alive"].as<std::uint32_t>();
        if (rule.definition.population_group_id == 0u || rule.name.empty() ||
            rule.definition.max_alive == 0u ||
            rule.definition.max_alive > 256u) {
            throw std::runtime_error(
                "prop population rule id and name must be non-empty and "
                "max_alive must be between 1 and 256: " + path);
        }
        const bool duplicate = std::any_of(
            rules.begin(),
            rules.end(),
            [&](const PropPopulationRuleConfig& candidate) {
                return candidate.name == rule.name ||
                    candidate.definition.population_group_id ==
                        rule.definition.population_group_id;
            });
        if (duplicate) {
            throw std::runtime_error(
                "prop population rule id and name must be unique: " + path);
        }
        rules.push_back(std::move(rule));
    }
    return rules;
}

std::uint32_t prop_population_group_id_from_ref(
    const YAML::Node& node,
    const std::vector<PropPopulationRuleConfig>& rules) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error(
            "prop population_group reference must be a scalar");
    }
    const std::string value = node.as<std::string>();
    const auto found = std::find_if(
        rules.begin(),
        rules.end(),
        [&value](const PropPopulationRuleConfig& rule) {
            return rule.name == value;
        });
    if (found == rules.end()) {
        throw std::runtime_error("unknown prop population_group: " + value);
    }
    return found->definition.population_group_id;
}

KernelWeaponMechanicsDefinition hitscan_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size,
    float max_range) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    weapon.weapon_id = weapon_id;
    weapon.fire_mode = KernelWeaponFireMode_Hitscan;
    weapon.magazine_size = magazine_size;
    weapon.reserve_magazines = kDefaultReserveMagazines;
    // damage and collision_mask are left at zero on purpose:
    // apply_weapon_template_references fills them in from the projectile
    // template this weapon points at, the same way it does for every other
    // fire mode. One authoring surface for both.
    weapon.max_range = max_range;
    weapon.pellet_count = 1;
    return weapon;
}

KernelWeaponMechanicsDefinition shotgun_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size,
    float max_range,
    std::uint8_t pellet_count,
    float pellet_spread) {
    KernelWeaponMechanicsDefinition weapon =
        hitscan_weapon(weapon_id, magazine_size, max_range);
    weapon.fire_mode = KernelWeaponFireMode_Shotgun;
    weapon.pellet_count = pellet_count;
    weapon.pellet_spread = pellet_spread;
    return weapon;
}

KernelWeaponMechanicsDefinition area_effect_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    weapon.weapon_id = weapon_id;
    weapon.fire_mode = KernelWeaponFireMode_Projectile;
    weapon.magazine_size = magazine_size;
    weapon.reserve_magazines = kDefaultReserveMagazines;
    // damage is left at zero on purpose: apply_weapon_template_references
    // fills it in from the projectile template this weapon points at.
    weapon.pellet_count = 1;
    weapon.projectile_template_id = weapon_id;
    return weapon;
}

KernelWeaponMechanicsDefinition beam_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    weapon.weapon_id = weapon_id;
    weapon.fire_mode = KernelWeaponFireMode_Projectile;
    weapon.magazine_size = magazine_size;
    weapon.reserve_magazines = kDefaultReserveMagazines;
    // damage is left at zero on purpose: apply_weapon_template_references
    // fills it in from the projectile template this weapon points at.
    weapon.pellet_count = 1;
    weapon.projectile_template_id = weapon_id;
    return weapon;
}

bool validate_weapon_mechanics(
    const KernelWeaponMechanicsDefinition& weapon) {
    if (weapon.struct_size < sizeof(KernelWeaponMechanicsDefinition) ||
        weapon.magazine_size == 0 ||
        (weapon.fire_mode != KernelWeaponFireMode_Projectile &&
         weapon.damage == 0) ||
        weapon.fire_action_template_id == 0u ||
        weapon.reload_action_template_id == 0u ||
        weapon.fire_mode > KernelWeaponFireMode_Projectile) {
        return false;
    }
    if (weapon.fire_mode == KernelWeaponFireMode_Projectile) {
        return weapon.projectile_template_id != 0;
    }
    if (weapon.max_range <= 0.0f) {
        return false;
    }
    if ((weapon.fire_mode == KernelWeaponFireMode_Hitscan ||
         weapon.fire_mode == KernelWeaponFireMode_Shotgun) &&
        weapon.segment_collider_template_id == 0) {
        return false;
    }
    return weapon.fire_mode != KernelWeaponFireMode_Shotgun ||
           weapon.pellet_count != 0;
}

std::string trim_ascii(const std::string& value) {
    const auto first = std::find_if_not(
        value.begin(),
        value.end(),
        [](unsigned char ch) { return std::isspace(ch); });
    const auto last = std::find_if_not(
        value.rbegin(),
        value.rend(),
        [](unsigned char ch) { return std::isspace(ch); })
                          .base();
    return first >= last ? std::string{} : std::string(first, last);
}

std::uint32_t collision_mask_token_from_yaml(const std::string& token) {
    if (token == "actor" || token == "damageable") {
        return KERNEL_COLLISION_MASK_DAMAGEABLE;
    }
    if (token == "none" || token == "0") {
        return KERNEL_COLLISION_MASK_NONE;
    }
    if (token == "hostile_side") {
        return KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
    }
    if (token == "player_side") {
        return KERNEL_COLLISION_LAYER_PLAYER_SIDE;
    }
    if (token == "neutral") {
        return KERNEL_COLLISION_LAYER_NEUTRAL;
    }
    if (token == "projectile") {
        return KERNEL_COLLISION_LAYER_PROJECTILE;
    }
    if (token == "area_effect") {
        return KERNEL_COLLISION_LAYER_PROJECTILE;
    }
    if (token == "agent_vision") {
        return KERNEL_COLLISION_LAYER_AGENT_VISION;
    }
    if (token == "terrain") {
        return KERNEL_COLLISION_LAYER_TERRAIN;
    }
    if (token == "obstacle" || token == "static_obstacle") {
        return KERNEL_COLLISION_LAYER_STATIC_OBSTACLE;
    }
    if (token == "prop") {
        return KERNEL_COLLISION_MASK_PROP;
    }
    // A rig's own bones. Pair it with a side -- gameplay_category_mask is built
    // from the side bits alone, so "limb" by itself matches nothing and the
    // shot passes straight through.
    if (token == "limb") {
        return KERNEL_COLLISION_LAYER_LIMB;
    }
    throw std::runtime_error("unsupported collision_mask: " + token);
}

std::uint32_t collision_mask_from_yaml(
    const YAML::Node& node,
    std::uint32_t default_mask = KERNEL_COLLISION_MASK_DAMAGEABLE) {
    if (!node) {
        return default_mask;
    }
    const std::string value = node.as<std::string>();
    std::uint32_t mask = 0;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t separator = value.find('|', start);
        const std::string token = trim_ascii(value.substr(
            start,
            separator == std::string::npos ? std::string::npos : separator - start));
        if (token.empty()) {
            throw std::runtime_error("empty collision_mask token: " + value);
        }
        mask |= collision_mask_token_from_yaml(token);
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    return mask;
}

// movement.collision_mask names the geometry that stops a body, which is a
// different axis from the gameplay categories collision_mask_from_yaml parses --
// see the KERNEL_MOVEMENT_LAYER_* comment in kernel_types.h. It is spelled the
// same way as a projectile's collision_mask (projectile_templates/rocket.yaml)
// because it is authored the same way -- a '|' list of layer names -- but it is
// kept as its own token set so the two vocabularies cannot be confused:
// "hostile_side" is meaningless under movement and must fail loudly rather than
// resolve to some unrelated bit.
std::uint32_t movement_collision_layer_token_from_yaml(
    const std::string& token) {
    if (token == "terrain") {
        return KERNEL_MOVEMENT_LAYER_TERRAIN;
    }
    if (token == "obstacle" || token == "static_obstacle") {
        return KERNEL_MOVEMENT_LAYER_STATIC_OBSTACLE;
    }
    if (token == "actor") {
        return KERNEL_MOVEMENT_LAYER_ACTOR;
    }
    // A rig's own bones. Naming it is the only way to collide with legs, which
    // is what keeps the cost off every template that has no use for them.
    if (token == "limb") {
        return KERNEL_MOVEMENT_LAYER_LIMB;
    }
    throw std::runtime_error("unsupported movement collision_mask: " + token);
}

std::uint32_t movement_collision_mask_from_yaml(const YAML::Node& node) {
    const std::string value = node.as<std::string>();
    std::uint32_t mask = 0u;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t separator = value.find('|', start);
        const std::string token = trim_ascii(value.substr(
            start,
            separator == std::string::npos
                ? std::string::npos
                : separator - start));
        if (token.empty()) {
            throw std::runtime_error(
                "empty movement collision_mask token: " + value);
        }
        mask |= movement_collision_layer_token_from_yaml(token);
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    if (mask == 0u) {
        throw std::runtime_error(
            "movement collision_mask must name at least one layer: " + value);
    }
    return mask;
}

// hit_zone is authored as a decimal multiplier and stored as hundredths: 0.5
// becomes 50. Rounded rather than truncated, or 0.29 lands on 28 the moment the
// literal is not exactly representable. The step is therefore 0.01, and a value
// finer than that is rounded rather than rejected -- but anything that is not a
// sane multiplier fails loudly here rather than becoming a silently harmless or
// absurdly lethal volume.
std::uint16_t hit_zone_from_yaml(
    const YAML::Node& node,
    const std::string& context) {
    if (!node) {
        return KERNEL_HIT_ZONE_UNSCALED;
    }
    const double value = node.as<double>();
    if (!std::isfinite(value) || value < 0.0) {
        throw std::runtime_error(
            "hit_zone must be finite and non-negative: " + context);
    }
    const double hundredths = std::round(value * 100.0);
    if (hundredths > 65535.0) {
        throw std::runtime_error(
            "hit_zone exceeds the maximum multiplier of 655.35: " + context);
    }
    return static_cast<std::uint16_t>(hundredths);
}

void require_supported_collision_mask(
    std::uint32_t mask,
    std::uint32_t supported_mask,
    const std::string& context) {
    if ((mask & ~supported_mask) != 0u) {
        throw std::runtime_error(
            context + " contains unsupported collision_mask bits");
    }
}

std::uint8_t motion_model_from_yaml(const YAML::Node& node) {
    const std::string value = node ? node.as<std::string>() : "linear";
    if (value == "linear") {
        return KernelProjectileMotionModel_Linear;
    }
    if (value == "parabolic") {
        return KernelProjectileMotionModel_Parabolic;
    }
    if (value == "homing") {
        return KernelProjectileMotionModel_Homing;
    }
    throw std::runtime_error("unsupported projectile movement_model: " + value);
}

std::uint8_t hit_response_from_yaml(const YAML::Node& node) {
    const std::string value = node ? node.as<std::string>() : "destroy";
    if (value == "destroy") {
        return KernelProjectileHitResponse_Destroy;
    }
    if (value == "continue") {
        return KernelProjectileHitResponse_Continue;
    }
    if (value == "bounce" || value == "attach") {
        throw std::runtime_error("projectile hit response is reserved: " + value);
    }
    throw std::runtime_error("unsupported projectile hit_response: " + value);
}

std::uint8_t damage_shape_from_yaml(const YAML::Node& node) {
    const std::string value = node ? node.as<std::string>() : "direct_hit";
    if (value == "direct_hit") {
        return KernelProjectileDamageShape_DirectHit;
    }
    if (value == "none") {
        return KernelProjectileDamageShape_None;
    }
    if (value == "explosion") {
        throw std::runtime_error(
            "projectile damage_shape explosion has moved to an impact trigger");
    }
    if (value == "piercing_segment") {
        return KernelProjectileDamageShape_PiercingSegment;
    }
    if (value == "persistent_beam") {
        throw std::runtime_error("beam damage shape is not supported in this phase");
    }
    throw std::runtime_error("unsupported projectile damage_shape: " + value);
}

std::uint8_t projectile_type_from_yaml(const YAML::Node& node) {
    const std::string value = node ? node.as<std::string>() : "projectile";
    if (value == "projectile" || value == "standard") {
        return KernelProjectileType_Standard;
    }
    if (value == "area_effect") {
        return KernelProjectileType_AreaEffect;
    }
    if (value == "beam") {
        return KernelProjectileType_Beam;
    }
    throw std::runtime_error("unsupported projectile type: " + value);
}

std::uint8_t damage_falloff_from_yaml(const YAML::Node& node) {
    const std::string value = node ? node.as<std::string>() : "none";
    if (value == "none") {
        return KernelProjectileDamageFalloff_None;
    }
    if (value == "linear") {
        return KernelProjectileDamageFalloff_Linear;
    }
    throw std::runtime_error("unsupported projectile damage falloff: " + value);
}

std::uint8_t collision_query_mode_from_yaml(const YAML::Node& node) {
    const std::string value = node ? node.as<std::string>() : "auto";
    if (value == "auto") {
        return KernelProjectileCollisionQueryMode_Auto;
    }
    if (value == "overlap") {
        return KernelProjectileCollisionQueryMode_Overlap;
    }
    if (value == "sweep" || value == "swept") {
        return KernelProjectileCollisionQueryMode_Sweep;
    }
    if (value == "ray") {
        return KernelProjectileCollisionQueryMode_Ray;
    }
    throw std::runtime_error("unsupported projectile collision_query_mode: " + value);
}

std::uint8_t homing_mode_from_yaml(const YAML::Node& node) {
    const std::string value = node ? node.as<std::string>() : "fire_and_forget";
    if (value == "fire_and_forget") {
        return KernelHomingMode_FireAndForget;
    }
    throw std::runtime_error("unsupported homing_mode: " + value);
}

std::uint8_t projectile_sync_mode_from_yaml(const YAML::Node& node) {
    const std::string value =
        node ? node.as<std::string>() : "hybrid_deterministic_then_snapshot";
    if (value == "local_predicted_deterministic") {
        return KernelProjectileSyncMode_LocalPredictedDeterministic;
    }
    if (value == "server_snapshot_only") {
        return KernelProjectileSyncMode_ServerSnapshotOnly;
    }
    if (value == "hybrid_deterministic_then_snapshot") {
        return KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
    }
    throw std::runtime_error("unsupported projectile sync_mode: " + value);
}

std::uint8_t projectile_sync_mode_from_weapon_yaml(const YAML::Node&) {
    return KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
}

KernelVec3 vec3_from_yaml(const YAML::Node& node) {
    if (!node) {
        return KernelVec3{0.0f, 0.0f, 0.0f};
    }
    return KernelVec3{
        node["x"].as<float>(),
        node["y"].as<float>(),
        node["z"].as<float>()};
}

bool finite_vec3_value(const KernelVec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

float vec3_length_squared(const KernelVec3& value) {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

std::optional<KernelVec3> optional_vec3_default_from_yaml(
    const YAML::Node& node,
    const std::string& parameter_name,
    const std::string& path) {
    if (!node || !node.IsMap()) {
        return std::nullopt;
    }
    if (parameter_name != "direction" || node.size() != 3u ||
        !node["x"] || !node["y"] || !node["z"] ||
        !node["x"].IsScalar() || !node["y"].IsScalar() ||
        !node["z"].IsScalar()) {
        throw std::runtime_error(
            "action graph vec3 default is only supported for parameters.direction: " +
            path);
    }
    const KernelVec3 value = vec3_from_yaml(node);
    if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
        !std::isfinite(value.z) ||
        (value.x == 0.0f && value.y == 0.0f && value.z == 0.0f)) {
        throw std::runtime_error(
            "action graph parameters.direction must be a non-zero finite vec3: " +
            path);
    }
    return value;
}

// Plain suffix match, deliberately not std::filesystem::path::extension(): the
// interesting suffix here is ".skeleton_manifest.json", and extension() would
// only ever see ".json".
bool has_extension(std::string_view path, std::string_view extension) {
    return path.size() >= extension.size() &&
           path.compare(
               path.size() - extension.size(),
               extension.size(),
               extension) == 0;
}

class GameplayConfigSource {
public:
    virtual ~GameplayConfigSource() = default;
    virtual YAML::Node load_yaml(const std::string& path) const = 0;
    virtual std::vector<std::uint8_t> load_bytes(
        const std::string& path) const = 0;
    // Sorted, so a directory scan feeds templates to the loader in a stable
    // order regardless of how the filesystem or the archive enumerates them.
    virtual std::vector<std::string> list_files(
        const std::string& directory,
        std::string_view extension) const = 0;
    std::vector<std::string> list_yaml_files(
        const std::string& directory) const {
        return list_files(directory, ".yaml");
    }
    std::vector<std::string> list_json_files(
        const std::string& directory) const {
        return list_files(directory, ".json");
    }
    virtual std::string parent_path(const std::string& path) const = 0;
    virtual std::string resolve_path(
        const std::string& base_path,
        const YAML::Node& node) const = 0;
    virtual std::string default_collider_template_dir_for_weapon_dir(
        const std::string& directory) const = 0;
    virtual std::string default_projectile_template_dir_for_weapon_dir(
        const std::string& directory) const = 0;
    virtual std::uint32_t source_kind() const = 0;
};

class FilesystemGameplayConfigSource final : public GameplayConfigSource {
public:
    YAML::Node load_yaml(const std::string& path) const override {
        try {
            return YAML::LoadFile(path);
        } catch (const YAML::Exception& error) {
            throw DataLoadError(
                KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_YAML,
                error.what(),
                path,
                {},
                source_kind(),
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_UNKNOWN,
                0,
                0,
                error.mark.line >= 0 ? error.mark.line + 1 : -1,
                error.mark.column >= 0 ? error.mark.column + 1 : -1);
        }
    }

    std::vector<std::uint8_t> load_bytes(
        const std::string& path) const override {
        std::ifstream input(path, std::ios::binary);
        const std::vector<std::uint8_t> bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        if (input.bad() || bytes.empty()) {
            throw std::runtime_error("failed to read binary asset: " + path);
        }
        return bytes;
    }

    std::vector<std::string> list_files(
        const std::string& directory,
        std::string_view extension) const override {
        std::vector<std::string> files;
        if (!std::filesystem::exists(directory)) {
            return files;
        }
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(directory)) {
            if (entry.is_regular_file() &&
                has_extension(entry.path().filename().string(), extension)) {
                files.push_back(entry.path().string());
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    std::string parent_path(const std::string& path) const override {
        return std::filesystem::path(path).parent_path().string();
    }

    std::string resolve_path(
        const std::string& base_path,
        const YAML::Node& node) const override {
        std::filesystem::path path = node.as<std::string>();
        if (path.is_relative()) {
            path = std::filesystem::path(base_path) / path;
        }
        return path.lexically_normal().string();
    }

    std::string default_collider_template_dir_for_weapon_dir(
        const std::string& directory) const override {
        return (std::filesystem::path(directory).parent_path() /
                "collider_templates")
            .lexically_normal()
            .string();
    }

    std::string default_projectile_template_dir_for_weapon_dir(
        const std::string& directory) const override {
        return (std::filesystem::path(directory).parent_path() /
                "projectile_templates")
            .lexically_normal()
            .string();
    }

    std::uint32_t source_kind() const override {
        return KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_FILESYSTEM;
    }
};

std::string normalize_archive_path(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find(':') != std::string::npos) {
        throw DataLoadError(
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_ARCHIVE_PATH,
            "invalid archive path: " + path,
            path,
            {},
            KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE);
    }

    std::vector<std::string> parts;
    std::string part;
    for (const char value : path) {
        const char normalized = value == '\\' ? '/' : value;
        if (normalized == '/') {
            if (!part.empty() && part != ".") {
                if (part == "..") {
                    throw DataLoadError(
                        KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_ARCHIVE_PATH,
                        "invalid archive path: " + path,
                        path,
                        {},
                        KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE);
                }
                parts.push_back(part);
            }
            part.clear();
            continue;
        }
        part.push_back(normalized);
    }
    if (!part.empty() && part != ".") {
        if (part == "..") {
            throw DataLoadError(
                KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_ARCHIVE_PATH,
                "invalid archive path: " + path,
                path,
                {},
                KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE);
        }
        parts.push_back(part);
    }
    if (parts.empty()) {
        throw DataLoadError(
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_ARCHIVE_PATH,
            "invalid archive path: " + path,
            path,
            {},
            KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE);
    }

    std::string normalized;
    for (const std::string& component : parts) {
        if (!normalized.empty()) {
            normalized.push_back('/');
        }
        normalized += component;
    }
    return normalized;
}

std::string archive_parent_path(const std::string& path) {
    const std::string normalized = normalize_archive_path(path);
    const std::size_t separator = normalized.find_last_of('/');
    return separator == std::string::npos ? std::string{} : normalized.substr(0, separator);
}

std::string archive_join_path(
    const std::string& base_path,
    const std::string& relative_path) {
    if (base_path.empty()) {
        return normalize_archive_path(relative_path);
    }
    return normalize_archive_path(base_path + "/" + relative_path);
}

bool has_yaml_extension(const std::string& path) {
    return has_extension(path, ".yaml");
}

bool has_json_extension(const std::string& path) {
    return has_extension(path, ".json");
}

bool has_ozz_extension(const std::string& path) {
    return has_extension(path, ".ozz");
}

class MemoryZipGameplayConfigSource final : public GameplayConfigSource {
public:
    MemoryZipGameplayConfigSource(
        const std::uint8_t* bundle_bytes,
        std::uint32_t bundle_size) {
        if (bundle_bytes == nullptr || bundle_size == 0) {
            throw std::runtime_error("gameplay catalog bundle is empty");
        }

        struct ZipStreamCloser {
            void operator()(struct zip_t* zip) const {
                zip_stream_close(zip);
            }
        };

        std::unique_ptr<struct zip_t, ZipStreamCloser> archive(zip_stream_open(
            reinterpret_cast<const char*>(bundle_bytes),
            bundle_size,
            0,
            'r'));
        if (!archive) {
            throw std::runtime_error("failed to open gameplay catalog bundle");
        }

        const ssize_t total_entries = zip_entries_total(archive.get());
        if (total_entries < 0) {
            throw std::runtime_error("failed to list gameplay catalog bundle entries");
        }

        std::uint64_t total_yaml_bytes = 0;
        std::unordered_set<std::string> seen_paths;
        for (ssize_t index = 0; index < total_entries; ++index) {
            if (zip_entry_openbyindex(archive.get(), static_cast<std::size_t>(index)) != 0) {
                throw std::runtime_error("failed to open gameplay catalog bundle entry");
            }

            const char* entry_name = zip_entry_name(archive.get());
            if (entry_name == nullptr) {
                throw std::runtime_error("gameplay catalog bundle entry has no name");
            }
            const std::string path = normalize_archive_path(entry_name);
            if (!seen_paths.insert(path).second) {
                throw DataLoadError(
                    KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_DUPLICATE_ARCHIVE_ENTRY,
                    "duplicate archive entry: " + path,
                    path,
                    {},
                    KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE);
            }
            if (zip_entry_issymlink(archive.get())) {
                throw std::runtime_error("archive symlink entries are not supported: " + path);
            }
            if (zip_entry_isdir(archive.get())) {
                zip_entry_close(archive.get());
                continue;
            }
            const bool document_entry =
                has_yaml_extension(path) || has_json_extension(path);
            const bool skeleton_entry = has_ozz_extension(path);
            if (!document_entry && !skeleton_entry) {
                zip_entry_close(archive.get());
                continue;
            }

            const unsigned long long entry_size = zip_entry_size(archive.get());
            const std::uint64_t entry_limit = document_entry
                ? kMaxYamlEntryBytes
                : kMaxSkeletonAssetBytes;
            if (entry_size > entry_limit) {
                throw std::runtime_error("archive entry exceeds size limit: " + path);
            }
            if (document_entry) {
                total_yaml_bytes += entry_size;
                if (total_yaml_bytes > kMaxTotalYamlBytes) {
                    throw std::runtime_error(
                        "archive YAML content exceeds total size limit");
                }
            }
            std::string data(static_cast<std::size_t>(entry_size), '\0');
            if (entry_size > 0) {
                const ssize_t read_size = zip_entry_noallocread(
                    archive.get(),
                    data.data(),
                    data.size());
                if (read_size != static_cast<ssize_t>(data.size())) {
                    throw std::runtime_error("failed to read archive entry: " + path);
                }
            }
            files_.emplace(path, std::move(data));
            zip_entry_close(archive.get());
        }
    }

    YAML::Node load_yaml(const std::string& path) const override {
        const std::string normalized = normalize_archive_path(path);
        const auto found = files_.find(normalized);
        if (found == files_.end()) {
            throw DataLoadError(
                KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_MISSING_BUNDLE_ENTRY,
                "missing YAML file in bundle: " + normalized,
                normalized,
                {},
                KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE);
        }
        try {
            return YAML::Load(found->second);
        } catch (const YAML::Exception& error) {
            throw DataLoadError(
                KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_YAML,
                error.what(),
                normalized,
                {},
                source_kind(),
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_UNKNOWN,
                0,
                0,
                error.mark.line >= 0 ? error.mark.line + 1 : -1,
                error.mark.column >= 0 ? error.mark.column + 1 : -1);
        }
    }

    std::vector<std::uint8_t> load_bytes(
        const std::string& path) const override {
        const std::string normalized = normalize_archive_path(path);
        const auto found = files_.find(normalized);
        if (found == files_.end()) {
            throw DataLoadError(
                KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_MISSING_BUNDLE_ENTRY,
                "missing binary asset in bundle: " + normalized,
                normalized,
                {},
                KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE);
        }
        return std::vector<std::uint8_t>(
            found->second.begin(),
            found->second.end());
    }

    std::vector<std::string> list_files(
        const std::string& directory,
        std::string_view extension) const override {
        const std::string normalized_directory = normalize_archive_path(directory);
        const std::string prefix = normalized_directory + "/";
        std::vector<std::string> files;
        for (const auto& entry : files_) {
            const std::string& path = entry.first;
            if (!has_extension(path, extension) ||
                path.rfind(prefix, 0) != 0) {
                continue;
            }
            const std::string rest = path.substr(prefix.size());
            if (rest.find('/') == std::string::npos) {
                files.push_back(path);
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    std::string parent_path(const std::string& path) const override {
        return archive_parent_path(path);
    }

    std::string resolve_path(
        const std::string& base_path,
        const YAML::Node& node) const override {
        return archive_join_path(base_path, node.as<std::string>());
    }

    std::string default_collider_template_dir_for_weapon_dir(
        const std::string& directory) const override {
        return archive_join_path(
            archive_parent_path(directory),
            "collider_templates");
    }

    std::string default_projectile_template_dir_for_weapon_dir(
        const std::string& directory) const override {
        return archive_join_path(
            archive_parent_path(directory),
            "projectile_templates");
    }

    std::uint32_t source_kind() const override {
        return KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE;
    }

private:
    std::unordered_map<std::string, std::string> files_;
};

std::int32_t yaml_line(const YAML::Node& node) {
    return node.Mark().line >= 0 ? node.Mark().line + 1 : -1;
}

std::int32_t yaml_column(const YAML::Node& node) {
    return node.Mark().column >= 0 ? node.Mark().column + 1 : -1;
}

bool key_allowed(const std::string& key, std::initializer_list<const char*> keys) {
    for (const char* allowed : keys) {
        if (key == allowed) {
            return true;
        }
    }
    return false;
}

void reject_unknown_keys(
    const YAML::Node& node,
    std::initializer_list<const char*> keys,
    const std::string& path,
    std::uint32_t source_kind,
    std::uint32_t template_kind,
    std::uint32_t template_id) {
    if (!node || !node.IsMap()) {
        return;
    }
    for (const auto& entry : node) {
        if (!entry.first.IsScalar()) {
            throw DataLoadError(
                KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_FIELD_TYPE,
                "YAML field key must be a scalar",
                path,
                {},
                source_kind,
                template_kind,
                template_id,
                0,
                yaml_line(entry.first),
                yaml_column(entry.first));
        }
        const std::string key = entry.first.as<std::string>();
        if (!key_allowed(key, keys)) {
            throw DataLoadError(
                KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNKNOWN_FIELD,
                "unknown field: " + key,
                path,
                key,
                source_kind,
                template_kind,
                template_id,
                0,
                yaml_line(entry.first),
                yaml_column(entry.first));
        }
    }
}

bool valid_action_template_definition(
    const KernelActionTemplateDefinition& definition) {
    constexpr std::uint8_t kKnownFlags =
        KernelActionTemplateFlag_CancelOnRelease |
        KernelActionTemplateFlag_CancelOnDeath |
        KernelActionTemplateFlag_CancelOnWeaponChange |
        KernelActionTemplateFlag_CancelBeforeFirstCommit;
    if (definition.struct_size < sizeof(KernelActionTemplateDefinition) ||
        definition.action_template_id == 0u ||
        definition.trigger_mode > KernelActionTriggerMode_Hold ||
        (definition.flags & ~kKnownFlags) != 0u ||
        (definition.max_commit_count != 1u &&
         definition.commit_interval_ticks == 0u)) {
        return false;
    }
    if (definition.trigger_mode == KernelActionTriggerMode_Press) {
        return definition.max_commit_count >= 1u &&
               definition.hold_input_timeout_ticks == 0u;
    }
    return definition.hold_input_timeout_ticks > 0u;
}

std::uint8_t action_flag_from_name(const std::string& name) {
    if (name == "cancel_on_release") {
        return KernelActionTemplateFlag_CancelOnRelease;
    }
    if (name == "cancel_on_death") {
        return KernelActionTemplateFlag_CancelOnDeath;
    }
    if (name == "cancel_on_weapon_change") {
        return KernelActionTemplateFlag_CancelOnWeaponChange;
    }
    if (name == "cancel_before_first_commit") {
        return KernelActionTemplateFlag_CancelBeforeFirstCommit;
    }
    throw std::runtime_error("unknown action flag: " + name);
}

ActionTemplateConfig action_template_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind) {
    reject_unknown_keys(
        node,
        {
            "id",
            "name",
            "trigger_mode",
            "flags",
            "ammo_cost_per_commit",
            "commit_offset_ticks",
            "commit_interval_ticks",
            "max_commit_count",
            "recovery_ticks",
            "hold_input_timeout_ticks",
        },
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTION);
    ActionTemplateConfig action;
    action.name = node["name"].as<std::string>();
    action.source_path = path;
    action.source_kind = source_kind;
    action.commit_interval_line = yaml_line(node["commit_interval_ticks"]);
    action.commit_interval_column = yaml_column(node["commit_interval_ticks"]);
    KernelActionTemplateDefinition& definition = action.definition;
    definition.struct_size = sizeof(definition);
    definition.action_template_id = node["id"].as<std::uint32_t>();
    const std::string trigger_mode = node["trigger_mode"].as<std::string>();
    if (trigger_mode == "press") {
        definition.trigger_mode = KernelActionTriggerMode_Press;
    } else if (trigger_mode == "hold") {
        definition.trigger_mode = KernelActionTriggerMode_Hold;
    } else {
        throw DataLoadError(
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_ENUM_VALUE,
            "unknown action trigger_mode: " + trigger_mode,
            path,
            "trigger_mode",
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTION,
            definition.action_template_id);
    }
    const YAML::Node flags = node["flags"];
    if (!flags || !flags.IsSequence()) {
        throw DataLoadError(
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_FIELD_TYPE,
            "action flags must be a sequence",
            path,
            "flags",
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTION,
            definition.action_template_id);
    }
    for (const YAML::Node& flag : flags) {
        definition.flags |= action_flag_from_name(flag.as<std::string>());
    }
    definition.ammo_cost_per_commit =
        node["ammo_cost_per_commit"].as<std::uint16_t>();
    definition.commit_offset_ticks =
        node["commit_offset_ticks"].as<std::uint32_t>();
    definition.commit_interval_ticks =
        node["commit_interval_ticks"].as<std::uint32_t>();
    definition.max_commit_count =
        node["max_commit_count"].as<std::uint32_t>();
    definition.recovery_ticks = node["recovery_ticks"].as<std::uint32_t>();
    definition.hold_input_timeout_ticks =
        node["hold_input_timeout_ticks"].as<std::uint32_t>();
    const auto reject_numeric = [&](const char* field, const char* diagnostic) {
        throw DataLoadError(
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_NUMERIC_RANGE,
            diagnostic,
            path,
            field,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTION,
            definition.action_template_id,
            0,
            yaml_line(node[field]),
            yaml_column(node[field]));
    };
    if (definition.action_template_id == 0u) {
        reject_numeric("id", "action template id must be greater than 0");
    }
    if (definition.trigger_mode == KernelActionTriggerMode_Press &&
        definition.max_commit_count == 0u) {
        reject_numeric(
            "max_commit_count",
            "Press action max_commit_count must be at least 1");
    }
    if (definition.trigger_mode == KernelActionTriggerMode_Press &&
        definition.hold_input_timeout_ticks != 0u) {
        reject_numeric(
            "hold_input_timeout_ticks",
            "Press action hold_input_timeout_ticks must be 0");
    }
    if (definition.trigger_mode == KernelActionTriggerMode_Hold &&
        definition.hold_input_timeout_ticks == 0u) {
        reject_numeric(
            "hold_input_timeout_ticks",
            "Hold action hold_input_timeout_ticks must be greater than 0");
    }
    if ((definition.max_commit_count == 0u ||
         definition.max_commit_count > 1u) &&
        definition.commit_interval_ticks == 0u) {
        reject_numeric(
            "commit_interval_ticks",
            "multi-commit action commit_interval_ticks must be greater than 0");
    }
    if (action.name.empty() || !valid_action_template_definition(definition)) {
        throw DataLoadError(
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_NUMERIC_RANGE,
            "action template must satisfy the Kernel action contract",
            path,
            {},
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTION,
            definition.action_template_id);
    }
    return action;
}

std::vector<ActionTemplateConfig> load_action_templates_from_source(
    const GameplayConfigSource& source,
    const std::string& directory) {
    std::vector<ActionTemplateConfig> actions;
    for (const std::string& file : source.list_yaml_files(directory)) {
        ActionTemplateConfig action = action_template_from_yaml(
            source.load_yaml(file), file, source.source_kind());
        for (const ActionTemplateConfig& existing : actions) {
            if (existing.definition.action_template_id ==
                action.definition.action_template_id) {
                throw DataLoadError(
                    KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_DUPLICATE_TEMPLATE_ID,
                    "duplicate action template id",
                    file,
                    "id",
                    source.source_kind(),
                    KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTION,
                    action.definition.action_template_id);
            }
            if (existing.name == action.name) {
                throw DataLoadError(
                    KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_DUPLICATE_TEMPLATE_NAME,
                    "duplicate action template name: " + action.name,
                    file,
                    "name",
                    source.source_kind(),
                    KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTION,
                    action.definition.action_template_id);
            }
        }
        actions.push_back(std::move(action));
    }
    std::sort(
        actions.begin(),
        actions.end(),
        [](const ActionTemplateConfig& lhs, const ActionTemplateConfig& rhs) {
            return lhs.definition.action_template_id <
                   rhs.definition.action_template_id;
        });
    return actions;
}

std::string parameter_reference_from_yaml(
    const YAML::Node& node,
    const std::string& field) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error(
            "action graph " + field + " must be a params.* reference");
    }
    const std::string value = node.as<std::string>();
    constexpr std::string_view kPrefix = "params.";
    if (!value.starts_with(kPrefix) || value.size() == kPrefix.size()) {
        throw std::runtime_error(
            "action graph " + field + " must be a params.* reference");
    }
    return value.substr(kPrefix.size());
}

ActionGraphTemplateConfig action_graph_template_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind) {
    reject_unknown_keys(
        node,
        {"id", "parameters", "actions"},
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_UNKNOWN);
    ActionGraphTemplateConfig graph;
    graph.id = node["id"].as<std::string>();
    const YAML::Node parameters = node["parameters"];
    if (graph.id.empty() || !parameters || !parameters.IsMap()) {
        throw std::runtime_error(
            "action graph requires id and parameters map: " + path);
    }
    for (const auto& entry : parameters) {
        const std::string name = entry.first.as<std::string>();
        if (name.empty() ||
            std::any_of(
                graph.parameters.begin(),
                graph.parameters.end(),
                [&](const ActionGraphParameterConfig& parameter) {
                    return parameter.name == name;
                })) {
            throw std::runtime_error(
                "action graph parameter name must be unique: " + path);
        }
        const bool has_default = !entry.second.IsNull();
        const std::optional<KernelVec3> default_vec3 =
            optional_vec3_default_from_yaml(entry.second, name, path);
        graph.parameters.push_back(ActionGraphParameterConfig{
            name,
            has_default,
            has_default
                ? (entry.second.IsScalar()
                       ? entry.second.as<std::string>()
                       : std::string{})
                : std::string{},
            default_vec3,
        });
    }
    const YAML::Node actions = node["actions"];
    if (!actions || !actions.IsSequence() ||
        actions.size() > KERNEL_MAX_ACTION_GRAPH_ACTIONS) {
        throw std::runtime_error(
            "action graph requires between zero and " +
            std::to_string(KERNEL_MAX_ACTION_GRAPH_ACTIONS) +
            " actions: " + path);
    }
    for (std::size_t action_index = 0; action_index < actions.size();
         ++action_index) {
        const YAML::Node action = actions[action_index];
        reject_unknown_keys(
            action,
            {
                "type",
                "projectile_template",
                "entity_template",
                "position",
                "direction",
                "owner",
                "target",
                "amount",
                "strength",
                "status",
                "operation",
                "value",
                "collision_mask",
                "item_template",
                "quantity",
                "when",
            },
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_UNKNOWN);
        if (!action["type"]) {
            throw std::runtime_error(
                "action graph action requires type: " + path);
        }
        ActionGraphActionConfig compiled_action;
        compiled_action.action_type = action["type"].as<std::string>();
        if (action["when"]) {
            const std::string condition = action["when"].as<std::string>();
            if (condition != "event.has_target") {
                throw std::runtime_error(
                    "unsupported action condition: " + condition);
            }
            compiled_action.condition_type =
                KernelActionConditionType_EventHasTarget;
        }
        std::vector<const std::string*> action_parameters;
        if (compiled_action.action_type == "spawn_projectile") {
            if (action["entity_template"] || action["owner"] ||
                action["target"] || action["amount"] ||
                action["item_template"] || action["quantity"]) {
                throw std::runtime_error(
                    "spawn_projectile action has unsupported fields: " + path);
            }
            compiled_action.projectile_template_parameter =
                parameter_reference_from_yaml(
                    action["projectile_template"], "projectile_template");
            compiled_action.position_parameter =
                parameter_reference_from_yaml(action["position"], "position");
            compiled_action.direction_parameter =
                parameter_reference_from_yaml(action["direction"], "direction");
            action_parameters = {
                &compiled_action.projectile_template_parameter,
                &compiled_action.position_parameter,
                &compiled_action.direction_parameter,
            };
        } else if (compiled_action.action_type == "spawn_entity") {
            if (action["projectile_template"] || action["target"] ||
                action["amount"]) {
                throw std::runtime_error(
                    "spawn_entity action has unsupported fields: " + path);
            }
            compiled_action.entity_template_parameter =
                parameter_reference_from_yaml(
                    action["entity_template"], "entity_template");
            compiled_action.position_parameter =
                parameter_reference_from_yaml(action["position"], "position");
            if (action["direction"]) {
                compiled_action.direction_parameter =
                    parameter_reference_from_yaml(
                        action["direction"], "direction");
            }
            compiled_action.owner_parameter =
                parameter_reference_from_yaml(action["owner"], "owner");
            if (action["item_template"] || action["quantity"]) {
                if (!action["item_template"] || !action["quantity"]) {
                    throw std::runtime_error(
                        "spawn_entity item_template and quantity must be authored together: " +
                        path);
                }
                compiled_action.item_template_ref =
                    action["item_template"].as<std::string>();
                compiled_action.quantity = action["quantity"].as<std::uint32_t>();
                if (compiled_action.item_template_ref.empty() ||
                    compiled_action.quantity == 0u) {
                    throw std::runtime_error(
                        "spawn_entity item quantity must be positive: " + path);
                }
            }
            action_parameters = {
                &compiled_action.entity_template_parameter,
                &compiled_action.position_parameter,
                &compiled_action.owner_parameter,
            };
            if (!compiled_action.direction_parameter.empty()) {
                action_parameters.push_back(
                    &compiled_action.direction_parameter);
            }
        } else if (compiled_action.action_type == "apply_damage" ||
                   compiled_action.action_type == "apply_health_change") {
            if (action["projectile_template"] || action["position"] ||
                action["direction"] || action["entity_template"] ||
                action["owner"] || action["item_template"] ||
                action["quantity"] || action["strength"] ||
                action["collision_mask"]) {
                throw std::runtime_error(
                    compiled_action.action_type +
                    " action has unsupported fields: " + path);
            }
            compiled_action.target_parameter =
                parameter_reference_from_yaml(action["target"], "target");
            compiled_action.amount_parameter =
                parameter_reference_from_yaml(action["amount"], "amount");
            action_parameters = {
                &compiled_action.target_parameter,
                &compiled_action.amount_parameter,
            };
        } else if (compiled_action.action_type == "apply_impulse") {
            if (action["projectile_template"] || action["position"] ||
                action["owner"] || action["amount"] ||
                action["entity_template"] || action["item_template"] ||
                action["quantity"] || !action["direction"]) {
                throw std::runtime_error(
                    "apply_impulse requires target, strength, and direction only: " +
                    path);
            }
            compiled_action.target_parameter =
                parameter_reference_from_yaml(action["target"], "target");
            compiled_action.strength_parameter =
                parameter_reference_from_yaml(action["strength"], "strength");
            compiled_action.direction_parameter =
                parameter_reference_from_yaml(action["direction"], "direction");
            compiled_action.collision_mask = collision_mask_from_yaml(
                action["collision_mask"], KERNEL_COLLISION_MASK_ACTOR);
            if ((compiled_action.collision_mask &
                 ~(KERNEL_COLLISION_MASK_ACTOR | KERNEL_COLLISION_MASK_PROP)) != 0u ||
                compiled_action.collision_mask == KERNEL_COLLISION_MASK_NONE) {
                throw std::runtime_error(
                    "apply_impulse collision_mask must contain actor and/or prop: " +
                    path);
            }
            action_parameters = {
                &compiled_action.target_parameter,
                &compiled_action.strength_parameter,
                &compiled_action.direction_parameter,
            };
        } else if (compiled_action.action_type == "apply_status" ||
                   compiled_action.action_type == "remove_status") {
            if (action["projectile_template"] || action["position"] ||
                action["direction"] || action["owner"] || action["amount"] ||
                action["strength"] || action["operation"] || action["value"] ||
                action["entity_template"] || action["item_template"] ||
                action["quantity"] || action["collision_mask"]) {
                throw std::runtime_error(
                    compiled_action.action_type +
                    " action has unsupported fields: " + path);
            }
            compiled_action.target_parameter =
                parameter_reference_from_yaml(action["target"], "target");
            compiled_action.status_parameter =
                parameter_reference_from_yaml(action["status"], "status");
            action_parameters = {
                &compiled_action.target_parameter,
                &compiled_action.status_parameter,
            };
        } else if (compiled_action.action_type == "apply_speed_modifier") {
            if (action["projectile_template"] || action["position"] ||
                action["direction"] || action["owner"] || action["amount"] ||
                action["strength"] || action["status"] ||
                action["entity_template"] || action["item_template"] ||
                action["quantity"] || action["collision_mask"]) {
                throw std::runtime_error(
                    "apply_speed_modifier action has unsupported fields: " +
                    path);
            }
            compiled_action.target_parameter =
                parameter_reference_from_yaml(action["target"], "target");
            compiled_action.operation_parameter =
                parameter_reference_from_yaml(action["operation"], "operation");
            compiled_action.value_parameter =
                parameter_reference_from_yaml(action["value"], "value");
            action_parameters = {
                &compiled_action.target_parameter,
                &compiled_action.operation_parameter,
                &compiled_action.value_parameter,
            };
        } else {
            throw std::runtime_error(
                "unsupported action graph action type: " +
                compiled_action.action_type);
        }
        for (const std::string* action_parameter : action_parameters) {
            if (std::none_of(
                    graph.parameters.begin(),
                    graph.parameters.end(),
                    [&](const ActionGraphParameterConfig& parameter) {
                        return parameter.name == *action_parameter;
                    })) {
                throw std::runtime_error(
                    "action references undeclared graph parameter: " +
                    *action_parameter);
            }
        }
        graph.actions.push_back(std::move(compiled_action));
    }
    return graph;
}

std::vector<ActionGraphTemplateConfig> load_action_graph_templates_from_source(
    const GameplayConfigSource& source,
    const std::string& directory) {
    std::vector<ActionGraphTemplateConfig> graphs;
    for (const std::string& file : source.list_yaml_files(directory)) {
        ActionGraphTemplateConfig graph = action_graph_template_from_yaml(
            source.load_yaml(file), file, source.source_kind());
        if (std::any_of(
                graphs.begin(),
                graphs.end(),
                [&](const ActionGraphTemplateConfig& existing) {
                    return existing.id == graph.id;
                })) {
            throw std::runtime_error(
                "duplicate action graph id: " + graph.id);
        }
        graphs.push_back(std::move(graph));
    }
    std::sort(
        graphs.begin(),
        graphs.end(),
        [](const ActionGraphTemplateConfig& lhs,
           const ActionGraphTemplateConfig& rhs) {
            return lhs.id < rhs.id;
        });
    return graphs;
}

StatusEffectTemplateConfig status_effect_template_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind) {
    reject_unknown_keys(
        node,
        {"id", "name", "kind", "channel", "duration_ticks",
         "interval_ticks", "replace_policy", "max_stacks",
         "refresh_on_stack", "triggers"},
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_UNKNOWN);
    if (!node["id"] || !node["name"] || !node["channel"] ||
        !node["duration_ticks"] || !node["interval_ticks"] ||
        !node["replace_policy"] || !node["triggers"] || !node["kind"] ||
        node["kind"].as<std::string>("status_effect") != "status_effect") {
        throw std::runtime_error("status effect requires id, name, kind, channel, duration_ticks, interval_ticks, replace_policy, and triggers: " + path);
    }
    StatusEffectTemplateConfig status;
    status.status_effect_id = node["id"].as<std::uint32_t>();
    status.name = node["name"].as<std::string>();
    status.channel_name = node["channel"].as<std::string>();
    status.channel_id = stable_channel_id(status.channel_name);
    status.duration_ticks = node["duration_ticks"].as<std::uint32_t>();
    status.interval_ticks = node["interval_ticks"].as<std::uint32_t>();
    const std::string replacement_policy =
        node["replace_policy"].as<std::string>();
    if (replacement_policy == "replace") {
        status.replacement_policy =
            KernelStatusEffectReplacementPolicy_Replace;
    } else if (replacement_policy == "refresh") {
        status.replacement_policy =
            KernelStatusEffectReplacementPolicy_Refresh;
    } else if (replacement_policy == "stack") {
        status.replacement_policy =
            KernelStatusEffectReplacementPolicy_Stack;
    } else {
        throw std::runtime_error(
            "status effect replace_policy must be replace, refresh, or stack: " +
            path);
    }
    if (status.replacement_policy ==
        KernelStatusEffectReplacementPolicy_Stack) {
        if (!node["max_stacks"] || !node["refresh_on_stack"]) {
            throw std::runtime_error(
                "stack status requires max_stacks and refresh_on_stack: " + path);
        }
        status.max_stacks = node["max_stacks"].as<std::uint16_t>();
        status.refresh_on_stack = node["refresh_on_stack"].as<bool>();
        if (status.max_stacks < 2u || status.max_stacks > 32u) {
            throw std::runtime_error(
                "stack status max_stacks must be between 2 and 32: " + path);
        }
    } else if (node["max_stacks"] || node["refresh_on_stack"]) {
        throw std::runtime_error(
            "max_stacks and refresh_on_stack are only valid for stack status: " +
            path);
    }
    const YAML::Node triggers = node["triggers"];
    reject_unknown_keys(
        triggers,
        {"on_apply", "on_tick", "on_expire"},
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_UNKNOWN);
    const auto parse_trigger = [&](const char* name) {
        TriggerBindingConfig binding;
        const YAML::Node trigger = triggers[name];
        if (!trigger) {
            return binding;
        }
        reject_unknown_keys(
            trigger,
            {"action_graph", "parameters"},
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_UNKNOWN);
        binding.action_graph_ref = trigger["action_graph"].as<std::string>();
        if (trigger["parameters"]) {
            if (!trigger["parameters"].IsMap()) {
                throw std::runtime_error("status trigger parameters must be a map: " + path);
            }
            for (const auto& entry : trigger["parameters"]) {
                binding.parameters.emplace_back(
                    entry.first.as<std::string>(), entry.second.as<std::string>());
            }
        }
        return binding;
    };
    status.on_apply_trigger = parse_trigger("on_apply");
    status.on_tick_trigger = parse_trigger("on_tick");
    status.on_expire_trigger = parse_trigger("on_expire");
    if (status.status_effect_id == 0u || status.name.empty() ||
        status.channel_name.empty() || status.duration_ticks == 0u ||
        status.interval_ticks > status.duration_ticks ||
        (status.interval_ticks == 0u &&
         !status.on_tick_trigger.action_graph_ref.empty())) {
        throw std::runtime_error("invalid status effect timing or identity: " + path);
    }
    return status;
}

std::vector<StatusEffectTemplateConfig> load_status_effect_templates_from_source(
    const GameplayConfigSource& source,
    const std::string& directory) {
    std::vector<StatusEffectTemplateConfig> statuses;
    for (const std::string& file : source.list_yaml_files(directory)) {
        StatusEffectTemplateConfig status = status_effect_template_from_yaml(
            source.load_yaml(file), file, source.source_kind());
        if (std::any_of(
                statuses.begin(), statuses.end(),
                [&](const StatusEffectTemplateConfig& existing) {
                    return existing.status_effect_id == status.status_effect_id ||
                        existing.name == status.name;
                })) {
            throw std::runtime_error("duplicate status effect id or name: " + status.name);
        }
        if (std::any_of(
                statuses.begin(), statuses.end(),
                [&](const StatusEffectTemplateConfig& existing) {
                    return existing.channel_id == status.channel_id &&
                        existing.channel_name != status.channel_name;
                })) {
            throw std::runtime_error("status channel hash collision: " + status.channel_name);
        }
        statuses.push_back(std::move(status));
    }
    std::sort(
        statuses.begin(), statuses.end(),
        [](const StatusEffectTemplateConfig& lhs,
           const StatusEffectTemplateConfig& rhs) {
            return lhs.status_effect_id < rhs.status_effect_id;
        });
    return statuses;
}

const ActionGraphTemplateConfig* action_graph_template_from_ref(
    const std::string& id,
    const std::vector<ActionGraphTemplateConfig>& graphs) {
    const auto found = std::find_if(
        graphs.begin(),
        graphs.end(),
        [&](const ActionGraphTemplateConfig& graph) { return graph.id == id; });
    if (found == graphs.end()) {
        throw std::runtime_error("unknown action_graph reference: " + id);
    }
    return &*found;
}

const ActionTemplateConfig* action_template_from_ref(
    const YAML::Node& node,
    const std::vector<ActionTemplateConfig>& actions,
    const std::string& field) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error(field + " reference must be a scalar");
    }
    const std::string value = node.as<std::string>();
    const bool numeric = !value.empty() &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        });
    for (const ActionTemplateConfig& action : actions) {
        if ((numeric && action.definition.action_template_id ==
                            static_cast<std::uint32_t>(std::stoul(value))) ||
            (!numeric && action.name == value)) {
            return &action;
        }
    }
    throw DataLoadError(
        KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_MISSING_TEMPLATE_REFERENCE,
        "unknown " + field + " reference: " + value,
        {},
        field,
        KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_UNKNOWN,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTION);
}

KernelWeaponMechanicsDefinition weapon_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind) {
    const int authored_id = node["id"].as<int>();
    if (authored_id < 0 || authored_id > UINT8_MAX) {
        throw DataLoadError(
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_NUMERIC_RANGE,
            "weapon id must be in uint8 range",
            path,
            "id",
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_WEAPON);
    }
    const auto id = static_cast<std::uint8_t>(authored_id);
    reject_unknown_keys(
        node,
        {
            "id",
            "name",
            "weapon_type",
            "magazine_size",
            "reserve_magazines",
            "max_range",
            "pellet_count",
            "pellet_spread",
            "segment_collider",
            "projectile_template",
            "burst_count",
            "burst_spread_degrees",
            "area_effect",
            "beam",
            "fire_action_template",
            "reload_action_template",
        },
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_WEAPON,
        id);
    const std::uint16_t magazine_size = node["magazine_size"].as<std::uint16_t>();
    const std::uint16_t reserve_magazines =
        node["reserve_magazines"]
            ? node["reserve_magazines"].as<std::uint16_t>()
            : kDefaultReserveMagazines;
    const std::string type = node["weapon_type"].as<std::string>();
    if (type == "hitscan" || type == "shotgun") {
        if (node["projectile"] || node["area_effect"] || node["beam"]) {
            throw std::runtime_error(
                "instant weapons must not define projectile, area_effect, or beam");
        }
        if (!node["projectile_template"]) {
            throw std::runtime_error(
                "instant weapon requires projectile_template");
        }
        if (type == "shotgun") {
            KernelWeaponMechanicsDefinition weapon = shotgun_weapon(
                id,
                magazine_size,
                node["max_range"].as<float>(),
                static_cast<std::uint8_t>(node["pellet_count"].as<int>()),
                node["pellet_spread"].as<float>());
            weapon.reserve_magazines = reserve_magazines;
            return weapon;
        }
        KernelWeaponMechanicsDefinition weapon =
            hitscan_weapon(id, magazine_size, node["max_range"].as<float>());
        weapon.reserve_magazines = reserve_magazines;
        return weapon;
    }
    if (type == "projectile") {
        if (node["area_effect"] || node["beam"]) {
            throw std::runtime_error(
                "projectile weapons must not define area_effect or beam");
        }
        if (node["projectile"]) {
            throw std::runtime_error(
                "projectile weapons must use projectile_template, not inline projectile data");
        }
        if (!node["projectile_template"]) {
            throw std::runtime_error(
                "projectile weapon requires projectile_template");
        }
        KernelWeaponMechanicsDefinition weapon{};
        weapon.struct_size = sizeof(KernelWeaponMechanicsDefinition);
        weapon.weapon_id = id;
        weapon.fire_mode = KernelWeaponFireMode_Projectile;
        weapon.magazine_size = magazine_size;
        weapon.reserve_magazines = reserve_magazines;
        weapon.pellet_count =
            node["burst_count"]
                ? static_cast<std::uint8_t>(node["burst_count"].as<int>())
                : 1;
        weapon.pellet_spread =
            node["burst_spread_degrees"] ? node["burst_spread_degrees"].as<float>() : 0.0f;
        return weapon;
    }
    if (type == "area_effect") {
        if (node["projectile"] || node["area_effect"] || node["beam"]) {
            throw std::runtime_error(
                "area_effect weapons must use projectile_template, not inline mechanics");
        }
        if (!node["projectile_template"]) {
            throw std::runtime_error(
                "area_effect weapon requires projectile_template");
        }
        KernelWeaponMechanicsDefinition weapon =
            area_effect_weapon(id, magazine_size);
        weapon.reserve_magazines = reserve_magazines;
        return weapon;
    }
    if (type == "beam") {
        if (node["projectile"] || node["area_effect"] || node["beam"]) {
            throw std::runtime_error(
                "beam weapons must use projectile_template, not inline mechanics");
        }
        if (!node["projectile_template"]) {
            throw std::runtime_error("beam weapon requires projectile_template");
        }
        KernelWeaponMechanicsDefinition weapon =
            beam_weapon(id, magazine_size);
        weapon.reserve_magazines = reserve_magazines;
        return weapon;
    }
    throw std::runtime_error("unsupported weapon_type: " + type);
}

std::uint16_t entity_type_from_yaml(const YAML::Node& node) {
    const std::string value = node.as<std::string>();
    if (value == "actor") {
        return kEntityTypeActor;
    }
    if (value == "director") {
        return KernelEntityType_Director;
    }
    if (value == "player" || value == "enemy" || value == "agent") {
        return kEntityTypeActor;
    }
    if (value == "projectile") {
        return 3;
    }
    if (value == "area_effect" || value == "explosion") {
        return 4;
    }
    throw std::runtime_error("unsupported collider entity_type: " + value);
}

std::uint16_t actor_type_from_yaml(const YAML::Node& node) {
    if (!node) {
        return KernelActorType_Unknown;
    }
    const std::string value = node.as<std::string>();
    if (value == "player") {
        return kActorTypePlayer;
    }
    if (value == "enemy" || value == "agent") {
        return kActorTypeAgent;
    }
    return KernelActorType_Unknown;
}

std::uint8_t movement_controller_type_from_yaml(const YAML::Node& node) {
    const std::string value = node.as<std::string>();
    if (value == "none") {
        return KernelMovementControllerType_None;
    }
    if (value == "grounded") {
        return KernelMovementControllerType_Grounded;
    }
    if (value == "kinematic") {
        return KernelMovementControllerType_Kinematic;
    }
    if (value == "character") {
        return KernelMovementControllerType_Character;
    }
    throw std::runtime_error("unsupported movement controller: " + value);
}

std::uint16_t authored_entity_type_from_yaml(const YAML::Node& node) {
    const std::string value = node.as<std::string>();
    if (value == "actor") {
        return kEntityTypeActor;
    }
    if (value == "director") {
        return KernelEntityType_Director;
    }
    if (value == "prop") {
        return KernelEntityType_Prop;
    }
    throw std::runtime_error("unsupported entity_type: " + value);
}

TriggerBindingConfig trigger_binding_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind,
    std::uint32_t template_kind,
    std::uint32_t template_id,
    bool allow_collision_mask = false);

std::uint32_t item_capability_from_yaml(const std::string& value);

std::uint8_t camp_from_yaml(const YAML::Node& node) {
    if (!node) {
        return KernelAgentCamp_Unknown;
    }
    const std::string value = node.as<std::string>();
    if (value == "player_side") {
        return KernelAgentCamp_PlayerSide;
    }
    if (value == "enemy_side") {
        return KernelAgentCamp_EnemySide;
    }
    if (value == "neutral") {
        return KernelAgentCamp_Neutral;
    }
    throw std::runtime_error("unsupported camp: " + value);
}

std::uint8_t collider_shape_type_from_yaml(const YAML::Node& node) {
    const std::string value = node.as<std::string>();
    if (value == "aabb") {
        return KernelColliderShapeType_Aabb;
    }
    if (value == "sphere") {
        return KernelColliderShapeType_Sphere;
    }
    if (value == "oriented_box") {
        return KernelColliderShapeType_OrientedBox;
    }
    if (value == "segment") {
        return KernelColliderShapeType_Segment;
    }
    if (value == "cone") {
        return KernelColliderShapeType_Cone;
    }
    if (value == "capsule") {
        return KernelColliderShapeType_Capsule;
    }
    throw std::runtime_error("unsupported collider shape: " + value);
}

std::uint32_t collider_purpose_from_yaml(const YAML::Node& node) {
    const std::string value = node.as<std::string>();
    if (value == "hit") {
        return KernelColliderPurpose_Hit;
    }
    if (value == "damage") {
        return KernelColliderPurpose_Damage;
    }
    if (value == "vision") {
        return KernelColliderPurpose_Vision;
    }
    if (value == "movement") {
        return KernelColliderPurpose_Movement;
    }
    throw std::runtime_error("unsupported collider purpose: " + value);
}

std::uint32_t collider_layer_from_yaml(const YAML::Node& node) {
    return collision_mask_from_yaml(node);
}

std::uint32_t skeleton_collision_purpose_token_from_yaml(const std::string& token) {
    if (token == "limb") {
        return KernelColliderPurpose_Limb;
    }
    if (token == "hit") {
        return KernelColliderPurpose_Hit;
    }
    if (token == "damage") {
        return KernelColliderPurpose_Damage;
    }
    if (token == "trigger") {
        return KernelColliderPurpose_Trigger;
    }
    // Deliberately no "movement" and no "vision": a bone-carried collider is
    // never the character controller's capsule, and vision is a root-anchored
    // cone. Both would be silently ignored downstream, so reject them here.
    throw std::runtime_error("unsupported limb collider purpose: " + token);
}

// A '|' list, unlike collider templates' single-valued purpose: a limb is
// usually several things at once (a limb that also blocks and can be shot).
std::uint32_t skeleton_collision_purpose_from_yaml(const YAML::Node& node) {
    if (!node) {
        return KernelColliderPurpose_Limb;
    }
    const std::string value = node.as<std::string>();
    std::uint32_t flags = 0;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t separator = value.find('|', start);
        const std::string token = trim_ascii(value.substr(
            start,
            separator == std::string::npos ? std::string::npos
                                           : separator - start));
        if (token.empty()) {
            throw std::runtime_error(
                "empty limb collider purpose token: " + value);
        }
        flags |= skeleton_collision_purpose_token_from_yaml(token);
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    // Carried by a bone whatever else it is, so the runtime can tell limbs from
    // root-anchored colliders without consulting the bone index.
    return flags | KernelColliderPurpose_Limb;
}

std::uint8_t skeleton_collider_shape_from_yaml(const YAML::Node& node) {
    if (!node) {
        return KernelColliderShapeType_OrientedBox;
    }
    const std::string value = node.as<std::string>();
    if (value == "oriented_box" || value == "box") {
        return KernelColliderShapeType_OrientedBox;
    }
    if (value == "sphere") {
        return KernelColliderShapeType_Sphere;
    }
    throw std::runtime_error("unsupported limb collider shape: " + value);
}

KernelVec4 collider_shape_params_from_yaml(
    std::uint8_t shape_type,
    const YAML::Node& node) {
    if (shape_type == KernelColliderShapeType_Aabb ||
        shape_type == KernelColliderShapeType_OrientedBox) {
        return KernelVec4{
            node["half_extents"]["x"].as<float>(),
            node["half_extents"]["y"].as<float>(),
            node["half_extents"]["z"].as<float>(),
            0.0f,
        };
    }
    if (shape_type == KernelColliderShapeType_Sphere) {
        return KernelVec4{
            node["radius"].as<float>(),
            0.0f,
            0.0f,
            0.0f,
        };
    }
    if (shape_type == KernelColliderShapeType_Segment) {
        // A weapon segment's two endpoints are computed at fire time from the
        // shooter's muzzle, aim, and the weapon's max_range, so the template
        // has no reach of its own to declare -- it used to carry a `length`
        // that duplicated max_range and was never read. `radius` stays: a
        // segment with thickness is a real shape the query could honour.
        return KernelVec4{
            0.0f,
            node["radius"] ? node["radius"].as<float>() : 0.0f,
            0.0f,
            0.0f,
        };
    }
    if (shape_type == KernelColliderShapeType_Cone) {
        return KernelVec4{
            node["range"].as<float>(),
            node["fov_degrees"].as<float>(),
            0.0f,
            0.0f,
        };
    }
    if (shape_type == KernelColliderShapeType_Capsule) {
        return KernelVec4{
            node["half_height"].as<float>(),
            node["radius"].as<float>(),
            0.0f,
            0.0f,
        };
    }
    return KernelVec4{};
}

void apply_default_non_weapon_config(GameServerGameplayConfig* config);
void apply_catalog_player_config(
    const YAML::Node& document,
    GameServerGameplayConfig* config);
void apply_catalog_director_preload_config(
    const YAML::Node& document,
    GameServerGameplayConfig* config);
void apply_catalog_agent_config(
    const YAML::Node& document,
    GameServerGameplayConfig* config);
std::uint32_t collider_template_id_from_ref(
    const YAML::Node& node,
    const ColliderCatalogConfig& colliders);

ColliderCatalogConfig load_collider_catalog_from_source(
    const GameplayConfigSource& source,
    const std::string& directory) {
    ColliderCatalogConfig colliders;
    std::unordered_map<std::string, std::uint32_t> template_ids;
    std::unordered_map<std::uint32_t, std::string> template_names_by_id;
    const std::vector<std::string> files = source.list_yaml_files(directory);
    for (const std::string& file : files) {
        const YAML::Node node = source.load_yaml(file);
        reject_unknown_keys(
            node,
            {
                "id",
                "name",
                "shape",
                "center",
                "half_extents",
                "half_height",
                "radius",
                "range",
                "fov_degrees",
                "lifetime_ticks",
                "purpose",
                "layer",
            },
            file,
            source.source_kind(),
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_COLLIDER);
        ColliderTemplateConfig collider_template;
        collider_template.name = node["name"].as<std::string>();
        KernelColliderTemplateDefinition& definition =
            collider_template.definition;
        definition.struct_size = sizeof(KernelColliderTemplateDefinition);
        definition.template_id = node["id"].as<std::uint32_t>();
        definition.shape_type = collider_shape_type_from_yaml(node["shape"]);
        definition.center =
            node["center"] ? vec3_from_yaml(node["center"]) : KernelVec3{};
        definition.shape_params =
            collider_shape_params_from_yaml(definition.shape_type, node);
        definition.lifetime_ticks =
            node["lifetime_ticks"] ? node["lifetime_ticks"].as<std::uint32_t>() : 0u;
        definition.purpose_flags = collider_purpose_from_yaml(node["purpose"]);
        definition.layer_mask = collider_layer_from_yaml(node["layer"]);
        if (template_ids.contains(collider_template.name)) {
            throw std::runtime_error(
                "duplicate collider template name: " + collider_template.name);
        }
        if (template_names_by_id.contains(definition.template_id)) {
            throw DataLoadError(
                KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_DUPLICATE_TEMPLATE_ID,
                "duplicate collider template id: " +
                    std::to_string(definition.template_id),
                file,
                "id",
                source.source_kind(),
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_COLLIDER,
                definition.template_id);
        }
        template_ids[collider_template.name] = definition.template_id;
        template_names_by_id[definition.template_id] = collider_template.name;
        colliders.templates.push_back(collider_template);
    }
    if (colliders.templates.empty()) {
        throw std::runtime_error(
            "collider template directory is empty: " + directory);
    }
    std::sort(
        colliders.templates.begin(),
        colliders.templates.end(),
        [](const ColliderTemplateConfig& lhs,
           const ColliderTemplateConfig& rhs) {
            return lhs.definition.template_id < rhs.definition.template_id;
        });
    return colliders;
}

ActorTemplateConfig default_player_actor_template() {
    ActorTemplateConfig actor_template;
    actor_template.actor_template_id = 1;
    actor_template.name = "player";
    actor_template.entity_type = kEntityTypeActor;
    actor_template.actor_type = kActorTypePlayer;
    actor_template.collider_template_id = 1;
    actor_template.health = EntityHealthDefinition{100, 100};
    actor_template.hitbox_center = KernelVec3{0.0f, 0.9f, 0.0f};
    actor_template.hitbox_half_extents = KernelVec3{0.35f, 0.9f, 0.35f};
    actor_template.move_speed_meters_per_second = 5.0f;
    actor_template.movement_controller_type = KernelMovementControllerType_Character;
    actor_template.movement_collider_template_id = 10;
    actor_template.weapon_ids[0] = kWeaponRifle;
    actor_template.weapon_ids[1] = kWeaponShotgun;
    actor_template.weapon_slot_count = 2;
    actor_template.active_weapon_slot = 0;
    actor_template.vision.struct_size = sizeof(KernelAgentVisionConfig);
    actor_template.vision.camp = KernelAgentCamp_PlayerSide;
    return actor_template;
}

ActorTemplateConfig default_sentry_actor_template() {
    ActorTemplateConfig actor_template;
    actor_template.actor_template_id = 2;
    actor_template.name = "sentry_grunt";
    actor_template.entity_type = kEntityTypeActor;
    actor_template.actor_type = kActorTypeAgent;
    actor_template.collider_template_id = 2;
    actor_template.health =
        EntityHealthDefinition{kAgentInitialHp, kAgentInitialHp};
    actor_template.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    actor_template.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    actor_template.move_speed_meters_per_second = 2.5f;
    actor_template.movement_controller_type = KernelMovementControllerType_Grounded;
    actor_template.movement_collider_template_id = 11;
    actor_template.weapon_ids[0] = kAgentSpammerWeaponId;
    actor_template.weapon_slot_count = 1;
    actor_template.active_weapon_slot = 0;
    actor_template.animation_idle = 0;
    actor_template.animation_chasing = 0;
    actor_template.sentry.weapon_id = active_weapon_id(actor_template);
    actor_template.sentry.animation_idle = actor_template.animation_idle;
    actor_template.sentry.animation_attack = actor_template.animation_chasing;
    actor_template.vision.struct_size = sizeof(KernelAgentVisionConfig);
    actor_template.vision.camp = KernelAgentCamp_EnemySide;
    actor_template.vision.vision_collider_template_id = 9;
    actor_template.vision.max_visible_hostiles = KERNEL_MAX_VISIBLE_HOSTILES;
    actor_template.vision.max_visible_allies = KERNEL_MAX_VISIBLE_ALLIES;
    actor_template.vision.max_visible_neutrals = KERNEL_MAX_VISIBLE_NEUTRALS;
    actor_template.vision.local_forward = KernelVec3{-1.0f, 0.0f, 0.0f};
    actor_template.ai_controller_type = KernelAiControllerType_Sentry;
    actor_template.ai_tick_interval = 1;
    return actor_template;
}

void apply_default_actor_templates(GameServerGameplayConfig* config) {
    config->actor_templates.clear();
    config->actor_templates.push_back(default_player_actor_template());
    config->actor_templates.push_back(default_sentry_actor_template());
    config->entity_templates = config->actor_templates;
    config->player.actor_template_id = 1;
    config->agent.actor_template_id = 2;
}

std::uint16_t sentry_animation_from_yaml(
    const YAML::Node& node,
    const ActorTemplateConfig& actor_template,
    const std::string& field) {
    const std::string animation_ref = node.as<std::string>();
    if (animation_ref == "idle") {
        return actor_template.animation_idle;
    }
    if (animation_ref == "chasing") {
        return actor_template.animation_chasing;
    }
    try {
        return node.as<std::uint16_t>();
    } catch (const std::exception&) {
        throw std::runtime_error("unsupported sentry " + field + ": " + animation_ref);
    }
}

bool actor_template_has_weapon(
    const ActorTemplateConfig& actor_template,
    std::uint8_t weapon_id) {
    for (std::uint8_t slot = 0; slot < actor_template.weapon_slot_count; ++slot) {
        if (actor_template.weapon_ids[slot] == weapon_id) {
            return true;
        }
    }
    return false;
}

AgentSentryConfig sentry_config_from_yaml(
    const YAML::Node& node,
    const ActorTemplateConfig& actor_template,
    const WeaponCatalogConfig& weapons,
    const std::string& path,
    std::uint32_t source_kind) {
    AgentSentryConfig sentry = actor_template.sentry;
    sentry.move_speed_meters_per_second =
        actor_template.move_speed_meters_per_second;
    sentry.weapon_id = active_weapon_id(actor_template);
    sentry.animation_idle = actor_template.animation_idle;
    sentry.animation_attack = actor_template.animation_chasing;

    const YAML::Node sentry_node = node ? node["sentry"] : YAML::Node{};
    if (sentry_node) {
        reject_unknown_keys(
            sentry_node,
            {
                "alert_ticks",
                "forget_ticks",
                "ballistic_retry_cooldown_ticks",
                "patrol_rotation_interval_ticks",
                "patrol_rotation_min_degrees",
                "patrol_rotation_max_degrees",
                "passive_patrol",
                "patrol_extent_x_meters",
                "patrol_input_magnitude",
                "weapon_id",
                "animation_idle",
                "animation_attack",
            },
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
            actor_template.actor_template_id);
        if (sentry_node["alert_ticks"]) {
            sentry.alert_ticks = sentry_node["alert_ticks"].as<std::uint32_t>();
        }
        if (sentry_node["forget_ticks"]) {
            sentry.forget_ticks = sentry_node["forget_ticks"].as<std::uint32_t>();
        }
        if (sentry_node["ballistic_retry_cooldown_ticks"]) {
            sentry.ballistic_retry_cooldown_ticks =
                sentry_node["ballistic_retry_cooldown_ticks"]
                    .as<std::uint32_t>();
        }
        if (sentry_node["patrol_rotation_interval_ticks"]) {
            sentry.patrol_rotation_interval_ticks =
                sentry_node["patrol_rotation_interval_ticks"].as<std::uint32_t>();
        }
        if (sentry_node["patrol_rotation_min_degrees"]) {
            sentry.patrol_rotation_min_degrees =
                sentry_node["patrol_rotation_min_degrees"].as<float>();
        }
        if (sentry_node["patrol_rotation_max_degrees"]) {
            sentry.patrol_rotation_max_degrees =
                sentry_node["patrol_rotation_max_degrees"].as<float>();
        }
        if (sentry_node["passive_patrol"]) {
            sentry.passive_patrol = sentry_node["passive_patrol"].as<bool>();
        }
        if (sentry_node["patrol_extent_x_meters"]) {
            sentry.patrol_extent_x_meters =
                sentry_node["patrol_extent_x_meters"].as<float>();
        }
        if (sentry_node["patrol_input_magnitude"]) {
            sentry.patrol_input_magnitude =
                sentry_node["patrol_input_magnitude"].as<float>();
        }
        if (sentry_node["weapon_id"]) {
            const int authored_weapon_id = sentry_node["weapon_id"].as<int>();
            if (authored_weapon_id < 0 || authored_weapon_id > UINT8_MAX) {
                throw std::runtime_error(
                    "actor template sentry weapon id is out of uint8 range: " +
                    actor_template.name);
            }
            const auto weapon_id =
                static_cast<std::uint8_t>(authored_weapon_id);
            if (!weapons.configured[weapon_id] ||
                !actor_template_has_weapon(actor_template, weapon_id)) {
                throw std::runtime_error(
                    "actor template sentry references unknown weapon id: " +
                    actor_template.name);
            }
            sentry.weapon_id = weapon_id;
        }
        if (sentry_node["animation_idle"]) {
            sentry.animation_idle = sentry_animation_from_yaml(
                sentry_node["animation_idle"],
                actor_template,
                "animation_idle");
        }
        if (sentry_node["animation_attack"]) {
            sentry.animation_attack = sentry_animation_from_yaml(
                sentry_node["animation_attack"],
                actor_template,
                "animation_attack");
        }
    }
    return sentry;
}

AgentChaseTuning chaser_config_from_yaml(
    const YAML::Node& node,
    const ActorTemplateConfig& actor_template,
    const std::string& path,
    std::uint32_t source_kind) {
    AgentChaseTuning chaser = actor_template.chaser;
    const YAML::Node chaser_node = node ? node["chaser"] : YAML::Node{};
    if (!chaser_node) {
        return chaser;
    }
    reject_unknown_keys(
        chaser_node,
        {
            "stop_distance_meters",
            "resume_distance_meters",
            "input_magnitude",
        },
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
        actor_template.actor_template_id);
    if (chaser_node["stop_distance_meters"]) {
        chaser.stop_distance_meters =
            chaser_node["stop_distance_meters"].as<float>();
    }
    if (chaser_node["resume_distance_meters"]) {
        chaser.resume_distance_meters =
            chaser_node["resume_distance_meters"].as<float>();
    }
    if (chaser_node["input_magnitude"]) {
        chaser.input_magnitude = chaser_node["input_magnitude"].as<float>();
    }
    return chaser;
}

KernelAgentVisionConfig vision_config_from_yaml(
    const YAML::Node& node,
    const ActorTemplateConfig& actor_template,
    const ColliderCatalogConfig& colliders,
    const std::string& path,
    std::uint32_t source_kind) {
    KernelAgentVisionConfig vision = actor_template.vision;
    vision.struct_size = sizeof(KernelAgentVisionConfig);
    if (!node) {
        return vision;
    }
    reject_unknown_keys(
        node,
        {
            "collider_template",
            "max_visible_hostiles",
            "max_visible_allies",
            "max_visible_neutrals",
        },
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
        actor_template.actor_template_id);
    if (node["collider_template"]) {
        vision.vision_collider_template_id =
            collider_template_id_from_ref(node["collider_template"], colliders);
    }
    if (node["max_visible_hostiles"]) {
        vision.max_visible_hostiles = node["max_visible_hostiles"].as<std::uint32_t>();
    }
    if (node["max_visible_allies"]) {
        vision.max_visible_allies = node["max_visible_allies"].as<std::uint32_t>();
    }
    if (node["max_visible_neutrals"]) {
        vision.max_visible_neutrals = node["max_visible_neutrals"].as<std::uint32_t>();
    }
    return vision;
}

std::uint32_t actor_collider_template_id_from_yaml(
    const YAML::Node& node,
    const ColliderCatalogConfig& colliders) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error("actor collider_template reference must be a scalar");
    }
    const std::string value = node.as<std::string>();
    if (!value.empty() &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0;
        })) {
        const std::uint32_t template_id = static_cast<std::uint32_t>(
            std::stoul(value));
        for (const ColliderTemplateConfig& collider_template : colliders.templates) {
            if (collider_template.definition.template_id == template_id) {
                return template_id;
            }
        }
        throw std::runtime_error("unknown actor collider_template id: " + value);
    }
    for (const ColliderTemplateConfig& collider_template : colliders.templates) {
        if (collider_template.name == value) {
            return collider_template.definition.template_id;
        }
    }
    throw std::runtime_error("unknown actor collider_template name: " + value);
}

std::uint64_t uint64_from_yaml(const YAML::Node& node) {
    const std::string value = node.as<std::string>();
    std::size_t consumed = 0u;
    const std::uint64_t parsed = std::stoull(value, &consumed, 0);
    if (consumed != value.size()) {
        throw std::runtime_error("invalid uint64 value: " + value);
    }
    return parsed;
}

// Every *.skeleton_manifest.json in one directory, so adding a rig is a build
// rule plus a file rather than a catalog edit. Listing is sorted, so asset order
// -- and therefore the catalog hash -- does not depend on directory iteration
// order.
constexpr std::string_view kSkeletonManifestSuffix = ".skeleton_manifest.json";
constexpr std::string_view kSkeletonRigSuffix = ".rig.yaml";

std::uint32_t skeleton_bone_index(
    const SkeletonAssetConfig& asset,
    const std::string& name,
    const std::string& field);

// Reads <name>.rig.yaml, the hand-authored half of a rig's description. It says
// what the rig IS -- today, which bones carry colliders and what shape they are.
// Sizes are absent by design: the rigs express a collider's dimensions as the
// bone's own rest scale, so repeating them here would create a second copy that
// can disagree with the skeleton. Gameplay meaning is absent too, because whose
// side an actor is on is not a property of its skeleton.
void load_skeleton_rig(
    const GameplayConfigSource& source,
    const std::string& rig_path,
    SkeletonAssetConfig* asset) {
    const YAML::Node rig = source.load_yaml(rig_path);
    reject_unknown_keys(
        rig,
        {
            "rig_version",
            "skeleton",
            "forward_axis",
            "root_bone",
            "body_bone",
            "knee_hinge_local",
            "legs",
            "colliders",
        },
        rig_path,
        source.source_kind(),
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG,
        asset->skeleton_asset_id);
    const std::uint32_t rig_version = rig["rig_version"]
        ? rig["rig_version"].as<std::uint32_t>()
        : 0u;
    if (rig_version != 2u) {
        throw std::runtime_error("unsupported rig_version: " + rig_path);
    }
    // The file names the rig it describes, so a mis-copied file is caught here
    // rather than silently attaching one rig's colliders to another's bones.
    if (!rig["skeleton"] ||
        rig["skeleton"].as<std::string>() != asset->name) {
        throw std::runtime_error(
            "rig file does not name skeleton " + asset->name + ": " + rig_path);
    }
    if (!rig["forward_axis"] || !rig["root_bone"] || !rig["body_bone"] ||
        !rig["legs"] || !rig["legs"].IsSequence()) {
        throw std::runtime_error(
            "rig requires forward_axis, root_bone, body_bone and legs: " +
            rig_path);
    }
    asset->forward_axis = rig["forward_axis"].as<std::string>();
    if (asset->forward_axis != "positive_z") {
        throw std::runtime_error("unsupported rig forward_axis: " + rig_path);
    }
    asset->root_bone = rig["root_bone"].as<std::string>();
    asset->body_bone = rig["body_bone"].as<std::string>();
    asset->root_bone_index =
        skeleton_bone_index(*asset, asset->root_bone, "root_bone");
    asset->body_bone_index =
        skeleton_bone_index(*asset, asset->body_bone, "body_bone");
    asset->knee_hinge_local = rig["knee_hinge_local"]
        ? vec3_from_yaml(rig["knee_hinge_local"])
        : KernelVec3{0.0f, 0.0f, 1.0f};
    if (!finite_vec3_value(asset->knee_hinge_local) ||
        vec3_length_squared(asset->knee_hinge_local) <= 0.0f) {
        throw std::runtime_error("invalid rig knee_hinge_local: " + rig_path);
    }
    if (rig["legs"].size() == 0u ||
        rig["legs"].size() > KERNEL_MAX_SKELETON_LEGS) {
        throw std::runtime_error("invalid rig leg count: " + rig_path);
    }
    std::unordered_set<std::string> rig_leg_ids;
    for (const YAML::Node& leg_node : rig["legs"]) {
        reject_unknown_keys(
            leg_node,
            {"id", "hip", "knee", "foot", "pole_local"},
            rig_path,
            source.source_kind(),
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG,
            asset->skeleton_asset_id);
        if (!leg_node["id"] || !leg_node["hip"] || !leg_node["knee"] ||
            !leg_node["foot"]) {
            throw std::runtime_error(
                "rig leg requires id and hip/knee/foot bones: " + rig_path);
        }
        RigLegConfig leg;
        leg.id = leg_node["id"].as<std::string>();
        if (!rig_leg_ids.insert(leg.id).second) {
            throw std::runtime_error("duplicate rig leg id: " + leg.id);
        }
        leg.hip_bone = leg_node["hip"].as<std::string>();
        leg.knee_bone = leg_node["knee"].as<std::string>();
        leg.foot_bone = leg_node["foot"].as<std::string>();
        leg.hip_bone_index =
            skeleton_bone_index(*asset, leg.hip_bone, leg.id + ".hip");
        leg.knee_bone_index =
            skeleton_bone_index(*asset, leg.knee_bone, leg.id + ".knee");
        leg.foot_bone_index =
            skeleton_bone_index(*asset, leg.foot_bone, leg.id + ".foot");
        // The two-bone chain has to actually be a chain in this skeleton. This
        // is now checkable where the claim is made, instead of once per
        // template that happens to use the rig.
        if (asset->bones[leg.knee_bone_index].parent_index !=
                static_cast<std::int32_t>(leg.hip_bone_index) ||
            asset->bones[leg.foot_bone_index].parent_index !=
                static_cast<std::int32_t>(leg.knee_bone_index)) {
            throw std::runtime_error(
                "rig " + asset->name + " leg " + leg.id + " bones " +
                leg.hip_bone + " -> " + leg.knee_bone + " -> " +
                leg.foot_bone + " has invalid two-bone hierarchy");
        }
        leg.pole_local = vec3_from_yaml(leg_node["pole_local"]);
        if (!finite_vec3_value(leg.pole_local) ||
            vec3_length_squared(leg.pole_local) <= 0.0f) {
            throw std::runtime_error(
                "invalid rig pole_local for leg " + leg.id + ": " + rig_path);
        }
        asset->legs.push_back(std::move(leg));
    }
    if (!rig["colliders"]) {
        return;
    }
    if (!rig["colliders"].IsSequence()) {
        throw std::runtime_error("rig colliders must be a sequence: " + rig_path);
    }
    if (rig["colliders"].size() > KERNEL_MAX_SKELETON_COLLIDERS) {
        throw std::runtime_error(
            "rig colliders exceeds " +
            std::to_string(KERNEL_MAX_SKELETON_COLLIDERS) + ": " + rig_path);
    }
    std::unordered_set<std::uint32_t> bone_indices;
    for (const YAML::Node& collider_node : rig["colliders"]) {
        reject_unknown_keys(
            collider_node,
            {"bone", "leg", "shape", "hit_zone"},
            rig_path,
            source.source_kind(),
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG,
            asset->skeleton_asset_id);
        if (!collider_node["bone"]) {
            throw std::runtime_error(
                "rig collider requires bone: " + rig_path);
        }
        RigColliderConfig collider;
        collider.bone = collider_node["bone"].as<std::string>();
        collider.bone_index =
            skeleton_bone_index(*asset, collider.bone, "colliders.bone");
        // One collider per bone: two would share a bone frame and so occupy the
        // same space, and the runtime keys them by bone.
        if (!bone_indices.insert(collider.bone_index).second) {
            throw std::runtime_error(
                "duplicate rig collider bone: " + collider.bone);
        }
        collider.shape_type =
            skeleton_collider_shape_from_yaml(collider_node["shape"]);
        // Resolved against the entity template's legs when the two are merged:
        // leg indices are template-scoped until the legs themselves move here.
        if (collider_node["leg"]) {
            collider.leg_id = collider_node["leg"].as<std::string>();
        }
        // What being hit here costs, as a multiplier. Belongs to the rig rather
        // than the template because it is a property of which bone this is -- a
        // shin is a shin whoever is wearing it -- while whose side it is on is
        // not. A collider that says nothing inherits the template's default.
        if (collider_node["hit_zone"]) {
            collider.hit_zone = hit_zone_from_yaml(
                collider_node["hit_zone"], rig_path + " " + collider.bone);
            collider.has_hit_zone = true;
        }
        asset->colliders.push_back(std::move(collider));
    }
}

std::vector<SkeletonAssetConfig> load_skeleton_assets_from_directory(
    const GameplayConfigSource& source,
    const std::string& catalog_base_path,
    const YAML::Node& manifest_dir_node) {
    if (!manifest_dir_node || !manifest_dir_node.IsScalar()) {
        throw std::runtime_error("skeleton_manifests_dir must be a scalar");
    }
    const std::string logical_directory =
        manifest_dir_node.as<std::string>();
    const std::string resolved_directory =
        source.resolve_path(catalog_base_path, manifest_dir_node);
    const std::vector<std::string> manifest_paths =
        source.list_files(resolved_directory, kSkeletonManifestSuffix);
    if (manifest_paths.empty()) {
        throw std::runtime_error(
            "skeleton_manifests_dir contains no *" +
            std::string(kSkeletonManifestSuffix) + ": " + logical_directory);
    }

    // Authored rig files live beside the generated ones and are keyed by the
    // basename all three of a rig's files share, so the same scan finds them.
    std::unordered_map<std::string, std::string> rig_paths;
    for (const std::string& rig_path :
         source.list_files(resolved_directory, kSkeletonRigSuffix)) {
        const std::size_t rig_separator = rig_path.find_last_of("/\\");
        const std::string rig_basename = rig_separator == std::string::npos
            ? rig_path
            : rig_path.substr(rig_separator + 1);
        rig_paths.emplace(
            rig_basename.substr(
                0, rig_basename.size() - kSkeletonRigSuffix.size()),
            rig_path);
    }

    std::vector<SkeletonAssetConfig> assets;
    for (const std::string& manifest_path : manifest_paths) {
        // The logical reference is rebuilt from the authored directory and the
        // file's own name rather than from the resolved path, because the
        // filesystem and archive sources resolve to different shapes and
        // entity templates match this string against source_manifest.
        const std::size_t separator = manifest_path.find_last_of("/\\");
        const std::string basename = separator == std::string::npos
            ? manifest_path
            : manifest_path.substr(separator + 1);
        const std::string logical_manifest =
            logical_directory.empty() ? basename
                                      : logical_directory + "/" + basename;
        const YAML::Node manifest = source.load_yaml(manifest_path);
        reject_unknown_keys(
            manifest,
            {
                "manifest_version",
                "asset_id",
                "name",
                "content_hash",
                "runtime_skeleton",
                "bone_count",
                "bones",
            },
            manifest_path,
            source.source_kind(),
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG);
        // 2 added a per-bone rest pose for clients that build their bone
        // hierarchy from the manifest instead of importing the source mesh.
        // The server ignores it -- it reads the rest pose out of the .ozz --
        // so 1 remains loadable.
        const std::uint32_t manifest_version =
            manifest["manifest_version"]
                ? manifest["manifest_version"].as<std::uint32_t>()
                : 0u;
        if ((manifest_version != 1u && manifest_version != 2u) ||
            !manifest["asset_id"] || !manifest["name"] ||
            !manifest["content_hash"] || !manifest["runtime_skeleton"] ||
            !manifest["bone_count"] || !manifest["bones"] ||
            !manifest["bones"].IsSequence()) {
            throw std::runtime_error(
                "invalid skeleton manifest: " + manifest_path);
        }

        SkeletonAssetConfig asset;
        asset.skeleton_asset_id =
            manifest["asset_id"].as<std::uint32_t>();
        asset.name = manifest["name"].as<std::string>();
        asset.content_hash = uint64_from_yaml(manifest["content_hash"]);
        asset.manifest_reference = logical_manifest;
        const std::string runtime_file =
            manifest["runtime_skeleton"].as<std::string>();
        asset.runtime_reference =
            (std::filesystem::path(logical_manifest).parent_path() /
             runtime_file)
                .lexically_normal()
                .generic_string();
        const std::string runtime_path = source.resolve_path(
            source.parent_path(manifest_path),
            manifest["runtime_skeleton"]);
        asset.runtime_skeleton = source.load_bytes(runtime_path);

        std::uint64_t runtime_hash = 14695981039346656037ull;
        hash_bytes(
            &runtime_hash,
            asset.runtime_skeleton.data(),
            asset.runtime_skeleton.size());
        if (runtime_hash != asset.content_hash) {
            throw std::runtime_error(
                "skeleton content hash mismatch: " + manifest_path);
        }

        const std::uint32_t bone_count =
            manifest["bone_count"].as<std::uint32_t>();
        if (bone_count == 0u || bone_count > 1024u ||
            manifest["bones"].size() != bone_count) {
            throw std::runtime_error(
                "invalid skeleton bone_count: " + manifest_path);
        }
        std::unordered_set<std::string> bone_names;
        asset.bones.reserve(bone_count);
        for (std::uint32_t index = 0u; index < bone_count; ++index) {
            const YAML::Node bone = manifest["bones"][index];
            reject_unknown_keys(
                bone,
                {
                    "index",
                    "name",
                    "parent_index",
                    // manifest_version 2, client-side only.
                    "rest_translation",
                    "rest_rotation",
                    "rest_scale",
                },
                manifest_path,
                source.source_kind(),
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG,
                asset.skeleton_asset_id);
            const std::int32_t parent_index =
                bone["parent_index"].as<std::int32_t>();
            const std::string name = bone["name"].as<std::string>();
            if (bone["index"].as<std::uint32_t>() != index || name.empty() ||
                !bone_names.insert(name).second || parent_index < -1 ||
                parent_index >= static_cast<std::int32_t>(index)) {
                throw std::runtime_error(
                    "invalid skeleton bone entry: " + manifest_path);
            }
            asset.bones.push_back(
                SkeletonManifestBoneConfig{name, parent_index});
        }
        if (asset.skeleton_asset_id == 0u || asset.content_hash == 0u ||
            std::any_of(
                assets.begin(),
                assets.end(),
                [&asset](const SkeletonAssetConfig& candidate) {
                    return candidate.skeleton_asset_id ==
                               asset.skeleton_asset_id ||
                        candidate.name == asset.name ||
                        candidate.manifest_reference ==
                            asset.manifest_reference;
                })) {
            throw std::runtime_error(
                "duplicate or invalid skeleton asset: " + manifest_path);
        }
        // The authored companion to this generated manifest, matched by the
        // basename they share. Optional: a rig that declares no colliders
        // simply has no file.
        const auto rig_entry = rig_paths.find(asset.name);
        if (rig_entry != rig_paths.end()) {
            load_skeleton_rig(source, rig_entry->second, &asset);
        }
        assets.push_back(std::move(asset));
    }
    return assets;
}

std::uint32_t skeleton_bone_index(
    const SkeletonAssetConfig& asset,
    const std::string& name,
    const std::string& field) {
    const auto found = std::find_if(
        asset.bones.begin(),
        asset.bones.end(),
        [&name](const SkeletonManifestBoneConfig& bone) {
            return bone.name == name;
        });
    if (found == asset.bones.end()) {
        throw std::runtime_error(
            "skeleton asset " + asset.name + " missing " + field +
            " bone: " + name);
    }
    return static_cast<std::uint32_t>(
        std::distance(asset.bones.begin(), found));
}

bool is_skeleton_locomotion_diagnostic(std::string_view diagnostic) {
    return diagnostic.find("skeleton") != std::string_view::npos ||
        diagnostic.find("bone") != std::string_view::npos ||
        diagnostic.find("locomotion") != std::string_view::npos ||
        diagnostic.find("gait") != std::string_view::npos ||
        diagnostic.find("foothold") != std::string_view::npos ||
        diagnostic.find("processing_order") != std::string_view::npos;
}

std::uint32_t template_id_for_diagnostic(const YAML::Node& node) {
    if (!node["id"] || !node["id"].IsScalar()) {
        return 0u;
    }
    try {
        return node["id"].as<std::uint32_t>();
    } catch (...) {
        return 0u;
    }
}

[[noreturn]] void throw_skeleton_locomotion_data_error(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind,
    const std::exception& error) {
    const std::string diagnostic = error.what();
    const std::uint32_t template_id = template_id_for_diagnostic(node);
    const bool skeleton_field =
        diagnostic.find("skeleton") != std::string::npos ||
        diagnostic.find("bone") != std::string::npos;
    throw DataLoadError(
        KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_YAML,
        "entity template " + std::to_string(template_id) + ": " +
            diagnostic,
        path,
        skeleton_field ? "skeleton" : "locomotion",
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
        template_id);
}

ActorTemplateConfig actor_template_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind,
    const WeaponCatalogConfig& weapons,
    const ColliderCatalogConfig& colliders,
    const std::vector<SkeletonAssetConfig>& skeleton_assets) {
    reject_unknown_keys(
        node,
        {
            "id",
            "name",
            "entity_type",
            "actor_type",
            "camp",
            "collider_template",
            "health",
            "impulse_resistance",
            "movement",
            "hitbox",
            "weapon_slots",
            "active_weapon_slot",
            "inventory_slot_capacity",
            "inventory_slots",
            "animations",
            "ai",
            "vision",
            "skeleton",
            "locomotion",
        },
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR);
    ActorTemplateConfig actor_template;
    actor_template.actor_template_id = node["id"].as<std::uint32_t>();
    actor_template.name = node["name"].as<std::string>();
    actor_template.entity_type = entity_type_from_yaml(node["entity_type"]);
    actor_template.actor_type =
        node["actor_type"] ? actor_type_from_yaml(node["actor_type"])
                           : actor_type_from_yaml(node["entity_type"]);
    actor_template.vision.struct_size = sizeof(KernelAgentVisionConfig);
    if (actor_template.actor_type == kActorTypeAgent) {
        actor_template.vision.camp = KernelAgentCamp_EnemySide;
        actor_template.vision.vision_collider_template_id = 9;
        actor_template.vision.max_visible_hostiles = KERNEL_MAX_VISIBLE_HOSTILES;
        actor_template.vision.max_visible_allies = KERNEL_MAX_VISIBLE_ALLIES;
        actor_template.vision.max_visible_neutrals = KERNEL_MAX_VISIBLE_NEUTRALS;
        actor_template.vision.local_forward = KernelVec3{-1.0f, 0.0f, 0.0f};
        actor_template.ai_controller_type = KernelAiControllerType_Sentry;
        actor_template.ai_tick_interval = 1;
    } else if (actor_template.actor_type == kActorTypePlayer) {
        actor_template.vision.camp = KernelAgentCamp_PlayerSide;
    }
    if (node["camp"]) {
        actor_template.vision.camp = camp_from_yaml(node["camp"]);
    }
    actor_template.collider_template_id =
        actor_collider_template_id_from_yaml(node["collider_template"], colliders);
    if (node["impulse_resistance"]) {
        actor_template.impulse_resistance =
            node["impulse_resistance"].as<float>();
    }

    const YAML::Node health = node["health"];
    if (!health) {
        throw std::runtime_error(
            "actor template requires health: " + actor_template.name);
    }
    reject_unknown_keys(
        health,
        {"hp", "max_hp"},
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
        actor_template.actor_template_id);
    actor_template.health.hp = health["hp"].as<std::uint16_t>();
    actor_template.health.max_hp = health["max_hp"].as<std::uint16_t>();

    const YAML::Node movement = node["movement"];
    if (!movement || !movement["controller"] ||
        !movement["move_speed_meters_per_second"] ||
        !movement["collider_template"]) {
        throw std::runtime_error(
            "actor template requires movement controller, speed, and collider: " +
            actor_template.name);
    }
    reject_unknown_keys(
        movement,
        {
            "controller",
            "move_speed_meters_per_second",
            "collider_template",
            "gravity",
            "max_slope_degrees",
            "step_height",
            "ground_probe_distance",
            "ground_snap_distance",
            "max_yaw_degrees_per_second",
            "collision_mask",
        },
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
        actor_template.actor_template_id);
    actor_template.move_speed_meters_per_second =
        movement["move_speed_meters_per_second"].as<float>();
    actor_template.movement_controller_type =
        movement_controller_type_from_yaml(movement["controller"]);
    actor_template.movement_collider_template_id =
        actor_collider_template_id_from_yaml(
            movement["collider_template"], colliders);
    if (movement["gravity"]) {
        actor_template.movement_gravity = vec3_from_yaml(movement["gravity"]);
    }
    if (movement["max_slope_degrees"]) {
        actor_template.movement_max_slope_degrees =
            movement["max_slope_degrees"].as<float>();
    }
    if (movement["step_height"]) {
        actor_template.movement_step_height = movement["step_height"].as<float>();
    }
    if (movement["ground_probe_distance"]) {
        actor_template.movement_ground_probe_distance =
            movement["ground_probe_distance"].as<float>();
    }
    if (movement["ground_snap_distance"]) {
        actor_template.movement_ground_snap_distance =
            movement["ground_snap_distance"].as<float>();
    }
    if (movement["max_yaw_degrees_per_second"]) {
        actor_template.movement_max_yaw_degrees_per_second =
            movement["max_yaw_degrees_per_second"].as<float>();
    }
    if (movement["collision_mask"]) {
        actor_template.movement_collision_mask =
            movement_collision_mask_from_yaml(movement["collision_mask"]);
    }

    const YAML::Node hitbox = node["hitbox"];
    if (!hitbox || !hitbox["center"] || !hitbox["half_extents"]) {
        throw std::runtime_error(
            "actor template requires hitbox center and half_extents: " +
            actor_template.name);
    }
    reject_unknown_keys(
        hitbox,
        {"center", "half_extents"},
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
        actor_template.actor_template_id);
    actor_template.hitbox_center = vec3_from_yaml(hitbox["center"]);
    actor_template.hitbox_half_extents = vec3_from_yaml(hitbox["half_extents"]);

    const YAML::Node weapon_slots = node["weapon_slots"];
    if (!weapon_slots || !weapon_slots.IsSequence()) {
        throw std::runtime_error(
            "actor template requires weapon_slots: " + actor_template.name);
    }
    if (weapon_slots.size() == 0 || weapon_slots.size() > actor_template.weapon_ids.size()) {
        throw std::runtime_error(
            "actor template weapon_slots count must be 1 to 4: " +
            actor_template.name);
    }
    actor_template.weapon_slot_count =
        static_cast<std::uint8_t>(weapon_slots.size());
    for (std::size_t index = 0; index < weapon_slots.size(); ++index) {
        const int authored_weapon_id = weapon_slots[index].as<int>();
        if (authored_weapon_id < 0 || authored_weapon_id > UINT8_MAX) {
            throw std::runtime_error(
                "actor template weapon id is out of uint8 range: " +
                actor_template.name);
        }
        const auto weapon_id =
            static_cast<std::uint8_t>(authored_weapon_id);
        if (!weapons.configured[weapon_id]) {
            throw std::runtime_error(
                "actor template references unknown weapon id: " +
                actor_template.name);
        }
        actor_template.weapon_ids[index] = weapon_id;
    }
    actor_template.active_weapon_slot =
        node["active_weapon_slot"]
            ? static_cast<std::uint8_t>(node["active_weapon_slot"].as<int>())
            : 0;
    if (actor_template.active_weapon_slot >= actor_template.weapon_slot_count) {
        throw std::runtime_error(
            "actor template active_weapon_slot is out of range: " +
            actor_template.name);
    }

    const bool has_inventory_slot_capacity =
        static_cast<bool>(node["inventory_slot_capacity"]);
    const bool has_inventory_slots = static_cast<bool>(node["inventory_slots"]);
    if (has_inventory_slot_capacity != has_inventory_slots) {
        throw std::runtime_error(
            "actor template inventory_slot_capacity and inventory_slots must "
            "be specified together: " + actor_template.name);
    }
    if (has_inventory_slot_capacity) {
        const std::uint32_t capacity =
            node["inventory_slot_capacity"].as<std::uint32_t>();
        if (capacity == 0 || capacity > UINT16_MAX) {
            throw std::runtime_error(
                "actor template inventory_slot_capacity must be in uint16 "
                "range: " + actor_template.name);
        }
        const YAML::Node inventory_slots = node["inventory_slots"];
        if (!inventory_slots.IsSequence()) {
            throw std::runtime_error(
                "actor template inventory_slots must be a sequence: " +
                actor_template.name);
        }
        if (inventory_slots.size() > capacity) {
            throw std::runtime_error(
                "actor template inventory_slots exceeds capacity: " +
                actor_template.name);
        }
        actor_template.inventory_slot_capacity =
            static_cast<std::uint16_t>(capacity);
        for (const YAML::Node& slot_node : inventory_slots) {
            if (!slot_node || !slot_node.IsMap()) {
                throw std::runtime_error(
                    "actor template inventory slot must be a mapping: " +
                    actor_template.name);
            }
            reject_unknown_keys(
                slot_node,
                {"item_template", "quantity"},
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                actor_template.actor_template_id);
            if (!slot_node["item_template"] || !slot_node["quantity"]) {
                throw std::runtime_error(
                    "actor template inventory slot requires item_template and "
                    "quantity: " + actor_template.name);
            }
            InventorySlotConfig slot;
            slot.item_template_ref =
                slot_node["item_template"].as<std::string>();
            slot.quantity = slot_node["quantity"].as<std::uint32_t>();
            actor_template.inventory_slots.push_back(std::move(slot));
        }
    }

    const YAML::Node animations = node["animations"];
    if (animations) {
        reject_unknown_keys(
            animations,
            {"idle", "chasing"},
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
            actor_template.actor_template_id);
        if (animations["idle"]) {
            actor_template.animation_idle =
                animations["idle"].as<std::uint16_t>();
        }
        if (animations["chasing"]) {
            actor_template.animation_chasing =
                animations["chasing"].as<std::uint16_t>();
        }
    }
    if (node["ai"]) {
        reject_unknown_keys(
            node["ai"],
            {
                "controller",
                "profile",
                "tick_interval",
                "sentry",
                "chaser",
            },
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
            actor_template.actor_template_id);
        if (node["ai"]["controller"]) {
            const std::string controller =
                node["ai"]["controller"].as<std::string>();
            if (controller == "sentry") {
                actor_template.ai_controller_type = KernelAiControllerType_Sentry;
            } else if (controller == "chaser") {
                actor_template.ai_controller_type = KernelAiControllerType_Chaser;
            } else if (controller == "director") {
                actor_template.ai_controller_type = KernelAiControllerType_Director;
            } else {
                throw std::runtime_error("unsupported ai.controller: " + controller);
            }
        }
        if (node["ai"]["tick_interval"]) {
            actor_template.ai_tick_interval =
                node["ai"]["tick_interval"].as<std::uint32_t>();
        }
    }
    actor_template.sentry =
        sentry_config_from_yaml(node["ai"], actor_template, weapons, path, source_kind);
    actor_template.chaser =
        chaser_config_from_yaml(node["ai"], actor_template, path, source_kind);
    actor_template.vision = vision_config_from_yaml(
        node["vision"],
        actor_template,
        colliders,
        path,
        source_kind);
    if (node["locomotion"] && !node["skeleton"]) {
        throw std::runtime_error(
            "actor template locomotion requires skeleton: " +
            actor_template.name);
    }
    if (node["skeleton"]) {
        const YAML::Node skeleton = node["skeleton"];
        reject_unknown_keys(
            skeleton,
            {
                "runtime_asset",
                "source_manifest",
                "content_hash",
                "collision_flags",
            },
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
            actor_template.actor_template_id);
        if (!skeleton["runtime_asset"] || !skeleton["source_manifest"] ||
            !node["locomotion"]) {
            throw std::runtime_error(
                "skeleton requires runtime_asset, source_manifest and "
                "locomotion: " + actor_template.name);
        }
        SkeletonBindingConfig& binding = actor_template.skeleton;
        binding.runtime_asset =
            skeleton["runtime_asset"].as<std::string>();
        binding.source_manifest =
            skeleton["source_manifest"].as<std::string>();
        const auto asset = std::find_if(
            skeleton_assets.begin(),
            skeleton_assets.end(),
            [&binding](const SkeletonAssetConfig& candidate) {
                return candidate.manifest_reference ==
                           binding.source_manifest &&
                    candidate.runtime_reference == binding.runtime_asset;
            });
        if (asset == skeleton_assets.end()) {
            throw std::runtime_error(
                "unknown skeleton asset/manifest pair: " +
                binding.runtime_asset + " / " + binding.source_manifest);
        }
        if (skeleton["content_hash"] &&
            uint64_from_yaml(skeleton["content_hash"]) !=
                asset->content_hash) {
            throw std::runtime_error(
                "entity template skeleton content_hash mismatch: " +
                actor_template.name);
        }
        binding.enabled = true;
        binding.skeleton_asset_id = asset->skeleton_asset_id;
        binding.content_hash = asset->content_hash;
        binding.bone_count =
            static_cast<std::uint32_t>(asset->bones.size());
        // Bone identity is a fact about the rig, so it is read off the asset
        // rather than re-authored per template. A rig with no .rig.yaml has no
        // legs to walk and is rejected below.
        binding.root_bone = asset->root_bone;
        binding.body_bone = asset->body_bone;
        binding.root_bone_index = asset->root_bone_index;
        binding.body_bone_index = asset->body_bone_index;

        const YAML::Node locomotion = node["locomotion"];
        reject_unknown_keys(
            locomotion,
            {
                "type",
                "input_deadzone",
                "gait",
                "foothold",
                "body",
                "leg_defaults",
                "legs",
            },
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
            actor_template.actor_template_id);
        if (!locomotion["type"] || !locomotion["gait"] ||
            !locomotion["foothold"] || !locomotion["legs"] ||
            !locomotion["legs"].IsSequence()) {
            throw std::runtime_error(
                "locomotion requires type, gait, and legs: " +
                actor_template.name);
        }
        if (asset->legs.empty()) {
            throw std::runtime_error(
                "skeleton asset " + asset->name +
                " declares no legs; it needs a .rig.yaml to be walked: " +
                actor_template.name);
        }
        binding.locomotion_type = locomotion["type"].as<std::string>();
        // Which way the model faces is a fact about the rig.
        binding.forward_axis = asset->forward_axis;
        binding.input_deadzone = locomotion["input_deadzone"]
            ? locomotion["input_deadzone"].as<float>()
            : 0.01f;
        if (binding.locomotion_type != "procedural_legged" ||
            binding.forward_axis != "positive_z" ||
            !std::isfinite(binding.input_deadzone) ||
            binding.input_deadzone < 0.0f || binding.input_deadzone >= 1.0f ||
            !std::isfinite(
                actor_template.movement_max_yaw_degrees_per_second) ||
            actor_template.movement_max_yaw_degrees_per_second <= 0.0f ||
            locomotion["legs"].size() == 0u ||
            locomotion["legs"].size() > KERNEL_MAX_SKELETON_LEGS) {
            throw std::runtime_error(
                "unsupported or invalid skeleton locomotion: " +
                actor_template.name);
        }
        // Per-leg tuning laid over the rig's legs. Defaults first, because all
        // three base rigs use one step height and reach ratio for every leg.
        float default_step_height = 0.0f;
        float default_max_reach_ratio = 0.95f;
        if (locomotion["leg_defaults"]) {
            const YAML::Node leg_defaults = locomotion["leg_defaults"];
            reject_unknown_keys(
                leg_defaults,
                {"step_height_meters", "max_reach_ratio"},
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                actor_template.actor_template_id);
            default_step_height = leg_defaults["step_height_meters"]
                ? leg_defaults["step_height_meters"].as<float>()
                : default_step_height;
            default_max_reach_ratio = leg_defaults["max_reach_ratio"]
                ? leg_defaults["max_reach_ratio"].as<float>()
                : default_max_reach_ratio;
        }
        // The rig fixes the leg order, so leg indices are the same for every
        // template that walks it. Entries here name a rig leg and may appear in
        // any order; every leg must be tuned exactly once.
        binding.legs.assign(asset->legs.size(), SkeletonLegConfig{});
        for (std::size_t rig_leg = 0u; rig_leg < asset->legs.size();
             ++rig_leg) {
            binding.legs[rig_leg].id = asset->legs[rig_leg].id;
            binding.legs[rig_leg].step_height_meters = default_step_height;
            binding.legs[rig_leg].max_reach_ratio = default_max_reach_ratio;
        }
        std::vector<bool> leg_tuned(asset->legs.size(), false);
        for (const YAML::Node& leg_node : locomotion["legs"]) {
            reject_unknown_keys(
                leg_node,
                {
                    "id",
                    "gait_group",
                    "step_height_meters",
                    "max_reach_ratio",
                },
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                actor_template.actor_template_id);
            if (!leg_node["id"]) {
                throw std::runtime_error(
                    "locomotion leg requires id: " + actor_template.name);
            }
            const std::string leg_id = leg_node["id"].as<std::string>();
            const auto rig_leg = std::find_if(
                asset->legs.begin(),
                asset->legs.end(),
                [&leg_id](const RigLegConfig& candidate) {
                    return candidate.id == leg_id;
                });
            if (rig_leg == asset->legs.end()) {
                throw std::runtime_error(
                    "locomotion leg " + leg_id + " is not a leg of rig " +
                    asset->name + ": " + actor_template.name);
            }
            const std::size_t leg_index = static_cast<std::size_t>(
                std::distance(asset->legs.begin(), rig_leg));
            if (leg_tuned[leg_index]) {
                throw std::runtime_error(
                    "duplicate locomotion leg id: " + leg_id);
            }
            leg_tuned[leg_index] = true;
            SkeletonLegConfig& leg = binding.legs[leg_index];
            leg.gait_group = leg_node["gait_group"]
                ? leg_node["gait_group"].as<std::uint32_t>()
                : 0u;
            leg.step_height_meters = leg_node["step_height_meters"]
                ? leg_node["step_height_meters"].as<float>()
                : default_step_height;
            leg.max_reach_ratio = leg_node["max_reach_ratio"]
                ? leg_node["max_reach_ratio"].as<float>()
                : default_max_reach_ratio;
            if (!std::isfinite(leg.step_height_meters) ||
                !std::isfinite(leg.max_reach_ratio) ||
                leg.step_height_meters < 0.0f ||
                leg.max_reach_ratio <= 0.0f || leg.max_reach_ratio > 1.0f) {
                throw std::runtime_error(
                    "invalid locomotion leg tuning: " + leg_id);
            }
        }
        if (std::find(leg_tuned.begin(), leg_tuned.end(), false) !=
            leg_tuned.end()) {
            throw std::runtime_error(
                "locomotion does not tune every leg of rig " + asset->name +
                ": " + actor_template.name);
        }

        const YAML::Node gait = locomotion["gait"];
        reject_unknown_keys(
            gait,
            {
                "type",
                "step_threshold_meters",
                "step_duration_ticks",
                "max_swinging_legs",
                "processing_order",
            },
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
            actor_template.actor_template_id);
        if (!gait["type"] ||
            gait["type"].as<std::string>() != "displacement_threshold" ||
            !gait["step_threshold_meters"] ||
            !gait["step_duration_ticks"] ||
            !gait["max_swinging_legs"] || !gait["processing_order"] ||
            !gait["processing_order"].IsSequence()) {
            throw std::runtime_error(
                "locomotion gait requires displacement_threshold parameters: " +
                actor_template.name);
        }
        binding.step_threshold_meters =
            gait["step_threshold_meters"].as<float>();
        binding.step_duration_ticks =
            gait["step_duration_ticks"].as<std::uint32_t>();
        binding.max_swinging_legs =
            gait["max_swinging_legs"].as<std::uint32_t>();
        std::unordered_set<std::uint32_t> processing_order;
        for (const YAML::Node& leg_ref : gait["processing_order"]) {
            const std::string leg_id = leg_ref.as<std::string>();
            const auto leg = std::find_if(
                binding.legs.begin(),
                binding.legs.end(),
                [&leg_id](const SkeletonLegConfig& candidate) {
                    return candidate.id == leg_id;
                });
            if (leg == binding.legs.end()) {
                throw std::runtime_error(
                    "processing_order references undefined leg: " + leg_id);
            }
            const std::uint32_t leg_index = static_cast<std::uint32_t>(
                std::distance(binding.legs.begin(), leg));
            if (!processing_order.insert(leg_index).second) {
                throw std::runtime_error(
                    "processing_order repeats leg: " + leg_id);
            }
            binding.processing_order.push_back(leg_index);
        }
        if (binding.processing_order.size() != binding.legs.size() ||
            !std::isfinite(binding.step_threshold_meters) ||
            binding.step_threshold_meters <= 0.0f ||
            binding.step_duration_ticks == 0u ||
            binding.max_swinging_legs == 0u ||
            binding.max_swinging_legs > binding.legs.size()) {
            throw std::runtime_error(
                "invalid locomotion gait values: " + actor_template.name);
        }
        for (const SkeletonLegConfig& leg : binding.legs) {
            if (leg.gait_group >= binding.legs.size()) {
                throw std::runtime_error(
                    "locomotion gait_group is out of range: " +
                    actor_template.name);
            }
        }

        const YAML::Node foothold = locomotion["foothold"];
        reject_unknown_keys(
            foothold,
            {
                "query",
                "query_start_height_meters",
                "query_distance_meters",
                "candidate_offsets_meters",
            },
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
            actor_template.actor_template_id);
        if (!foothold["query"] ||
            foothold["query"].as<std::string>() != "raycast" ||
            !foothold["query_start_height_meters"] ||
            !foothold["query_distance_meters"] ||
            !foothold["candidate_offsets_meters"] ||
            !foothold["candidate_offsets_meters"].IsSequence() ||
            foothold["candidate_offsets_meters"].size() == 0u ||
            foothold["candidate_offsets_meters"].size() >
                KERNEL_MAX_FOOTHOLD_CANDIDATES) {
            throw std::runtime_error(
                "invalid locomotion foothold query: " + actor_template.name);
        }
        binding.foothold_query_type = KernelFootholdQueryType_Raycast;
        binding.foothold_query_start_height_meters =
            foothold["query_start_height_meters"].as<float>();
        binding.foothold_query_distance_meters =
            foothold["query_distance_meters"].as<float>();
        if (!std::isfinite(binding.foothold_query_start_height_meters) ||
            binding.foothold_query_start_height_meters < 0.0f ||
            !std::isfinite(binding.foothold_query_distance_meters) ||
            binding.foothold_query_distance_meters <= 0.0f) {
            throw std::runtime_error(
                "invalid locomotion foothold distances: " +
                actor_template.name);
        }
        for (const YAML::Node& offset_node :
             foothold["candidate_offsets_meters"]) {
            if (!offset_node["x"] || !offset_node["z"]) {
                throw std::runtime_error(
                    "locomotion foothold candidate requires x/z: " +
                    actor_template.name);
            }
            const KernelVec2 offset{
                offset_node["x"].as<float>(),
                offset_node["z"].as<float>(),
            };
            if (!std::isfinite(offset.x) || !std::isfinite(offset.y)) {
                throw std::runtime_error(
                    "non-finite locomotion foothold candidate: " +
                    actor_template.name);
            }
            binding.foothold_candidate_offsets.push_back(offset);
        }

        // Optional body grounding follow. Absent = 0 (physics owns body height
        // and tilt; the legs read the resolved root only as a world anchor).
        const YAML::Node body = locomotion["body"];
        if (body) {
            reject_unknown_keys(
                body,
                {
                    "follow_speed",
                    "slope_alignment",
                    "stance_crouch_meters",
                },
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                actor_template.actor_template_id);
            binding.body_follow_speed =
                body["follow_speed"] ? body["follow_speed"].as<float>() : 0.0f;
            binding.slope_alignment = body["slope_alignment"]
                ? body["slope_alignment"].as<float>()
                : 0.0f;
            binding.stance_crouch_meters = body["stance_crouch_meters"]
                ? body["stance_crouch_meters"].as<float>()
                : 0.0f;
            if (!std::isfinite(binding.body_follow_speed) ||
                binding.body_follow_speed < 0.0f ||
                !std::isfinite(binding.slope_alignment) ||
                binding.slope_alignment < 0.0f ||
                binding.slope_alignment > 1.0f ||
                !std::isfinite(binding.stance_crouch_meters) ||
                binding.stance_crouch_meters < 0.0f) {
                throw std::runtime_error(
                    "invalid locomotion body follow values: " +
                    actor_template.name);
            }
        }

        // What this actor's rig colliders MEAN. The rig file says which bones
        // carry colliders and what shape they are; this says whose side they
        // are on and what may hit them, which is a property of the actor rather
        // than of the skeleton -- the same rig can be a hostile monster or a
        // friendly one. Absent means the actor takes no part in per-bone
        // collision even if its rig describes some.
        if (skeleton["collision_flags"]) {
            const YAML::Node flags = skeleton["collision_flags"];
            reject_unknown_keys(
                flags,
                {"purpose", "layer", "hit_zone"},
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                actor_template.actor_template_id);
            // layer is required rather than defaulted: this block exists to
            // state whose side these colliders are on, and collider_layer_from_yaml
            // would otherwise quietly answer "damageable" for an author who
            // simply forgot.
            if (!flags["layer"]) {
                throw std::runtime_error(
                    "skeleton collision_flags requires a layer: " +
                    actor_template.name);
            }
            binding.has_collision_flags = true;
            binding.collision_flags.purpose_flags =
                skeleton_collision_purpose_from_yaml(flags["purpose"]);
            binding.collision_flags.layer_mask =
                collider_layer_from_yaml(flags["layer"]);
            // The default for colliders whose rig entry says nothing.
            binding.collision_flags.hit_zone = hit_zone_from_yaml(
                flags["hit_zone"], actor_template.name);
            if (asset->colliders.empty()) {
                throw std::runtime_error(
                    "skeleton collision_flags set but rig " + asset->name +
                    " declares no colliders: " + actor_template.name);
            }
            // The tilt a follower cannot reproduce moves a foot by the tilt
            // times the stance radius, which is metres on these rigs -- so an
            // actor that puts colliders on its bones may not also opt into it.
            // See solve_legged_locomotion_follower_pose.
            if (binding.slope_alignment > 0.0f) {
                throw std::runtime_error(
                    "skeleton colliders require slope_alignment 0, since a "
                    "follower holds an identity tilt: " + actor_template.name);
            }
        }
    }
    return actor_template;
}

std::uint32_t projectile_template_id_from_ref(
    const YAML::Node& node,
    const std::vector<ProjectileTemplateConfig>& projectile_templates) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error("projectile_template reference must be a scalar");
    }
    const std::string value = node.as<std::string>();
    if (!value.empty() &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        })) {
        const std::uint32_t template_id =
            static_cast<std::uint32_t>(std::stoul(value));
        const auto found = std::find_if(
            projectile_templates.begin(),
            projectile_templates.end(),
            [template_id](const ProjectileTemplateConfig& candidate) {
                return candidate.definition.projectile_template_id ==
                    template_id;
            });
        if (found != projectile_templates.end()) return template_id;
        throw std::runtime_error("unknown projectile_template id: " + value);
    }
    const auto found = std::find_if(
        projectile_templates.begin(),
        projectile_templates.end(),
        [&value](const ProjectileTemplateConfig& candidate) {
            return candidate.name == value;
        });
    if (found == projectile_templates.end()) {
        throw std::runtime_error("unknown projectile_template name: " + value);
    }
    return found->definition.projectile_template_id;
}

EntityTemplateConfig entity_template_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind,
    const WeaponCatalogConfig& weapons,
    const ColliderCatalogConfig& colliders,
    const std::vector<ProjectileTemplateConfig>& projectile_templates,
    const std::vector<PropPopulationRuleConfig>& prop_population_rules,
    const std::vector<SkeletonAssetConfig>& skeleton_assets) {
    const std::uint16_t entity_type =
        authored_entity_type_from_yaml(node["entity_type"]);
    if (entity_type == kEntityTypeActor) {
        EntityTemplateConfig entity_template =
            actor_template_from_yaml(
                node,
                path,
                source_kind,
                weapons,
                colliders,
                skeleton_assets);
        entity_template.entity_type = kEntityTypeActor;
        return entity_template;
    }

    if (entity_type == KernelEntityType_Prop) {
        reject_unknown_keys(
            node,
            {
                "id",
                "name",
                "entity_type",
                "server_only",
                "transform",
                "health",
                // A prop is what an impulse actually pushes, so it carries the
                // resistance the same way an actor does.
                "impulse_resistance",
                "physics",
                "interaction",
                "throw",
                "carry_offset",
                "lifecycle",
                "triggers",
            },
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR);
        EntityTemplateConfig entity_template;
        entity_template.actor_template_id = node["id"].as<std::uint32_t>();
        entity_template.name = node["name"].as<std::string>();
        entity_template.entity_type = KernelEntityType_Prop;
        entity_template.server_only =
            node["server_only"] ? node["server_only"].as<bool>() : false;
        if (node["impulse_resistance"]) {
            entity_template.impulse_resistance =
                node["impulse_resistance"].as<float>();
        }
        if (node["transform"]) {
            reject_unknown_keys(
                node["transform"],
                {"position"},
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                entity_template.actor_template_id);
            if (node["transform"]["position"]) {
                entity_template.transform_position =
                    vec3_from_yaml(node["transform"]["position"]);
            }
        }
        if (node["health"]) {
            reject_unknown_keys(
                node["health"],
                {"hp", "max_hp"},
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                entity_template.actor_template_id);
            entity_template.health.hp = node["health"]["hp"].as<std::uint16_t>();
            entity_template.health.max_hp =
                node["health"]["max_hp"].as<std::uint16_t>();
        }
        if (node["physics"]) {
            reject_unknown_keys(
                node["physics"],
                {"collider_template"},
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                entity_template.actor_template_id);
            if (node["physics"]["collider_template"]) {
                entity_template.collider_template_id = collider_template_id_from_ref(
                    node["physics"]["collider_template"], colliders);
            }
        }
        entity_template.prop.struct_size = sizeof(entity_template.prop);
        entity_template.prop.interaction.struct_size =
            sizeof(entity_template.prop.interaction);
        if (node["lifecycle"]) {
            reject_unknown_keys(
                node["lifecycle"],
                {"lifetime_ticks", "population_group"},
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                entity_template.actor_template_id);
            if (!node["lifecycle"]["lifetime_ticks"] &&
                !node["lifecycle"]["population_group"]) {
                throw std::runtime_error(
                    "prop lifecycle requires lifetime_ticks or "
                    "population_group: " + path);
            }
            if (node["lifecycle"]["lifetime_ticks"]) {
                entity_template.prop.lifetime_ticks =
                    node["lifecycle"]["lifetime_ticks"].as<std::uint32_t>();
                if (entity_template.prop.lifetime_ticks == 0u) {
                    throw std::runtime_error(
                        "prop lifecycle lifetime_ticks must be positive: " +
                        path);
                }
            }
            if (node["lifecycle"]["population_group"]) {
                entity_template.prop.population_group_id =
                    prop_population_group_id_from_ref(
                        node["lifecycle"]["population_group"],
                        prop_population_rules);
            }
        }
        if (node["interaction"]) {
            reject_unknown_keys(
                node["interaction"],
                {"capabilities", "range", "line_of_sight_required",
                 "blocking_mask"},
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                entity_template.actor_template_id);
            if (!node["interaction"]["capabilities"].IsSequence()) {
                throw std::runtime_error(
                    "prop interaction capabilities must be a sequence: " + path);
            }
            for (const YAML::Node& capability :
                 node["interaction"]["capabilities"]) {
                entity_template.prop.interaction.capability_flags |=
                    item_capability_from_yaml(capability.as<std::string>());
            }
            entity_template.prop.interaction.interaction_range =
                node["interaction"]["range"].as<float>();
            entity_template.prop.interaction.line_of_sight_required =
                node["interaction"]["line_of_sight_required"] &&
                    node["interaction"]["line_of_sight_required"].as<bool>()
                ? 1u
                : 0u;
            entity_template.prop.interaction.line_of_sight_blocking_mask =
                node["interaction"]["blocking_mask"]
                ? collision_mask_from_yaml(node["interaction"]["blocking_mask"])
                : 0u;
            require_supported_collision_mask(
                entity_template.prop.interaction.line_of_sight_blocking_mask,
                KERNEL_COLLISION_MASK_ACTOR |
                    KERNEL_COLLISION_MASK_STATIC_WORLD,
                "prop interaction blocking_mask");
        }
        if (node["throw"]) {
            reject_unknown_keys(
                node["throw"],
                {"trajectory_projectile"},
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                entity_template.actor_template_id);
            if (!node["throw"]["trajectory_projectile"]) {
                throw std::runtime_error(
                    "prop throw requires trajectory_projectile: " + path);
            }
            entity_template.prop.throw_trajectory_projectile_template_id =
                projectile_template_id_from_ref(
                    node["throw"]["trajectory_projectile"],
                    projectile_templates);
        }
        if (node["carry_offset"]) {
            const KernelVec3 offset = vec3_from_yaml(node["carry_offset"]);
            entity_template.prop.carry_offset_x = offset.x;
            entity_template.prop.carry_offset_y = offset.y;
            entity_template.prop.carry_offset_z = offset.z;
        }
        if (node["triggers"]) {
            reject_unknown_keys(
                node["triggers"],
                {
                    "on_activated",
                    "on_collision",
                    "on_health_depleted",
                    "on_destroy_entity",
                },
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                entity_template.actor_template_id);
            if (node["triggers"]["on_activated"]) {
                entity_template.activated_trigger = trigger_binding_from_yaml(
                    node["triggers"]["on_activated"],
                    path,
                    source_kind,
                    KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                    entity_template.actor_template_id);
            }
            if (node["triggers"]["on_collision"]) {
                const YAML::Node collision =
                    node["triggers"]["on_collision"];
                if (!collision["collision_mask"]) {
                    throw std::runtime_error(
                        "on_collision requires collision_mask: " + path);
                }
                entity_template.collision_trigger_mask =
                    collision_mask_from_yaml(collision["collision_mask"]);
                require_supported_collision_mask(
                    entity_template.collision_trigger_mask,
                    KERNEL_COLLISION_MASK_ACTOR |
                        KERNEL_COLLISION_MASK_STATIC_WORLD |
                        KERNEL_COLLISION_LAYER_LIMB,
                    "on_collision");
                entity_template.collision_trigger = trigger_binding_from_yaml(
                    collision,
                    path,
                    source_kind,
                    KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                    entity_template.actor_template_id,
                    true);
            }
            if (node["triggers"]["on_health_depleted"]) {
                entity_template.health_depleted_trigger =
                    trigger_binding_from_yaml(
                        node["triggers"]["on_health_depleted"],
                        path,
                        source_kind,
                        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                        entity_template.actor_template_id);
            }
            if (node["triggers"]["on_destroy_entity"]) {
                entity_template.destroy_entity_trigger =
                    trigger_binding_from_yaml(
                        node["triggers"]["on_destroy_entity"],
                        path,
                        source_kind,
                        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                        entity_template.actor_template_id);
            }
        }
        return entity_template;
    }

    reject_unknown_keys(
        node,
        {
            "id",
            "name",
            "entity_type",
            "server_only",
            "transform",
            "ai",
            "director",
        },
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR);
    EntityTemplateConfig entity_template;
    entity_template.actor_template_id = node["id"].as<std::uint32_t>();
    entity_template.name = node["name"].as<std::string>();
    entity_template.entity_type = KernelEntityType_Director;
    entity_template.server_only =
        node["server_only"] ? node["server_only"].as<bool>() : true;

    if (node["transform"]) {
        reject_unknown_keys(
            node["transform"],
            {"position"},
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
            entity_template.actor_template_id);
        if (node["transform"]["position"]) {
            entity_template.transform_position =
                vec3_from_yaml(node["transform"]["position"]);
        }
    }
    entity_template.director_spawn_position = entity_template.transform_position;

    const YAML::Node ai = node["ai"];
    if (!ai || !ai["controller"]) {
        throw std::runtime_error("director template requires ai.controller: " +
                                 entity_template.name);
    }
    reject_unknown_keys(
        ai,
        {
            "controller",
            "profile",
            "tick_interval",
        },
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
        entity_template.actor_template_id);
    const std::string controller = ai["controller"].as<std::string>();
    if (controller != "director") {
        throw std::runtime_error("director template requires ai.controller: director");
    }
    entity_template.ai_controller_type = KernelAiControllerType_Director;
    entity_template.ai_tick_interval =
        ai["tick_interval"] ? ai["tick_interval"].as<std::uint32_t>() : 1u;

    const YAML::Node director = node["director"];
    if (!director) {
        throw std::runtime_error("director template requires director block: " +
                                 entity_template.name);
    }
    reject_unknown_keys(
        director,
        {"kind", "spawn", "graph"},
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
        entity_template.actor_template_id);
    const std::string kind =
        director["kind"] ? director["kind"].as<std::string>() : std::string{};
    if (kind != "world_rule" && kind != "game_rule") {
        throw std::runtime_error("unsupported director.kind: " + kind);
    }
    entity_template.director_kind = kind == "world_rule"
        ? KernelDirectorKind_WorldRule
        : KernelDirectorKind_GameRule;
    if (kind == "game_rule") {
        if (director["spawn"] || !director["graph"]) {
            throw std::runtime_error(
                "game_rule director requires graph and must not define spawn: " +
                entity_template.name);
        }
        const YAML::Node graph = director["graph"];
        reject_unknown_keys(
            graph,
            {"nodes"},
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_GAME_RULE,
            entity_template.actor_template_id);
        const YAML::Node nodes = graph["nodes"];
        if (!nodes || !nodes.IsSequence() || nodes.size() == 0u ||
            nodes.size() > KERNEL_MAX_GAME_RULE_NODES) {
            throw std::runtime_error(
                "game_rule graph requires 1..64 nodes: " + entity_template.name);
        }
        std::unordered_map<std::string, std::uint32_t> node_ids;
        std::unordered_set<std::string> groups;
        for (std::size_t index = 0u; index < nodes.size(); ++index) {
            const YAML::Node authored = nodes[index];
            reject_unknown_keys(
                authored,
                {"id", "on_activate", "condition", "next"},
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_GAME_RULE,
                entity_template.actor_template_id);
            if (!authored["id"] || !authored["condition"] ||
                (authored["on_activate"] &&
                 (!authored["on_activate"].IsSequence() ||
                  authored["on_activate"].size() != 1u))) {
                throw std::runtime_error(
                    "game_rule node requires id, condition, and at most one on_activate effect");
            }
            ActorTemplateConfig::GameRuleNodeConfig node_config;
            node_config.node_id = static_cast<std::uint32_t>(index + 1u);
            node_config.id = authored["id"].as<std::string>();
            if (node_config.id.empty() ||
                !node_ids.emplace(node_config.id, node_config.node_id).second) {
                throw std::runtime_error("duplicate or empty game_rule node id");
            }
            const YAML::Node condition = authored["condition"];
            reject_unknown_keys(
                condition,
                {"type", "group", "count"},
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_GAME_RULE,
                entity_template.actor_template_id);
            if (!condition["type"]) {
                throw std::runtime_error(
                    "game_rule condition requires type");
            }
            const std::string condition_type =
                condition["type"].as<std::string>();
            if (condition_type == "group_eliminated") {
                if (!condition["group"] || condition["count"]) {
                    throw std::runtime_error(
                        "group_eliminated requires group and must not define count");
                }
                node_config.condition_type =
                    KernelGameRuleConditionType_GroupEliminated;
                node_config.group = condition["group"].as<std::string>();
                node_config.group_id = node_config.node_id;
                if (node_config.group.empty() ||
                    !groups.insert(node_config.group).second) {
                    throw std::runtime_error(
                        "duplicate or empty game_rule group");
                }
            } else if (condition_type == "player_count_at_least") {
                if (!condition["count"] || condition["group"]) {
                    throw std::runtime_error(
                        "player_count_at_least requires count and must not define group");
                }
                node_config.condition_type =
                    KernelGameRuleConditionType_PlayerCountAtLeast;
                node_config.condition_count =
                    condition["count"].as<std::uint32_t>();
                if (node_config.condition_count == 0u) {
                    throw std::runtime_error(
                        "player_count_at_least count must be greater than zero");
                }
            } else {
                throw std::runtime_error(
                    "unsupported game_rule condition: " + condition_type);
            }
            if (authored["on_activate"]) {
                if (node_config.condition_type !=
                    KernelGameRuleConditionType_GroupEliminated) {
                    throw std::runtime_error(
                        "player_count_at_least node must not define on_activate");
                }
                const YAML::Node effect = authored["on_activate"][0];
                reject_unknown_keys(
                    effect,
                    {"type", "group", "count", "entity_template", "position", "radius", "seed"},
                    path,
                    source_kind,
                    KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_GAME_RULE,
                    entity_template.actor_template_id);
                if (!effect["type"] ||
                    effect["type"].as<std::string>() != "spawn_group" ||
                    !effect["group"] ||
                    effect["group"].as<std::string>() != node_config.group ||
                    !effect["count"] || !effect["entity_template"]) {
                    throw std::runtime_error(
                        "game_rule node requires one matching spawn_group effect");
                }
                node_config.has_spawn_effect = true;
                node_config.spawn_count = effect["count"].as<std::uint32_t>();
                node_config.spawn_entity_template_ref =
                    effect["entity_template"].as<std::string>();
                node_config.spawn_position = effect["position"]
                    ? vec3_from_yaml(effect["position"])
                    : entity_template.transform_position;
                node_config.spawn_radius =
                    effect["radius"] ? effect["radius"].as<float>() : 0.0f;
                node_config.spawn_seed =
                    effect["seed"] ? effect["seed"].as<std::uint32_t>() : 1u;
                if (node_config.spawn_count == 0u ||
                    node_config.spawn_entity_template_ref.empty() ||
                    !std::isfinite(node_config.spawn_position.x) ||
                    !std::isfinite(node_config.spawn_position.y) ||
                    !std::isfinite(node_config.spawn_position.z) ||
                    !std::isfinite(node_config.spawn_radius) ||
                    node_config.spawn_radius < 0.0f) {
                    throw std::runtime_error(
                        "invalid game_rule spawn_group values");
                }
            } else if (node_config.condition_type ==
                       KernelGameRuleConditionType_GroupEliminated) {
                throw std::runtime_error(
                    "group_eliminated node requires a matching spawn_group effect");
            }
            if (authored["next"]) {
                if (!authored["next"].IsSequence()) {
                    throw std::runtime_error("game_rule node.next must be a sequence");
                }
                for (const YAML::Node& next : authored["next"]) {
                    node_config.next_refs.push_back(next.as<std::string>());
                }
            }
            entity_template.game_rule_nodes.push_back(std::move(node_config));
        }
        std::size_t edge_count = 0u;
        for (ActorTemplateConfig::GameRuleNodeConfig& node_config :
             entity_template.game_rule_nodes) {
            std::unordered_set<std::uint32_t> targets;
            for (const std::string& next_ref : node_config.next_refs) {
                const auto found = node_ids.find(next_ref);
                if (found == node_ids.end() || found->second == node_config.node_id ||
                    !targets.insert(found->second).second) {
                    throw std::runtime_error("invalid game_rule edge: " + next_ref);
                }
                node_config.next_node_ids.push_back(found->second);
                ++edge_count;
            }
        }
        if (edge_count > KERNEL_MAX_GAME_RULE_EDGES) {
            throw std::runtime_error("game_rule graph exceeds 256 edges");
        }
        std::vector<std::uint8_t> visit(entity_template.game_rule_nodes.size(), 0u);
        const std::function<bool(std::uint32_t)> has_cycle =
            [&](std::uint32_t node_id) {
                std::uint8_t& state = visit[node_id - 1u];
                if (state == 1u) {
                    return true;
                }
                if (state == 2u) {
                    return false;
                }
                state = 1u;
                for (const std::uint32_t next_id :
                     entity_template.game_rule_nodes[node_id - 1u].next_node_ids) {
                    if (has_cycle(next_id)) {
                        return true;
                    }
                }
                state = 2u;
                return false;
            };
        for (const ActorTemplateConfig::GameRuleNodeConfig& node_config :
             entity_template.game_rule_nodes) {
            if (has_cycle(node_config.node_id)) {
                throw std::runtime_error("game_rule graph must be acyclic");
            }
        }
        return entity_template;
    }
    if (director["graph"]) {
        throw std::runtime_error("world_rule director must not define graph");
    }
    const YAML::Node spawn = director["spawn"];
    if (!spawn) {
        throw std::runtime_error("director template requires director.spawn: " +
                                 entity_template.name);
    }
    reject_unknown_keys(
        spawn,
        {
            "target_count",
            "entity_template",
            "radius",
            "seed",
            "position",
        },
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
        entity_template.actor_template_id);
    entity_template.director_spawn_target_count =
        spawn["target_count"] ? spawn["target_count"].as<std::uint32_t>() : 0u;
    if (!spawn["entity_template"]) {
        throw std::runtime_error("director.spawn requires entity_template: " +
                                 entity_template.name);
    }
    entity_template.director_spawn_entity_template_ref =
        spawn["entity_template"].as<std::string>();
    entity_template.director_spawn_radius =
        spawn["radius"] ? spawn["radius"].as<float>() : 0.0f;
    entity_template.director_spawn_seed =
        spawn["seed"] ? spawn["seed"].as<std::uint32_t>() : 1u;
    if (spawn["position"]) {
        entity_template.director_spawn_position = vec3_from_yaml(spawn["position"]);
    }
    return entity_template;
}

std::uint32_t actor_template_ref_from_yaml(
    const YAML::Node& node,
    const std::vector<ActorTemplateConfig>& actor_templates) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error("actor_template reference must be a scalar");
    }
    const std::string value = node.as<std::string>();
    if (!value.empty() &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        })) {
        const std::uint32_t actor_template_id =
            static_cast<std::uint32_t>(std::stoul(value));
        for (const ActorTemplateConfig& actor_template : actor_templates) {
            if (actor_template.actor_template_id == actor_template_id) {
                return actor_template_id;
            }
        }
        throw std::runtime_error("unknown actor_template id: " + value);
    }
    for (const ActorTemplateConfig& actor_template : actor_templates) {
        if (actor_template.name == value) {
            return actor_template.actor_template_id;
        }
    }
    throw std::runtime_error("unknown actor_template name: " + value);
}

std::uint32_t entity_template_ref_from_yaml(
    const YAML::Node& node,
    const std::vector<EntityTemplateConfig>& entity_templates) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error("entity_template reference must be a scalar");
    }
    const std::string value = node.as<std::string>();
    if (!value.empty() &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0;
        })) {
        const std::uint32_t entity_template_id =
            static_cast<std::uint32_t>(std::stoul(value));
        for (const EntityTemplateConfig& entity_template : entity_templates) {
            if (entity_template.actor_template_id == entity_template_id) {
                return entity_template_id;
            }
        }
        throw std::runtime_error("unknown entity_template id: " + value);
    }
    for (const EntityTemplateConfig& entity_template : entity_templates) {
        if (entity_template.name == value) {
            return entity_template.actor_template_id;
        }
    }
    throw std::runtime_error("unknown entity_template name: " + value);
}

std::vector<ActorTemplateConfig> actor_templates_from_entity_templates(
    const std::vector<EntityTemplateConfig>& entity_templates) {
    std::vector<ActorTemplateConfig> actor_templates;
    for (const EntityTemplateConfig& entity_template : entity_templates) {
        if (entity_template.entity_type == kEntityTypeActor) {
            actor_templates.push_back(entity_template);
        }
    }
    return actor_templates;
}

std::vector<ActorTemplateConfig> load_actor_templates_from_source(
    const GameplayConfigSource& source,
    const std::string& directory,
    const WeaponCatalogConfig& weapons,
    const ColliderCatalogConfig& colliders,
    const std::vector<SkeletonAssetConfig>& skeleton_assets) {
    std::vector<ActorTemplateConfig> actor_templates;
    std::unordered_map<std::uint32_t, std::string> ids;
    std::unordered_map<std::string, std::uint32_t> names;
    const std::vector<std::string> files = source.list_yaml_files(directory);
    for (const std::string& file : files) {
        const YAML::Node node = source.load_yaml(file);
        ActorTemplateConfig actor_template;
        try {
            actor_template = actor_template_from_yaml(
                node,
                file,
                source.source_kind(),
                weapons,
                colliders,
                skeleton_assets);
        } catch (const DataLoadError&) {
            throw;
        } catch (const std::exception& error) {
            if (!is_skeleton_locomotion_diagnostic(error.what())) {
                throw;
            }
            throw_skeleton_locomotion_data_error(
                node, file, source.source_kind(), error);
        }
        if (ids.contains(actor_template.actor_template_id)) {
            throw std::runtime_error("duplicate actor template id: " + file);
        }
        if (names.contains(actor_template.name)) {
            throw std::runtime_error("duplicate actor template name: " + file);
        }
        ids.emplace(actor_template.actor_template_id, file);
        names.emplace(actor_template.name, actor_template.actor_template_id);
        actor_templates.push_back(actor_template);
    }
    if (actor_templates.empty()) {
        throw std::runtime_error("actor template directory is empty: " + directory);
    }
    std::sort(
        actor_templates.begin(),
        actor_templates.end(),
        [](const ActorTemplateConfig& lhs, const ActorTemplateConfig& rhs) {
            return lhs.actor_template_id < rhs.actor_template_id;
        });
    return actor_templates;
}

std::vector<EntityTemplateConfig> load_entity_templates_from_source(
    const GameplayConfigSource& source,
    const std::string& directory,
    const WeaponCatalogConfig& weapons,
    const ColliderCatalogConfig& colliders,
    const std::vector<ProjectileTemplateConfig>& projectile_templates,
    const std::vector<PropPopulationRuleConfig>& prop_population_rules,
    const std::vector<SkeletonAssetConfig>& skeleton_assets) {
    std::vector<EntityTemplateConfig> entity_templates;
    std::unordered_map<std::uint32_t, std::string> ids;
    std::unordered_map<std::string, std::uint32_t> names;
    const std::vector<std::string> files = source.list_yaml_files(directory);
    for (const std::string& file : files) {
        const YAML::Node node = source.load_yaml(file);
        EntityTemplateConfig entity_template;
        try {
            entity_template = entity_template_from_yaml(
                node,
                file,
                source.source_kind(),
                weapons,
                colliders,
                projectile_templates,
                prop_population_rules,
                skeleton_assets);
        } catch (const DataLoadError&) {
            throw;
        } catch (const std::exception& error) {
            if (!is_skeleton_locomotion_diagnostic(error.what())) {
                throw;
            }
            throw_skeleton_locomotion_data_error(
                node, file, source.source_kind(), error);
        }
        if (ids.contains(entity_template.actor_template_id)) {
            throw std::runtime_error("duplicate entity template id: " + file);
        }
        if (names.contains(entity_template.name)) {
            throw std::runtime_error("duplicate entity template name: " + file);
        }
        ids.emplace(entity_template.actor_template_id, file);
        names.emplace(entity_template.name, entity_template.actor_template_id);
        entity_templates.push_back(entity_template);
    }
    if (entity_templates.empty()) {
        throw std::runtime_error("entity template directory is empty: " + directory);
    }
    std::sort(
        entity_templates.begin(),
        entity_templates.end(),
        [](const EntityTemplateConfig& lhs, const EntityTemplateConfig& rhs) {
            return lhs.actor_template_id < rhs.actor_template_id;
        });
    for (EntityTemplateConfig& entity_template : entity_templates) {
        if (entity_template.entity_type != KernelEntityType_Director) {
            continue;
        }
        if (entity_template.director_kind == KernelDirectorKind_WorldRule) {
            const YAML::Node ref(
                entity_template.director_spawn_entity_template_ref);
            entity_template.director_spawn_entity_template_id =
                entity_template_ref_from_yaml(ref, entity_templates);
            const auto actor_match = std::find_if(
                entity_templates.begin(),
                entity_templates.end(),
                [&](const EntityTemplateConfig& candidate) {
                    return candidate.actor_template_id ==
                               entity_template.director_spawn_entity_template_id &&
                           candidate.entity_type == kEntityTypeActor;
                });
            if (actor_match == entity_templates.end()) {
                throw std::runtime_error(
                    "director.spawn entity_template must reference an actor: " +
                    entity_template.name);
            }
            entity_template.director_spawn_actor_template_id =
                entity_template.director_spawn_entity_template_id;
            continue;
        }
        for (ActorTemplateConfig::GameRuleNodeConfig& node :
             entity_template.game_rule_nodes) {
            if (!node.has_spawn_effect) {
                continue;
            }
            const YAML::Node ref(node.spawn_entity_template_ref);
            node.spawn_entity_template_id =
                entity_template_ref_from_yaml(ref, entity_templates);
            const auto actor_match = std::find_if(
                entity_templates.begin(),
                entity_templates.end(),
                [&](const EntityTemplateConfig& candidate) {
                    return candidate.actor_template_id ==
                               node.spawn_entity_template_id &&
                           candidate.entity_type == KernelEntityType_Actor &&
                           candidate.actor_type == KernelActorType_Agent;
                });
            if (actor_match == entity_templates.end()) {
                throw std::runtime_error(
                    "game_rule spawn_group entity_template must reference an agent actor: " +
                    entity_template.name);
            }
        }
    }
    return entity_templates;
}

std::uint32_t collider_template_id_from_ref(
    const YAML::Node& node,
    const ColliderCatalogConfig& colliders) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error("collider template reference must be a scalar");
    }
    const std::string value = node.as<std::string>();
    if (!value.empty() &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        })) {
        const std::uint32_t template_id =
            static_cast<std::uint32_t>(std::stoul(value));
        for (const ColliderTemplateConfig& collider_template :
             colliders.templates) {
            if (collider_template.definition.template_id == template_id) {
                return template_id;
            }
        }
        throw std::runtime_error("unknown collider template id: " + value);
    }
    for (const ColliderTemplateConfig& collider_template : colliders.templates) {
        if (collider_template.name == value) {
            return collider_template.definition.template_id;
        }
    }
    throw std::runtime_error("unknown collider template name: " + value);
}

const ColliderTemplateConfig* collider_template_from_id(
    const ColliderCatalogConfig& colliders,
    std::uint32_t template_id) {
    for (const ColliderTemplateConfig& collider_template : colliders.templates) {
        if (collider_template.definition.template_id == template_id) {
            return &collider_template;
        }
    }
    return nullptr;
}

float collider_template_radius_for_area(
    const ColliderTemplateConfig& collider_template) {
    const KernelColliderTemplateDefinition& definition =
        collider_template.definition;
    if (definition.shape_type == KernelColliderShapeType_Sphere ||
        definition.shape_type == KernelColliderShapeType_Segment) {
        return definition.shape_params.x;
    }
    return std::max(
        definition.shape_params.x,
        std::max(definition.shape_params.y, definition.shape_params.z));
}

TriggerBindingConfig trigger_binding_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind,
    std::uint32_t template_kind,
    std::uint32_t template_id,
    bool allow_collision_mask) {
    if (allow_collision_mask) {
        reject_unknown_keys(
            node,
            {"action_graph", "parameters", "collision_mask"},
            path,
            source_kind,
            template_kind,
            template_id);
    } else {
        reject_unknown_keys(
            node,
            {"action_graph", "parameters"},
            path,
            source_kind,
            template_kind,
            template_id);
    }
    if (!node["action_graph"] || !node["action_graph"].IsScalar() ||
        !node["parameters"] || !node["parameters"].IsMap()) {
        throw std::runtime_error(
            "trigger requires action_graph and parameters map: " + path);
    }
    TriggerBindingConfig binding;
    binding.action_graph_ref = node["action_graph"].as<std::string>();
    for (const auto& entry : node["parameters"]) {
        if (!entry.first.IsScalar() || !entry.second.IsScalar()) {
            throw std::runtime_error(
                "trigger parameters must be scalar values: " + path);
        }
        binding.parameters.emplace_back(
            entry.first.as<std::string>(),
            entry.second.as<std::string>());
    }
    return binding;
}

std::uint32_t portable_state_field_id(const std::string& name) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char ch : name) {
        hash ^= ch;
        hash *= 16777619u;
    }
    return hash == 0u ? 1u : hash;
}

std::uint32_t item_capability_from_yaml(const std::string& value) {
    if (value == "pickupable") return KernelItemCapability_Pickupable;
    if (value == "deployable") return KernelItemCapability_Deployable;
    if (value == "carryable") return KernelItemCapability_Carryable;
    if (value == "consumable") return KernelItemCapability_Consumable;
    if (value == "throwable") return KernelItemCapability_Throwable;
    if (value == "interactable") return KernelItemCapability_Interactable;
    throw std::runtime_error("unknown item capability: " + value);
}

ItemTemplateConfig item_template_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind,
    const std::vector<EntityTemplateConfig>& entity_templates,
    const std::vector<ProjectileTemplateConfig>& projectile_templates) {
    reject_unknown_keys(
        node,
        {"id", "name", "mode", "max_stack", "capabilities",
         "entity_template", "world_interaction", "throw", "use", "portable_state",
         "triggers"},
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ITEM);
    if (!node["id"] || !node["name"] || !node["mode"] ||
        !node["max_stack"] || !node["capabilities"]) {
        throw std::runtime_error("item template is missing required fields: " + path);
    }
    ItemTemplateConfig item;
    item.name = node["name"].as<std::string>();
    KernelItemTemplateDefinition& definition = item.definition;
    definition.struct_size = sizeof(definition);
    definition.item_template_id = node["id"].as<std::uint32_t>();
    const std::string mode = node["mode"].as<std::string>();
    if (mode == "fungible") {
        definition.item_mode = KernelItemMode_Fungible;
    } else if (mode == "stateful") {
        definition.item_mode = KernelItemMode_Stateful;
    } else {
        throw std::runtime_error("unknown item mode: " + mode);
    }
    definition.max_stack = node["max_stack"].as<std::uint16_t>();
    if (!node["capabilities"].IsSequence()) {
        throw std::runtime_error("item capabilities must be a sequence: " + path);
    }
    for (const YAML::Node& capability : node["capabilities"]) {
        definition.capability_flags |=
            item_capability_from_yaml(capability.as<std::string>());
    }
    if (node["entity_template"]) {
        item.entity_template_ref = node["entity_template"].as<std::string>();
        definition.entity_template_id =
            entity_template_ref_from_yaml(node["entity_template"], entity_templates);
    }
    if (node["world_interaction"]) {
        reject_unknown_keys(
            node["world_interaction"],
            {"range", "line_of_sight_required", "blocking_mask"},
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ITEM,
            definition.item_template_id);
        definition.interaction_range =
            node["world_interaction"]["range"].as<float>();
        definition.line_of_sight_required =
            node["world_interaction"]["line_of_sight_required"] &&
                node["world_interaction"]["line_of_sight_required"].as<bool>()
            ? 1u
            : 0u;
        definition.line_of_sight_blocking_mask =
            node["world_interaction"]["blocking_mask"]
            ? collision_mask_from_yaml(
                  node["world_interaction"]["blocking_mask"])
            : 0u;
        require_supported_collision_mask(
            definition.line_of_sight_blocking_mask,
            KERNEL_COLLISION_MASK_ACTOR |
                KERNEL_COLLISION_MASK_STATIC_WORLD,
            "item interaction blocking_mask");
    }
    definition.throw_policy.struct_size = sizeof(definition.throw_policy);
    if (node["throw"]) {
        reject_unknown_keys(
            node["throw"],
            {"mode", "trajectory_projectile"},
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ITEM,
            definition.item_template_id);
        const std::string throw_mode = node["throw"]["mode"]
            ? node["throw"]["mode"].as<std::string>()
            : "none";
        if (throw_mode == "none") {
            definition.throw_policy.mode = KernelItemThrowMode_None;
        } else if (throw_mode == "identity_preserving") {
            definition.throw_policy.mode =
                KernelItemThrowMode_IdentityPreserving;
        } else if (throw_mode == "consume_and_spawn") {
            definition.throw_policy.mode = KernelItemThrowMode_ConsumeAndSpawn;
        } else {
            throw std::runtime_error("unknown item throw mode: " + throw_mode);
        }
        definition.throw_policy.trajectory_projectile_template_id =
            node["throw"]["trajectory_projectile"]
            ? projectile_template_id_from_ref(
                  node["throw"]["trajectory_projectile"],
                  projectile_templates)
            : 0u;
    }
    definition.use_policy.struct_size = sizeof(definition.use_policy);
    if (node["use"]) {
        reject_unknown_keys(
            node["use"],
            {"quantity_cost", "charge_field", "cooldown_ticks",
             "destroy_when_empty"},
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ITEM,
            definition.item_template_id);
        definition.use_policy.quantity_cost = node["use"]["quantity_cost"]
            ? node["use"]["quantity_cost"].as<std::uint32_t>()
            : 0u;
        definition.use_policy.cooldown_ticks = node["use"]["cooldown_ticks"]
            ? node["use"]["cooldown_ticks"].as<std::uint32_t>()
            : 0u;
        definition.use_policy.destroy_when_empty =
            node["use"]["destroy_when_empty"] &&
                node["use"]["destroy_when_empty"].as<bool>()
            ? 1u
            : 0u;
        if (node["use"]["charge_field"]) {
            item.charge_field_ref =
                node["use"]["charge_field"].as<std::string>();
            definition.use_policy.charge_field_id =
                portable_state_field_id(item.charge_field_ref);
        }
    }
    if (node["portable_state"]) {
        if (!node["portable_state"].IsSequence() ||
            node["portable_state"].size() > KERNEL_MAX_PORTABLE_STATE_FIELDS) {
            throw std::runtime_error("invalid item portable_state: " + path);
        }
        for (const YAML::Node& authored_field : node["portable_state"]) {
            reject_unknown_keys(
                authored_field,
                {"id", "type", "default", "world_projection"},
                path,
                source_kind,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ITEM,
                definition.item_template_id);
            KernelPortableStateFieldDefinition& field =
                definition.portable_state_fields[
                    definition.portable_state_field_count++];
            const std::string field_name = authored_field["id"].as<std::string>();
            field.field_id = portable_state_field_id(field_name);
            const std::string type = authored_field["type"].as<std::string>();
            if (type == "uint32") {
                field.type = KernelPortableStateType_Uint32;
                field.uint32_default =
                    authored_field["default"].as<std::uint32_t>();
            } else if (type == "float") {
                field.type = KernelPortableStateType_Float;
                field.float_default = authored_field["default"].as<float>();
            } else if (type == "bool") {
                field.type = KernelPortableStateType_Bool;
                field.bool_default = authored_field["default"].as<bool>() ? 1u : 0u;
            } else {
                throw std::runtime_error("unknown portable state type: " + type);
            }
            const std::string projection = authored_field["world_projection"]
                ? authored_field["world_projection"].as<std::string>()
                : "none";
            if (projection == "none") {
                field.world_projection = KernelPortableStateProjection_None;
            } else if (projection == "health_current") {
                field.world_projection =
                    KernelPortableStateProjection_HealthCurrent;
            } else {
                throw std::runtime_error(
                    "unknown portable state projection: " + projection);
            }
        }
    }
    definition.item_used_trigger.struct_size =
        sizeof(definition.item_used_trigger);
    if (node["triggers"] && node["triggers"]["on_item_used"]) {
        item.item_used_trigger = trigger_binding_from_yaml(
            node["triggers"]["on_item_used"],
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ITEM,
            definition.item_template_id);
    }
    return item;
}

std::vector<ItemTemplateConfig> load_item_templates_from_source(
    const GameplayConfigSource& source,
    const std::string& directory,
    const std::vector<EntityTemplateConfig>& entity_templates,
    const std::vector<ProjectileTemplateConfig>& projectile_templates) {
    std::vector<ItemTemplateConfig> items;
    std::unordered_set<std::uint32_t> ids;
    std::unordered_set<std::string> names;
    for (const std::string& file : source.list_yaml_files(directory)) {
        ItemTemplateConfig item = item_template_from_yaml(
            source.load_yaml(file),
            file,
            source.source_kind(),
            entity_templates,
            projectile_templates);
        if (!ids.insert(item.definition.item_template_id).second ||
            !names.insert(item.name).second) {
            throw std::runtime_error("duplicate item template: " + file);
        }
        items.push_back(std::move(item));
    }
    std::sort(
        items.begin(),
        items.end(),
        [](const ItemTemplateConfig& lhs, const ItemTemplateConfig& rhs) {
            return lhs.definition.item_template_id < rhs.definition.item_template_id;
        });
    return items;
}

void resolve_inventory_item_template_references(
    const std::vector<ItemTemplateConfig>& item_templates,
    std::vector<EntityTemplateConfig>* entity_templates) {
    for (EntityTemplateConfig& entity_template : *entity_templates) {
        for (InventorySlotConfig& slot : entity_template.inventory_slots) {
            const auto item = std::find_if(
                item_templates.begin(),
                item_templates.end(),
                [&slot](const ItemTemplateConfig& candidate) {
                    return candidate.name == slot.item_template_ref ||
                        std::to_string(candidate.definition.item_template_id) ==
                            slot.item_template_ref;
                });
            if (item != item_templates.end()) {
                slot.item_template_id = item->definition.item_template_id;
            }
        }
    }
}

ProjectileTemplateConfig projectile_template_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind,
    const ColliderCatalogConfig& colliders) {
    reject_unknown_keys(
        node,
        {
            "id",
            "name",
            "type",
            "projectile_type",
            "kind",
            "damage",
            "sync_mode",
            "collider_template",
            "movement_model",
            "hit_response",
            "damage_shape",
            "collision_query",
            "collision_query_mode",
            "speed",
            "lifetime_ticks",
            "damage_behavior",
            "collision_mask",
            "hit_instigator",
            "max_hit_count",
            "gravity",
            "triggers",
            "homing",
            "beam",
        },
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_PROJECTILE);
    ProjectileTemplateConfig projectile_template;
    projectile_template.name = node["name"].as<std::string>();
    KernelProjectileTemplateDefinition& definition =
        projectile_template.definition;
    definition.struct_size = sizeof(KernelProjectileTemplateDefinition);
    definition.projectile_template_id = node["id"].as<std::uint32_t>();
    KernelProjectileMechanicsDefinition& mechanics = definition.mechanics;
    mechanics.struct_size = sizeof(KernelProjectileMechanicsDefinition);
    const std::string removed_radius_key = std::string("explosion_") + "radius";
    if (node[removed_radius_key]) {
        throw std::runtime_error(
            "projectile template must use triggers.on_projectile_impact "
            "instead of removed radius field: " +
            projectile_template.name);
    }
    if (node["collision_query"] && node["collision_query_mode"]) {
        throw std::runtime_error(
            "projectile template must not set both collision_query and collision_query_mode: " +
            projectile_template.name);
    }
    mechanics.projectile_type = projectile_type_from_yaml(
        node["projectile_type"] ? node["projectile_type"]
                                : (node["type"] ? node["type"] : node["kind"]));
    mechanics.collider_template_id =
        collider_template_id_from_ref(node["collider_template"], colliders);
    const std::uint32_t static_collision_mask =
        KERNEL_COLLISION_MASK_ACTOR | KERNEL_COLLISION_MASK_STATIC_WORLD;
    mechanics.collision_mask = collision_mask_from_yaml(
        node["collision_mask"],
        mechanics.projectile_type == KernelProjectileType_AreaEffect
            ? KERNEL_COLLISION_MASK_ACTOR
            : static_collision_mask);
        const std::uint32_t supported_collision_mask =
            KERNEL_COLLISION_LAYER_LIMB |
            (mechanics.projectile_type == KernelProjectileType_AreaEffect
            ? KERNEL_COLLISION_MASK_ACTOR | KERNEL_COLLISION_MASK_PROP
            : mechanics.projectile_type == KernelProjectileType_Beam
                ? static_collision_mask
                : static_collision_mask | KERNEL_COLLISION_LAYER_PROJECTILE);
    require_supported_collision_mask(
        mechanics.collision_mask,
        supported_collision_mask,
        "projectile " + projectile_template.name);
    mechanics.collision_query_mode = collision_query_mode_from_yaml(
        node["collision_query_mode"] ? node["collision_query_mode"]
                                     : node["collision_query"]);
    // Parsed before the area_effect branch below, which returns early. An
    // area effect authors triggers like any other projectile -- rocket_explosion
    // carries the damage-and-impulse graph -- and leaving this after that return
    // dropped them silently, since "triggers" is a known key and so survives
    // reject_unknown_keys either way.
    const YAML::Node triggers = node["triggers"];
    if (triggers) {
        reject_unknown_keys(
            triggers,
            {"on_projectile_impact", "on_expired"},
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_PROJECTILE,
            definition.projectile_template_id);
        if (triggers["on_projectile_impact"]) {
            projectile_template.projectile_impact_trigger =
                trigger_binding_from_yaml(
                    triggers["on_projectile_impact"],
                    path,
                    source_kind,
                    KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_PROJECTILE,
                    definition.projectile_template_id);
        }
        if (triggers["on_expired"]) {
            projectile_template.expired_trigger =
                trigger_binding_from_yaml(
                    triggers["on_expired"],
                    path,
                    source_kind,
                    KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_PROJECTILE,
                    definition.projectile_template_id);
        }
    }

    if (mechanics.projectile_type == KernelProjectileType_AreaEffect) {
        const YAML::Node damage_behavior = node["damage_behavior"];
        if (!damage_behavior) {
            throw std::runtime_error(
                "area_effect projectile requires damage_behavior: " +
                projectile_template.name);
        }
        reject_unknown_keys(
            damage_behavior,
            {
                "type",
                "damage_interval_ticks",
                "falloff",
            },
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_PROJECTILE,
            definition.projectile_template_id);
        const std::string damage_type =
            damage_behavior["type"] ? damage_behavior["type"].as<std::string>()
                                    : "";
        if (damage_type != "area_interval") {
            throw std::runtime_error(
                "area_effect projectile requires damage_behavior.type area_interval: " +
                projectile_template.name);
        }
        // Three of the four fields below are the area effect's own to decide,
        // and authoring them silently did nothing: an area effect spawns with
        // zero velocity, so a motion model has nothing to integrate; it always
        // ends on its lifetime; and its damage comes from damage_behavior, not
        // from a shape. Rejecting them says so where it can be read, the same
        // way a key nothing implements is rejected.
        for (const char* overridden :
             {"movement_model", "hit_response", "damage_shape"}) {
            if (node[overridden]) {
                throw std::runtime_error(
                    std::string("area_effect projectile must not author ") +
                    overridden + ": " + projectile_template.name);
            }
        }
        mechanics.motion_model = KernelProjectileMotionModel_Linear;
        // sync_mode is the one that is a real choice. It defaults to
        // server-only, as it always has, but a locally predicted area effect is
        // a shape the kernel implements -- an impact impulse on the local
        // player is predicted only for one it owns -- and forcing the field
        // here made that unreachable from authoring while still accepting the
        // key.
        mechanics.sync_mode = node["sync_mode"]
            ? projectile_sync_mode_from_yaml(node["sync_mode"])
            : KernelProjectileSyncMode_ServerSnapshotOnly;
        mechanics.hit_response = KernelProjectileHitResponse_Destroy;
        mechanics.damage_shape = KernelProjectileDamageShape_DirectHit;
        mechanics.damage = node["damage"].as<std::uint16_t>();
        mechanics.damage_falloff =
            damage_falloff_from_yaml(damage_behavior["falloff"]);
        mechanics.max_hit_count = 1;
        mechanics.area_effect.struct_size =
            sizeof(KernelAreaEffectMechanicsDefinition);
        const ColliderTemplateConfig* area_collider =
            collider_template_from_id(colliders, mechanics.collider_template_id);
        mechanics.area_effect.radius =
            area_collider == nullptr ? 0.0f : collider_template_radius_for_area(*area_collider);
        mechanics.area_effect.damage_per_interval = mechanics.damage;
        mechanics.area_effect.damage_interval_ticks =
            damage_behavior["damage_interval_ticks"].as<std::uint32_t>();
        mechanics.area_effect.lifetime_ticks =
            node["lifetime_ticks"].as<std::uint32_t>();
        mechanics.area_effect.collision_mask = mechanics.collision_mask;
        // Off unless authored: an area effect that reaches its own shooter also
        // damages them, so it has to be asked for rather than inherited.
        mechanics.area_effect.hit_instigator = static_cast<std::uint8_t>(
            node["hit_instigator"] && node["hit_instigator"].as<bool>() ? 1 : 0);
        return projectile_template;
    }

    if (node["hit_instigator"]) {
        // Only the area effect query is authored here. A standard projectile or
        // a beam filters its shooter out somewhere else entirely, so accepting
        // the key on one would be a promise nothing keeps.
        throw std::runtime_error(
            "hit_instigator is only supported on area_effect projectiles: " +
            projectile_template.name);
    }
    mechanics.motion_model = motion_model_from_yaml(node["movement_model"]);
    mechanics.sync_mode = projectile_sync_mode_from_yaml(node["sync_mode"]);
    mechanics.hit_response = hit_response_from_yaml(node["hit_response"]);
    mechanics.damage_shape = damage_shape_from_yaml(node["damage_shape"]);
    mechanics.damage = node["damage"].as<std::uint16_t>();
    mechanics.speed = node["speed"].as<float>();
    mechanics.lifetime_ticks = node["lifetime_ticks"].as<std::uint32_t>();
    mechanics.gravity = vec3_from_yaml(node["gravity"]);
    mechanics.max_hit_count =
        node["max_hit_count"] ? node["max_hit_count"].as<std::uint32_t>() : 1u;

    if (mechanics.projectile_type == KernelProjectileType_Beam) {
        const YAML::Node beam = node["beam"];
        if (!beam) {
            throw std::runtime_error(
                "beam projectile template requires beam block: " +
                projectile_template.name);
        }
        reject_unknown_keys(
            beam,
            {
                "length",
                "radius",
                "lifetime_ticks",
            },
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_PROJECTILE,
            definition.projectile_template_id);
        mechanics.beam.struct_size = sizeof(KernelBeamMechanicsDefinition);
        mechanics.beam.length = beam["length"].as<float>();
        mechanics.beam.radius = beam["radius"].as<float>();
        mechanics.beam.lifetime_ticks =
            beam["lifetime_ticks"] ? beam["lifetime_ticks"].as<std::uint32_t>() : 2u;
        // Derived, not authored. A beam's damage is the template's `damage`
        // read as "per tick", and its targets are the template's
        // `collision_mask` -- beam_system only ever reads these two copies, so
        // deriving them is what stops the block and the template disagreeing.
        mechanics.beam.damage_per_tick = mechanics.damage;
        mechanics.beam.collision_mask = mechanics.collision_mask;
    } else if (node["beam"]) {
        throw std::runtime_error("beam block requires projectile type: beam");
    }

    if (mechanics.motion_model == KernelProjectileMotionModel_Homing) {
        const YAML::Node homing = node["homing"];
        if (!homing) {
            throw std::runtime_error(
                "homing projectile template requires homing block: " +
                projectile_template.name);
        }
        reject_unknown_keys(
            homing,
            {
                "homing_mode",
                "sync_mode",
                "boost_ticks",
                "lock_on_range",
                "lose_target_range",
                "lock_cone_degrees",
                "max_turn_degrees_per_tick",
                "acceleration",
                "max_speed",
            },
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_PROJECTILE,
            definition.projectile_template_id);
        mechanics.homing.struct_size =
            sizeof(KernelHomingMechanicsDefinition);
        mechanics.homing.homing_mode =
            homing_mode_from_yaml(homing["homing_mode"]);
        mechanics.homing.sync_mode =
            projectile_sync_mode_from_yaml(
                homing["sync_mode"] ? homing["sync_mode"] : node["sync_mode"]);
        if (mechanics.homing.sync_mode != mechanics.sync_mode) {
            throw std::runtime_error(
                "projectile sync_mode must match homing sync_mode: " +
                projectile_template.name);
        }
        mechanics.homing.boost_ticks =
            homing["boost_ticks"].as<std::uint32_t>();
        mechanics.homing.lock_on_range =
            homing["lock_on_range"].as<float>();
        mechanics.homing.lose_target_range =
            homing["lose_target_range"].as<float>();
        mechanics.homing.lock_cone_degrees =
            homing["lock_cone_degrees"].as<float>();
        mechanics.homing.max_turn_degrees_per_tick =
            homing["max_turn_degrees_per_tick"].as<float>();
        mechanics.homing.acceleration =
            homing["acceleration"].as<float>();
        mechanics.homing.max_speed = homing["max_speed"].as<float>();
    } else if (node["homing"]) {
        throw std::runtime_error(
            "homing block requires movement_model: homing");
    }
    return projectile_template;
}

ProjectileTemplateConfig* projectile_template_from_ref(
    const YAML::Node& node,
    std::vector<ProjectileTemplateConfig>* projectile_templates);

std::string trigger_parameter_value(
    const ProjectileTriggerBindingConfig& binding,
    const ActionGraphParameterConfig& parameter) {
    const auto found = std::find_if(
        binding.parameters.begin(),
        binding.parameters.end(),
        [&](const auto& value) { return value.first == parameter.name; });
    if (found != binding.parameters.end()) {
        return found->second;
    }
    if (parameter.has_default) {
        return parameter.default_value;
    }
    throw std::runtime_error(
        "required action graph parameter is missing: " + parameter.name);
}

bool event_expression_available(
    std::string_view trigger_name,
    std::string_view expression) {
    if (!expression.starts_with("event.")) {
        return true;
    }
    if (expression == "event.subject" || expression == "event.position") {
        return true;
    }
    if (expression == "event.item") {
        return trigger_name == "on_item_used";
    }
    if (expression == "event.target") {
        return trigger_name == "on_activated" ||
            trigger_name == "on_item_used" ||
            trigger_name == "on_collision" ||
            trigger_name == "on_projectile_impact";
    }
    if (expression == "event.instigator") {
        return trigger_name == "on_activated" ||
            trigger_name == "on_item_used" ||
            trigger_name == "on_health_depleted" ||
            trigger_name == "on_destroy_entity" ||
            trigger_name == "on_projectile_impact" ||
            trigger_name == "on_expired" ||
            trigger_name == "on_apply" ||
            trigger_name == "on_tick" ||
            trigger_name == "on_expire";
    }
    if (expression == "event.direction") {
        return trigger_name == "on_activated" ||
            trigger_name == "on_item_used" ||
            trigger_name == "on_collision" ||
            trigger_name == "on_projectile_impact" ||
            trigger_name == "on_expired";
    }
    return false;
}

void validate_trigger_parameters(
    const TriggerBindingConfig& binding,
    std::string_view trigger_name,
    const ActionGraphTemplateConfig& graph) {
    std::unordered_set<std::string> seen_parameters;
    for (const auto& parameter : binding.parameters) {
        if (!seen_parameters.insert(parameter.first).second ||
            std::none_of(
                graph.parameters.begin(),
                graph.parameters.end(),
                [&](const ActionGraphParameterConfig& declaration) {
                    return declaration.name == parameter.first;
                })) {
            throw std::runtime_error(
                "trigger binding passes undeclared or duplicate parameter: " +
                parameter.first);
        }
        if (!event_expression_available(trigger_name, parameter.second)) {
            throw std::runtime_error(
                std::string(trigger_name) + " does not provide " +
                parameter.second);
        }
    }
    for (const ActionGraphParameterConfig& parameter : graph.parameters) {
        if (parameter.default_vec3.has_value() &&
            std::none_of(
                binding.parameters.begin(),
                binding.parameters.end(),
                [&](const auto& value) { return value.first == parameter.name; })) {
            continue;
        }
        const std::string value = trigger_parameter_value(binding, parameter);
        if (!event_expression_available(trigger_name, value)) {
            throw std::runtime_error(
                std::string(trigger_name) + " does not provide " + value);
        }
    }
}

void mirror_first_action(KernelActionTriggerDefinition* trigger) {
    if (trigger == nullptr || trigger->action_count == 0u) {
        return;
    }
    const KernelActionDefinition& action = trigger->actions[0];
    trigger->action_type = action.action_type;
    trigger->target_source = action.target_source;
    trigger->damage_amount = action.damage_amount;
    trigger->spawn_entity_template_id = action.spawn_entity_template_id;
    trigger->spawn_projectile_template_id = action.spawn_projectile_template_id;
    trigger->position_source = action.position_source;
    trigger->direction_source = action.direction_source;
    trigger->owner_source = action.owner_source;
    trigger->spawn_item_template_id = action.spawn_item_template_id;
    trigger->spawn_item_quantity = action.spawn_item_quantity;
    trigger->health_change_amount = action.health_change_amount;
    trigger->condition_type = action.condition_type;
    trigger->impulse_strength = action.impulse_strength;
    trigger->impulse_collision_mask = action.impulse_collision_mask;
    trigger->impulse_direction = action.impulse_direction;
    trigger->status_effect_id = action.status_effect_id;
    trigger->modifier_operation = action.modifier_operation;
    trigger->modifier_value = action.modifier_value;
}

void compile_projectile_trigger_binding(
    const ProjectileTriggerBindingConfig& binding,
    bool expired,
    const std::vector<ActionGraphTemplateConfig>& action_graph_templates,
    std::vector<ProjectileTemplateConfig>* projectile_templates,
    ProjectileTemplateConfig* projectile_template) {
    if (binding.action_graph_ref.empty()) {
        return;
    }
    const ActionGraphTemplateConfig* graph = action_graph_template_from_ref(
        binding.action_graph_ref, action_graph_templates);
    const std::string_view trigger_name =
        expired ? "on_expired" : "on_projectile_impact";
    validate_trigger_parameters(binding, trigger_name, *graph);
    const auto graph_parameter = [&](const std::string& name)
        -> const ActionGraphParameterConfig& {
        const auto found = std::find_if(
            graph->parameters.begin(),
            graph->parameters.end(),
            [&](const ActionGraphParameterConfig& parameter) {
                return parameter.name == name;
            });
        if (found == graph->parameters.end()) {
            throw std::runtime_error(
                "action references undeclared graph parameter: " + name);
        }
        return *found;
    };
    const auto entity_ref_source = [](const std::string& expression)
        -> std::uint8_t {
        if (expression == "self") return KernelEntityRefSource_Self;
        if (expression == "event.subject") return KernelEntityRefSource_EventSubject;
        if (expression == "event.target") return KernelEntityRefSource_EventTarget;
        if (expression == "event.instigator") return KernelEntityRefSource_EventInstigator;
        throw std::runtime_error(
            "action parameter must be an entity reference expression");
    };
    KernelActionTriggerDefinition& compiled =
        expired
        ? projectile_template->definition.mechanics
              .expired_trigger
        : projectile_template->definition.mechanics
              .projectile_impact_trigger;
    compiled.struct_size = sizeof(KernelActionTriggerDefinition);
    compiled.action_count = static_cast<std::uint32_t>(graph->actions.size());
    for (std::size_t index = 0; index < graph->actions.size(); ++index) {
        const ActionGraphActionConfig& action = graph->actions[index];
        KernelActionDefinition& compiled_action = compiled.actions[index];
        compiled_action.condition_type = action.condition_type;
        if (action.action_type == "apply_damage" ||
            action.action_type == "apply_health_change" ||
            action.action_type == "apply_impulse") {
            const std::string target = trigger_parameter_value(
                binding, graph_parameter(action.target_parameter));
            compiled_action.target_source = entity_ref_source(target);
            if (action.action_type == "apply_impulse") {
                const std::string strength = trigger_parameter_value(
                    binding, graph_parameter(action.strength_parameter));
                const ActionGraphParameterConfig& direction_parameter =
                    graph_parameter(action.direction_parameter);
                const auto direction_binding = std::find_if(
                    binding.parameters.begin(),
                    binding.parameters.end(),
                    [&](const auto& value) {
                        return value.first == direction_parameter.name;
                    });
                std::size_t parsed = 0;
                const float parsed_strength = std::stof(strength, &parsed);
                if (parsed != strength.size() ||
                    !std::isfinite(parsed_strength) || parsed_strength <= 0.0f) {
                    throw std::runtime_error(
                        "apply_impulse projectile trigger requires positive strength");
                }
                compiled_action.action_type =
                    KernelEntityTriggerActionType_ApplyImpulse;
                if (direction_binding == binding.parameters.end() &&
                    direction_parameter.default_vec3.has_value()) {
                    compiled_action.direction_source =
                        KernelEventVec3Source_Literal;
                    compiled_action.impulse_direction =
                        *direction_parameter.default_vec3;
                } else {
                    const std::string direction = trigger_parameter_value(
                        binding, direction_parameter);
                    if (direction != "event.direction") {
                        throw std::runtime_error(
                            "apply_impulse projectile trigger direction must be event.direction or a direction vec3 default");
                    }
                    compiled_action.direction_source =
                        KernelEventVec3Source_Direction;
                }
                compiled_action.impulse_strength = parsed_strength;
                compiled_action.impulse_collision_mask = action.collision_mask;
            } else {
                const std::string amount = trigger_parameter_value(
                    binding, graph_parameter(action.amount_parameter));
                std::size_t parsed = 0;
                const long parsed_amount = std::stol(amount, &parsed);
                if (parsed != amount.size() || parsed_amount <= 0 ||
                    parsed_amount > std::numeric_limits<std::uint16_t>::max()) {
                    throw std::runtime_error(
                        "projectile damage action amount must be a positive uint16");
                }
                compiled_action.action_type = action.action_type == "apply_damage"
                    ? KernelEntityTriggerActionType_ApplyDamage
                    : KernelEntityTriggerActionType_ApplyHealthChange;
                if (compiled_action.action_type ==
                    KernelEntityTriggerActionType_ApplyDamage) {
                    compiled_action.damage_amount =
                        static_cast<std::uint16_t>(parsed_amount);
                } else {
                    compiled_action.health_change_amount =
                        static_cast<std::int32_t>(parsed_amount);
                }
            }
            continue;
        }
        if (action.action_type != "spawn_projectile") {
            throw std::runtime_error(
                "projectile trigger requires spawn_projectile actions: " +
                binding.action_graph_ref);
        }
        const std::string projectile_ref = trigger_parameter_value(
            binding,
            graph_parameter(action.projectile_template_parameter));
        const std::string position = trigger_parameter_value(
            binding, graph_parameter(action.position_parameter));
        const std::string direction = trigger_parameter_value(
            binding, graph_parameter(action.direction_parameter));
        if (position != "event.position" || direction != "event.direction") {
            throw std::runtime_error(
                "spawn_projectile trigger must bind position and direction to "
                "event.position and event.direction");
        }
        ProjectileTemplateConfig* spawned_projectile =
            projectile_template_from_ref(
                YAML::Node(projectile_ref), projectile_templates);
        compiled_action.action_type =
            KernelEntityTriggerActionType_SpawnProjectile;
        compiled_action.spawn_projectile_template_id =
            spawned_projectile->definition.projectile_template_id;
        compiled_action.position_source = KernelEventVec3Source_Position;
        compiled_action.direction_source = KernelEventVec3Source_Direction;
    }
    mirror_first_action(&compiled);
}

KernelActionTriggerDefinition compile_action_trigger_binding(
    const TriggerBindingConfig& binding,
    std::string_view trigger_name,
    const std::vector<ActionGraphTemplateConfig>& action_graph_templates,
    const std::vector<EntityTemplateConfig>& entity_templates,
    const std::vector<ProjectileTemplateConfig>* projectile_templates = nullptr,
    const std::vector<ItemTemplateConfig>* item_templates = nullptr,
    const std::vector<StatusEffectTemplateConfig>* status_effect_templates = nullptr) {
    KernelActionTriggerDefinition compiled{};
    if (binding.action_graph_ref.empty()) {
        return compiled;
    }
    const ActionGraphTemplateConfig* graph = action_graph_template_from_ref(
        binding.action_graph_ref, action_graph_templates);
    validate_trigger_parameters(binding, trigger_name, *graph);
    const auto graph_parameter = [&](const std::string& name)
        -> const ActionGraphParameterConfig& {
        const auto found = std::find_if(
            graph->parameters.begin(),
            graph->parameters.end(),
            [&](const ActionGraphParameterConfig& parameter) {
                return parameter.name == name;
            });
        if (found == graph->parameters.end()) {
            throw std::runtime_error(
                "action references undeclared graph parameter: " + name);
        }
        return *found;
    };
    const auto entity_ref_source = [](const std::string& expression)
        -> std::uint8_t {
        if (expression == "self") {
            return KernelEntityRefSource_Self;
        }
        if (expression == "event.subject") {
            return KernelEntityRefSource_EventSubject;
        }
        if (expression == "event.target") {
            return KernelEntityRefSource_EventTarget;
        }
        if (expression == "event.instigator") {
            return KernelEntityRefSource_EventInstigator;
        }
        throw std::runtime_error(
            "action parameter must be an entity reference expression");
    };
    const auto status_ref = [&](const std::string& expression) {
        if (status_effect_templates == nullptr) {
            throw std::runtime_error(
                std::string(trigger_name) + " cannot reference status effects in this context");
        }
        const auto found = std::find_if(
            status_effect_templates->begin(), status_effect_templates->end(),
            [&](const StatusEffectTemplateConfig& status) {
                return status.name == expression ||
                    std::to_string(status.status_effect_id) == expression;
            });
        if (found == status_effect_templates->end()) {
            throw std::runtime_error("unknown status effect: " + expression);
        }
        return found->status_effect_id;
    };
    compiled.struct_size = sizeof(KernelActionTriggerDefinition);
    compiled.action_count = static_cast<std::uint32_t>(graph->actions.size());
    for (std::size_t index = 0; index < graph->actions.size(); ++index) {
        const ActionGraphActionConfig& action = graph->actions[index];
        KernelActionDefinition& compiled_action = compiled.actions[index];
        compiled_action.condition_type = action.condition_type;
        if (action.action_type == "spawn_projectile") {
            if (projectile_templates == nullptr) {
                throw std::runtime_error(
                    std::string(trigger_name) +
                    " cannot spawn projectiles in this context");
            }
            const std::string projectile_ref = trigger_parameter_value(
                binding,
                graph_parameter(action.projectile_template_parameter));
            const std::string position = trigger_parameter_value(
                binding,
                graph_parameter(action.position_parameter));
            const std::string direction = trigger_parameter_value(
                binding,
                graph_parameter(action.direction_parameter));
            if (position != "event.position" ||
                direction != "event.direction") {
                throw std::runtime_error(
                    "spawn_projectile requires event position and direction");
            }
            const auto found = std::find_if(
                projectile_templates->begin(),
                projectile_templates->end(),
                [&](const ProjectileTemplateConfig& candidate) {
                    return candidate.name == projectile_ref ||
                        std::to_string(
                            candidate.definition.projectile_template_id) ==
                            projectile_ref;
                });
            if (found == projectile_templates->end()) {
                throw std::runtime_error(
                    "unknown projectile template: " + projectile_ref);
            }
            compiled_action.action_type =
                KernelEntityTriggerActionType_SpawnProjectile;
            compiled_action.spawn_projectile_template_id =
                found->definition.projectile_template_id;
            compiled_action.position_source = KernelEventVec3Source_Position;
            compiled_action.direction_source = KernelEventVec3Source_Direction;
            continue;
        }
        if (action.action_type == "spawn_entity") {
            const std::string entity_template = trigger_parameter_value(
                binding, graph_parameter(action.entity_template_parameter));
            const std::string position = trigger_parameter_value(
                binding, graph_parameter(action.position_parameter));
            const std::string direction =
                action.direction_parameter.empty()
                ? ""
                : trigger_parameter_value(
                      binding, graph_parameter(action.direction_parameter));
            const std::string owner = trigger_parameter_value(
                binding, graph_parameter(action.owner_parameter));
            if (position != "event.position" ||
                (!action.direction_parameter.empty() &&
                 direction != "event.direction")) {
                throw std::runtime_error(
                    "spawn_entity position/direction must bind to event.position/event.direction");
            }
            compiled_action.action_type =
                KernelEntityTriggerActionType_SpawnEntity;
            compiled_action.spawn_entity_template_id =
                entity_template_ref_from_yaml(
                    YAML::Node(entity_template), entity_templates);
            compiled_action.position_source = KernelEventVec3Source_Position;
            if (!action.direction_parameter.empty()) {
                compiled_action.direction_source =
                    KernelEventVec3Source_Direction;
            }
            compiled_action.owner_source = entity_ref_source(owner);
            if (!action.item_template_ref.empty()) {
                if (item_templates == nullptr) {
                    throw std::runtime_error(
                        std::string(trigger_name) +
                        " cannot spawn item-backed entities in this context");
                }
                const auto item = std::find_if(
                    item_templates->begin(),
                    item_templates->end(),
                    [&](const ItemTemplateConfig& candidate) {
                        return candidate.name == action.item_template_ref ||
                            std::to_string(
                                candidate.definition.item_template_id) ==
                                action.item_template_ref;
                    });
                if (item == item_templates->end()) {
                    throw std::runtime_error(
                        "unknown item template: " + action.item_template_ref);
                }
                if (item->definition.entity_template_id !=
                        compiled_action.spawn_entity_template_id ||
                    action.quantity > item->definition.max_stack ||
                    (item->definition.item_mode == KernelItemMode_Stateful &&
                     action.quantity != 1u)) {
                    throw std::runtime_error(
                        "spawn_entity item template/quantity does not match entity template policy");
                }
                compiled_action.spawn_item_template_id =
                    item->definition.item_template_id;
                compiled_action.spawn_item_quantity = action.quantity;
            }
            continue;
        }
        if (action.action_type == "apply_impulse") {
            const std::string target = trigger_parameter_value(
                binding, graph_parameter(action.target_parameter));
            const std::string strength = trigger_parameter_value(
                binding, graph_parameter(action.strength_parameter));
            const ActionGraphParameterConfig& direction_parameter =
                graph_parameter(action.direction_parameter);
            const auto direction_binding = std::find_if(
                binding.parameters.begin(),
                binding.parameters.end(),
                [&](const auto& value) {
                    return value.first == direction_parameter.name;
                });
            std::size_t parsed = 0;
            const float parsed_strength = std::stof(strength, &parsed);
            if (parsed != strength.size() || !std::isfinite(parsed_strength) ||
                parsed_strength <= 0.0f) {
                throw std::runtime_error(
                    "apply_impulse strength must be a positive finite float");
            }
            compiled_action.action_type =
                KernelEntityTriggerActionType_ApplyImpulse;
            compiled_action.target_source = entity_ref_source(target);
            if (direction_binding == binding.parameters.end() &&
                direction_parameter.default_vec3.has_value()) {
                compiled_action.direction_source =
                    KernelEventVec3Source_Literal;
                compiled_action.impulse_direction =
                    *direction_parameter.default_vec3;
            } else {
                const std::string direction = trigger_parameter_value(
                    binding, direction_parameter);
                if (direction != "event.direction") {
                    throw std::runtime_error(
                        "apply_impulse direction must be event.direction or a direction vec3 default");
                }
                compiled_action.direction_source =
                    KernelEventVec3Source_Direction;
            }
            compiled_action.impulse_strength = parsed_strength;
            compiled_action.impulse_collision_mask = action.collision_mask;
            continue;
        }
        if (action.action_type == "apply_status" ||
            action.action_type == "remove_status") {
            const std::string target = trigger_parameter_value(
                binding, graph_parameter(action.target_parameter));
            const std::string status = trigger_parameter_value(
                binding, graph_parameter(action.status_parameter));
            compiled_action.action_type = action.action_type == "apply_status"
                ? KernelEntityTriggerActionType_ApplyStatus
                : KernelEntityTriggerActionType_RemoveStatus;
            compiled_action.target_source = entity_ref_source(target);
            compiled_action.status_effect_id = status_ref(status);
            continue;
        }
        if (action.action_type == "apply_speed_modifier") {
            if (trigger_name != "on_apply") {
                throw std::runtime_error(
                    "apply_speed_modifier is only valid in status on_apply");
            }
            const std::string target = trigger_parameter_value(
                binding, graph_parameter(action.target_parameter));
            const std::string operation = trigger_parameter_value(
                binding, graph_parameter(action.operation_parameter));
            const std::string value = trigger_parameter_value(
                binding, graph_parameter(action.value_parameter));
            std::size_t operation_parsed = 0;
            const float modifier_value = std::stof(value, &operation_parsed);
            if (operation_parsed != value.size() ||
                !std::isfinite(modifier_value)) {
                throw std::runtime_error(
                    "apply_speed_modifier value must be a finite float");
            }
            if (operation == "additive") {
                compiled_action.modifier_operation =
                    KernelStatModifierOperation_Additive;
            } else if (operation == "multiplier") {
                compiled_action.modifier_operation =
                    KernelStatModifierOperation_Multiplier;
            } else {
                throw std::runtime_error(
                    "apply_speed_modifier operation must be additive or multiplier");
            }
            compiled_action.action_type =
                KernelEntityTriggerActionType_ApplySpeedModifier;
            compiled_action.target_source = entity_ref_source(target);
            compiled_action.modifier_value = modifier_value;
            continue;
        }
        if (action.action_type != "apply_damage" &&
            action.action_type != "apply_health_change") {
            throw std::runtime_error(
                std::string(trigger_name) +
                " requires apply_damage, apply_health_change, or spawn_entity actions: " +
                binding.action_graph_ref);
        }
        const std::string target = trigger_parameter_value(
            binding, graph_parameter(action.target_parameter));
        const std::string amount = trigger_parameter_value(
            binding, graph_parameter(action.amount_parameter));
        std::size_t parsed = 0;
        const long parsed_amount = std::stol(amount, &parsed);
        if (action.action_type == "apply_damage") {
            if (parsed != amount.size() || parsed_amount <= 0 ||
                parsed_amount > std::numeric_limits<std::uint16_t>::max()) {
                throw std::runtime_error(
                    "apply_damage amount must be a positive uint16");
            }
            compiled_action.action_type =
                KernelEntityTriggerActionType_ApplyDamage;
            compiled_action.damage_amount =
                static_cast<std::uint16_t>(parsed_amount);
        } else {
            if (parsed != amount.size() || parsed_amount == 0 ||
                parsed_amount <
                    -static_cast<long>(
                        std::numeric_limits<std::uint16_t>::max()) ||
                parsed_amount >
                    static_cast<long>(
                        std::numeric_limits<std::uint16_t>::max())) {
                throw std::runtime_error(
                    "apply_health_change amount must be within signed uint16 range");
            }
            compiled_action.action_type =
                KernelEntityTriggerActionType_ApplyHealthChange;
            compiled_action.health_change_amount =
                static_cast<std::int32_t>(parsed_amount);
        }
        compiled_action.target_source = entity_ref_source(target);
    }
    mirror_first_action(&compiled);
    return compiled;
}

std::vector<ProjectileTemplateConfig> load_projectile_templates_from_source(
    const GameplayConfigSource& source,
    const std::string& directory,
    const ColliderCatalogConfig& colliders,
    const std::vector<ActionGraphTemplateConfig>& action_graph_templates) {
    std::vector<ProjectileTemplateConfig> projectile_templates;
    std::unordered_map<std::uint32_t, std::string> ids;
    std::unordered_map<std::string, std::uint32_t> names;
    const std::vector<std::string> files = source.list_yaml_files(directory);
    for (const std::string& file : files) {
        ProjectileTemplateConfig projectile_template =
            projectile_template_from_yaml(
                source.load_yaml(file),
                file,
                source.source_kind(),
                colliders);
        if (ids.contains(projectile_template.definition.projectile_template_id)) {
            throw std::runtime_error("duplicate projectile template id: " + file);
        }
        if (names.contains(projectile_template.name)) {
            throw std::runtime_error(
                "duplicate projectile template name: " + projectile_template.name);
        }
        ids.emplace(projectile_template.definition.projectile_template_id, file);
        names.emplace(
            projectile_template.name,
            projectile_template.definition.projectile_template_id);
        projectile_templates.push_back(projectile_template);
    }
    if (projectile_templates.empty()) {
        throw std::runtime_error(
            "projectile template directory is empty: " + directory);
    }
    std::sort(
        projectile_templates.begin(),
        projectile_templates.end(),
        [](const ProjectileTemplateConfig& lhs,
           const ProjectileTemplateConfig& rhs) {
            return lhs.definition.projectile_template_id <
                   rhs.definition.projectile_template_id;
    });
    for (ProjectileTemplateConfig& projectile_template : projectile_templates) {
        compile_projectile_trigger_binding(
            projectile_template.projectile_impact_trigger,
            false,
            action_graph_templates,
            &projectile_templates,
            &projectile_template);
        compile_projectile_trigger_binding(
            projectile_template.expired_trigger,
            true,
            action_graph_templates,
            &projectile_templates,
            &projectile_template);
    }
    for (const ProjectileTemplateConfig& projectile_template : projectile_templates) {
        std::vector<std::uint32_t> visited;
        std::function<void(const ProjectileTemplateConfig*)> visit =
            [&](const ProjectileTemplateConfig* current) {
            const std::uint32_t current_id =
                current->definition.projectile_template_id;
            if (std::find(visited.begin(), visited.end(), current_id) !=
                visited.end()) {
                throw std::runtime_error(
                    "projectile trigger graph reference cycle: " +
                    projectile_template.name);
            }
            visited.push_back(current_id);
            for (const std::uint32_t next_id : {
                     current->definition.mechanics.projectile_impact_trigger
                         .spawn_projectile_template_id,
                     current->definition.mechanics.expired_trigger
                         .spawn_projectile_template_id,
                 }) {
                if (next_id == 0u) {
                    continue;
                }
                visit(projectile_template_from_ref(
                    YAML::Node(std::to_string(next_id)),
                    &projectile_templates));
            }
            visited.pop_back();
        };
        visit(&projectile_template);
    }
    return projectile_templates;
}

ProjectileTemplateConfig* projectile_template_from_ref(
    const YAML::Node& node,
    std::vector<ProjectileTemplateConfig>* projectile_templates) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error("projectile_template reference must be a scalar");
    }
    const std::string value = node.as<std::string>();
    if (!value.empty() &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        })) {
        const std::uint32_t template_id =
            static_cast<std::uint32_t>(std::stoul(value));
        for (ProjectileTemplateConfig& projectile_template :
             *projectile_templates) {
            if (projectile_template.definition.projectile_template_id == template_id) {
                return &projectile_template;
            }
        }
        throw std::runtime_error("unknown projectile_template id: " + value);
    }
    for (ProjectileTemplateConfig& projectile_template : *projectile_templates) {
        if (projectile_template.name == value) {
            return &projectile_template;
        }
    }
    throw std::runtime_error("unknown projectile_template name: " + value);
}

void apply_weapon_template_references(
    const GameplayConfigSource& source,
    const std::string& directory,
    const ColliderCatalogConfig& colliders,
    std::vector<ProjectileTemplateConfig>* projectile_templates,
    const std::vector<ActionTemplateConfig>& action_templates,
    WeaponCatalogConfig* weapons) {
    const std::vector<std::string> files = source.list_yaml_files(directory);
    for (const std::string& file : files) {
        const YAML::Node document = source.load_yaml(file);
        const auto weapon_id =
            static_cast<std::uint8_t>(document["id"].as<int>());
        if (document["fire_action_template"]) {
            const ActionTemplateConfig* fire_action =
                action_template_from_ref(
                    document["fire_action_template"],
                    action_templates,
                    "fire_action_template");
            if (fire_action->definition.commit_interval_ticks == 0u) {
                throw DataLoadError(
                    KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_NUMERIC_RANGE,
                    "weapon fire_action_template requires "
                    "commit_interval_ticks greater than 0",
                    fire_action->source_path,
                    "commit_interval_ticks",
                    fire_action->source_kind,
                    KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTION,
                    fire_action->definition.action_template_id,
                    0,
                    fire_action->commit_interval_line,
                    fire_action->commit_interval_column);
            }
            weapons->definitions[weapon_id].fire_action_template_id =
                fire_action->definition.action_template_id;
        }
        // A weapon that names its own reload owns both the duration and the
        // shape of it: the action template carries commit_offset_ticks (how
        // long the reload takes), but also trigger_mode, flags, and the commit
        // count -- so a shell-at-a-time reload that the player can interrupt is
        // expressible here without a second authoring surface. A weapon that
        // names none falls back to the catalog's shared reload, filled in after
        // this pass.
        if (document["reload_action_template"]) {
            weapons->definitions[weapon_id].reload_action_template_id =
                action_template_from_ref(
                    document["reload_action_template"],
                    action_templates,
                    "reload_action_template")
                    ->definition.action_template_id;
        }
        const std::string type = document["weapon_type"].as<std::string>();
        if (type == "hitscan" || type == "shotgun") {
            if (!document["segment_collider"]) {
                throw std::runtime_error(
                    "instant weapon requires segment_collider: " + file);
            }
            const std::uint32_t template_id =
                collider_template_id_from_ref(document["segment_collider"], colliders);
            weapons->definitions[weapon_id].segment_collider_template_id = template_id;
            weapons->collider_template_ids[weapon_id] = template_id;
            // An instant weapon spawns nothing, but its shot is still described
            // by a projectile template: that is where its damage and its target
            // mask are authored, exactly as they are for a weapon that does
            // spawn something. One place to look for either number, whatever
            // the fire mode.
            //
            // fire_mode stays Hitscan/Shotgun and collider_template_ids keeps
            // the segment above -- only the two authored values are taken.
            ProjectileTemplateConfig* shot_template =
                projectile_template_from_ref(
                    document["projectile_template"],
                    projectile_templates);
            KernelWeaponMechanicsDefinition& instant_weapon =
                weapons->definitions[weapon_id];
            shot_template->definition.weapon_id = weapon_id;
            instant_weapon.projectile_template_id =
                shot_template->definition.projectile_template_id;
            instant_weapon.damage = shot_template->definition.mechanics.damage;
            instant_weapon.collision_mask =
                shot_template->definition.mechanics.collision_mask;
            continue;
        }

        if (type == "projectile" || type == "area_effect" || type == "beam") {
            ProjectileTemplateConfig* projectile_template =
                projectile_template_from_ref(
                    document["projectile_template"],
                    projectile_templates);
            KernelWeaponMechanicsDefinition& weapon =
                weapons->definitions[weapon_id];
            const KernelProjectileTemplateDefinition& definition =
                projectile_template->definition;
            projectile_template->definition.weapon_id = weapon_id;
            weapon.fire_mode = KernelWeaponFireMode_Projectile;
            weapon.projectile_template_id = definition.projectile_template_id;
            weapon.damage = definition.mechanics.damage;
            weapon.collision_mask = definition.mechanics.collision_mask;
            weapons->projectile_sync_modes[weapon_id] =
                definition.mechanics.sync_mode;
            weapons->collider_template_ids[weapon_id] =
                definition.mechanics.collider_template_id;
            continue;
        }

        YAML::Node collider_ref;
        if (type == "area_effect") {
            collider_ref = document["area_effect"]["collider_template"];
        } else if (type == "beam") {
            collider_ref = document["beam"]["collider_template"];
        }
        if (!collider_ref) {
            throw std::runtime_error(
                "weapon requires collider_template binding: " + file);
        }
        weapons->collider_template_ids[weapon_id] =
            collider_template_id_from_ref(collider_ref, colliders);
    }
}

WeaponCatalogConfig load_weapon_catalog_from_source(
    const GameplayConfigSource& source,
    const std::string& directory) {
    WeaponCatalogConfig weapons;
    weapons.projectile_sync_modes.fill(
        KernelProjectileSyncMode_HybridDeterministicThenSnapshot);
    std::array<bool, kWeaponIdCount> seen{};
    const std::vector<std::string> files = source.list_yaml_files(directory);
    for (const std::string& file : files) {
        const YAML::Node document = source.load_yaml(file);
        KernelWeaponMechanicsDefinition weapon =
            weapon_from_yaml(document, file, source.source_kind());
        if (seen[weapon.weapon_id]) {
            throw std::runtime_error("duplicate weapon id: " + file);
        }
        const std::string name =
            document["name"] ? document["name"].as<std::string>()
                             : std::filesystem::path(file).stem().string();
        for (std::size_t index = 0; index < weapons.names.size(); ++index) {
            if (seen[index] && weapons.names[index] == name) {
                throw DataLoadError(
                    KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_DUPLICATE_TEMPLATE_NAME,
                    "duplicate weapon name: " + name,
                    file,
                    "name",
                    source.source_kind(),
                    KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_WEAPON,
                    weapon.weapon_id);
            }
        }
        seen[weapon.weapon_id] = true;
        weapons.configured[weapon.weapon_id] = true;
        weapons.definitions[weapon.weapon_id] = weapon;
        weapons.projectile_sync_modes[weapon.weapon_id] =
            projectile_sync_mode_from_weapon_yaml(document);
        weapons.names[weapon.weapon_id] = name;
    }
    return weapons;
}

GameServerGameplayConfig load_gameplay_config_from_weapon_template_source(
    const GameplayConfigSource& source,
    const std::string& directory) {
    GameServerGameplayConfig config;
    const std::string action_template_dir = source.resolve_path(
        source.parent_path(directory), YAML::Node("action_templates"));
    config.action_templates =
        load_action_templates_from_source(source, action_template_dir);
    ActionTemplateConfig reload_action;
    reload_action.name = "shared_reload";
    reload_action.definition.struct_size =
        sizeof(KernelActionTemplateDefinition);
    reload_action.definition.action_template_id = 4199u;
    reload_action.definition.trigger_mode = KernelActionTriggerMode_Press;
    reload_action.definition.flags =
        KernelActionTemplateFlag_CancelOnDeath |
        KernelActionTemplateFlag_CancelOnWeaponChange |
        KernelActionTemplateFlag_CancelBeforeFirstCommit;
    reload_action.definition.commit_offset_ticks = 30u;
    reload_action.definition.max_commit_count = 1u;
    config.action_templates.push_back(reload_action);
    config.weapons = load_weapon_catalog_from_source(source, directory);
    apply_default_non_weapon_config(&config);
    config.colliders = load_collider_catalog_from_source(
        source,
        source.default_collider_template_dir_for_weapon_dir(directory));
    const std::string action_graph_template_dir = source.resolve_path(
        source.parent_path(directory), YAML::Node("action_graph_templates"));
    config.action_graph_templates = load_action_graph_templates_from_source(
        source, action_graph_template_dir);
    config.projectile_templates = load_projectile_templates_from_source(
        source,
        source.default_projectile_template_dir_for_weapon_dir(directory),
        config.colliders,
        config.action_graph_templates);
    apply_weapon_template_references(
        source,
        directory,
        config.colliders,
        &config.projectile_templates,
        config.action_templates,
        &config.weapons);
    // Only where the weapon named none of its own. This used to overwrite every
    // weapon unconditionally, which is why a per-weapon reload had nowhere to
    // live and `reload_ticks` sat in the templates doing nothing.
    for (std::size_t id = 0; id < config.weapons.definitions.size(); ++id) {
        if (config.weapons.configured[id] &&
            config.weapons.definitions[id].reload_action_template_id == 0u) {
            config.weapons.definitions[id].reload_action_template_id =
                reload_action.definition.action_template_id;
        }
    }
    apply_default_actor_templates(&config);
    config.weapons.catalog_hash = compute_gameplay_catalog_hash(config);
    const std::vector<std::string> errors = validate_gameplay_config(config);
    if (!errors.empty()) {
        throw std::runtime_error(errors.front());
    }
    return config;
}

GameServerGameplayConfig load_gameplay_config_from_catalog_source(
    const GameplayConfigSource& source,
    const std::string& path) {
    const YAML::Node document = source.load_yaml(path);
    reject_unknown_keys(
        document,
        {
            "catalog_version",
            "action_template_dir",
            "action_graph_template_dir",
            "status_effect_template_dir",
            "reload_action_template",
            "weapon_template_dir",
            "projectile_template_dir",
            "actor_template_dir",
            "entity_template_dir",
            "item_template_dir",
            "collider_template_dir",
            "prop_population_rules",
            "static_collision_scene",
            "skeleton_manifests",
            "skeleton_manifests_dir",
            "player",
            "enemy",
            "preload_directors",
        },
        path,
        source.source_kind(),
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG);
    if (document["player"]) {
        reject_unknown_keys(
            document["player"],
            {"actor_template", "entity_template"},
            path,
            source.source_kind(),
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG);
    }
    if (document["enemy"]) {
        reject_unknown_keys(
            document["enemy"],
            {
                "actor_template",
                "entity_template",
                "spawn_count",
                "spawn_radius",
                "spawn_seed",
                "spawn_position",
            },
            path,
            source.source_kind(),
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG);
    }
    if (document["static_collision_scene"]) {
        reject_unknown_keys(
            document["static_collision_scene"],
            {"entry_path", "scene_id", "collider_id", "collision_layer"},
            path,
            source.source_kind(),
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG);
    }
    const std::uint32_t catalog_version =
        document["catalog_version"] ? document["catalog_version"].as<std::uint32_t>()
                                    : 1u;
    // 11 added the optional skeleton.colliders block. 10 replaced the
    // skeleton_manifests list with skeleton_manifests_dir. 8, 9 and 10 stay
    // loadable because nothing else about them changed; a catalog that still
    // carries the old key is caught below with a migration message rather than
    // a bare unknown-key error. The bump exists because this loader rejects
    // unknown keys, so a catalog carrying colliders would otherwise fail
    // an older kernel with a bare field error instead of a version one.
    if (catalog_version != 8u && catalog_version != 9u &&
        catalog_version != 10u && catalog_version != 11u &&
        catalog_version != 12u && catalog_version != 13u) {
        throw DataLoadError(
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNSUPPORTED_CATALOG_VERSION,
            "unsupported catalog_version: " + std::to_string(catalog_version),
            path,
            "catalog_version",
            source.source_kind(),
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG,
            0,
            0,
            yaml_line(document["catalog_version"]),
            yaml_column(document["catalog_version"]));
    }
    if (!document["weapon_template_dir"] || !document["projectile_template_dir"] ||
        !document["collider_template_dir"]) {
        throw std::runtime_error(
            "gameplay catalog requires weapon_template_dir and "
            "projectile_template_dir and collider_template_dir: " +
            path);
    }

    const std::string base_path = source.parent_path(path);
    const std::string weapon_template_dir =
        source.resolve_path(base_path, document["weapon_template_dir"]);
    GameServerGameplayConfig config;
    if (document["skeleton_manifests"]) {
        throw DataLoadError(
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_YAML,
            "skeleton_manifests was replaced by skeleton_manifests_dir in "
            "catalog_version 10: point it at the directory holding the "
            "*.skeleton_manifest.json files instead of listing them",
            path,
            "skeleton_manifests",
            source.source_kind(),
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG,
            0,
            0,
            yaml_line(document["skeleton_manifests"]),
            yaml_column(document["skeleton_manifests"]));
    }
    if (document["skeleton_manifests_dir"]) {
        config.skeleton_assets = load_skeleton_assets_from_directory(
            source,
            base_path,
            document["skeleton_manifests_dir"]);
    }
    if (document["prop_population_rules"]) {
        config.prop_population_rules = prop_population_rules_from_yaml(
            document["prop_population_rules"],
            path,
            source.source_kind());
    }
    if (document["static_collision_scene"]) {
        const YAML::Node scene = document["static_collision_scene"];
        if (!scene["entry_path"] || !scene["scene_id"] ||
            !scene["collider_id"] || !scene["collision_layer"]) {
            throw std::runtime_error(
                "static_collision_scene requires entry_path, scene_id, "
                "collider_id, and collision_layer: " +
                path);
        }
        config.static_collision_scene.entry_path =
            scene["entry_path"].as<std::string>();
        config.static_collision_scene.scene_id =
            scene["scene_id"].as<std::uint32_t>();
        config.static_collision_scene.collider_id =
            scene["collider_id"].as<std::uint32_t>();
        config.static_collision_scene.collision_layer =
            scene["collision_layer"].as<std::uint32_t>();
    }
    if (document["action_template_dir"]) {
        const std::string action_template_dir =
            source.resolve_path(base_path, document["action_template_dir"]);
        config.action_templates =
            load_action_templates_from_source(source, action_template_dir);
    }
    if (!document["reload_action_template"]) {
        throw std::runtime_error(
            "gameplay catalog requires reload_action_template: " + path);
    }
    ActionTemplateConfig reload_action = action_template_from_yaml(
        document["reload_action_template"],
        path,
        source.source_kind());
    config.action_templates.push_back(reload_action);
    config.weapons = load_weapon_catalog_from_source(source, weapon_template_dir);
    apply_default_non_weapon_config(&config);
    config.weapons.catalog_version = catalog_version;

    const std::string collider_template_dir =
        source.resolve_path(base_path, document["collider_template_dir"]);
    config.colliders =
        load_collider_catalog_from_source(source, collider_template_dir);
    const std::string projectile_template_dir =
        source.resolve_path(base_path, document["projectile_template_dir"]);
    if (document["action_graph_template_dir"]) {
        const std::string action_graph_template_dir =
            source.resolve_path(base_path, document["action_graph_template_dir"]);
        config.action_graph_templates = load_action_graph_templates_from_source(
            source, action_graph_template_dir);
    }
    if (document["status_effect_template_dir"]) {
        const std::string status_effect_template_dir =
            source.resolve_path(base_path, document["status_effect_template_dir"]);
        config.status_effect_templates =
            load_status_effect_templates_from_source(
                source, status_effect_template_dir);
    }
    config.projectile_templates = load_projectile_templates_from_source(
        source,
        projectile_template_dir,
        config.colliders,
        config.action_graph_templates);
    apply_weapon_template_references(
        source,
        weapon_template_dir,
        config.colliders,
        &config.projectile_templates,
        config.action_templates,
        &config.weapons);
    // Only where the weapon named none of its own. This used to overwrite every
    // weapon unconditionally, which is why a per-weapon reload had nowhere to
    // live and `reload_ticks` sat in the templates doing nothing.
    for (std::size_t id = 0; id < config.weapons.definitions.size(); ++id) {
        if (config.weapons.configured[id] &&
            config.weapons.definitions[id].reload_action_template_id == 0u) {
            config.weapons.definitions[id].reload_action_template_id =
                reload_action.definition.action_template_id;
        }
    }

    if (document["entity_template_dir"]) {
        const std::string entity_template_dir =
            source.resolve_path(base_path, document["entity_template_dir"]);
        config.entity_templates = load_entity_templates_from_source(
            source,
            entity_template_dir,
            config.weapons,
            config.colliders,
            config.projectile_templates,
            config.prop_population_rules,
            config.skeleton_assets);
        config.actor_templates =
            actor_templates_from_entity_templates(config.entity_templates);
    } else if (document["actor_template_dir"]) {
        const std::string actor_template_dir =
            source.resolve_path(base_path, document["actor_template_dir"]);
        config.actor_templates = load_actor_templates_from_source(
            source,
            actor_template_dir,
            config.weapons,
            config.colliders,
            config.skeleton_assets);
        config.entity_templates = config.actor_templates;
    } else {
        apply_default_actor_templates(&config);
    }

    if (document["item_template_dir"]) {
        const std::string item_template_dir =
            source.resolve_path(base_path, document["item_template_dir"]);
        config.item_templates = load_item_templates_from_source(
            source,
            item_template_dir,
            config.entity_templates,
            config.projectile_templates);
    }
    resolve_inventory_item_template_references(
        config.item_templates, &config.entity_templates);
    config.actor_templates =
        actor_templates_from_entity_templates(config.entity_templates);

    apply_catalog_player_config(document, &config);
    apply_catalog_director_preload_config(document, &config);
    apply_catalog_agent_config(document, &config);

    const std::vector<std::string> errors = validate_gameplay_config(config);
    if (!errors.empty()) {
        throw std::runtime_error(errors.front());
    }
    config.weapons.catalog_hash = compute_gameplay_catalog_hash(config);
    return config;
}

void apply_default_non_weapon_config(GameServerGameplayConfig* config) {
    config->player = PlayerGameplayDefinition{};
    config->agent = AgentSpawnDefinition{};
    apply_default_actor_templates(config);
}

void apply_catalog_player_config(
    const YAML::Node& document,
    GameServerGameplayConfig* config) {
    const YAML::Node player = document["player"];
    if (!player) {
        return;
    }
    if (player["actor_template"]) {
        config->player.actor_template_id =
            actor_template_ref_from_yaml(player["actor_template"], config->actor_templates);
    }
    if (player["entity_template"]) {
        config->player.actor_template_id =
            entity_template_ref_from_yaml(player["entity_template"], config->entity_templates);
    }
}

void apply_catalog_director_preload_config(
    const YAML::Node& document,
    GameServerGameplayConfig* config) {
    const YAML::Node preload_directors = document["preload_directors"];
    if (!preload_directors) {
        return;
    }
    if (!preload_directors.IsSequence()) {
        throw std::runtime_error("preload_directors must be a sequence");
    }

    std::unordered_set<std::uint32_t> configured_template_ids;
    for (const YAML::Node& ref : preload_directors) {
        const std::uint32_t template_id =
            entity_template_ref_from_yaml(ref, config->entity_templates);
        const auto entity_template = std::find_if(
            config->entity_templates.begin(),
            config->entity_templates.end(),
            [template_id](const EntityTemplateConfig& candidate) {
                return candidate.actor_template_id == template_id;
            });
        if (entity_template == config->entity_templates.end() ||
            entity_template->entity_type != KernelEntityType_Director) {
            throw std::runtime_error(
                "preload_directors must reference director entity templates");
        }
        if (!configured_template_ids.insert(template_id).second) {
            throw std::runtime_error(
                "duplicate preload_directors entity template: " +
                std::to_string(template_id));
        }
        config->preload_director_template_ids.push_back(template_id);
    }
}

void apply_catalog_agent_config(
    const YAML::Node& document,
    GameServerGameplayConfig* config) {
    const YAML::Node agent = document["enemy"];
    if (!agent) {
        const auto director = std::find_if(
            config->entity_templates.begin(),
            config->entity_templates.end(),
            [](const EntityTemplateConfig& entity_template) {
                return entity_template.entity_type == KernelEntityType_Director &&
                       entity_template.director_kind ==
                           KernelDirectorKind_WorldRule &&
                       entity_template.director_spawn_actor_template_id != 0u;
            });
        if (director != config->entity_templates.end()) {
            config->agent.actor_template_id =
                director->director_spawn_actor_template_id;
            config->agent.spawn_count = director->director_spawn_target_count;
            config->agent.spawn_radius = director->director_spawn_radius;
            config->agent.spawn_seed = director->director_spawn_seed;
            config->agent.spawn_position = director->director_spawn_position;
        }
        return;
    }
    if (agent["actor_template"] && agent["entity_template"]) {
        throw std::runtime_error(
            "enemy cannot define both actor_template and entity_template");
    }
    config->agent.override_director_spawn = true;
    if (agent["spawn_count"]) {
        config->agent.spawn_count = agent["spawn_count"].as<std::uint32_t>();
    }
    if (agent["spawn_radius"]) {
        config->agent.spawn_radius = agent["spawn_radius"].as<float>();
    }
    if (agent["spawn_seed"]) {
        config->agent.spawn_seed = agent["spawn_seed"].as<std::uint32_t>();
    }
    if (agent["spawn_position"]) {
        config->agent.spawn_position = vec3_from_yaml(agent["spawn_position"]);
    }
    if (agent["actor_template"]) {
        config->agent.actor_template_id =
            actor_template_ref_from_yaml(agent["actor_template"], config->actor_templates);
    }
    if (agent["entity_template"]) {
        config->agent.actor_template_id =
            entity_template_ref_from_yaml(
                agent["entity_template"], config->entity_templates);
    }
}

}  // namespace

std::uint64_t compute_gameplay_catalog_hash(const WeaponCatalogConfig& weapons) {
    std::uint64_t hash = kFnvOffsetBasis;
    hash_scalar(&hash, weapons.catalog_version);
    for (std::size_t index = 0; index < weapons.definitions.size(); ++index) {
        if (!weapons.configured[index]) {
            continue;
        }
        const auto canonical_index = static_cast<std::uint32_t>(index);
        hash_scalar(&hash, canonical_index);
        hash_string(&hash, weapons.names[index]);
        hash_scalar(&hash, weapons.projectile_sync_modes[index]);
        hash_scalar(&hash, weapons.collider_template_ids[index]);
        hash_weapon(&hash, weapons.definitions[index]);
    }
    return hash == 0 ? kFnvOffsetBasis : hash;
}

std::uint64_t compute_gameplay_catalog_hash(
    const GameServerGameplayConfig& config) {
    std::uint64_t hash = compute_gameplay_catalog_hash(config.weapons);
    std::vector<PropPopulationRuleConfig> prop_population_rules =
        config.prop_population_rules;
    std::sort(
        prop_population_rules.begin(),
        prop_population_rules.end(),
        [](const PropPopulationRuleConfig& lhs,
           const PropPopulationRuleConfig& rhs) {
            return lhs.definition.population_group_id <
                rhs.definition.population_group_id;
        });
    for (const PropPopulationRuleConfig& rule : prop_population_rules) {
        hash_string(&hash, rule.name);
        hash_scalar(&hash, rule.definition.population_group_id);
        hash_scalar(&hash, rule.definition.max_alive);
    }
    std::vector<ActionGraphTemplateConfig> action_graph_templates =
        config.action_graph_templates;
    std::sort(
        action_graph_templates.begin(),
        action_graph_templates.end(),
        [](const ActionGraphTemplateConfig& lhs,
           const ActionGraphTemplateConfig& rhs) {
            return lhs.id < rhs.id;
        });
    for (const ActionGraphTemplateConfig& graph : action_graph_templates) {
        hash_string(&hash, graph.id);
        for (const ActionGraphParameterConfig& parameter : graph.parameters) {
            hash_string(&hash, parameter.name);
            hash_scalar(&hash, parameter.has_default);
            hash_string(&hash, parameter.default_value);
            hash_scalar(&hash, parameter.default_vec3.has_value());
            if (parameter.default_vec3.has_value()) {
                hash_scalar(&hash, parameter.default_vec3->x);
                hash_scalar(&hash, parameter.default_vec3->y);
                hash_scalar(&hash, parameter.default_vec3->z);
            }
        }
        for (const ActionGraphActionConfig& action : graph.actions) {
            hash_string(&hash, action.action_type);
            hash_string(&hash, action.projectile_template_parameter);
            hash_string(&hash, action.entity_template_parameter);
            hash_string(&hash, action.position_parameter);
            hash_string(&hash, action.direction_parameter);
            hash_string(&hash, action.owner_parameter);
            hash_string(&hash, action.target_parameter);
            hash_string(&hash, action.amount_parameter);
            hash_string(&hash, action.strength_parameter);
            hash_string(&hash, action.status_parameter);
            hash_string(&hash, action.operation_parameter);
            hash_string(&hash, action.value_parameter);
            hash_scalar(&hash, action.collision_mask);
            hash_string(&hash, action.item_template_ref);
            hash_scalar(&hash, action.quantity);
            hash_scalar(&hash, action.condition_type);
        }
    }
    std::vector<StatusEffectTemplateConfig> status_effect_templates =
        config.status_effect_templates;
    std::sort(
        status_effect_templates.begin(), status_effect_templates.end(),
        [](const StatusEffectTemplateConfig& lhs,
           const StatusEffectTemplateConfig& rhs) {
            return lhs.status_effect_id < rhs.status_effect_id;
        });
    for (const StatusEffectTemplateConfig& status : status_effect_templates) {
        hash_scalar(&hash, status.status_effect_id);
        hash_string(&hash, status.name);
        hash_scalar(&hash, status.channel_id);
        hash_string(&hash, status.channel_name);
        hash_scalar(&hash, status.duration_ticks);
        hash_scalar(&hash, status.interval_ticks);
        hash_scalar(&hash, status.replacement_policy);
        hash_scalar(&hash, status.max_stacks);
        hash_scalar(&hash, status.refresh_on_stack);
        const auto hash_trigger = [&](const TriggerBindingConfig& trigger) {
            hash_string(&hash, trigger.action_graph_ref);
            for (const auto& parameter : trigger.parameters) {
                hash_string(&hash, parameter.first);
                hash_string(&hash, parameter.second);
            }
        };
        hash_trigger(status.on_apply_trigger);
        hash_trigger(status.on_tick_trigger);
        hash_trigger(status.on_expire_trigger);
    }
    std::vector<ActionTemplateConfig> action_templates = config.action_templates;
    std::sort(
        action_templates.begin(),
        action_templates.end(),
        [](const ActionTemplateConfig& lhs, const ActionTemplateConfig& rhs) {
            return lhs.definition.action_template_id <
                   rhs.definition.action_template_id;
        });
    for (const ActionTemplateConfig& action_template : action_templates) {
        hash_string(&hash, action_template.name);
        hash_action_template(&hash, action_template.definition);
    }
    std::vector<ItemTemplateConfig> item_templates = config.item_templates;
    std::sort(
        item_templates.begin(),
        item_templates.end(),
        [](const ItemTemplateConfig& lhs, const ItemTemplateConfig& rhs) {
            return lhs.definition.item_template_id <
                rhs.definition.item_template_id;
        });
    for (const ItemTemplateConfig& item : item_templates) {
        const KernelItemTemplateDefinition& definition = item.definition;
        hash_string(&hash, item.name);
        hash_scalar(&hash, definition.item_template_id);
        hash_scalar(&hash, definition.item_mode);
        hash_scalar(&hash, definition.max_stack);
        hash_scalar(&hash, definition.capability_flags);
        hash_scalar(&hash, definition.entity_template_id);
        hash_float(&hash, definition.interaction_range);
        hash_scalar(&hash, definition.line_of_sight_required);
        hash_scalar(&hash, definition.line_of_sight_blocking_mask);
        hash_scalar(&hash, definition.throw_policy.mode);
        hash_scalar(
            &hash,
            definition.throw_policy.trajectory_projectile_template_id);
        hash_scalar(&hash, definition.use_policy.quantity_cost);
        hash_scalar(&hash, definition.use_policy.charge_field_id);
        hash_scalar(&hash, definition.use_policy.cooldown_ticks);
        hash_scalar(&hash, definition.use_policy.destroy_when_empty);
        hash_scalar(&hash, definition.portable_state_field_count);
        for (std::uint32_t index = 0;
             index < definition.portable_state_field_count;
             ++index) {
            const KernelPortableStateFieldDefinition& field =
                definition.portable_state_fields[index];
            hash_scalar(&hash, field.field_id);
            hash_scalar(&hash, field.type);
            hash_scalar(&hash, field.world_projection);
            hash_scalar(&hash, field.uint32_default);
            hash_float(&hash, field.float_default);
            hash_scalar(&hash, field.bool_default);
        }
        hash_string(&hash, item.item_used_trigger.action_graph_ref);
        for (const auto& parameter : item.item_used_trigger.parameters) {
            hash_string(&hash, parameter.first);
            hash_string(&hash, parameter.second);
        }
    }
    hash_scalar(&hash, config.player.actor_template_id);
    hash_scalar(&hash, config.agent.actor_template_id);
    hash_vec3(&hash, config.agent.spawn_position);
    hash_scalar(&hash, config.agent.spawn_count);
    hash_float(&hash, config.agent.spawn_radius);
    hash_scalar(&hash, config.agent.spawn_seed);
    hash_scalar(&hash, config.agent.override_director_spawn);
    hash_scalar(
        &hash,
        static_cast<std::uint32_t>(
            config.preload_director_template_ids.size()));
    for (const std::uint32_t template_id :
         config.preload_director_template_ids) {
        hash_scalar(&hash, template_id);
    }
    hash_string(&hash, config.static_collision_scene.entry_path);
    hash_scalar(&hash, config.static_collision_scene.scene_id);
    hash_scalar(&hash, config.static_collision_scene.collider_id);
    hash_scalar(&hash, config.static_collision_scene.collision_layer);
    std::vector<SkeletonAssetConfig> skeleton_assets = config.skeleton_assets;
    std::sort(
        skeleton_assets.begin(),
        skeleton_assets.end(),
        [](const SkeletonAssetConfig& lhs, const SkeletonAssetConfig& rhs) {
            return lhs.skeleton_asset_id < rhs.skeleton_asset_id;
        });
    for (const SkeletonAssetConfig& asset : skeleton_assets) {
        hash_scalar(&hash, asset.skeleton_asset_id);
        hash_scalar(&hash, asset.content_hash);
        hash_scalar(
            &hash,
            static_cast<std::uint32_t>(asset.bones.size()));
        // The authored .rig.yaml is not covered by content_hash, which is the
        // .ozz bytes alone, so it has to be folded in here or a rig edit would
        // reuse a cached bundle.
        hash_string(&hash, asset.forward_axis);
        hash_string(&hash, asset.root_bone);
        hash_string(&hash, asset.body_bone);
        hash_vec3(&hash, asset.knee_hinge_local);
        hash_scalar(&hash, static_cast<std::uint32_t>(asset.legs.size()));
        for (const RigLegConfig& leg : asset.legs) {
            hash_string(&hash, leg.id);
            hash_scalar(&hash, leg.hip_bone_index);
            hash_scalar(&hash, leg.knee_bone_index);
            hash_scalar(&hash, leg.foot_bone_index);
            hash_vec3(&hash, leg.pole_local);
        }
        hash_scalar(
            &hash,
            static_cast<std::uint32_t>(asset.colliders.size()));
        for (const RigColliderConfig& collider : asset.colliders) {
            hash_string(&hash, collider.bone);
            hash_string(&hash, collider.leg_id);
            hash_scalar(&hash, collider.bone_index);
            hash_scalar(&hash, collider.shape_type);
            hash_scalar(&hash, collider.hit_zone);
            hash_scalar(&hash, collider.has_hit_zone ? 1u : 0u);
        }
    }
    std::vector<ActorTemplateConfig> actor_templates = config.actor_templates;
    std::sort(
        actor_templates.begin(),
        actor_templates.end(),
        [](const ActorTemplateConfig& lhs, const ActorTemplateConfig& rhs) {
            return lhs.actor_template_id < rhs.actor_template_id;
        });
    for (const ActorTemplateConfig& actor_template : actor_templates) {
        hash_actor_template(&hash, actor_template);
    }
    std::vector<EntityTemplateConfig> entity_templates = config.entity_templates;
    std::sort(
        entity_templates.begin(),
        entity_templates.end(),
        [](const EntityTemplateConfig& lhs, const EntityTemplateConfig& rhs) {
            return lhs.actor_template_id < rhs.actor_template_id;
        });
    for (const EntityTemplateConfig& entity_template : entity_templates) {
        hash_actor_template(&hash, entity_template);
    }
    std::vector<ColliderTemplateConfig> templates = config.colliders.templates;
    std::sort(
        templates.begin(),
        templates.end(),
        [](const ColliderTemplateConfig& lhs, const ColliderTemplateConfig& rhs) {
            return lhs.definition.template_id < rhs.definition.template_id;
        });
    for (const ColliderTemplateConfig& collider_template : templates) {
        hash_collider_template(&hash, collider_template);
    }
    std::vector<ProjectileTemplateConfig> projectile_templates =
        config.projectile_templates;
    std::sort(
        projectile_templates.begin(),
        projectile_templates.end(),
        [](const ProjectileTemplateConfig& lhs,
           const ProjectileTemplateConfig& rhs) {
            return lhs.definition.projectile_template_id <
                   rhs.definition.projectile_template_id;
        });
    for (const ProjectileTemplateConfig& projectile_template : projectile_templates) {
        hash_projectile_template(&hash, projectile_template);
    }
    return hash == 0 ? kFnvOffsetBasis : hash;
}

GameServerGameplayConfig default_game_server_gameplay_config() {
    return load_gameplay_config_from_catalog_file(kDefaultGameplayCatalogPath);
}

GameServerGameplayConfig load_gameplay_config_from_weapon_template_directory(
    const std::string& directory) {
    const FilesystemGameplayConfigSource source;
    return load_gameplay_config_from_weapon_template_source(source, directory);
}

GameServerGameplayConfig load_gameplay_config_from_catalog_file(
    const std::string& path) {
    const FilesystemGameplayConfigSource source;
    return load_gameplay_config_from_catalog_source(source, path);
}

GameServerGameplayConfig load_gameplay_config_from_bundle_memory(
    const std::uint8_t* bundle_bytes,
    std::uint32_t bundle_size,
    const std::string& entry_path) {
    const MemoryZipGameplayConfigSource source(bundle_bytes, bundle_size);
    return load_gameplay_config_from_catalog_source(source, entry_path);
}

std::vector<std::uint8_t> load_gameplay_bundle_entry_bytes(
    const std::uint8_t* bundle_bytes,
    std::uint32_t bundle_size,
    const std::string& entry_path) {
    if (bundle_bytes == nullptr || bundle_size == 0 || entry_path.empty()) {
        throw std::runtime_error("gameplay bundle entry request is empty");
    }
    const std::string normalized_entry = normalize_archive_path(entry_path);
    struct ZipStreamCloser {
        void operator()(struct zip_t* zip) const { zip_stream_close(zip); }
    };
    std::unique_ptr<struct zip_t, ZipStreamCloser> archive(zip_stream_open(
        reinterpret_cast<const char*>(bundle_bytes),
        bundle_size,
        0,
        'r'));
    if (!archive) {
        throw std::runtime_error("failed to open gameplay catalog bundle");
    }
    const ssize_t total_entries = zip_entries_total(archive.get());
    for (ssize_t index = 0; index < total_entries; ++index) {
        if (zip_entry_openbyindex(
                archive.get(), static_cast<std::size_t>(index)) != 0) {
            throw std::runtime_error("failed to open gameplay bundle entry");
        }
        const char* entry_name = zip_entry_name(archive.get());
        const bool matches = entry_name != nullptr &&
            normalize_archive_path(entry_name) == normalized_entry;
        if (!matches) {
            zip_entry_close(archive.get());
            continue;
        }
        if (zip_entry_issymlink(archive.get()) || zip_entry_isdir(archive.get())) {
            throw std::runtime_error(
                "gameplay bundle collision entry is not a regular file: " +
                normalized_entry);
        }
        const unsigned long long entry_size = zip_entry_size(archive.get());
        if (entry_size > KERNEL_STATIC_COLLISION_SCENE_MAX_BYTES) {
            throw std::runtime_error(
                "gameplay bundle collision entry exceeds size limit: " +
                normalized_entry);
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(entry_size));
        if (!bytes.empty()) {
            const ssize_t read_size = zip_entry_noallocread(
                archive.get(), bytes.data(), bytes.size());
            if (read_size != static_cast<ssize_t>(bytes.size())) {
                throw std::runtime_error(
                    "failed to read gameplay bundle entry: " + normalized_entry);
            }
        }
        zip_entry_close(archive.get());
        return bytes;
    }
    throw DataLoadError(
        KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_MISSING_BUNDLE_ENTRY,
        "missing collision asset in bundle: " + normalized_entry,
        normalized_entry,
        {},
        KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE);
}

const ActorTemplateConfig* find_actor_template(
    const GameServerGameplayConfig& config,
    std::uint32_t actor_template_id) {
    for (const ActorTemplateConfig& actor_template : config.actor_templates) {
        if (actor_template.actor_template_id == actor_template_id) {
            return &actor_template;
        }
    }
    return nullptr;
}

std::uint8_t active_weapon_id(const ActorTemplateConfig& actor_template) {
    if (actor_template.weapon_slot_count == 0 ||
        actor_template.active_weapon_slot >= actor_template.weapon_slot_count) {
        return 0;
    }
    return static_cast<std::uint8_t>(
        actor_template.weapon_ids[actor_template.active_weapon_slot]);
}

std::vector<std::string> validate_gameplay_config(
    const GameServerGameplayConfig& config) {
    std::vector<std::string> errors;
    std::vector<std::uint32_t> prop_population_rule_ids;
    std::vector<std::string> prop_population_rule_names;
    for (const PropPopulationRuleConfig& rule :
         config.prop_population_rules) {
        if (rule.definition.struct_size <
                sizeof(KernelPropPopulationRuleDefinition) ||
            rule.definition.population_group_id == 0u ||
            rule.definition.max_alive == 0u ||
            rule.definition.max_alive > 256u || rule.name.empty() ||
            std::find(
                prop_population_rule_ids.begin(),
                prop_population_rule_ids.end(),
                rule.definition.population_group_id) !=
                prop_population_rule_ids.end() ||
            std::find(
                prop_population_rule_names.begin(),
                prop_population_rule_names.end(),
                rule.name) != prop_population_rule_names.end()) {
            errors.push_back("prop population rule must be valid and unique");
        }
        prop_population_rule_ids.push_back(
            rule.definition.population_group_id);
        prop_population_rule_names.push_back(rule.name);
    }
    for (const EntityTemplateConfig& entity_template :
         config.entity_templates) {
        if (!std::isfinite(entity_template.impulse_resistance) ||
            entity_template.impulse_resistance < 0.0f) {
            errors.push_back("impulse_resistance must be finite and non-negative");
        }
        if (entity_template.entity_type != KernelEntityType_Prop &&
            (entity_template.prop.lifetime_ticks != 0u ||
             entity_template.prop.population_group_id != 0u)) {
            errors.push_back("only prop templates may declare lifecycle");
        }
        if (entity_template.prop.population_group_id != 0u &&
            std::find(
                prop_population_rule_ids.begin(),
                prop_population_rule_ids.end(),
                entity_template.prop.population_group_id) ==
                prop_population_rule_ids.end()) {
            errors.push_back(
                "prop lifecycle must reference a valid population group");
        }
    }
    for (const ItemTemplateConfig& item : config.item_templates) {
        if (item.definition.entity_template_id == 0u) {
            continue;
        }
        const auto entity_template = std::find_if(
            config.entity_templates.begin(),
            config.entity_templates.end(),
            [&](const EntityTemplateConfig& candidate) {
                return candidate.actor_template_id ==
                    item.definition.entity_template_id;
            });
        if (entity_template != config.entity_templates.end() &&
            (entity_template->prop.lifetime_ticks != 0u ||
             entity_template->prop.population_group_id != 0u)) {
            errors.push_back(
                "item-backed prop must not declare lifecycle or population");
        }
    }
    const StaticCollisionSceneConfig& static_scene =
        config.static_collision_scene;
    const bool has_static_scene = !static_scene.entry_path.empty() ||
        static_scene.scene_id != 0u || static_scene.collider_id != 0u ||
        static_scene.collision_layer != 0u;
    if (has_static_scene &&
        (static_scene.entry_path.empty() || static_scene.scene_id == 0u ||
         static_scene.collider_id == 0u ||
         static_scene.collision_layer != KERNEL_STATIC_COLLISION_LAYER_TERRAIN)) {
        errors.push_back("static collision scene must be valid");
    }
    std::vector<std::uint32_t> action_template_ids;
    std::vector<std::string> action_template_names;
    for (const ActionTemplateConfig& action_template : config.action_templates) {
        if (action_template.name.empty() ||
            !valid_action_template_definition(action_template.definition)) {
            errors.push_back("action template must be valid");
        }
        if (std::find(
                action_template_ids.begin(),
                action_template_ids.end(),
                action_template.definition.action_template_id) !=
            action_template_ids.end()) {
            errors.push_back("action template id must be unique");
        }
        if (std::find(
                action_template_names.begin(),
                action_template_names.end(),
                action_template.name) != action_template_names.end()) {
            errors.push_back("action template name must be unique");
        }
        action_template_ids.push_back(action_template.definition.action_template_id);
        action_template_names.push_back(action_template.name);
    }
    for (std::size_t index = 0; index < config.weapons.definitions.size(); ++index) {
        if (!config.weapons.configured[index]) {
            continue;
        }
        const KernelWeaponMechanicsDefinition& weapon =
            config.weapons.definitions[index];
        if (weapon.weapon_id != index) {
            errors.push_back("weapon id must match catalog index");
        }
        if (!validate_weapon_mechanics(weapon)) {
            errors.push_back("weapon mechanics must be valid");
        }
        if (weapon.fire_action_template_id != 0u &&
            std::find(
                action_template_ids.begin(),
                action_template_ids.end(),
                weapon.fire_action_template_id) == action_template_ids.end()) {
            errors.push_back("weapon action template reference must be valid");
        }
        if (weapon.reload_action_template_id == 0u ||
            std::find(
                action_template_ids.begin(),
                action_template_ids.end(),
                weapon.reload_action_template_id) == action_template_ids.end()) {
            errors.push_back("weapon reload action template reference must be valid");
        }
        if (config.weapons.projectile_sync_modes[index] >
            KernelProjectileSyncMode_ServerSnapshotOnly) {
            errors.push_back("projectile sync mode must be valid");
        }
        if (config.weapons.collider_template_ids[index] == 0) {
            errors.push_back("weapon collider template binding must be valid");
        }
    }
    if (config.actor_templates.empty()) {
        errors.push_back("actor templates must not be empty");
    }
    std::vector<std::uint32_t> actor_template_ids;
    std::vector<std::string> actor_template_names;
    for (const ActorTemplateConfig& actor_template : config.actor_templates) {
        if (actor_template.actor_template_id == 0 || actor_template.name.empty() ||
            actor_template.entity_type != kEntityTypeActor ||
            (actor_template.actor_type != kActorTypePlayer &&
             actor_template.actor_type != kActorTypeAgent) ||
            actor_template.collider_template_id == 0 ||
            actor_template.health.hp == 0 ||
            actor_template.health.max_hp == 0 ||
            actor_template.health.hp > actor_template.health.max_hp ||
            actor_template.move_speed_meters_per_second <= 0.0f ||
            actor_template.movement_controller_type >
                KernelMovementControllerType_Character ||
            actor_template.movement_controller_type ==
                KernelMovementControllerType_None ||
            actor_template.movement_collider_template_id == 0u ||
            actor_template.movement_max_slope_degrees <= 0.0f ||
            actor_template.movement_max_slope_degrees >= 90.0f ||
            actor_template.movement_step_height < 0.0f ||
            actor_template.movement_ground_probe_distance <= 0.0f ||
            actor_template.movement_ground_snap_distance < 0.0f ||
            (actor_template.skeleton.enabled &&
             (!std::isfinite(
                  actor_template.movement_max_yaw_degrees_per_second) ||
              actor_template.movement_max_yaw_degrees_per_second <= 0.0f)) ||
            actor_template.hitbox_half_extents.x <= 0.0f ||
            actor_template.hitbox_half_extents.y <= 0.0f ||
            actor_template.hitbox_half_extents.z <= 0.0f ||
            actor_template.weapon_slot_count == 0 ||
            actor_template.weapon_slot_count > actor_template.weapon_ids.size() ||
            actor_template.active_weapon_slot >= actor_template.weapon_slot_count) {
            errors.push_back("actor template must be valid");
        }
        for (std::uint8_t slot = 0; slot < actor_template.weapon_slot_count; ++slot) {
            const std::uint32_t weapon_id = actor_template.weapon_ids[slot];
            if (weapon_id > UINT8_MAX ||
                !config.weapons.configured[weapon_id]) {
                errors.push_back("actor template weapon slot must reference a valid weapon");
            }
        }
        const bool has_inventory = actor_template.inventory_slot_capacity != 0 ||
            !actor_template.inventory_slots.empty();
        if (has_inventory && actor_template.actor_type != kActorTypePlayer) {
            errors.push_back("only player actor templates may configure inventory");
        }
        if (has_inventory &&
            (actor_template.inventory_slot_capacity == 0 ||
             actor_template.inventory_slots.size() >
                 actor_template.inventory_slot_capacity)) {
            errors.push_back("actor template inventory capacity must be valid");
        }
        for (const InventorySlotConfig& slot : actor_template.inventory_slots) {
            const auto item = std::find_if(
                config.item_templates.begin(),
                config.item_templates.end(),
                [&slot](const ItemTemplateConfig& candidate) {
                    return candidate.definition.item_template_id ==
                        slot.item_template_id;
                });
            if (item == config.item_templates.end()) {
                errors.push_back(
                    "actor template inventory slot must reference a valid item");
                continue;
            }
            if (slot.quantity == 0 ||
                slot.quantity > item->definition.max_stack ||
                (item->definition.item_mode == KernelItemMode_Stateful &&
                 slot.quantity != 1)) {
                errors.push_back("actor template inventory quantity must be valid");
            }
        }
        if (actor_template.actor_type == kActorTypeAgent &&
            (actor_template.sentry.weapon_id > UINT8_MAX ||
             !config.weapons.configured[actor_template.sentry.weapon_id] ||
             !actor_template_has_weapon(
                 actor_template,
                 static_cast<std::uint8_t>(
                     actor_template.sentry.weapon_id)) ||
             actor_template.sentry.alert_ticks == 0 ||
             actor_template.sentry.forget_ticks == 0 ||
             actor_template.sentry.ballistic_retry_cooldown_ticks == 0 ||
             actor_template.sentry.patrol_rotation_interval_ticks == 0 ||
             actor_template.sentry.patrol_rotation_min_degrees <= 0.0f ||
             actor_template.sentry.patrol_rotation_max_degrees <
                 actor_template.sentry.patrol_rotation_min_degrees ||
             !std::isfinite(actor_template.sentry.patrol_extent_x_meters) ||
             !std::isfinite(actor_template.sentry.patrol_input_magnitude) ||
             (actor_template.sentry.passive_patrol &&
              (actor_template.sentry.patrol_extent_x_meters <= 0.0f ||
               actor_template.sentry.patrol_input_magnitude <= 0.0f ||
               actor_template.sentry.patrol_input_magnitude > 1.0f ||
               !std::isfinite(
                   actor_template.sentry.move_speed_meters_per_second) ||
               actor_template.sentry.move_speed_meters_per_second <= 0.0f)) ||
             (!actor_template.sentry.passive_patrol &&
              (actor_template.sentry.patrol_extent_x_meters != 0.0f ||
               actor_template.sentry.patrol_input_magnitude != 0.0f)))) {
            errors.push_back("agent sentry actor template must be valid");
        }
        if (actor_template.actor_type == kActorTypeAgent &&
            actor_template.ai_controller_type == KernelAiControllerType_Chaser &&
            (!std::isfinite(actor_template.chaser.stop_distance_meters) ||
             !std::isfinite(actor_template.chaser.resume_distance_meters) ||
             !std::isfinite(actor_template.chaser.input_magnitude) ||
             actor_template.chaser.stop_distance_meters < 0.0f ||
             // Equal thresholds make the agent start and stop on alternating
             // ticks once it arrives.
             actor_template.chaser.resume_distance_meters <=
                 actor_template.chaser.stop_distance_meters ||
             actor_template.chaser.input_magnitude <= 0.0f ||
             actor_template.chaser.input_magnitude > 1.0f ||
             actor_template.sentry.passive_patrol ||
             !std::isfinite(actor_template.sentry.move_speed_meters_per_second) ||
             actor_template.sentry.move_speed_meters_per_second <= 0.0f)) {
            errors.push_back("agent chaser actor template must be valid");
        }
        if (actor_template.vision.struct_size < sizeof(KernelAgentVisionConfig) ||
            actor_template.vision.camp > KernelAgentCamp_Neutral) {
            errors.push_back("actor template vision must be valid");
        }
        if (std::find(
                actor_template_ids.begin(),
                actor_template_ids.end(),
                actor_template.actor_template_id) != actor_template_ids.end()) {
            errors.push_back("actor template id must be unique");
        }
        if (std::find(
                actor_template_names.begin(),
                actor_template_names.end(),
                actor_template.name) != actor_template_names.end()) {
            errors.push_back("actor template name must be unique");
        }
        actor_template_ids.push_back(actor_template.actor_template_id);
        actor_template_names.push_back(actor_template.name);
    }
    const ActorTemplateConfig* player_actor =
        find_actor_template(config, config.player.actor_template_id);
    if (player_actor == nullptr || player_actor->entity_type != kEntityTypeActor ||
        player_actor->actor_type != kActorTypePlayer) {
        errors.push_back("player actor template must reference a player actor");
    }
    const ActorTemplateConfig* agent_actor =
        find_actor_template(config, config.agent.actor_template_id);
    if (agent_actor == nullptr || agent_actor->entity_type != kEntityTypeActor ||
        agent_actor->actor_type != kActorTypeAgent ||
        config.agent.spawn_count == 0 || config.agent.spawn_radius < 0.0f) {
        errors.push_back("agent gameplay config must be valid");
    }
    if (config.colliders.templates.empty() || !config.colliders.bindings.empty()) {
        errors.push_back("collider catalog must include templates and no bindings");
    }
    std::vector<std::uint32_t> collider_template_ids;
    for (const ColliderTemplateConfig& collider_template :
         config.colliders.templates) {
        const KernelColliderTemplateDefinition& definition =
            collider_template.definition;
        if (definition.struct_size < sizeof(KernelColliderTemplateDefinition) ||
            definition.template_id == 0 ||
            definition.shape_type > KernelColliderShapeType_Capsule ||
            definition.purpose_flags == 0 ||
            definition.layer_mask == 0 ||
            (definition.shape_type == KernelColliderShapeType_Aabb &&
             (definition.shape_params.x <= 0.0f ||
              definition.shape_params.y <= 0.0f ||
              definition.shape_params.z <= 0.0f)) ||
            (definition.shape_type == KernelColliderShapeType_OrientedBox &&
             (definition.shape_params.x <= 0.0f ||
              definition.shape_params.y <= 0.0f ||
              definition.shape_params.z <= 0.0f)) ||
            (definition.shape_type == KernelColliderShapeType_Sphere &&
             definition.shape_params.x <= 0.0f) ||
            // A segment carries only an optional thickness: its reach and the
            // lifetime of the instance built from it are decided at fire time,
            // not authored here.
            (definition.shape_type == KernelColliderShapeType_Segment &&
             definition.shape_params.y < 0.0f) ||
            (definition.shape_type == KernelColliderShapeType_Cone &&
             ((definition.purpose_flags & KernelColliderPurpose_Vision) == 0u ||
              definition.shape_params.x <= 0.0f ||
              definition.shape_params.y <= 0.0f ||
              definition.shape_params.y > 360.0f))) {
            errors.push_back("collider template must be valid");
        } else if (definition.shape_type == KernelColliderShapeType_Capsule &&
                   (definition.shape_params.x <= 0.0f ||
                    definition.shape_params.y <= 0.0f ||
                    definition.lifetime_ticks != 0u ||
                    (definition.purpose_flags &
                     KernelColliderPurpose_Movement) == 0u)) {
            errors.push_back("collider template must be valid");
        }
        collider_template_ids.push_back(definition.template_id);
    }
    for (const ActorTemplateConfig& actor_template : config.actor_templates) {
        if (std::find(
                collider_template_ids.begin(),
                collider_template_ids.end(),
                actor_template.collider_template_id) == collider_template_ids.end()) {
            errors.push_back("actor template collider must reference a valid template");
        }
        const auto movement_collider = std::find_if(
            config.colliders.templates.begin(),
            config.colliders.templates.end(),
            [&](const ColliderTemplateConfig& collider_template) {
                return collider_template.definition.template_id ==
                    actor_template.movement_collider_template_id;
            });
        if (movement_collider == config.colliders.templates.end() ||
            movement_collider->definition.shape_type !=
                KernelColliderShapeType_Capsule ||
            (movement_collider->definition.purpose_flags &
             KernelColliderPurpose_Movement) == 0u ||
            movement_collider->definition.lifetime_ticks != 0u) {
            errors.push_back(
                "actor movement must reference a persistent capsule movement collider");
        }
        if (actor_template.vision.vision_collider_template_id == 0u) {
            continue;
        }
        const auto vision_collider = std::find_if(
            config.colliders.templates.begin(),
            config.colliders.templates.end(),
            [&](const ColliderTemplateConfig& collider_template) {
                return collider_template.definition.template_id ==
                    actor_template.vision.vision_collider_template_id;
            });
        if (vision_collider == config.colliders.templates.end() ||
            vision_collider->definition.shape_type != KernelColliderShapeType_Cone ||
            (vision_collider->definition.purpose_flags & KernelColliderPurpose_Vision) == 0u) {
            errors.push_back(
                "actor template vision must reference a cone vision collider");
        }
    }
    if (config.projectile_templates.empty()) {
        errors.push_back("projectile templates must not be empty");
    }
    std::vector<std::uint32_t> projectile_template_ids;
    std::vector<std::string> projectile_template_names;
    for (const ProjectileTemplateConfig& projectile_template :
         config.projectile_templates) {
        const KernelProjectileTemplateDefinition& definition =
            projectile_template.definition;
        const KernelProjectileMechanicsDefinition& mechanics =
            definition.mechanics;
        if (definition.struct_size < sizeof(KernelProjectileTemplateDefinition) ||
            definition.projectile_template_id == 0 ||
            projectile_template.name.empty() ||
            mechanics.struct_size < sizeof(KernelProjectileMechanicsDefinition) ||
            mechanics.projectile_type > KernelProjectileType_Beam ||
            mechanics.motion_model > KernelProjectileMotionModel_Homing ||
            mechanics.sync_mode > KernelProjectileSyncMode_ServerSnapshotOnly ||
            mechanics.hit_response > KernelProjectileHitResponse_Attach ||
            mechanics.hit_response == KernelProjectileHitResponse_Bounce ||
            mechanics.hit_response == KernelProjectileHitResponse_Attach ||
            (mechanics.damage_shape != KernelProjectileDamageShape_DirectHit &&
             mechanics.damage_shape != KernelProjectileDamageShape_None &&
             mechanics.damage_shape != KernelProjectileDamageShape_PiercingSegment) ||
            mechanics.damage_falloff > KernelProjectileDamageFalloff_Linear ||
            mechanics.collision_query_mode > KernelProjectileCollisionQueryMode_Ray ||
            (mechanics.damage_shape == KernelProjectileDamageShape_None
                 ? mechanics.damage != 0
                 : mechanics.damage == 0) ||
            std::find(
                collider_template_ids.begin(),
                collider_template_ids.end(),
                mechanics.collider_template_id) == collider_template_ids.end() ||
            (mechanics.projectile_type == KernelProjectileType_Standard &&
             (mechanics.speed <= 0.0f ||
              mechanics.lifetime_ticks == 0 ||
              mechanics.max_hit_count == 0)) ||
            (mechanics.projectile_type == KernelProjectileType_AreaEffect &&
             (mechanics.area_effect.struct_size <
                  sizeof(KernelAreaEffectMechanicsDefinition) ||
              mechanics.area_effect.radius <= 0.0f ||
              mechanics.area_effect.damage_per_interval == 0 ||
              mechanics.area_effect.damage_interval_ticks == 0 ||
              mechanics.area_effect.lifetime_ticks == 0)) ||
            (mechanics.projectile_type == KernelProjectileType_Beam &&
             (mechanics.beam.struct_size < sizeof(KernelBeamMechanicsDefinition) ||
              mechanics.beam.length <= 0.0f ||
              mechanics.beam.radius <= 0.0f ||
              mechanics.beam.damage_per_tick == 0 ||
              mechanics.beam.lifetime_ticks == 0)) ||
            (mechanics.motion_model != KernelProjectileMotionModel_Homing
                 ? mechanics.homing.struct_size != 0
                 : mechanics.homing.struct_size <
                           sizeof(KernelHomingMechanicsDefinition) ||
                       mechanics.homing.homing_mode !=
                           KernelHomingMode_FireAndForget ||
                       mechanics.homing.sync_mode >
                           KernelProjectileSyncMode_ServerSnapshotOnly ||
                       mechanics.homing.lock_on_range <= 0.0f ||
                       mechanics.homing.lose_target_range <
                           mechanics.homing.lock_on_range ||
                       mechanics.homing.lock_cone_degrees <= 0.0f ||
                       mechanics.homing.lock_cone_degrees > 180.0f ||
                       mechanics.homing
                               .max_turn_degrees_per_tick <= 0.0f ||
                       mechanics.homing.acceleration <= 0.0f ||
                       mechanics.homing.max_speed <= 0.0f)) {
            errors.push_back("projectile template must be valid");
        }
        const auto projectile_collider = std::find_if(
            config.colliders.templates.begin(),
            config.colliders.templates.end(),
            [&](const ColliderTemplateConfig& collider_template) {
                return collider_template.definition.template_id ==
                    mechanics.collider_template_id;
            });
        if (projectile_collider != config.colliders.templates.end() &&
            projectile_collider->definition.shape_type == KernelColliderShapeType_Cone) {
            errors.push_back("projectile template must not reference cone collider");
        }
        if (std::find(
                projectile_template_ids.begin(),
                projectile_template_ids.end(),
                definition.projectile_template_id) !=
            projectile_template_ids.end()) {
            errors.push_back("projectile template id must be unique");
        }
        if (std::find(
                projectile_template_names.begin(),
                projectile_template_names.end(),
                projectile_template.name) != projectile_template_names.end()) {
            errors.push_back("projectile template name must be unique");
        }
        projectile_template_ids.push_back(definition.projectile_template_id);
        projectile_template_names.push_back(projectile_template.name);
    }
    for (std::size_t id = 0; id < config.weapons.definitions.size(); ++id) {
        if (!config.weapons.configured[id]) {
            continue;
        }
        const KernelWeaponMechanicsDefinition& weapon =
            config.weapons.definitions[id];
        if (weapon.fire_mode == KernelWeaponFireMode_Projectile &&
            std::find(
                projectile_template_ids.begin(),
                projectile_template_ids.end(),
                weapon.projectile_template_id) ==
                projectile_template_ids.end()) {
            errors.push_back(
                "projectile weapon must reference a valid projectile template");
        }
    }
    return errors;
}

KernelCombatStateDefinition make_combat_state_from_actor_template(
    const GameServerGameplayConfig& config,
    const ActorTemplateConfig& actor_template);

KernelGameplayCatalogStorage build_kernel_gameplay_catalog(
    const GameServerGameplayConfig& config) {
    KernelGameplayCatalogStorage storage;
    storage.skeleton_asset_bytes.reserve(config.skeleton_assets.size());
    storage.skeleton_assets.reserve(config.skeleton_assets.size());
    for (const SkeletonAssetConfig& authored_asset : config.skeleton_assets) {
        storage.skeleton_asset_bytes.push_back(
            authored_asset.runtime_skeleton);
    }
    for (std::size_t index = 0u;
         index < config.skeleton_assets.size();
         ++index) {
        const SkeletonAssetConfig& authored_asset =
            config.skeleton_assets[index];
        const std::vector<std::uint8_t>& bytes =
            storage.skeleton_asset_bytes[index];
        storage.skeleton_assets.push_back(KernelSkeletonAssetDefinition{
            sizeof(KernelSkeletonAssetDefinition),
            authored_asset.skeleton_asset_id,
            authored_asset.content_hash,
            bytes.data(),
            static_cast<std::uint32_t>(bytes.size()),
            static_cast<std::uint32_t>(authored_asset.bones.size()),
        });
    }
    for (const ActorTemplateConfig& actor_template : config.actor_templates) {
        KernelActorTemplateDefinition definition{};
        definition.struct_size = sizeof(KernelActorTemplateDefinition);
        definition.actor_template_id = actor_template.actor_template_id;
        definition.entity_type = actor_template.entity_type;
        definition.actor_type = actor_template.actor_type;
        definition.collider_template_id = actor_template.collider_template_id;
        definition.vision = actor_template.vision;
        definition.vision.struct_size = sizeof(KernelAgentVisionConfig);
        storage.actor_templates.push_back(definition);
    }

    const std::vector<EntityTemplateConfig>& entity_templates =
        config.entity_templates.empty() ? config.actor_templates : config.entity_templates;
    for (const EntityTemplateConfig& authored_template : entity_templates) {
        KernelEntityTemplateDefinition entity_template{};
        entity_template.struct_size = sizeof(KernelEntityTemplateDefinition);
        entity_template.entity_template_id = authored_template.actor_template_id;
        entity_template.entity_type = authored_template.entity_type;
        entity_template.actor_type = authored_template.actor_type;
        entity_template.actor_template_id =
            authored_template.entity_type == kEntityTypeActor
                ? authored_template.actor_template_id
                : 0u;
        entity_template.collider_template_id = authored_template.collider_template_id;
        entity_template.ai.struct_size = sizeof(KernelEntityAiDefinition);
        entity_template.movement.struct_size = sizeof(KernelMovementDefinition);
        entity_template.movement.controller_type =
            authored_template.movement_controller_type;
        entity_template.movement.movement_collider_template_id =
            authored_template.movement_collider_template_id;
        entity_template.movement.gravity = authored_template.movement_gravity;
        entity_template.movement.max_slope_degrees =
            authored_template.movement_max_slope_degrees;
        entity_template.movement.step_height =
            authored_template.movement_step_height;
        entity_template.movement.ground_probe_distance =
            authored_template.movement_ground_probe_distance;
        entity_template.movement.ground_snap_distance =
            authored_template.movement_ground_snap_distance;
        entity_template.movement.max_yaw_degrees_per_second =
            authored_template.movement_max_yaw_degrees_per_second;
        entity_template.movement.movement_collision_mask =
            authored_template.movement_collision_mask;
        entity_template.impulse_resistance =
            authored_template.impulse_resistance;
        entity_template.activated_trigger = compile_action_trigger_binding(
            authored_template.activated_trigger,
            "on_activated",
            config.action_graph_templates,
            config.entity_templates,
            &config.projectile_templates,
            nullptr,
            &config.status_effect_templates);
        entity_template.collision_trigger = compile_action_trigger_binding(
            authored_template.collision_trigger,
            "on_collision",
            config.action_graph_templates,
            config.entity_templates,
            &config.projectile_templates,
            nullptr,
            &config.status_effect_templates);
        entity_template.collision_trigger_mask =
            authored_template.collision_trigger_mask;
        entity_template.health_depleted_trigger = compile_action_trigger_binding(
            authored_template.health_depleted_trigger,
            "on_health_depleted",
            config.action_graph_templates,
            config.entity_templates,
            &config.projectile_templates,
            nullptr,
            &config.status_effect_templates);
        entity_template.destroy_entity_trigger = compile_action_trigger_binding(
            authored_template.destroy_entity_trigger,
            "on_destroy_entity",
            config.action_graph_templates,
            config.entity_templates,
            &config.projectile_templates,
            nullptr,
            &config.status_effect_templates);
        if (authored_template.skeleton.enabled) {
            entity_template.skeleton.struct_size =
                sizeof(KernelSkeletonBindingDefinition);
            entity_template.skeleton.skeleton_asset_id =
                authored_template.skeleton.skeleton_asset_id;
            entity_template.skeleton.skeleton_content_hash =
                authored_template.skeleton.content_hash;
            entity_template.skeleton.bone_count =
                authored_template.skeleton.bone_count;
            entity_template.skeleton.root_bone_index =
                authored_template.skeleton.root_bone_index;
            entity_template.skeleton.body_bone_index =
                authored_template.skeleton.body_bone_index;
            entity_template.skeleton.leg_count =
                static_cast<std::uint32_t>(
                    authored_template.skeleton.legs.size());
            entity_template.skeleton.processing_order_count =
                static_cast<std::uint32_t>(
                    authored_template.skeleton.processing_order.size());
            entity_template.skeleton.input_deadzone =
                authored_template.skeleton.input_deadzone;
            entity_template.skeleton.step_threshold_meters =
                authored_template.skeleton.step_threshold_meters;
            entity_template.skeleton.step_duration_ticks =
                authored_template.skeleton.step_duration_ticks;
            entity_template.skeleton.max_swinging_legs =
                authored_template.skeleton.max_swinging_legs;
            entity_template.skeleton.body_follow_speed =
                authored_template.skeleton.body_follow_speed;
            entity_template.skeleton.slope_alignment =
                authored_template.skeleton.slope_alignment;
            entity_template.skeleton.stance_crouch_meters =
                authored_template.skeleton.stance_crouch_meters;
            entity_template.skeleton.foothold_query_type =
                authored_template.skeleton.foothold_query_type;
            entity_template.skeleton.foothold_query_start_height_meters =
                authored_template.skeleton
                    .foothold_query_start_height_meters;
            entity_template.skeleton.foothold_query_distance_meters =
                authored_template.skeleton.foothold_query_distance_meters;
            entity_template.skeleton.foothold_candidate_count =
                static_cast<std::uint32_t>(
                    authored_template.skeleton
                        .foothold_candidate_offsets.size());
            for (std::uint32_t candidate = 0u;
                 candidate <
                     entity_template.skeleton.foothold_candidate_count;
                 ++candidate) {
                entity_template.skeleton.foothold_candidate_offsets[candidate] =
                    authored_template.skeleton
                        .foothold_candidate_offsets[candidate];
            }
            // Merge point: bones and bend geometry come from the rig, gait
            // grouping and step tuning from the template. The rig fixes the leg
            // order, so leg_index means the same thing on both sides.
            const auto rig_asset = std::find_if(
                config.skeleton_assets.begin(),
                config.skeleton_assets.end(),
                [&authored_template](const SkeletonAssetConfig& candidate) {
                    return candidate.skeleton_asset_id ==
                        authored_template.skeleton.skeleton_asset_id;
                });
            for (std::uint32_t leg_index = 0u;
                 rig_asset != config.skeleton_assets.end() &&
                 leg_index < entity_template.skeleton.leg_count &&
                 leg_index < rig_asset->legs.size();
                 ++leg_index) {
                const SkeletonLegConfig& leg =
                    authored_template.skeleton.legs[leg_index];
                const RigLegConfig& rig_leg = rig_asset->legs[leg_index];
                entity_template.skeleton.legs[leg_index] =
                    KernelSkeletonLegDefinition{
                        leg_index,
                        rig_leg.hip_bone_index,
                        rig_leg.knee_bone_index,
                        rig_leg.foot_bone_index,
                        leg.gait_group,
                        rig_leg.pole_local,
                        // One rig value fanned out: the ABI keeps mid_axis per
                        // leg, but it is read in the knee's own frame and these
                        // legs are copies of one limb.
                        rig_asset->knee_hinge_local,
                        leg.step_height_meters,
                        leg.max_reach_ratio,
                    };
                entity_template.skeleton.processing_order[leg_index] =
                    authored_template.skeleton.processing_order[leg_index];
            }
            // Merge point: the rig says WHICH bones carry colliders and what
            // shape they are, the template says what they MEAN. Neither half
            // can be authored without the other, so a template that sets
            // collision_flags on a rig with no colliders was rejected at parse.
            if (authored_template.skeleton.has_collision_flags) {
                if (rig_asset != config.skeleton_assets.end()) {
                    const SkeletonCollisionFlagsConfig& flags =
                        authored_template.skeleton.collision_flags;
                    entity_template.skeleton.collider_count =
                        static_cast<std::uint32_t>(rig_asset->colliders.size());
                    for (std::uint32_t collider_index = 0u;
                         collider_index <
                             entity_template.skeleton.collider_count;
                         ++collider_index) {
                        const RigColliderConfig& collider =
                            rig_asset->colliders[collider_index];
                        // Resolved inside the rig: it owns both the collider's
                        // leg reference and the leg order that indexes it.
                        std::uint32_t leg_index = KERNEL_MAX_SKELETON_LEGS;
                        if (!collider.leg_id.empty()) {
                            const auto leg = std::find_if(
                                rig_asset->legs.begin(),
                                rig_asset->legs.end(),
                                [&collider](const RigLegConfig& candidate) {
                                    return candidate.id == collider.leg_id;
                                });
                            if (leg != rig_asset->legs.end()) {
                                leg_index = static_cast<std::uint32_t>(
                                    std::distance(rig_asset->legs.begin(), leg));
                            }
                        }
                        entity_template.skeleton.colliders[collider_index] =
                            KernelSkeletonColliderDefinition{
                                collider.bone_index,
                                leg_index,
                                collider.shape_type,
                                0u,
                                collider.has_hit_zone ? collider.hit_zone
                                                      : flags.hit_zone,
                                flags.purpose_flags,
                                flags.layer_mask,
                            };
                    }
                }
            }
        }

        if (authored_template.entity_type == kEntityTypeActor) {
            entity_template.component_flags =
                KERNEL_ENTITY_COMPONENT_TRANSFORM |
                KERNEL_ENTITY_COMPONENT_VELOCITY |
                KERNEL_ENTITY_COMPONENT_HEALTH |
                KERNEL_ENTITY_COMPONENT_HITBOX |
                KERNEL_ENTITY_COMPONENT_WEAPON_STATE;
            entity_template.animation_state = 0;
            entity_template.combat =
                make_combat_state_from_actor_template(config, authored_template);
            entity_template.vision = authored_template.vision;
            entity_template.vision.struct_size = sizeof(KernelAgentVisionConfig);
            if (authored_template.actor_type == kActorTypeAgent) {
                entity_template.component_flags |=
                    KERNEL_ENTITY_COMPONENT_AGENT_RUNTIME |
                    KERNEL_ENTITY_COMPONENT_SENTRY_RUNTIME;
                entity_template.ai.controller_type =
                    authored_template.ai_controller_type == KernelAiControllerType_None
                        ? KernelAiControllerType_Sentry
                        : authored_template.ai_controller_type;
                entity_template.ai.tick_interval =
                    authored_template.ai_tick_interval == 0u
                        ? 1u
                        : authored_template.ai_tick_interval;
            }
        } else if (authored_template.entity_type == KernelEntityType_Director) {
            entity_template.component_flags =
                KERNEL_ENTITY_COMPONENT_TRANSFORM |
                KERNEL_ENTITY_COMPONENT_AGENT_RUNTIME |
                KERNEL_ENTITY_COMPONENT_DIRECTOR_RUNTIME;
            if (authored_template.server_only) {
                entity_template.component_flags |= KERNEL_ENTITY_COMPONENT_SERVER_ONLY;
            }
            entity_template.ai.controller_type = KernelAiControllerType_Director;
            entity_template.ai.director_kind = authored_template.director_kind;
            entity_template.ai.game_rule_definition_id =
                authored_template.director_kind == KernelDirectorKind_GameRule
                    ? authored_template.actor_template_id
                    : 0u;
            entity_template.ai.tick_interval =
                authored_template.ai_tick_interval == 0u
                    ? 1u
                    : authored_template.ai_tick_interval;
            if (authored_template.director_kind == KernelDirectorKind_GameRule) {
                const std::uint32_t first_node =
                    static_cast<std::uint32_t>(storage.game_rule_nodes.size());
                const std::uint32_t first_edge =
                    static_cast<std::uint32_t>(storage.game_rule_edges.size());
                const std::uint32_t first_effect =
                    static_cast<std::uint32_t>(storage.game_rule_effects.size());
                for (const ActorTemplateConfig::GameRuleNodeConfig& node :
                     authored_template.game_rule_nodes) {
                    storage.game_rule_nodes.push_back(KernelGameRuleNodeDefinition{
                        sizeof(KernelGameRuleNodeDefinition),
                        node.node_id,
                        node.condition_type,
                        node.group_id,
                        node.condition_count,
                    });
                    if (node.has_spawn_effect) {
                        storage.game_rule_effects.push_back(
                            KernelGameRuleSpawnGroupEffectDefinition{
                                sizeof(KernelGameRuleSpawnGroupEffectDefinition),
                                KernelGameRuleEffectType_SpawnGroup,
                                node.node_id,
                                node.group_id,
                                node.spawn_count,
                                node.spawn_entity_template_id,
                                node.spawn_position,
                                node.spawn_radius,
                                node.spawn_seed,
                            });
                    }
                    for (const std::uint32_t next_node_id :
                         node.next_node_ids) {
                        storage.game_rule_edges.push_back(
                            KernelGameRuleEdgeDefinition{
                                sizeof(KernelGameRuleEdgeDefinition),
                                node.node_id,
                                next_node_id,
                            });
                    }
                }
                storage.game_rules.push_back(KernelGameRuleDefinition{
                    sizeof(KernelGameRuleDefinition),
                    authored_template.actor_template_id,
                    first_node,
                    static_cast<std::uint32_t>(
                        authored_template.game_rule_nodes.size()),
                    first_edge,
                    static_cast<std::uint32_t>(
                        storage.game_rule_edges.size() - first_edge),
                    first_effect,
                    static_cast<std::uint32_t>(
                        storage.game_rule_effects.size() - first_effect),
                });
            } else if (config.agent.override_director_spawn) {
                entity_template.ai.spawn_target_count = config.agent.spawn_count;
                entity_template.ai.spawn_entity_template_id =
                    config.agent.actor_template_id;
                entity_template.ai.spawn_actor_template_id =
                    config.agent.actor_template_id;
                entity_template.ai.spawn_position = config.agent.spawn_position;
                entity_template.ai.spawn_radius = config.agent.spawn_radius;
                entity_template.ai.spawn_seed = config.agent.spawn_seed;
            } else {
                entity_template.ai.spawn_target_count =
                    authored_template.director_spawn_target_count;
                entity_template.ai.spawn_entity_template_id =
                    authored_template.director_spawn_entity_template_id;
                entity_template.ai.spawn_actor_template_id =
                    authored_template.director_spawn_actor_template_id;
                entity_template.ai.spawn_position =
                    authored_template.director_spawn_position;
                entity_template.ai.spawn_radius =
                    authored_template.director_spawn_radius;
                entity_template.ai.spawn_seed = authored_template.director_spawn_seed;
            }
        } else if (authored_template.entity_type == KernelEntityType_Prop) {
            entity_template.prop = authored_template.prop;
            entity_template.component_flags = KERNEL_ENTITY_COMPONENT_TRANSFORM;
            if (authored_template.server_only) {
                entity_template.component_flags |= KERNEL_ENTITY_COMPONENT_SERVER_ONLY;
            }
            if (authored_template.health.max_hp != 0u) {
                entity_template.component_flags |= KERNEL_ENTITY_COMPONENT_HEALTH;
                entity_template.combat.hp = authored_template.health.hp;
                entity_template.combat.max_hp = authored_template.health.max_hp;
            }
            if (authored_template.collider_template_id != 0u) {
                entity_template.component_flags |= KERNEL_ENTITY_COMPONENT_HITBOX;
                entity_template.combat.hitbox_center = authored_template.hitbox_center;
                entity_template.combat.hitbox_half_extents =
                    authored_template.hitbox_half_extents;
            }
        }
        if (authored_template.skeleton.enabled) {
            entity_template.component_flags |=
                KERNEL_ENTITY_COMPONENT_SKELETON;
        }
        storage.entity_templates.push_back(entity_template);
    }
    for (const ProjectileTemplateConfig& projectile_template :
         config.projectile_templates) {
        storage.projectile_templates.push_back(projectile_template.definition);
    }
    for (const ColliderTemplateConfig& collider_template :
         config.colliders.templates) {
        storage.collider_templates.push_back(collider_template.definition);
    }
    for (const ActionTemplateConfig& action_template : config.action_templates) {
        storage.action_templates.push_back(action_template.definition);
    }
    for (const ItemTemplateConfig& authored_item : config.item_templates) {
        KernelItemTemplateDefinition item = authored_item.definition;
        item.item_used_trigger = compile_action_trigger_binding(
            authored_item.item_used_trigger,
            "on_item_used",
            config.action_graph_templates,
            config.entity_templates,
            &config.projectile_templates,
            &config.item_templates,
            &config.status_effect_templates);
        storage.item_templates.push_back(item);
    }
    const auto compile_status_trigger = [&](const TriggerBindingConfig& binding,
                                            const char* trigger_name) {
        KernelActionTriggerDefinition trigger = compile_action_trigger_binding(
            binding,
            trigger_name,
            config.action_graph_templates,
            config.entity_templates,
            nullptr,
            nullptr,
            &config.status_effect_templates);
        for (std::uint32_t index = 0u; index < trigger.action_count; ++index) {
            const KernelActionDefinition& action = trigger.actions[index];
            const std::uint8_t action_type = action.action_type;
            const bool damage_or_health =
                action_type == KernelEntityTriggerActionType_ApplyDamage ||
                action_type == KernelEntityTriggerActionType_ApplyHealthChange;
            const bool speed_modifier =
                action_type == KernelEntityTriggerActionType_ApplySpeedModifier;
            if (!damage_or_health && !speed_modifier) {
                throw std::runtime_error(
                    "status lifecycle action graph only allows damage, health change, and on_apply speed modifiers");
            }
            if (speed_modifier && std::string_view(trigger_name) != "on_apply") {
                throw std::runtime_error(
                    "apply_speed_modifier is only allowed in status on_apply");
            }
            if (speed_modifier &&
                action.target_source != KernelEntityRefSource_Self &&
                action.target_source != KernelEntityRefSource_EventSubject) {
                throw std::runtime_error(
                    "status speed modifier target must be self or event.subject");
            }
        }
        return trigger;
    };
    for (const StatusEffectTemplateConfig& authored_status :
         config.status_effect_templates) {
        KernelStatusEffectDefinition status{};
        status.struct_size = sizeof(KernelStatusEffectDefinition);
        status.status_effect_id = authored_status.status_effect_id;
        status.channel_id = authored_status.channel_id;
        status.duration_ticks = authored_status.duration_ticks;
        status.interval_ticks = authored_status.interval_ticks;
        status.replacement_policy = authored_status.replacement_policy;
        status.on_apply_trigger = compile_status_trigger(
            authored_status.on_apply_trigger, "on_apply");
        status.on_tick_trigger = compile_status_trigger(
            authored_status.on_tick_trigger, "on_tick");
        status.on_expire_trigger = compile_status_trigger(
            authored_status.on_expire_trigger, "on_expire");
        status.max_stacks = authored_status.replacement_policy ==
                KernelStatusEffectReplacementPolicy_Stack
            ? authored_status.max_stacks
            : 0u;
        status.refresh_on_stack = authored_status.refresh_on_stack ? 1u : 0u;

        const std::uint32_t stack_scale =
            std::max<std::uint32_t>(1u, authored_status.max_stacks);
        const auto validate_scaled_trigger =
            [&](const KernelActionTriggerDefinition& trigger,
                const char* trigger_name) {
                for (std::uint32_t index = 0u;
                     index < trigger.action_count;
                     ++index) {
                    const KernelActionDefinition& action =
                        trigger.actions[index];
                    const bool scale_amount =
                        std::string_view(trigger_name) != "on_apply";
                    if (scale_amount &&
                        action.action_type ==
                            KernelEntityTriggerActionType_ApplyDamage &&
                        static_cast<std::uint64_t>(action.damage_amount) *
                                stack_scale >
                            UINT16_MAX) {
                        throw std::runtime_error(
                            "stacked status damage overflows uint16");
                    }
                    if (scale_amount &&
                        action.action_type ==
                            KernelEntityTriggerActionType_ApplyHealthChange) {
                        const std::int64_t scaled =
                            static_cast<std::int64_t>(
                                action.health_change_amount) *
                            stack_scale;
                        if (scaled < INT32_MIN || scaled > INT32_MAX) {
                            throw std::runtime_error(
                                "stacked status health change overflows int32");
                        }
                    }
                    if (action.action_type ==
                        KernelEntityTriggerActionType_ApplySpeedModifier) {
                        const double scaled =
                            action.modifier_operation ==
                                    KernelStatModifierOperation_Additive
                                ? static_cast<double>(action.modifier_value) *
                                    stack_scale
                                : std::pow(
                                      static_cast<double>(
                                          action.modifier_value),
                                      stack_scale);
                        if (!std::isfinite(scaled) ||
                            std::abs(scaled) >
                                std::numeric_limits<float>::max()) {
                            throw std::runtime_error(
                                "stacked status speed modifier is not finite");
                        }
                    }
                }
            };
        validate_scaled_trigger(status.on_apply_trigger, "on_apply");
        validate_scaled_trigger(status.on_tick_trigger, "on_tick");
        validate_scaled_trigger(status.on_expire_trigger, "on_expire");
        storage.status_effects.push_back(status);
    }
    for (const PropPopulationRuleConfig& authored_rule :
         config.prop_population_rules) {
        storage.prop_population_rules.push_back(authored_rule.definition);
    }
    storage.definition.struct_size = sizeof(storage.definition);
    storage.definition.catalog_version = config.weapons.catalog_version;
    storage.definition.catalog_hash = config.weapons.catalog_hash;
    storage.definition.actor_templates = storage.actor_templates.data();
    storage.definition.actor_template_count =
        static_cast<std::uint32_t>(storage.actor_templates.size());
    storage.definition.entity_templates = storage.entity_templates.data();
    storage.definition.entity_template_count =
        static_cast<std::uint32_t>(storage.entity_templates.size());
    storage.definition.projectile_templates =
        storage.projectile_templates.data();
    storage.definition.projectile_template_count =
        static_cast<std::uint32_t>(storage.projectile_templates.size());
    storage.definition.collider_templates = storage.collider_templates.data();
    storage.definition.collider_template_count =
        static_cast<std::uint32_t>(storage.collider_templates.size());
    storage.definition.action_templates = storage.action_templates.data();
    storage.definition.action_template_count =
        static_cast<std::uint32_t>(storage.action_templates.size());
    storage.definition.item_templates = storage.item_templates.data();
    storage.definition.item_template_count =
        static_cast<std::uint32_t>(storage.item_templates.size());
    storage.definition.prop_population_rules =
        storage.prop_population_rules.data();
    storage.definition.prop_population_rule_count =
        static_cast<std::uint32_t>(storage.prop_population_rules.size());
    storage.definition.status_effects = storage.status_effects.data();
    storage.definition.status_effect_count =
        static_cast<std::uint32_t>(storage.status_effects.size());
    storage.definition.skeleton_assets = storage.skeleton_assets.data();
    storage.definition.skeleton_asset_count =
        static_cast<std::uint32_t>(storage.skeleton_assets.size());
    storage.definition.game_rules = storage.game_rules.data();
    storage.definition.game_rule_count =
        static_cast<std::uint32_t>(storage.game_rules.size());
    storage.definition.game_rule_nodes = storage.game_rule_nodes.data();
    storage.definition.game_rule_node_count =
        static_cast<std::uint32_t>(storage.game_rule_nodes.size());
    storage.definition.game_rule_edges = storage.game_rule_edges.data();
    storage.definition.game_rule_edge_count =
        static_cast<std::uint32_t>(storage.game_rule_edges.size());
    storage.definition.game_rule_effects = storage.game_rule_effects.data();
    storage.definition.game_rule_effect_count =
        static_cast<std::uint32_t>(storage.game_rule_effects.size());
    return storage;
}

bool load_kernel_gameplay_catalog(
    KernelHandle* kernel,
    const GameServerGameplayConfig& config) {
    if (kernel == nullptr || config.weapons.catalog_hash == 0) {
        return false;
    }
    KernelGameplayCatalogStorage storage = build_kernel_gameplay_catalog(config);
    return Kernel_LoadGameplayCatalog(kernel, &storage.definition, nullptr);
}

KernelCombatStateDefinition make_combat_state_from_actor_template(
    const GameServerGameplayConfig& config,
    const ActorTemplateConfig& actor_template) {
    KernelCombatStateDefinition combat_state{};
    combat_state.struct_size = sizeof(KernelCombatStateDefinition);
    combat_state.hp = actor_template.health.hp;
    combat_state.max_hp = actor_template.health.max_hp;
    combat_state.active_weapon_slot = actor_template.active_weapon_slot;
    combat_state.weapon_slot_count = actor_template.weapon_slot_count;
    combat_state.collider_template_id = actor_template.collider_template_id;
    combat_state.move_speed_meters_per_second =
        actor_template.move_speed_meters_per_second;
    combat_state.hitbox_center = actor_template.hitbox_center;
    combat_state.hitbox_half_extents = actor_template.hitbox_half_extents;
    for (std::uint8_t slot = 0; slot < actor_template.weapon_slot_count; ++slot) {
        const std::uint32_t weapon_id = actor_template.weapon_ids[slot];
        const KernelWeaponMechanicsDefinition& weapon =
            config.weapons.definitions[weapon_id];
        combat_state.weapon_ids[slot] = weapon_id;
        combat_state.ammo[slot] = weapon.magazine_size;
        combat_state.reserve_magazines[slot] = weapon.reserve_magazines;
    }
    return combat_state;
}

KernelCombatStateDefinition make_player_combat_state(
    const GameServerGameplayConfig& config) {
    const ActorTemplateConfig* actor_template =
        find_actor_template(config, config.player.actor_template_id);
    if (actor_template == nullptr) {
        return KernelCombatStateDefinition{};
    }
    return make_combat_state_from_actor_template(config, *actor_template);
}

KernelCombatStateDefinition make_agent_combat_state(
    const GameServerGameplayConfig& config) {
    const ActorTemplateConfig* actor_template =
        find_actor_template(config, config.agent.actor_template_id);
    if (actor_template == nullptr) {
        return KernelCombatStateDefinition{};
    }
    return make_combat_state_from_actor_template(config, *actor_template);
}

}  // namespace network_example::game_server
