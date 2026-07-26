#include "game_server/gameplay_config.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
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
    hash_float(hash, mechanics.beam.length);
    hash_float(hash, mechanics.beam.radius);
    hash_scalar(hash, mechanics.beam.damage_per_tick);
    hash_scalar(hash, mechanics.beam.lifetime_ticks);
    hash_scalar(hash, mechanics.beam.collision_mask);
    for (const KernelActionTriggerDefinition* trigger : {
             &mechanics.projectile_impact_trigger,
             &mechanics.expired_trigger,
         }) {
        hash_scalar(hash, trigger->action_type);
        hash_scalar(hash, trigger->spawn_projectile_template_id);
        hash_scalar(hash, trigger->position_source);
        hash_scalar(hash, trigger->direction_source);
    }
}

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
    hash_scalar(hash, actor_template.weapon_slot_count);
    for (std::uint8_t index = 0; index < actor_template.weapon_slot_count; ++index) {
        hash_scalar(hash, actor_template.weapon_ids[index]);
    }
    hash_scalar(hash, actor_template.active_weapon_slot);
    hash_scalar(hash, actor_template.animation_idle);
    hash_scalar(hash, actor_template.animation_chasing);
    hash_scalar(hash, actor_template.sentry.alert_ticks);
    hash_scalar(hash, actor_template.sentry.forget_ticks);
    hash_scalar(hash, actor_template.sentry.patrol_rotation_interval_ticks);
    hash_float(hash, actor_template.sentry.patrol_rotation_min_degrees);
    hash_float(hash, actor_template.sentry.patrol_rotation_max_degrees);
    hash_scalar(hash, actor_template.sentry.weapon_id);
    hash_scalar(hash, actor_template.vision.camp);
    hash_scalar(hash, actor_template.vision.vision_collider_template_id);
    hash_scalar(hash, actor_template.vision.max_visible_hostiles);
    hash_scalar(hash, actor_template.vision.max_visible_allies);
    hash_scalar(hash, actor_template.vision.max_visible_neutrals);
    hash_vec3(hash, actor_template.vision.local_origin);
    hash_vec3(hash, actor_template.vision.local_forward);
    hash_scalar(hash, actor_template.ai_controller_type);
    hash_scalar(hash, actor_template.ai_tick_interval);
    hash_scalar(hash, actor_template.director_spawn_target_count);
    hash_scalar(hash, actor_template.director_spawn_entity_template_id);
    hash_scalar(hash, actor_template.director_spawn_actor_template_id);
    hash_string(hash, actor_template.director_spawn_entity_template_ref);
    hash_vec3(hash, actor_template.director_spawn_position);
    hash_float(hash, actor_template.director_spawn_radius);
    hash_scalar(hash, actor_template.director_spawn_seed);
    hash_string(hash, actor_template.activated_trigger.action_graph_ref);
    for (const auto& parameter : actor_template.activated_trigger.parameters) {
        hash_string(hash, parameter.first);
        hash_string(hash, parameter.second);
    }
    hash_string(hash, actor_template.collision_trigger.action_graph_ref);
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
}

KernelWeaponMechanicsDefinition hitscan_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size,
    std::uint16_t damage,
    std::uint32_t reload_ticks,
    float max_range) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    weapon.weapon_id = weapon_id;
    weapon.fire_mode = KernelWeaponFireMode_Hitscan;
    weapon.magazine_size = magazine_size;
    weapon.reserve_magazines = kDefaultReserveMagazines;
    weapon.damage = damage;
    (void)reload_ticks;
    weapon.max_range = max_range;
    weapon.pellet_count = 1;
    return weapon;
}

KernelWeaponMechanicsDefinition shotgun_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size,
    std::uint16_t damage,
    std::uint32_t reload_ticks,
    float max_range,
    std::uint8_t pellet_count,
    float pellet_spread) {
    KernelWeaponMechanicsDefinition weapon =
        hitscan_weapon(
            weapon_id,
            magazine_size,
            damage,
            reload_ticks,
            max_range);
    weapon.fire_mode = KernelWeaponFireMode_Shotgun;
    weapon.pellet_count = pellet_count;
    weapon.pellet_spread = pellet_spread;
    return weapon;
}

