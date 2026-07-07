#include "game_server/gameplay_config.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
    hash_scalar(hash, weapon.damage);
    hash_scalar(hash, weapon.cooldown_ticks);
    hash_scalar(hash, weapon.reload_ticks);
    hash_float(hash, weapon.max_range);
    hash_scalar(hash, weapon.pellet_count);
    hash_float(hash, weapon.pellet_spread);
    hash_scalar(hash, weapon.segment_collider_template_id);
    hash_scalar(hash, weapon.projectile_template_id);
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
    hash_scalar(hash, mechanics.flags);
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
    hash_scalar(hash, mechanics.impact_spawn_projectile_template_id);
    hash_scalar(hash, mechanics.expire_spawn_projectile_template_id);
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
    hash_scalar(hash, actor_template.weapon_slot_count);
    for (std::uint8_t index = 0; index < actor_template.weapon_slot_count; ++index) {
        hash_scalar(hash, actor_template.weapon_slots[index]);
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
    hash_scalar(hash, actor_template.sentry.magazine_size);
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
}

KernelWeaponMechanicsDefinition hitscan_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size,
    std::uint16_t damage,
    std::uint32_t cooldown_ticks,
    std::uint32_t reload_ticks,
    float max_range) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    weapon.weapon_id = weapon_id;
    weapon.fire_mode = KernelWeaponFireMode_Hitscan;
    weapon.magazine_size = magazine_size;
    weapon.damage = damage;
    weapon.cooldown_ticks = cooldown_ticks;
    weapon.reload_ticks = reload_ticks;
    weapon.max_range = max_range;
    weapon.pellet_count = 1;
    return weapon;
}

KernelWeaponMechanicsDefinition shotgun_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size,
    std::uint16_t damage,
    std::uint32_t cooldown_ticks,
    std::uint32_t reload_ticks,
    float max_range,
    std::uint8_t pellet_count,
    float pellet_spread) {
    KernelWeaponMechanicsDefinition weapon =
        hitscan_weapon(
            weapon_id,
            magazine_size,
            damage,
            cooldown_ticks,
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
    std::uint32_t cooldown_ticks,
    std::uint32_t reload_ticks) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    weapon.weapon_id = weapon_id;
    weapon.fire_mode = KernelWeaponFireMode_Projectile;
    weapon.magazine_size = magazine_size;
    weapon.damage = damage;
    weapon.cooldown_ticks = cooldown_ticks;
    weapon.reload_ticks = reload_ticks;
    weapon.pellet_count = 1;
    weapon.projectile_template_id = weapon_id;
    return weapon;
}

KernelWeaponMechanicsDefinition beam_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size,
    std::uint16_t damage,
    std::uint32_t cooldown_ticks,
    std::uint32_t reload_ticks) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    weapon.weapon_id = weapon_id;
    weapon.fire_mode = KernelWeaponFireMode_Projectile;
    weapon.magazine_size = magazine_size;
    weapon.damage = damage;
    weapon.cooldown_ticks = cooldown_ticks;
    weapon.reload_ticks = reload_ticks;
    weapon.pellet_count = 1;
    weapon.projectile_template_id = weapon_id;
    return weapon;
}

void fill_default_ammo(
    const WeaponCatalogConfig& weapons,
    KernelCombatStateDefinition* combat_state) {
    for (std::size_t index = 0; index < weapons.definitions.size(); ++index) {
        const KernelWeaponMechanicsDefinition& weapon = weapons.definitions[index];
        combat_state->ammo[index] = weapon.magazine_size;
        combat_state->reserve_ammo[index] =
            static_cast<std::uint16_t>(weapon.magazine_size * 3u);
    }
}

