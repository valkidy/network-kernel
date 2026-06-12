#include "game_server/gameplay_config.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <yaml-cpp/yaml.h>
#include <zip.h>

#include "kernel/public/kernel_api.h"

namespace network_example::game_server {
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

    hash_scalar(hash, weapon.projectile.motion_model);
    hash_scalar(hash, weapon.projectile.hit_response);
    hash_scalar(hash, weapon.projectile.damage_shape);
    hash_float(hash, weapon.projectile.speed);
    hash_float(hash, weapon.projectile.lifetime_seconds);
    hash_float(hash, weapon.projectile.explosion_radius);
    hash_vec3(hash, weapon.projectile.gravity);
    hash_scalar(hash, weapon.projectile.collision_mask);
    hash_scalar(hash, weapon.projectile.max_hit_count);
    hash_scalar(hash, weapon.projectile.homing.homing_mode);
    hash_scalar(hash, weapon.projectile.homing.sync_mode);
    hash_scalar(hash, weapon.projectile.homing.boost_ticks);
    hash_float(hash, weapon.projectile.homing.lock_on_range);
    hash_float(hash, weapon.projectile.homing.lose_target_range);
    hash_float(hash, weapon.projectile.homing.lock_cone_degrees);
    hash_float(hash, weapon.projectile.homing.max_turn_rate_degrees_per_second);
    hash_float(hash, weapon.projectile.homing.acceleration);
    hash_float(hash, weapon.projectile.homing.max_speed);

    hash_float(hash, weapon.area_effect.radius);
    hash_scalar(hash, weapon.area_effect.damage_per_interval);
    hash_scalar(hash, weapon.area_effect.damage_interval_ticks);
    hash_scalar(hash, weapon.area_effect.lifetime_ticks);
    hash_float(hash, weapon.area_effect.spawn_distance);
    hash_scalar(hash, weapon.area_effect.collision_mask);

    hash_float(hash, weapon.beam.length);
    hash_float(hash, weapon.beam.radius);
    hash_scalar(hash, weapon.beam.damage_per_second);
    hash_scalar(hash, weapon.beam.lifetime_ticks);
    hash_scalar(hash, weapon.beam.collision_mask);
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
    hash_vec3(hash, definition.half_extents);
    hash_float(hash, definition.radius);
    hash_scalar(hash, definition.purpose_flags);
    hash_scalar(hash, definition.layer_mask);
}

void hash_collider_binding(
    std::uint64_t* hash,
    const ColliderBindingConfig& collider_binding) {
    const KernelColliderBindingDefinition& definition =
        collider_binding.definition;
    hash_scalar(hash, definition.entity_type);
    hash_scalar(hash, definition.collider_template_id);
    hash_vec3(hash, definition.local_position);
    hash_float(hash, definition.local_rotation.x);
    hash_float(hash, definition.local_rotation.y);
    hash_float(hash, definition.local_rotation.z);
    hash_float(hash, definition.local_rotation.w);
}

void hash_actor_template(
    std::uint64_t* hash,
    const ActorTemplateConfig& actor_template) {
    hash_scalar(hash, actor_template.actor_template_id);
    hash_string(hash, actor_template.name);
    hash_scalar(hash, actor_template.entity_type);
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
    hash_scalar(hash, actor_template.ai.profile);
    hash_float(hash, actor_template.ai.chase_range);
    hash_float(hash, actor_template.ai.move_speed);
    hash_float(hash, actor_template.ai.patrol_speed);
    hash_float(hash, actor_template.ai.patrol_half_extent);
    hash_float(hash, actor_template.ai.fire_interval_seconds);
    hash_float(hash, actor_template.ai.reload_seconds);
    hash_scalar(hash, actor_template.ai.weapon_id);
    hash_scalar(hash, actor_template.ai.magazine_size);
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

KernelWeaponMechanicsDefinition projectile_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size,
    std::uint16_t damage,
    std::uint32_t cooldown_ticks,
    std::uint32_t reload_ticks,
    float projectile_speed,
    float projectile_lifetime_seconds,
    float explosion_radius,
    std::uint8_t motion_model,
    KernelVec3 gravity) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    weapon.weapon_id = weapon_id;
    weapon.fire_mode = KernelWeaponFireMode_Projectile;
    weapon.magazine_size = magazine_size;
    weapon.damage = damage;
    weapon.cooldown_ticks = cooldown_ticks;
    weapon.reload_ticks = reload_ticks;
    weapon.pellet_count = 1;
    weapon.projectile.struct_size = sizeof(KernelProjectileMechanicsDefinition);
    weapon.projectile.motion_model = motion_model;
    weapon.projectile.speed = projectile_speed;
    weapon.projectile.lifetime_seconds = projectile_lifetime_seconds;
    weapon.projectile.explosion_radius = explosion_radius;
    weapon.projectile.gravity = gravity;
    weapon.projectile.hit_response = KernelProjectileHitResponse_Destroy;
    weapon.projectile.damage_shape =
        explosion_radius > 0.0f ? KernelProjectileDamageShape_Explosion
                                : KernelProjectileDamageShape_DirectHit;
    weapon.projectile.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    weapon.projectile.max_hit_count = 1;
    return weapon;
}