KernelWeaponMechanicsDefinition area_effect_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size,
    std::uint16_t damage,
    std::uint32_t reload_ticks) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    weapon.weapon_id = weapon_id;
    weapon.fire_mode = KernelWeaponFireMode_Projectile;
    weapon.magazine_size = magazine_size;
    weapon.reserve_magazines = kDefaultReserveMagazines;
    weapon.damage = damage;
    (void)reload_ticks;
    weapon.pellet_count = 1;
    weapon.projectile_template_id = weapon_id;
    return weapon;
}

KernelWeaponMechanicsDefinition beam_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size,
    std::uint16_t damage,
    std::uint32_t reload_ticks) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    weapon.weapon_id = weapon_id;
    weapon.fire_mode = KernelWeaponFireMode_Projectile;
    weapon.magazine_size = magazine_size;
    weapon.reserve_magazines = kDefaultReserveMagazines;
    weapon.damage = damage;
    (void)reload_ticks;
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
    if (token == "damageable") {
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
    throw std::runtime_error("unsupported collision_mask: " + token);
}

std::uint32_t collision_mask_from_yaml(const YAML::Node& node) {
    if (!node) {
        return KERNEL_COLLISION_MASK_DAMAGEABLE;
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

class GameplayConfigSource {
public:
    virtual ~GameplayConfigSource() = default;
    virtual YAML::Node load_yaml(const std::string& path) const = 0;
    virtual std::vector<std::string> list_yaml_files(
        const std::string& directory) const = 0;
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

    std::vector<std::string> list_yaml_files(
        const std::string& directory) const override {
        std::vector<std::string> files;
        if (!std::filesystem::exists(directory)) {
            return files;
        }
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(directory)) {
            if (entry.is_regular_file() && entry.path().extension() == ".yaml") {
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
    constexpr const char* kYamlSuffix = ".yaml";
    return path.size() >= 5 &&
           path.compare(path.size() - 5, 5, kYamlSuffix) == 0;
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
            if (!has_yaml_extension(path)) {
                zip_entry_close(archive.get());
                continue;
            }

            const unsigned long long entry_size = zip_entry_size(archive.get());
            if (entry_size > kMaxYamlEntryBytes) {
                throw std::runtime_error("archive entry exceeds size limit: " + path);
            }
            total_yaml_bytes += entry_size;
            if (total_yaml_bytes > kMaxTotalYamlBytes) {
                throw std::runtime_error("archive YAML content exceeds total size limit");
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

    std::vector<std::string> list_yaml_files(
        const std::string& directory) const override {
        const std::string normalized_directory = normalize_archive_path(directory);
        const std::string prefix = normalized_directory + "/";
        std::vector<std::string> files;
        for (const auto& entry : files_) {
            const std::string& path = entry.first;
            if (!has_yaml_extension(path) || path.rfind(prefix, 0) != 0) {
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
    std::uint32_t template_id = 0) {
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
        if (has_default && !entry.second.IsScalar()) {
            throw std::runtime_error(
                "first action graph phase only supports scalar defaults: " +
                path);
        }
        graph.parameters.push_back(ActionGraphParameterConfig{
            name,
            has_default,
            has_default ? entry.second.as<std::string>() : std::string{},
        });
    }
    const YAML::Node actions = node["actions"];
    if (!actions || !actions.IsSequence() || actions.size() != 1u) {
        throw std::runtime_error(
            "first action graph phase requires exactly one action: " + path);
    }
    const YAML::Node action = actions[0];
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
        },
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_UNKNOWN);
    if (!action["type"]) {
        throw std::runtime_error("action graph action requires type: " + path);
    }
    graph.action_type = action["type"].as<std::string>();
    std::vector<const std::string*> action_parameters;
    if (graph.action_type == "spawn_projectile") {
        if (action["entity_template"] || action["owner"] ||
            action["target"] || action["amount"]) {
            throw std::runtime_error(
                "spawn_projectile action has unsupported fields: " + path);
        }
        graph.projectile_template_parameter = parameter_reference_from_yaml(
            action["projectile_template"], "projectile_template");
        graph.position_parameter =
            parameter_reference_from_yaml(action["position"], "position");
        graph.direction_parameter =
            parameter_reference_from_yaml(action["direction"], "direction");
        action_parameters = {
            &graph.projectile_template_parameter,
            &graph.position_parameter,
            &graph.direction_parameter,
        };
    } else if (graph.action_type == "spawn_entity") {
        if (action["projectile_template"] || action["direction"] ||
            action["target"] || action["amount"]) {
            throw std::runtime_error(
                "spawn_entity action has unsupported fields: " + path);
        }
        graph.entity_template_parameter = parameter_reference_from_yaml(
            action["entity_template"], "entity_template");
        graph.position_parameter =
            parameter_reference_from_yaml(action["position"], "position");
        graph.owner_parameter =
            parameter_reference_from_yaml(action["owner"], "owner");
        action_parameters = {
            &graph.entity_template_parameter,
            &graph.position_parameter,
            &graph.owner_parameter,
        };
    } else if (graph.action_type == "apply_damage") {
        if (action["projectile_template"] || action["position"] ||
            action["direction"] || action["entity_template"] ||
            action["owner"]) {
            throw std::runtime_error(
                "apply_damage action has unsupported fields: " + path);
        }
        graph.target_parameter =
            parameter_reference_from_yaml(action["target"], "target");
        graph.amount_parameter =
            parameter_reference_from_yaml(action["amount"], "amount");
        action_parameters = {
            &graph.target_parameter,
            &graph.amount_parameter,
        };
    } else {
        throw std::runtime_error(
            "unsupported action graph action type: " + graph.action_type);
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
    const std::vector<ActionTemplateConfig>& actions) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error("fire_action_template reference must be a scalar");
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
        "unknown fire_action_template reference: " + value,
        {},
        "fire_action_template",
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
            "damage",
            "reload_ticks",
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
    const std::uint32_t reload_ticks = node["reload_ticks"].as<std::uint32_t>();
    const std::string type = node["weapon_type"].as<std::string>();
    const std::uint16_t damage =
        node["damage"] ? node["damage"].as<std::uint16_t>() : 0u;
    if (type == "hitscan" || type == "shotgun") {
        if (node["projectile"] || node["area_effect"] || node["beam"]) {
            throw std::runtime_error(
                "instant weapons must not define projectile, area_effect, or beam");
        }
        if (type == "shotgun") {
            KernelWeaponMechanicsDefinition weapon = shotgun_weapon(
                id,
                magazine_size,
                damage,
                reload_ticks,
                node["max_range"].as<float>(),
                static_cast<std::uint8_t>(node["pellet_count"].as<int>()),
                node["pellet_spread"].as<float>());
            weapon.reserve_magazines = reserve_magazines;
            return weapon;
        }
        KernelWeaponMechanicsDefinition weapon = hitscan_weapon(
            id,
            magazine_size,
            damage,
            reload_ticks,
            node["max_range"].as<float>());
        weapon.reserve_magazines = reserve_magazines;
        return weapon;
    }
    if (type == "projectile") {
        if (node["area_effect"] || node["beam"]) {
            throw std::runtime_error(
                "projectile weapons must not define area_effect or beam");
        }
        if (node["projectile"] || node["damage"]) {
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
        (void)reload_ticks;
        weapon.pellet_count = 1;
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
        KernelWeaponMechanicsDefinition weapon = area_effect_weapon(
            id,
            magazine_size,
            damage,
            reload_ticks);
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
        KernelWeaponMechanicsDefinition weapon = beam_weapon(
            id,
            magazine_size,
            damage,
            reload_ticks);
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
    std::uint32_t template_id);

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
        return KernelVec4{
            node["length"].as<float>(),
            node["radius"] ? node["radius"].as<float>() : 0.0f,
            node["scatter_degrees"] ? node["scatter_degrees"].as<float>() : 0.0f,
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
                "length",
                "scatter_degrees",
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
                "patrol_rotation_interval_ticks",
                "patrol_rotation_min_degrees",
                "patrol_rotation_max_degrees",
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

ActorTemplateConfig actor_template_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind,
    const WeaponCatalogConfig& weapons,
    const ColliderCatalogConfig& colliders) {
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
            "movement",
            "hitbox",
            "weapon_slots",
            "active_weapon_slot",
            "animations",
            "ai",
            "vision",
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
    actor_template.vision = vision_config_from_yaml(
        node["vision"],
        actor_template,
        colliders,
        path,
        source_kind);
    return actor_template;
}

EntityTemplateConfig entity_template_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind,
    const WeaponCatalogConfig& weapons,
    const ColliderCatalogConfig& colliders) {
    const std::uint16_t entity_type =
        authored_entity_type_from_yaml(node["entity_type"]);
    if (entity_type == kEntityTypeActor) {
        EntityTemplateConfig entity_template =
            actor_template_from_yaml(node, path, source_kind, weapons, colliders);
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
                "physics",
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
                entity_template.collision_trigger = trigger_binding_from_yaml(
                    node["triggers"]["on_collision"],
                    path,
                    source_kind,
                    KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
                    entity_template.actor_template_id);
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
        {"kind", "spawn"},
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
        entity_template.actor_template_id);
    const std::string kind =
        director["kind"] ? director["kind"].as<std::string>() : std::string{};
    if (kind != "world_rule") {
        throw std::runtime_error("unsupported director.kind: " + kind);
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
    const ColliderCatalogConfig& colliders) {
    std::vector<ActorTemplateConfig> actor_templates;
    std::unordered_map<std::uint32_t, std::string> ids;
    std::unordered_map<std::string, std::uint32_t> names;
    const std::vector<std::string> files = source.list_yaml_files(directory);
    for (const std::string& file : files) {
        ActorTemplateConfig actor_template =
            actor_template_from_yaml(
                source.load_yaml(file),
                file,
                source.source_kind(),
                weapons,
                colliders);
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
    const ColliderCatalogConfig& colliders) {
    std::vector<EntityTemplateConfig> entity_templates;
    std::unordered_map<std::uint32_t, std::string> ids;
    std::unordered_map<std::string, std::uint32_t> names;
    const std::vector<std::string> files = source.list_yaml_files(directory);
    for (const std::string& file : files) {
        EntityTemplateConfig entity_template =
            entity_template_from_yaml(
                source.load_yaml(file),
                file,
                source.source_kind(),
                weapons,
                colliders);
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
        if (entity_template.entity_type != KernelEntityType_Director ||
            entity_template.director_spawn_entity_template_ref.empty()) {
            continue;
        }
        const YAML::Node ref(entity_template.director_spawn_entity_template_ref);
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
    std::uint32_t template_id) {
    reject_unknown_keys(
        node,
        {"action_graph", "parameters"},
        path,
        source_kind,
        template_kind,
        template_id);
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
    mechanics.collision_mask = collision_mask_from_yaml(node["collision_mask"]);
    mechanics.collision_query_mode = collision_query_mode_from_yaml(
        node["collision_query_mode"] ? node["collision_query_mode"]
                                     : node["collision_query"]);
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
                "damage_per_interval",
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
        mechanics.motion_model = KernelProjectileMotionModel_Linear;
        mechanics.sync_mode = KernelProjectileSyncMode_ServerSnapshotOnly;
        mechanics.hit_response = KernelProjectileHitResponse_Destroy;
        mechanics.damage_shape = KernelProjectileDamageShape_DirectHit;
        mechanics.damage =
            damage_behavior["damage_per_interval"].as<std::uint16_t>();
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
        return projectile_template;
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
                "damage_per_tick",
                "lifetime_ticks",
                "collision_mask",
            },
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_PROJECTILE,
            definition.projectile_template_id);
        mechanics.beam.struct_size = sizeof(KernelBeamMechanicsDefinition);
        mechanics.beam.length = beam["length"].as<float>();
        mechanics.beam.radius = beam["radius"].as<float>();
        mechanics.beam.damage_per_tick =
            beam["damage_per_tick"].as<std::uint16_t>();
        mechanics.beam.lifetime_ticks =
            beam["lifetime_ticks"] ? beam["lifetime_ticks"].as<std::uint32_t>() : 2u;
        mechanics.beam.collision_mask = collision_mask_from_yaml(beam["collision_mask"]);
    } else if (node["beam"]) {
        throw std::runtime_error("beam block requires projectile type: beam");
    }

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
    if (graph->action_type != "spawn_projectile") {
        throw std::runtime_error(
            "projectile trigger requires spawn_projectile action graph: " +
            binding.action_graph_ref);
    }
    std::unordered_set<std::string> seen_parameters;
    for (const auto& parameter : binding.parameters) {
        if (!seen_parameters.insert(parameter.first).second ||
            std::none_of(
                graph->parameters.begin(),
                graph->parameters.end(),
                [&](const ActionGraphParameterConfig& declaration) {
                    return declaration.name == parameter.first;
                })) {
            throw std::runtime_error(
                "trigger binding passes undeclared or duplicate parameter: " +
                parameter.first);
        }
    }
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
    for (const ActionGraphParameterConfig& parameter : graph->parameters) {
        (void)trigger_parameter_value(binding, parameter);
    }

    const std::string projectile_ref = trigger_parameter_value(
        binding, graph_parameter(graph->projectile_template_parameter));
    const std::string position = trigger_parameter_value(
        binding, graph_parameter(graph->position_parameter));
    const std::string direction = trigger_parameter_value(
        binding, graph_parameter(graph->direction_parameter));
    if (position != "event.position" || direction != "event.direction") {
        throw std::runtime_error(
            "spawn_projectile trigger must bind position and direction to "
            "event.position and event.direction");
    }
    ProjectileTemplateConfig* spawned_projectile = projectile_template_from_ref(
        YAML::Node(projectile_ref), projectile_templates);
    KernelActionTriggerDefinition& compiled =
        expired
        ? projectile_template->definition.mechanics
              .expired_trigger
        : projectile_template->definition.mechanics
              .projectile_impact_trigger;
    compiled.struct_size = sizeof(KernelActionTriggerDefinition);
    compiled.action_type = KernelEntityTriggerActionType_SpawnProjectile;
    compiled.spawn_projectile_template_id =
        spawned_projectile->definition.projectile_template_id;
    compiled.position_source = KernelEventVec3Source_Position;
    compiled.direction_source = KernelEventVec3Source_Direction;
}

KernelActionTriggerDefinition compile_action_trigger_binding(
    const TriggerBindingConfig& binding,
    std::string_view trigger_name,
    const std::vector<ActionGraphTemplateConfig>& action_graph_templates,
    const std::vector<EntityTemplateConfig>& entity_templates) {
    KernelActionTriggerDefinition compiled{};
    if (binding.action_graph_ref.empty()) {
        return compiled;
    }
    const ActionGraphTemplateConfig* graph = action_graph_template_from_ref(
        binding.action_graph_ref, action_graph_templates);
    if (graph->action_type != "apply_damage" &&
        graph->action_type != "spawn_entity") {
        throw std::runtime_error(
            std::string(trigger_name) +
            " requires apply_damage or spawn_entity action graph: " +
            binding.action_graph_ref);
    }
    std::unordered_set<std::string> seen_parameters;
    for (const auto& parameter : binding.parameters) {
        if (!seen_parameters.insert(parameter.first).second ||
            std::none_of(
                graph->parameters.begin(),
                graph->parameters.end(),
                [&](const ActionGraphParameterConfig& declaration) {
                    return declaration.name == parameter.first;
                })) {
            throw std::runtime_error(
                "trigger binding passes undeclared or duplicate parameter: " +
                parameter.first);
        }
    }
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
    for (const ActionGraphParameterConfig& parameter : graph->parameters) {
        (void)trigger_parameter_value(binding, parameter);
    }
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
    compiled.struct_size = sizeof(KernelActionTriggerDefinition);
    if (graph->action_type == "spawn_entity") {
        const std::string entity_template = trigger_parameter_value(
            binding, graph_parameter(graph->entity_template_parameter));
        const std::string position = trigger_parameter_value(
            binding, graph_parameter(graph->position_parameter));
        const std::string owner = trigger_parameter_value(
            binding, graph_parameter(graph->owner_parameter));
        if (position != "event.position") {
            throw std::runtime_error(
                "spawn_entity position must bind to event.position");
        }
        compiled.action_type = KernelEntityTriggerActionType_SpawnEntity;
        compiled.spawn_entity_template_id = entity_template_ref_from_yaml(
            YAML::Node(entity_template), entity_templates);
        compiled.position_source = KernelEventVec3Source_Position;
        compiled.owner_source = entity_ref_source(owner);
        return compiled;
    }

    const std::string target = trigger_parameter_value(
        binding, graph_parameter(graph->target_parameter));
    const std::string amount = trigger_parameter_value(
        binding, graph_parameter(graph->amount_parameter));
    const std::uint8_t target_source = entity_ref_source(target);
    std::size_t parsed = 0;
    const unsigned long parsed_amount = std::stoul(amount, &parsed);
    if (parsed != amount.size() || parsed_amount == 0u ||
        parsed_amount > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error(
            "apply_damage amount must be a positive uint16");
    }
    compiled.action_type = KernelEntityTriggerActionType_ApplyDamage;
    compiled.target_source = target_source;
    compiled.damage_amount = static_cast<std::uint16_t>(parsed_amount);
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
                    document["fire_action_template"], action_templates);
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
    for (std::size_t id = 0; id < config.weapons.definitions.size(); ++id) {
        if (config.weapons.configured[id]) {
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
            "reload_action_template",
            "weapon_template_dir",
            "projectile_template_dir",
            "actor_template_dir",
            "entity_template_dir",
            "collider_template_dir",
            "static_collision_scene",
            "player",
            "enemy",
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
    if (catalog_version != 3u) {
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
    for (std::size_t id = 0; id < config.weapons.definitions.size(); ++id) {
        if (config.weapons.configured[id]) {
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
            config.colliders);
        config.actor_templates =
            actor_templates_from_entity_templates(config.entity_templates);
    } else if (document["actor_template_dir"]) {
        const std::string actor_template_dir =
            source.resolve_path(base_path, document["actor_template_dir"]);
        config.actor_templates = load_actor_templates_from_source(
            source,
            actor_template_dir,
            config.weapons,
            config.colliders);
        config.entity_templates = config.actor_templates;
    } else {
        apply_default_actor_templates(&config);
    }

    apply_catalog_player_config(document, &config);
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
        }
        hash_string(&hash, graph.action_type);
        hash_string(&hash, graph.projectile_template_parameter);
        hash_string(&hash, graph.position_parameter);
        hash_string(&hash, graph.direction_parameter);
        hash_string(&hash, graph.target_parameter);
        hash_string(&hash, graph.amount_parameter);
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
    hash_scalar(&hash, config.player.actor_template_id);
    hash_scalar(&hash, config.agent.actor_template_id);
    hash_vec3(&hash, config.agent.spawn_position);
    hash_scalar(&hash, config.agent.spawn_count);
    hash_float(&hash, config.agent.spawn_radius);
    hash_scalar(&hash, config.agent.spawn_seed);
    hash_string(&hash, config.static_collision_scene.entry_path);
    hash_scalar(&hash, config.static_collision_scene.scene_id);
    hash_scalar(&hash, config.static_collision_scene.collider_id);
    hash_scalar(&hash, config.static_collision_scene.collision_layer);
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
        if (actor_template.actor_type == kActorTypeAgent &&
            (actor_template.sentry.weapon_id > UINT8_MAX ||
             !config.weapons.configured[actor_template.sentry.weapon_id] ||
             !actor_template_has_weapon(
                 actor_template,
                 static_cast<std::uint8_t>(
                     actor_template.sentry.weapon_id)) ||
             actor_template.sentry.alert_ticks == 0 ||
             actor_template.sentry.forget_ticks == 0 ||
             actor_template.sentry.patrol_rotation_interval_ticks == 0 ||
             actor_template.sentry.patrol_rotation_min_degrees <= 0.0f ||
             actor_template.sentry.patrol_rotation_max_degrees <
                 actor_template.sentry.patrol_rotation_min_degrees)) {
            errors.push_back("agent sentry actor template must be valid");
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
            (definition.shape_type == KernelColliderShapeType_Segment &&
             (definition.shape_params.x <= 0.0f ||
              definition.shape_params.y < 0.0f ||
              definition.shape_params.z < 0.0f ||
              definition.lifetime_ticks == 0)) ||
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

    bool has_director_template = false;
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
        entity_template.activated_trigger = compile_action_trigger_binding(
            authored_template.activated_trigger,
            "on_activated",
            config.action_graph_templates,
            config.entity_templates);
        entity_template.collision_trigger = compile_action_trigger_binding(
            authored_template.collision_trigger,
            "on_collision",
            config.action_graph_templates,
            config.entity_templates);
        entity_template.health_depleted_trigger = compile_action_trigger_binding(
            authored_template.health_depleted_trigger,
            "on_health_depleted",
            config.action_graph_templates,
            config.entity_templates);
        entity_template.destroy_entity_trigger = compile_action_trigger_binding(
            authored_template.destroy_entity_trigger,
            "on_destroy_entity",
            config.action_graph_templates,
            config.entity_templates);

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
            has_director_template = true;
            entity_template.component_flags =
                KERNEL_ENTITY_COMPONENT_TRANSFORM |
                KERNEL_ENTITY_COMPONENT_AGENT_RUNTIME |
                KERNEL_ENTITY_COMPONENT_DIRECTOR_RUNTIME;
            if (authored_template.server_only) {
                entity_template.component_flags |= KERNEL_ENTITY_COMPONENT_SERVER_ONLY;
            }
            entity_template.ai.controller_type = KernelAiControllerType_Director;
            entity_template.ai.tick_interval =
                authored_template.ai_tick_interval == 0u
                    ? 1u
                    : authored_template.ai_tick_interval;
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
        } else if (authored_template.entity_type == KernelEntityType_Prop) {
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
        storage.entity_templates.push_back(entity_template);
    }
    if (!has_director_template) {
        KernelEntityTemplateDefinition director_template{};
        director_template.struct_size = sizeof(KernelEntityTemplateDefinition);
        director_template.entity_template_id = kDefaultDirectorEntityTemplateId;
        director_template.entity_type = KernelEntityType_Director;
        director_template.component_flags =
            KERNEL_ENTITY_COMPONENT_TRANSFORM |
            KERNEL_ENTITY_COMPONENT_SERVER_ONLY |
            KERNEL_ENTITY_COMPONENT_AGENT_RUNTIME |
            KERNEL_ENTITY_COMPONENT_DIRECTOR_RUNTIME;
        director_template.ai.struct_size = sizeof(KernelEntityAiDefinition);
        director_template.ai.controller_type = KernelAiControllerType_Director;
        director_template.ai.tick_interval = 1;
        director_template.ai.spawn_target_count = config.agent.spawn_count;
        director_template.ai.spawn_entity_template_id = config.agent.actor_template_id;
        director_template.ai.spawn_actor_template_id = config.agent.actor_template_id;
        director_template.ai.spawn_position = config.agent.spawn_position;
        director_template.ai.spawn_radius = config.agent.spawn_radius;
        director_template.ai.spawn_seed = config.agent.spawn_seed;
        storage.entity_templates.push_back(director_template);
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