bool validate_weapon_mechanics(
    const KernelWeaponMechanicsDefinition& weapon) {
    if (weapon.struct_size < sizeof(KernelWeaponMechanicsDefinition) ||
        weapon.weapon_id >= kWeaponCount ||
        weapon.magazine_size == 0 ||
        weapon.damage == 0 ||
        weapon.cooldown_ticks == 0 ||
        weapon.reload_ticks == 0 ||
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
    if (value == "explosion") {
        throw std::runtime_error(
            "projectile damage_shape explosion has moved to impact_response");
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

bool impact_spawns_projectile_from_yaml(const YAML::Node& node) {
    const std::string value = node ? node.as<std::string>() : "none";
    if (value == "none") {
        return false;
    }
    if (value == "spawn_projectile") {
        return true;
    }
    throw std::runtime_error("unsupported projectile impact action: " + value);
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
    virtual std::string default_collider_path_for_weapon_dir(
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

    std::string default_collider_path_for_weapon_dir(
        const std::string& directory) const override {
        return (std::filesystem::path(directory).parent_path() /
                "collider_templates" /
                "default.yaml")
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
        for (ssize_t index = 0; index < total_entries; ++index) {
            if (zip_entry_openbyindex(archive.get(), static_cast<std::size_t>(index)) != 0) {
                throw std::runtime_error("failed to open gameplay catalog bundle entry");
            }

            const char* entry_name = zip_entry_name(archive.get());
            if (entry_name == nullptr) {
                throw std::runtime_error("gameplay catalog bundle entry has no name");
            }
            const std::string path = normalize_archive_path(entry_name);
            if (zip_entry_issymlink(archive.get())) {
                throw std::runtime_error("archive symlink entries are not supported: " + path);
            }
            if (zip_entry_isdir(archive.get())) {
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
            if (files_.contains(path)) {
                throw DataLoadError(
                    KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_DUPLICATE_ARCHIVE_ENTRY,
                    "duplicate archive entry: " + path,
                    path,
                    {},
                    KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE);
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

    std::string default_collider_path_for_weapon_dir(
        const std::string& directory) const override {
        return archive_join_path(
            archive_parent_path(directory),
            "collider_templates/default.yaml");
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

KernelWeaponMechanicsDefinition weapon_from_yaml(
    const YAML::Node& node,
    const std::string& path,
    std::uint32_t source_kind) {
    reject_unknown_keys(
        node,
        {
            "id",
            "name",
            "weapon_type",
            "magazine_size",
            "damage",
            "cooldown_ticks",
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
        },
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_WEAPON);
    const auto id = static_cast<std::uint8_t>(node["id"].as<int>());
    const std::uint16_t magazine_size = node["magazine_size"].as<std::uint16_t>();
    const std::uint32_t cooldown_ticks = node["cooldown_ticks"].as<std::uint32_t>();
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
            return shotgun_weapon(
                id,
                magazine_size,
                damage,
                cooldown_ticks,
                reload_ticks,
                node["max_range"].as<float>(),
                static_cast<std::uint8_t>(node["pellet_count"].as<int>()),
                node["pellet_spread"].as<float>());
        }
        return hitscan_weapon(
            id,
            magazine_size,
            damage,
            cooldown_ticks,
            reload_ticks,
            node["max_range"].as<float>());
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
        weapon.cooldown_ticks = cooldown_ticks;
        weapon.reload_ticks = reload_ticks;
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
        return area_effect_weapon(
            id,
            magazine_size,
            damage,
            cooldown_ticks,
            reload_ticks);
    }
    if (type == "beam") {
        if (node["projectile"] || node["area_effect"] || node["beam"]) {
            throw std::runtime_error(
                "beam weapons must use projectile_template, not inline mechanics");
        }
        if (!node["projectile_template"]) {
            throw std::runtime_error("beam weapon requires projectile_template");
        }
        return beam_weapon(
            id,
            magazine_size,
            damage,
            cooldown_ticks,
            reload_ticks);
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

std::uint16_t authored_entity_type_from_yaml(const YAML::Node& node) {
    const std::string value = node.as<std::string>();
    if (value == "actor") {
        return kEntityTypeActor;
    }
    if (value == "director") {
        return KernelEntityType_Director;
    }
    throw std::runtime_error("unsupported entity_type: " + value);
}

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
    const std::string& path) {
    const YAML::Node document = source.load_yaml(path);
    reject_unknown_keys(
        document,
        {"templates"},
        path,
        source.source_kind(),
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_COLLIDER);
    if (!document["templates"]) {
        throw std::runtime_error(
            "collider catalog requires templates: " + path);
    }

    ColliderCatalogConfig colliders;
    std::unordered_map<std::string, std::uint32_t> template_ids;
    std::unordered_map<std::uint32_t, std::string> template_names_by_id;
    for (const YAML::Node& node : document["templates"]) {
        reject_unknown_keys(
            node,
            {
                "id",
                "name",
                "shape",
                "center",
                "half_extents",
                "radius",
                "length",
                "scatter_degrees",
                "range",
                "fov_degrees",
                "lifetime_ticks",
                "purpose",
                "layer",
            },
            path,
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
                path,
                "id",
                source.source_kind(),
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_COLLIDER,
                definition.template_id);
        }
        template_ids[collider_template.name] = definition.template_id;
        template_names_by_id[definition.template_id] = collider_template.name;
        colliders.templates.push_back(collider_template);
    }
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
    actor_template.weapon_slots[0] = kWeaponRifle;
    actor_template.weapon_slots[1] = kWeaponShotgun;
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
    actor_template.weapon_slots[0] = kAgentSpammerWeaponId;
    actor_template.weapon_slot_count = 1;
    actor_template.active_weapon_slot = 0;
    actor_template.animation_idle = kAgentAnimationIdle;
    actor_template.animation_chasing = kAgentAnimationChasing;
    actor_template.sentry.weapon_id = kAgentSpammerWeaponId;
    actor_template.sentry.magazine_size = kAgentSpammerMagazine;
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

AgentSentryConfig sentry_config_from_yaml(
    const YAML::Node&,
    const ActorTemplateConfig& actor_template,
    const WeaponCatalogConfig& weapons) {
    AgentSentryConfig sentry = actor_template.sentry;
    const std::uint8_t weapon_id = active_weapon_id(actor_template);
    sentry.weapon_id = weapon_id;
    sentry.magazine_size = weapons.definitions[weapon_id].magazine_size;
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
    if (!movement || !movement["move_speed_meters_per_second"]) {
        throw std::runtime_error(
            "actor template requires movement.move_speed_meters_per_second: " +
            actor_template.name);
    }
    reject_unknown_keys(
        movement,
        {"move_speed_meters_per_second"},
        path,
        source_kind,
        KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR,
        actor_template.actor_template_id);
    actor_template.move_speed_meters_per_second =
        movement["move_speed_meters_per_second"].as<float>();

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
    if (weapon_slots.size() == 0 || weapon_slots.size() > actor_template.weapon_slots.size()) {
        throw std::runtime_error(
            "actor template weapon_slots count must be 1 to 4: " +
            actor_template.name);
    }
    actor_template.weapon_slot_count =
        static_cast<std::uint8_t>(weapon_slots.size());
    for (std::size_t index = 0; index < weapon_slots.size(); ++index) {
        const auto weapon_id =
            static_cast<std::uint8_t>(weapon_slots[index].as<int>());
        if (weapon_id >= kWeaponCount ||
            weapons.definitions[weapon_id].weapon_id != weapon_id) {
            throw std::runtime_error(
                "actor template references unknown weapon id: " +
                actor_template.name);
        }
        actor_template.weapon_slots[index] = weapon_id;
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
        sentry_config_from_yaml(node["ai"], actor_template, weapons);
    actor_template.sentry.animation_idle = actor_template.animation_idle;
    actor_template.sentry.animation_attack = actor_template.animation_chasing;
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
            "impact_response",
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
            "projectile template must use impact_response instead of removed radius field: " +
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
    mechanics.flags = 1u;

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

    const YAML::Node impact_response = node["impact_response"];
    if (impact_response) {
        reject_unknown_keys(
            impact_response,
            {"action", "projectile_template", "destroy_self"},
            path,
            source_kind,
            KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_PROJECTILE,
            definition.projectile_template_id);
        if (impact_response["destroy_self"] &&
            !impact_response["destroy_self"].as<bool>()) {
            mechanics.flags &= ~1u;
        }
        if (impact_spawns_projectile_from_yaml(impact_response["action"])) {
            if (!impact_response["projectile_template"]) {
                throw std::runtime_error(
                    "spawn_projectile impact response requires projectile_template: " +
                    projectile_template.name);
            }
            projectile_template.impact_projectile_template_ref =
                impact_response["projectile_template"].as<std::string>();
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

std::vector<ProjectileTemplateConfig> load_projectile_templates_from_source(
    const GameplayConfigSource& source,
    const std::string& directory,
    const ColliderCatalogConfig& colliders) {
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
        if (projectile_template.impact_projectile_template_ref.empty()) {
            continue;
        }
        YAML::Node ref_node(projectile_template.impact_projectile_template_ref);
        ProjectileTemplateConfig* impact_template =
            projectile_template_from_ref(ref_node, &projectile_templates);
        projectile_template.definition.mechanics.impact_spawn_projectile_template_id =
            impact_template->definition.projectile_template_id;
    }
    for (const ProjectileTemplateConfig& projectile_template : projectile_templates) {
        std::vector<std::uint32_t> visited;
        const ProjectileTemplateConfig* current = &projectile_template;
        while (current != nullptr &&
               current->definition.mechanics.impact_spawn_projectile_template_id != 0u) {
            const std::uint32_t current_id =
                current->definition.projectile_template_id;
            if (std::find(visited.begin(), visited.end(), current_id) !=
                visited.end()) {
                throw std::runtime_error(
                    "projectile impact_response cycle: " +
                    projectile_template.name);
            }
            visited.push_back(current_id);
            current = projectile_template_from_ref(
                YAML::Node(std::to_string(
                    current->definition.mechanics
                        .impact_spawn_projectile_template_id)),
                &projectile_templates);
        }
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
    WeaponCatalogConfig* weapons) {
    const std::vector<std::string> files = source.list_yaml_files(directory);
    for (const std::string& file : files) {
        const YAML::Node document = source.load_yaml(file);
        const auto weapon_id =
            static_cast<std::uint8_t>(document["id"].as<int>());
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
    std::array<bool, kWeaponCount> seen{
        false,
        false,
        false,
        false,
        false,
        false,
        false};
    const std::vector<std::string> files = source.list_yaml_files(directory);
    for (const std::string& file : files) {
        const YAML::Node document = source.load_yaml(file);
        const KernelWeaponMechanicsDefinition weapon =
            weapon_from_yaml(document, file, source.source_kind());
        if (weapon.weapon_id >= kWeaponCount) {
            throw std::runtime_error("weapon id out of range: " + file);
        }
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
        weapons.definitions[weapon.weapon_id] = weapon;
        weapons.projectile_sync_modes[weapon.weapon_id] =
            projectile_sync_mode_from_weapon_yaml(document);
        weapons.names[weapon.weapon_id] = name;
    }
    for (std::size_t index = 0; index < seen.size(); ++index) {
        if (!seen[index]) {
            throw std::runtime_error("missing weapon template id " + std::to_string(index));
        }
    }
    return weapons;
}

GameServerGameplayConfig load_gameplay_config_from_weapon_template_source(
    const GameplayConfigSource& source,
    const std::string& directory) {
    GameServerGameplayConfig config;
    config.weapons = load_weapon_catalog_from_source(source, directory);
    apply_default_non_weapon_config(&config);
    config.colliders = load_collider_catalog_from_source(
        source,
        source.default_collider_path_for_weapon_dir(directory));
    config.projectile_templates = load_projectile_templates_from_source(
        source,
        source.default_projectile_template_dir_for_weapon_dir(directory),
        config.colliders);
    apply_weapon_template_references(
        source,
        directory,
        config.colliders,
        &config.projectile_templates,
        &config.weapons);
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
            "weapon_template_dir",
            "projectile_template_dir",
            "actor_template_dir",
            "entity_template_dir",
            "collider_template_file",
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
    const std::uint32_t catalog_version =
        document["catalog_version"] ? document["catalog_version"].as<std::uint32_t>()
                                    : 1u;
    if (catalog_version != 1u) {
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
        !document["collider_template_file"]) {
        throw std::runtime_error(
            "gameplay catalog requires weapon_template_dir and "
            "projectile_template_dir and collider_template_file: " +
            path);
    }

    const std::string base_path = source.parent_path(path);
    const std::string weapon_template_dir =
        source.resolve_path(base_path, document["weapon_template_dir"]);
    GameServerGameplayConfig config;
    config.weapons = load_weapon_catalog_from_source(source, weapon_template_dir);
    apply_default_non_weapon_config(&config);
    config.weapons.catalog_version = catalog_version;

    const std::string collider_template_file =
        source.resolve_path(base_path, document["collider_template_file"]);
    config.colliders =
        load_collider_catalog_from_source(source, collider_template_file);
    const std::string projectile_template_dir =
        source.resolve_path(base_path, document["projectile_template_dir"]);
    config.projectile_templates = load_projectile_templates_from_source(
        source,
        projectile_template_dir,
        config.colliders);
    apply_weapon_template_references(
        source,
        weapon_template_dir,
        config.colliders,
        &config.projectile_templates,
        &config.weapons);

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

    config.weapons.catalog_hash = compute_gameplay_catalog_hash(config);
    const std::vector<std::string> errors = validate_gameplay_config(config);
    if (!errors.empty()) {
        throw std::runtime_error(errors.front());
    }
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
    hash_scalar(&hash, config.player.actor_template_id);
    hash_scalar(&hash, config.agent.actor_template_id);
    hash_vec3(&hash, config.agent.spawn_position);
    hash_scalar(&hash, config.agent.spawn_count);
    hash_float(&hash, config.agent.spawn_radius);
    hash_scalar(&hash, config.agent.spawn_seed);
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
    return actor_template.weapon_slots[actor_template.active_weapon_slot];
}

std::vector<std::string> validate_gameplay_config(
    const GameServerGameplayConfig& config) {
    std::vector<std::string> errors;
    for (std::size_t index = 0; index < config.weapons.definitions.size(); ++index) {
        const KernelWeaponMechanicsDefinition& weapon =
            config.weapons.definitions[index];
        if (weapon.weapon_id != index) {
            errors.push_back("weapon id must match catalog index");
        }
        if (!validate_weapon_mechanics(weapon)) {
            errors.push_back("weapon mechanics must be valid");
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
            actor_template.hitbox_half_extents.x <= 0.0f ||
            actor_template.hitbox_half_extents.y <= 0.0f ||
            actor_template.hitbox_half_extents.z <= 0.0f ||
            actor_template.weapon_slot_count == 0 ||
            actor_template.weapon_slot_count > actor_template.weapon_slots.size() ||
            actor_template.active_weapon_slot >= actor_template.weapon_slot_count) {
            errors.push_back("actor template must be valid");
        }
        for (std::uint8_t slot = 0; slot < actor_template.weapon_slot_count; ++slot) {
            if (actor_template.weapon_slots[slot] >= kWeaponCount) {
                errors.push_back("actor template weapon slot must reference a valid weapon");
            }
        }
        if (actor_template.actor_type == kActorTypeAgent &&
            (actor_template.sentry.weapon_id >= kWeaponCount ||
             actor_template.sentry.magazine_size == 0 ||
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
            definition.shape_type > KernelColliderShapeType_Cone ||
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
             mechanics.damage_shape != KernelProjectileDamageShape_PiercingSegment) ||
            mechanics.damage_falloff > KernelProjectileDamageFalloff_Linear ||
            mechanics.collision_query_mode > KernelProjectileCollisionQueryMode_Ray ||
            mechanics.damage == 0 ||
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
            (mechanics.impact_spawn_projectile_template_id == 0 &&
             !projectile_template.impact_projectile_template_ref.empty()) ||
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
    for (const KernelWeaponMechanicsDefinition& weapon :
         config.weapons.definitions) {
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

        if (authored_template.entity_type == kEntityTypeActor) {
            entity_template.component_flags =
                KERNEL_ENTITY_COMPONENT_TRANSFORM |
                KERNEL_ENTITY_COMPONENT_VELOCITY |
                KERNEL_ENTITY_COMPONENT_HEALTH |
                KERNEL_ENTITY_COMPONENT_HITBOX |
                KERNEL_ENTITY_COMPONENT_WEAPON_STATE;
            entity_template.animation_state = authored_template.animation_idle;
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
    return storage;
}

bool load_kernel_gameplay_catalog(
    KernelHandle* kernel,
    const GameServerGameplayConfig& config) {
    if (kernel == nullptr || config.weapons.catalog_hash == 0) {
        return false;
    }
    KernelGameplayCatalogStorage storage = build_kernel_gameplay_catalog(config);
    return Kernel_LoadGameplayCatalog(kernel, &storage.definition);
}

KernelCombatStateDefinition make_combat_state_from_actor_template(
    const GameServerGameplayConfig& config,
    const ActorTemplateConfig& actor_template) {
    KernelCombatStateDefinition combat_state{};
    combat_state.struct_size = sizeof(KernelCombatStateDefinition);
    combat_state.hp = actor_template.health.hp;
    combat_state.max_hp = actor_template.health.max_hp;
    combat_state.active_weapon_id = active_weapon_id(actor_template);
    combat_state.collider_template_id = actor_template.collider_template_id;
    combat_state.move_speed_meters_per_second =
        actor_template.move_speed_meters_per_second;
    combat_state.hitbox_center = actor_template.hitbox_center;
    combat_state.hitbox_half_extents = actor_template.hitbox_half_extents;
    for (std::uint8_t slot = 0; slot < actor_template.weapon_slot_count; ++slot) {
        const std::uint8_t weapon_id = actor_template.weapon_slots[slot];
        const KernelWeaponMechanicsDefinition& weapon =
            config.weapons.definitions[weapon_id];
        combat_state.ammo[weapon_id] = weapon.magazine_size;
        combat_state.reserve_ammo[weapon_id] =
            static_cast<std::uint16_t>(weapon.magazine_size * 2u);
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