KernelWeaponMechanicsDefinition area_effect_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size,
    std::uint16_t damage,
    std::uint32_t cooldown_ticks,
    std::uint32_t reload_ticks,
    float radius,
    std::uint16_t damage_per_interval,
    std::uint32_t damage_interval_ticks,
    std::uint32_t lifetime_ticks,
    float spawn_distance,
    std::uint32_t collision_mask) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    weapon.weapon_id = weapon_id;
    weapon.fire_mode = KernelWeaponFireMode_AreaEffect;
    weapon.magazine_size = magazine_size;
    weapon.damage = damage;
    weapon.cooldown_ticks = cooldown_ticks;
    weapon.reload_ticks = reload_ticks;
    weapon.pellet_count = 1;
    weapon.area_effect.struct_size = sizeof(KernelAreaEffectMechanicsDefinition);
    weapon.area_effect.radius = radius;
    weapon.area_effect.damage_per_interval = damage_per_interval;
    weapon.area_effect.damage_interval_ticks = damage_interval_ticks;
    weapon.area_effect.lifetime_ticks = lifetime_ticks;
    weapon.area_effect.spawn_distance = spawn_distance;
    weapon.area_effect.collision_mask = collision_mask;
    return weapon;
}

KernelWeaponMechanicsDefinition beam_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size,
    std::uint16_t damage,
    std::uint32_t cooldown_ticks,
    std::uint32_t reload_ticks,
    float length,
    float radius,
    std::uint16_t damage_per_second,
    std::uint32_t lifetime_ticks,
    std::uint32_t collision_mask) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    weapon.weapon_id = weapon_id;
    weapon.fire_mode = KernelWeaponFireMode_Beam;
    weapon.magazine_size = magazine_size;
    weapon.damage = damage;
    weapon.cooldown_ticks = cooldown_ticks;
    weapon.reload_ticks = reload_ticks;
    weapon.pellet_count = 1;
    weapon.beam.struct_size = sizeof(KernelBeamMechanicsDefinition);
    weapon.beam.length = length;
    weapon.beam.radius = radius;
    weapon.beam.damage_per_second = damage_per_second;
    weapon.beam.lifetime_ticks = lifetime_ticks;
    weapon.beam.collision_mask = collision_mask;
    return weapon;
}

KernelWeaponMechanicsDefinition homing_projectile_weapon(
    std::uint8_t weapon_id,
    std::uint16_t magazine_size,
    std::uint16_t damage,
    std::uint32_t cooldown_ticks,
    std::uint32_t reload_ticks,
    float projectile_speed,
    float projectile_lifetime_seconds,
    float explosion_radius,
    std::uint32_t boost_ticks,
    float lock_on_range,
    float lose_target_range,
    float lock_cone_degrees,
    float max_turn_rate_degrees_per_second,
    float acceleration,
    float max_speed,
    std::uint32_t collision_mask) {
    KernelWeaponMechanicsDefinition weapon = projectile_weapon(
        weapon_id,
        magazine_size,
        damage,
        cooldown_ticks,
        reload_ticks,
        projectile_speed,
        projectile_lifetime_seconds,
        explosion_radius,
        KernelProjectileMotionModel_Homing,
        KernelVec3{0.0f, 0.0f, 0.0f});
    weapon.projectile.damage_shape =
        explosion_radius > 0.0f ? KernelProjectileDamageShape_Explosion
                                : KernelProjectileDamageShape_DirectHit;
    weapon.projectile.collision_mask = collision_mask;
    weapon.projectile.homing.struct_size = sizeof(KernelHomingMechanicsDefinition);
    weapon.projectile.homing.homing_mode = KernelHomingMode_FireAndForget;
    weapon.projectile.homing.sync_mode =
        KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
    weapon.projectile.homing.boost_ticks = boost_ticks;
    weapon.projectile.homing.lock_on_range = lock_on_range;
    weapon.projectile.homing.lose_target_range = lose_target_range;
    weapon.projectile.homing.lock_cone_degrees = lock_cone_degrees;
    weapon.projectile.homing.max_turn_rate_degrees_per_second =
        max_turn_rate_degrees_per_second;
    weapon.projectile.homing.acceleration = acceleration;
    weapon.projectile.homing.max_speed = max_speed;
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
            weapon.fire_mode > KernelWeaponFireMode_Beam) {
        return false;
    }
    if (weapon.fire_mode == KernelWeaponFireMode_Projectile) {
        return weapon.projectile.struct_size >=
                   sizeof(KernelProjectileMechanicsDefinition) &&
               weapon.projectile.motion_model <= KernelProjectileMotionModel_Homing &&
               weapon.projectile.hit_response <= KernelProjectileHitResponse_Attach &&
               weapon.projectile.hit_response != KernelProjectileHitResponse_Bounce &&
               weapon.projectile.hit_response != KernelProjectileHitResponse_Attach &&
               weapon.projectile.damage_shape <= KernelProjectileDamageShape_PiercingSegment &&
               weapon.projectile.max_hit_count > 0 &&
               weapon.projectile.speed > 0.0f &&
               weapon.projectile.lifetime_seconds > 0.0f &&
               (weapon.projectile.motion_model != KernelProjectileMotionModel_Homing
                    ? weapon.projectile.homing.struct_size == 0
                    : weapon.projectile.homing.struct_size >=
                              sizeof(KernelHomingMechanicsDefinition) &&
                          weapon.projectile.homing.homing_mode ==
                              KernelHomingMode_FireAndForget &&
                          weapon.projectile.homing.sync_mode <=
                              KernelProjectileSyncMode_ServerSnapshotOnly &&
                          weapon.projectile.homing.lock_on_range > 0.0f &&
                          weapon.projectile.homing.lose_target_range >=
                              weapon.projectile.homing.lock_on_range &&
                          weapon.projectile.homing.lock_cone_degrees > 0.0f &&
                          weapon.projectile.homing.lock_cone_degrees <= 180.0f &&
                          weapon.projectile.homing
                                  .max_turn_rate_degrees_per_second > 0.0f &&
                          weapon.projectile.homing.acceleration > 0.0f &&
                          weapon.projectile.homing.max_speed > 0.0f);
    }
    if (weapon.fire_mode == KernelWeaponFireMode_AreaEffect) {
        return weapon.area_effect.struct_size >=
                   sizeof(KernelAreaEffectMechanicsDefinition) &&
               weapon.area_effect.radius > 0.0f &&
               weapon.area_effect.damage_per_interval > 0 &&
               weapon.area_effect.damage_interval_ticks > 0 &&
               weapon.area_effect.lifetime_ticks > 0 &&
               weapon.area_effect.spawn_distance >= 0.0f;
    }
    if (weapon.fire_mode == KernelWeaponFireMode_Beam) {
        return weapon.beam.struct_size >= sizeof(KernelBeamMechanicsDefinition) &&
               weapon.beam.length > 0.0f &&
               weapon.beam.radius > 0.0f &&
               weapon.beam.damage_per_second > 0 &&
               weapon.beam.lifetime_ticks > 0;
    }
    if (weapon.fire_mode != KernelWeaponFireMode_Beam &&
        weapon.max_range <= 0.0f) {
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
    if (token == "enemy") {
        return KERNEL_COLLISION_LAYER_ENEMY;
    }
    if (token == "player") {
        return KERNEL_COLLISION_LAYER_PLAYER;
    }
    if (token == "projectile") {
        return KERNEL_COLLISION_LAYER_PROJECTILE;
    }
    if (token == "area_effect") {
        return KERNEL_COLLISION_LAYER_AREA_EFFECT;
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
        return KernelProjectileDamageShape_Explosion;
    }
    if (value == "piercing_segment") {
        return KernelProjectileDamageShape_PiercingSegment;
    }
    if (value == "persistent_beam") {
        throw std::runtime_error("beam damage shape is not supported in this phase");
    }
    throw std::runtime_error("unsupported projectile damage_shape: " + value);
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

std::uint8_t projectile_sync_mode_from_weapon_yaml(const YAML::Node& node) {
    if (!node || node["weapon_type"].as<std::string>() != "projectile") {
        return KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
    }
    const YAML::Node projectile = node["projectile"];
    const std::uint8_t projectile_sync_mode =
        projectile_sync_mode_from_yaml(projectile["sync_mode"]);
    if (projectile["homing"] && projectile["homing"]["sync_mode"]) {
        const std::uint8_t homing_sync_mode =
            projectile_sync_mode_from_yaml(projectile["homing"]["sync_mode"]);
        if (homing_sync_mode != projectile_sync_mode) {
            throw std::runtime_error(
                "projectile sync_mode must match homing sync_mode");
        }
    }
    return projectile_sync_mode;
}

EnemyAiProfile enemy_ai_profile_from_yaml(const YAML::Node& node) {
    const std::string value = node ? node.as<std::string>() : "default";
    if (value == "default") {
        return EnemyAiProfile::kDefault;
    }
    throw std::runtime_error("unsupported enemy ai_profile: " + value);
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
};

class FilesystemGameplayConfigSource final : public GameplayConfigSource {
public:
    YAML::Node load_yaml(const std::string& path) const override {
        return YAML::LoadFile(path);
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
};

std::string normalize_archive_path(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find(':') != std::string::npos) {
        throw std::runtime_error("invalid archive path: " + path);
    }

    std::vector<std::string> parts;
    std::string part;
    for (const char value : path) {
        const char normalized = value == '\\' ? '/' : value;
        if (normalized == '/') {
            if (!part.empty() && part != ".") {
                if (part == "..") {
                    throw std::runtime_error("invalid archive path: " + path);
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
            throw std::runtime_error("invalid archive path: " + path);
        }
        parts.push_back(part);
    }
    if (parts.empty()) {
        throw std::runtime_error("invalid archive path: " + path);
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
                throw std::runtime_error("duplicate archive entry: " + path);
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
            throw std::runtime_error("missing YAML file in bundle: " + normalized);
        }
        return YAML::Load(found->second);
    }

    std::vector<std::string> list_yaml_files(
        const std::string& directory) const override {
        const std::string normalized_directory = normalize_archive_path(directory);
        const std::string prefix = normalized_directory + "/";
        std::vector<std::string> files;
        for (const auto& [path, contents] : files_) {
            (void)contents;
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

private:
    std::unordered_map<std::string, std::string> files_;
};

KernelWeaponMechanicsDefinition weapon_from_yaml(const YAML::Node& node) {
    const auto id = static_cast<std::uint8_t>(node["id"].as<int>());
    const std::uint16_t magazine_size = node["magazine_size"].as<std::uint16_t>();
    const std::uint16_t damage = node["damage"].as<std::uint16_t>();
    const std::uint32_t cooldown_ticks = node["cooldown_ticks"].as<std::uint32_t>();
    const std::uint32_t reload_ticks = node["reload_ticks"].as<std::uint32_t>();
    const std::string type = node["weapon_type"].as<std::string>();
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
        const YAML::Node projectile = node["projectile"];
        if (!projectile) {
            throw std::runtime_error("projectile weapon requires projectile block");
        }
        KernelWeaponMechanicsDefinition weapon = projectile_weapon(
            id,
            magazine_size,
            damage,
            cooldown_ticks,
            reload_ticks,
            projectile["speed"].as<float>(),
            projectile["lifetime_seconds"].as<float>(),
            projectile["explosion_radius"] ? projectile["explosion_radius"].as<float>() : 0.0f,
            motion_model_from_yaml(projectile["movement_model"]),
            vec3_from_yaml(projectile["gravity"]));
        weapon.pellet_count =
            node["burst_count"]
                ? static_cast<std::uint8_t>(node["burst_count"].as<int>())
                : 1;
        weapon.pellet_spread =
            node["burst_spread_degrees"] ? node["burst_spread_degrees"].as<float>() : 0.0f;
        weapon.projectile.hit_response = hit_response_from_yaml(projectile["hit_response"]);
        weapon.projectile.damage_shape = damage_shape_from_yaml(projectile["damage_shape"]);
        weapon.projectile.collision_mask =
            collision_mask_from_yaml(projectile["collision_mask"]);
        weapon.projectile.max_hit_count =
            projectile["max_hit_count"] ? projectile["max_hit_count"].as<std::uint32_t>() : 1u;
        if (weapon.projectile.motion_model == KernelProjectileMotionModel_Homing) {
            const YAML::Node homing = projectile["homing"];
            if (!homing) {
                throw std::runtime_error("homing projectile requires homing block");
            }
            weapon.projectile.homing.struct_size =
                sizeof(KernelHomingMechanicsDefinition);
            weapon.projectile.homing.homing_mode =
                homing_mode_from_yaml(homing["homing_mode"]);
            weapon.projectile.homing.sync_mode =
                projectile_sync_mode_from_yaml(
                    homing["sync_mode"] ? homing["sync_mode"]
                                        : projectile["sync_mode"]);
            weapon.projectile.homing.boost_ticks =
                homing["boost_ticks"].as<std::uint32_t>();
            weapon.projectile.homing.lock_on_range =
                homing["lock_on_range"].as<float>();
            weapon.projectile.homing.lose_target_range =
                homing["lose_target_range"].as<float>();
            weapon.projectile.homing.lock_cone_degrees =
                homing["lock_cone_degrees"].as<float>();
            weapon.projectile.homing.max_turn_rate_degrees_per_second =
                homing["max_turn_rate_degrees_per_second"].as<float>();
            weapon.projectile.homing.acceleration =
                homing["acceleration"].as<float>();
            weapon.projectile.homing.max_speed = homing["max_speed"].as<float>();
        } else if (projectile["homing"]) {
            throw std::runtime_error("homing block requires movement_model: homing");
        }
        return weapon;
    }
    if (type == "area_effect") {
        if (node["projectile"] || node["beam"]) {
            throw std::runtime_error(
                "area_effect weapons must not define projectile or beam");
        }
        const YAML::Node area_effect = node["area_effect"];
        if (!area_effect) {
            throw std::runtime_error("area_effect weapon requires area_effect block");
        }
        return area_effect_weapon(
            id,
            magazine_size,
            damage,
            cooldown_ticks,
            reload_ticks,
            area_effect["radius"].as<float>(),
            area_effect["damage_per_interval"].as<std::uint16_t>(),
            area_effect["damage_interval_ticks"].as<std::uint32_t>(),
            area_effect["lifetime_ticks"].as<std::uint32_t>(),
            area_effect["spawn_distance"].as<float>(),
            collision_mask_from_yaml(area_effect["collision_mask"]));
    }
    if (type == "beam") {
        const YAML::Node beam = node["beam"];
        if (!beam) {
            throw std::runtime_error("beam weapon requires beam block");
        }
        return beam_weapon(
            id,
            magazine_size,
            damage,
            cooldown_ticks,
            reload_ticks,
            beam["length"].as<float>(),
            beam["radius"].as<float>(),
            beam["damage_per_second"].as<std::uint16_t>(),
            beam["lifetime_ticks"] ? beam["lifetime_ticks"].as<std::uint32_t>() : 2u,
            collision_mask_from_yaml(beam["collision_mask"]));
    }
    throw std::runtime_error("unsupported weapon_type: " + type);
}

std::uint16_t entity_type_from_yaml(const YAML::Node& node) {
    const std::string value = node.as<std::string>();
    if (value == "player") {
        return kEntityTypePlayer;
    }
    if (value == "enemy") {
        return kEntityTypeEnemy;
    }
    if (value == "projectile") {
        return 3;
    }
    if (value == "area_effect" || value == "explosion") {
        return 4;
    }
    throw std::runtime_error("unsupported collider entity_type: " + value);
}

std::uint8_t collider_shape_type_from_yaml(const YAML::Node& node) {
    const std::string value = node.as<std::string>();
    if (value == "aabb") {
        return KernelColliderShapeType_Aabb;
    }
    if (value == "sphere") {
        return KernelColliderShapeType_Sphere;
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
    throw std::runtime_error("unsupported collider purpose: " + value);
}

std::uint32_t collider_layer_from_yaml(const YAML::Node& node) {
    return collision_mask_from_yaml(node);
}

void apply_default_non_weapon_config(GameServerGameplayConfig* config);
void apply_catalog_player_config(
    const YAML::Node& document,
    GameServerGameplayConfig* config);
void apply_catalog_enemy_config(
    const YAML::Node& document,
    GameServerGameplayConfig* config);

ColliderCatalogConfig load_collider_catalog_from_source(
    const GameplayConfigSource& source,
    const std::string& path) {
    const YAML::Node document = source.load_yaml(path);
    if (!document["templates"] || !document["bindings"]) {
        throw std::runtime_error(
            "collider catalog requires templates and bindings: " + path);
    }

    ColliderCatalogConfig colliders;
    std::unordered_map<std::string, std::uint32_t> template_ids;
    for (const YAML::Node& node : document["templates"]) {
        ColliderTemplateConfig collider_template;
        collider_template.name = node["name"].as<std::string>();
        KernelColliderTemplateDefinition& definition =
            collider_template.definition;
        definition.struct_size = sizeof(KernelColliderTemplateDefinition);
        definition.template_id = node["id"].as<std::uint32_t>();
        definition.shape_type = collider_shape_type_from_yaml(node["shape"]);
        definition.center = vec3_from_yaml(node["center"]);
        definition.half_extents = vec3_from_yaml(node["half_extents"]);
        definition.radius = node["radius"] ? node["radius"].as<float>() : 0.0f;
        definition.purpose_flags = collider_purpose_from_yaml(node["purpose"]);
        definition.layer_mask = collider_layer_from_yaml(node["layer"]);
        if (template_ids.contains(collider_template.name)) {
            throw std::runtime_error(
                "duplicate collider template name: " + collider_template.name);
        }
        template_ids[collider_template.name] = definition.template_id;
        colliders.templates.push_back(collider_template);
    }

    for (const YAML::Node& node : document["bindings"]) {
        const std::string template_name =
            node["collider_template"].as<std::string>();
        const auto found = template_ids.find(template_name);
        if (found == template_ids.end()) {
            throw std::runtime_error(
                "unknown collider template binding: " + template_name);
        }
        ColliderBindingConfig binding;
        KernelColliderBindingDefinition& definition = binding.definition;
        definition.struct_size = sizeof(KernelColliderBindingDefinition);
        definition.entity_type = entity_type_from_yaml(node["entity_type"]);
        definition.collider_template_id = found->second;
        definition.local_position = vec3_from_yaml(node["local_position"]);
        definition.local_rotation =
            KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        colliders.bindings.push_back(binding);
    }
    return colliders;
}

ActorTemplateConfig default_player_actor_template() {
    ActorTemplateConfig actor_template;
    actor_template.actor_template_id = 1;
    actor_template.name = "player";
    actor_template.entity_type = kEntityTypePlayer;
    actor_template.health = EntityHealthDefinition{100, 100};
    actor_template.hitbox_center = KernelVec3{0.0f, 0.9f, 0.0f};
    actor_template.hitbox_half_extents = KernelVec3{0.35f, 0.9f, 0.35f};
    actor_template.move_speed_meters_per_second = 5.0f;
    actor_template.weapon_slots[0] = kWeaponRifle;
    actor_template.weapon_slots[1] = kWeaponShotgun;
    actor_template.weapon_slot_count = 2;
    actor_template.active_weapon_slot = 0;
    return actor_template;
}

ActorTemplateConfig default_enemy_actor_template() {
    ActorTemplateConfig actor_template;
    actor_template.actor_template_id = 2;
    actor_template.name = "enemy_grunt";
    actor_template.entity_type = kEntityTypeEnemy;
    actor_template.health =
        EntityHealthDefinition{kEnemyInitialHp, kEnemyInitialHp};
    actor_template.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    actor_template.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    actor_template.move_speed_meters_per_second = 2.5f;
    actor_template.weapon_slots[0] = kEnemyRocketWeaponId;
    actor_template.weapon_slot_count = 1;
    actor_template.active_weapon_slot = 0;
    actor_template.animation_idle = kEnemyAnimationIdle;
    actor_template.animation_chasing = kEnemyAnimationChasing;
    actor_template.ai.weapon_id = kEnemyRocketWeaponId;
    actor_template.ai.magazine_size = kEnemyRocketMagazine;
    actor_template.ai.animation_idle = actor_template.animation_idle;
    actor_template.ai.animation_chasing = actor_template.animation_chasing;
    return actor_template;
}

void apply_default_actor_templates(GameServerGameplayConfig* config) {
    config->actor_templates.clear();
    config->actor_templates.push_back(default_player_actor_template());
    config->actor_templates.push_back(default_enemy_actor_template());
    config->player.actor_template_id = 1;
    config->enemy.actor_template_id = 2;
}

EnemyAiConfig ai_config_from_yaml(
    const YAML::Node& node,
    const ActorTemplateConfig& actor_template,
    const WeaponCatalogConfig& weapons) {
    EnemyAiConfig ai = actor_template.ai;
    if (node) {
        ai.profile = enemy_ai_profile_from_yaml(node["profile"]);
        if (node["chase_range"]) {
            ai.chase_range = node["chase_range"].as<float>();
        }
        if (node["move_speed"]) {
            ai.move_speed = node["move_speed"].as<float>();
        }
        if (node["patrol_speed"]) {
            ai.patrol_speed = node["patrol_speed"].as<float>();
        }
        if (node["patrol_half_extent"]) {
            ai.patrol_half_extent = node["patrol_half_extent"].as<float>();
        }
        if (node["fire_interval_seconds"]) {
            ai.fire_interval_seconds = node["fire_interval_seconds"].as<float>();
        }
        if (node["reload_seconds"]) {
            ai.reload_seconds = node["reload_seconds"].as<float>();
        }
    }
    const std::uint8_t weapon_id = active_weapon_id(actor_template);
    ai.weapon_id = weapon_id;
    ai.magazine_size = weapons.definitions[weapon_id].magazine_size;
    if (!node || !node["move_speed"]) {
        ai.move_speed = actor_template.move_speed_meters_per_second;
    }
    return ai;
}

ActorTemplateConfig actor_template_from_yaml(
    const YAML::Node& node,
    const WeaponCatalogConfig& weapons) {
    ActorTemplateConfig actor_template;
    actor_template.actor_template_id = node["id"].as<std::uint32_t>();
    actor_template.name = node["name"].as<std::string>();
    actor_template.entity_type = entity_type_from_yaml(node["entity_type"]);

    const YAML::Node health = node["health"];
    if (!health) {
        throw std::runtime_error(
            "actor template requires health: " + actor_template.name);
    }
    actor_template.health.hp = health["hp"].as<std::uint16_t>();
    actor_template.health.max_hp = health["max_hp"].as<std::uint16_t>();

    const YAML::Node movement = node["movement"];
    if (!movement || !movement["move_speed_meters_per_second"]) {
        throw std::runtime_error(
            "actor template requires movement.move_speed_meters_per_second: " +
            actor_template.name);
    }
    actor_template.move_speed_meters_per_second =
        movement["move_speed_meters_per_second"].as<float>();

    const YAML::Node hitbox = node["hitbox"];
    if (!hitbox || !hitbox["center"] || !hitbox["half_extents"]) {
        throw std::runtime_error(
            "actor template requires hitbox center and half_extents: " +
            actor_template.name);
    }
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
        if (animations["idle"]) {
            actor_template.animation_idle =
                animations["idle"].as<std::uint16_t>();
        }
        if (animations["chasing"]) {
            actor_template.animation_chasing =
                animations["chasing"].as<std::uint16_t>();
        }
    }
    actor_template.ai = ai_config_from_yaml(node["ai"], actor_template, weapons);
    actor_template.ai.animation_idle = actor_template.animation_idle;
    actor_template.ai.animation_chasing = actor_template.animation_chasing;
    return actor_template;
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

std::vector<ActorTemplateConfig> load_actor_templates_from_source(
    const GameplayConfigSource& source,
    const std::string& directory,
    const WeaponCatalogConfig& weapons) {
    std::vector<ActorTemplateConfig> actor_templates;
    std::unordered_map<std::uint32_t, std::string> ids;
    std::unordered_map<std::string, std::uint32_t> names;
    const std::vector<std::string> files = source.list_yaml_files(directory);
    for (const std::string& file : files) {
        ActorTemplateConfig actor_template =
            actor_template_from_yaml(source.load_yaml(file), weapons);
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

GameServerGameplayConfig load_gameplay_config_from_weapon_template_source(
    const GameplayConfigSource& source,
    const std::string& directory) {
    GameServerGameplayConfig config;
    config.weapons.projectile_sync_modes.fill(
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
        const KernelWeaponMechanicsDefinition weapon = weapon_from_yaml(document);
        if (weapon.weapon_id >= kWeaponCount) {
            throw std::runtime_error("weapon id out of range: " + file);
        }
        if (seen[weapon.weapon_id]) {
            throw std::runtime_error("duplicate weapon id: " + file);
        }
        seen[weapon.weapon_id] = true;
        config.weapons.definitions[weapon.weapon_id] = weapon;
        config.weapons.projectile_sync_modes[weapon.weapon_id] =
            projectile_sync_mode_from_weapon_yaml(document);
        config.weapons.names[weapon.weapon_id] =
            document["name"] ? document["name"].as<std::string>()
                             : std::filesystem::path(file).stem().string();
    }
    for (std::size_t index = 0; index < seen.size(); ++index) {
        if (!seen[index]) {
            throw std::runtime_error("missing weapon template id " + std::to_string(index));
        }
    }
    apply_default_non_weapon_config(&config);
    config.colliders = load_collider_catalog_from_source(
        source,
        source.default_collider_path_for_weapon_dir(directory));
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
    if (!document["weapon_template_dir"] || !document["collider_template_file"]) {
        throw std::runtime_error(
            "gameplay catalog requires weapon_template_dir and "
            "collider_template_file: " +
            path);
    }

    const std::string base_path = source.parent_path(path);
    const std::string weapon_template_dir =
        source.resolve_path(base_path, document["weapon_template_dir"]);
    GameServerGameplayConfig config =
        load_gameplay_config_from_weapon_template_source(
            source,
            weapon_template_dir);
    config.weapons.catalog_version =
        document["catalog_version"] ? document["catalog_version"].as<std::uint32_t>()
                                    : config.weapons.catalog_version;

    const std::string collider_template_file =
        source.resolve_path(base_path, document["collider_template_file"]);
    config.colliders =
        load_collider_catalog_from_source(source, collider_template_file);

    if (document["actor_template_dir"]) {
        const std::string actor_template_dir =
            source.resolve_path(base_path, document["actor_template_dir"]);
        config.actor_templates = load_actor_templates_from_source(
            source,
            actor_template_dir,
            config.weapons);
    } else {
        apply_default_actor_templates(&config);
    }

    apply_catalog_player_config(document, &config);
    apply_catalog_enemy_config(document, &config);

    config.weapons.catalog_hash = compute_gameplay_catalog_hash(config);
    const std::vector<std::string> errors = validate_gameplay_config(config);
    if (!errors.empty()) {
        throw std::runtime_error(errors.front());
    }
    return config;
}

void apply_default_non_weapon_config(GameServerGameplayConfig* config) {
    config->player = PlayerGameplayDefinition{};
    config->enemy = EnemyGameplayDefinition{};
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
}

void apply_catalog_enemy_config(
    const YAML::Node& document,
    GameServerGameplayConfig* config) {
    const YAML::Node enemy = document["enemy"];
    if (!enemy) {
        return;
    }
    if (enemy["spawn_count"]) {
        config->enemy.spawn_count = enemy["spawn_count"].as<std::uint32_t>();
    }
    if (enemy["spawn_radius"]) {
        config->enemy.spawn_radius = enemy["spawn_radius"].as<float>();
    }
    if (enemy["spawn_seed"]) {
        config->enemy.spawn_seed = enemy["spawn_seed"].as<std::uint32_t>();
    }
    if (enemy["spawn_position"]) {
        config->enemy.spawn_position = vec3_from_yaml(enemy["spawn_position"]);
    }
    if (enemy["actor_template"]) {
        config->enemy.actor_template_id =
            actor_template_ref_from_yaml(enemy["actor_template"], config->actor_templates);
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
        hash_weapon(&hash, weapons.definitions[index]);
    }
    return hash == 0 ? kFnvOffsetBasis : hash;
}

std::uint64_t compute_gameplay_catalog_hash(
    const GameServerGameplayConfig& config) {
    std::uint64_t hash = compute_gameplay_catalog_hash(config.weapons);
    hash_scalar(&hash, config.player.actor_template_id);
    hash_scalar(&hash, config.enemy.actor_template_id);
    hash_vec3(&hash, config.enemy.spawn_position);
    hash_scalar(&hash, config.enemy.spawn_count);
    hash_float(&hash, config.enemy.spawn_radius);
    hash_scalar(&hash, config.enemy.spawn_seed);
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
    std::vector<ColliderBindingConfig> bindings = config.colliders.bindings;
    std::sort(
        bindings.begin(),
        bindings.end(),
        [](const ColliderBindingConfig& lhs, const ColliderBindingConfig& rhs) {
            if (lhs.definition.entity_type != rhs.definition.entity_type) {
                return lhs.definition.entity_type < rhs.definition.entity_type;
            }
            return lhs.definition.collider_template_id <
                   rhs.definition.collider_template_id;
        });
    for (const ColliderBindingConfig& collider_binding : bindings) {
        hash_collider_binding(&hash, collider_binding);
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
    }
    if (config.actor_templates.empty()) {
        errors.push_back("actor templates must not be empty");
    }
    std::vector<std::uint32_t> actor_template_ids;
    std::vector<std::string> actor_template_names;
    for (const ActorTemplateConfig& actor_template : config.actor_templates) {
        if (actor_template.actor_template_id == 0 || actor_template.name.empty() ||
            (actor_template.entity_type != kEntityTypePlayer &&
             actor_template.entity_type != kEntityTypeEnemy) ||
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
        if (actor_template.entity_type == kEntityTypeEnemy &&
            (actor_template.ai.weapon_id >= kWeaponCount ||
             actor_template.ai.magazine_size == 0 ||
             actor_template.ai.fire_interval_seconds <= 0.0f ||
             actor_template.ai.reload_seconds <= 0.0f)) {
            errors.push_back("enemy actor template ai must be valid");
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
    if (player_actor == nullptr || player_actor->entity_type != kEntityTypePlayer) {
        errors.push_back("player actor template must reference a player actor");
    }
    const ActorTemplateConfig* enemy_actor =
        find_actor_template(config, config.enemy.actor_template_id);
    if (enemy_actor == nullptr || enemy_actor->entity_type != kEntityTypeEnemy ||
        config.enemy.spawn_count == 0 || config.enemy.spawn_radius < 0.0f) {
        errors.push_back("enemy gameplay config must be valid");
    }
    if (config.colliders.templates.empty() || config.colliders.bindings.empty()) {
        errors.push_back("collider catalog must include templates and bindings");
    }
    std::vector<std::uint32_t> collider_template_ids;
    for (const ColliderTemplateConfig& collider_template :
         config.colliders.templates) {
        const KernelColliderTemplateDefinition& definition =
            collider_template.definition;
        if (definition.struct_size < sizeof(KernelColliderTemplateDefinition) ||
            definition.template_id == 0 ||
            definition.shape_type > KernelColliderShapeType_Sphere ||
            definition.purpose_flags == 0 ||
            definition.layer_mask == 0 ||
            (definition.shape_type == KernelColliderShapeType_Aabb &&
             (definition.half_extents.x <= 0.0f ||
              definition.half_extents.y <= 0.0f ||
              definition.half_extents.z <= 0.0f)) ||
            (definition.shape_type == KernelColliderShapeType_Sphere &&
             definition.radius <= 0.0f)) {
            errors.push_back("collider template must be valid");
        }
        collider_template_ids.push_back(definition.template_id);
    }
    for (const ColliderBindingConfig& collider_binding :
         config.colliders.bindings) {
        const KernelColliderBindingDefinition& definition =
            collider_binding.definition;
        if (definition.struct_size < sizeof(KernelColliderBindingDefinition) ||
            definition.entity_type == 0 ||
            std::find(
                collider_template_ids.begin(),
                collider_template_ids.end(),
                definition.collider_template_id) == collider_template_ids.end()) {
            errors.push_back("collider binding must reference a valid template");
        }
    }
    return errors;
}

KernelGameplayCatalogStorage build_kernel_gameplay_catalog(
    const GameServerGameplayConfig& config) {
    KernelGameplayCatalogStorage storage;
    for (const KernelWeaponMechanicsDefinition& weapon :
         config.weapons.definitions) {
        if (weapon.fire_mode != KernelWeaponFireMode_Projectile) {
            continue;
        }
        KernelProjectileTemplateDefinition projectile_template{};
        projectile_template.struct_size = sizeof(projectile_template);
        projectile_template.projectile_template_id = weapon.weapon_id;
        projectile_template.weapon_id = weapon.weapon_id;
        projectile_template.motion_model = weapon.projectile.motion_model;
        projectile_template.sync_mode =
            config.weapons.projectile_sync_modes[weapon.weapon_id];
        projectile_template.hit_response = weapon.projectile.hit_response;
        projectile_template.damage_shape = weapon.projectile.damage_shape;
        projectile_template.damage = weapon.damage;
        projectile_template.speed = weapon.projectile.speed;
        projectile_template.lifetime_seconds = weapon.projectile.lifetime_seconds;
        projectile_template.explosion_radius = weapon.projectile.explosion_radius;
        projectile_template.gravity = weapon.projectile.gravity;
        projectile_template.collider_template_id = 3;
        projectile_template.explosion_template_id =
            weapon.projectile.explosion_radius > 0.0f ? 4u : 0u;
        projectile_template.collision_mask = weapon.projectile.collision_mask;
        projectile_template.max_hit_count = weapon.projectile.max_hit_count;
        storage.projectile_templates.push_back(projectile_template);
    }
    for (const ColliderTemplateConfig& collider_template :
         config.colliders.templates) {
        storage.collider_templates.push_back(collider_template.definition);
    }
    for (const ColliderBindingConfig& collider_binding :
         config.colliders.bindings) {
        storage.collider_bindings.push_back(collider_binding.definition);
    }

    storage.definition.struct_size = sizeof(storage.definition);
    storage.definition.catalog_version = config.weapons.catalog_version;
    storage.definition.catalog_hash = config.weapons.catalog_hash;
    storage.definition.projectile_templates =
        storage.projectile_templates.data();
    storage.definition.projectile_template_count =
        static_cast<std::uint32_t>(storage.projectile_templates.size());
    storage.definition.collider_templates = storage.collider_templates.data();
    storage.definition.collider_template_count =
        static_cast<std::uint32_t>(storage.collider_templates.size());
    storage.definition.collider_bindings = storage.collider_bindings.data();
    storage.definition.collider_binding_count =
        static_cast<std::uint32_t>(storage.collider_bindings.size());
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

KernelCombatStateDefinition make_enemy_combat_state(
    const GameServerGameplayConfig& config) {
    const ActorTemplateConfig* actor_template =
        find_actor_template(config, config.enemy.actor_template_id);
    if (actor_template == nullptr) {
        return KernelCombatStateDefinition{};
    }
    return make_combat_state_from_actor_template(config, *actor_template);
}

}  // namespace network_example::game_server
