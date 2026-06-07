#include "game_server/gameplay_config.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>

#include <yaml-cpp/yaml.h>

#include "kernel/public/kernel_api.h"

namespace network_example::game_server {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr const char* kDefaultGameplayCatalogPath =
    "game_server/gameplay_catalog.yaml";

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
               weapon.projectile.collision_mask != 0 &&
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
               weapon.area_effect.spawn_distance >= 0.0f &&
               weapon.area_effect.collision_mask != 0;
    }
    if (weapon.fire_mode == KernelWeaponFireMode_Beam) {
        return weapon.beam.struct_size >= sizeof(KernelBeamMechanicsDefinition) &&
               weapon.beam.length > 0.0f &&
               weapon.beam.radius > 0.0f &&
               weapon.beam.damage_per_second > 0 &&
               weapon.beam.lifetime_ticks > 0 &&
               weapon.beam.collision_mask != 0;
    }
    if (weapon.fire_mode != KernelWeaponFireMode_Beam &&
        weapon.max_range <= 0.0f) {
        return false;
    }
    return weapon.fire_mode != KernelWeaponFireMode_Shotgun ||
           weapon.pellet_count != 0;
}

std::uint32_t collision_mask_from_yaml(const YAML::Node& node) {
    if (!node || node.as<std::string>() == "damageable") {
        return KERNEL_COLLISION_MASK_DAMAGEABLE;
    }
    const std::string value = node.as<std::string>();
    if (value == "enemy") {
        return KERNEL_COLLISION_LAYER_ENEMY;
    }
    if (value == "player") {
        return KERNEL_COLLISION_LAYER_PLAYER;
    }
    if (value == "projectile") {
        return KERNEL_COLLISION_LAYER_PROJECTILE;
    }
    if (value == "area_effect") {
        return KERNEL_COLLISION_LAYER_AREA_EFFECT;
    }
    throw std::runtime_error("unsupported collision_mask: " + value);
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

KernelVec3 vec3_from_yaml(const YAML::Node& node) {
    if (!node) {
        return KernelVec3{0.0f, 0.0f, 0.0f};
    }
    return KernelVec3{
        node["x"].as<float>(),
        node["y"].as<float>(),
        node["z"].as<float>()};
}

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

std::filesystem::path resolve_catalog_path(
    const std::filesystem::path& base_path,
    const YAML::Node& node) {
    std::filesystem::path path = node.as<std::string>();
    if (path.is_relative()) {
        path = base_path / path;
    }
    return path.lexically_normal();
}

ColliderCatalogConfig load_collider_catalog_from_file(const std::string& path) {
    const YAML::Node document = YAML::LoadFile(path);
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

void apply_default_non_weapon_config(GameServerGameplayConfig* config) {
    config->player = PlayerGameplayDefinition{};
    config->enemy = EnemyGameplayDefinition{};
    config->enemy.weapon_id = kEnemyRocketWeaponId;
    config->enemy.ai.weapon_id = kEnemyRocketWeaponId;
    config->enemy.ai.magazine_size = kEnemyRocketMagazine;
    config->benchmark_projectile_groups = BenchmarkProjectileGroupsConfig{};
}

std::filesystem::path default_collider_template_path_for_weapon_dir(
    const std::string& directory) {
    return (std::filesystem::path(directory).parent_path() /
            "collider_templates" /
            "default.yaml")
        .lexically_normal();
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
    hash_scalar(&hash, config.benchmark_projectile_groups.event_spawn_weapon_id);
    hash_scalar(&hash, config.benchmark_projectile_groups.snapshot_only_weapon_id);
    hash_scalar(&hash, config.benchmark_projectile_groups.hybrid_weapon_id);
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
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".yaml") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& file : files) {
        const YAML::Node document = YAML::LoadFile(file.string());
        const KernelWeaponMechanicsDefinition weapon = weapon_from_yaml(document);
        if (weapon.weapon_id >= kWeaponCount) {
            throw std::runtime_error("weapon id out of range: " + file.string());
        }
        if (seen[weapon.weapon_id]) {
            throw std::runtime_error("duplicate weapon id: " + file.string());
        }
        seen[weapon.weapon_id] = true;
        config.weapons.definitions[weapon.weapon_id] = weapon;
        config.weapons.projectile_sync_modes[weapon.weapon_id] =
            projectile_sync_mode_from_weapon_yaml(document);
        config.weapons.names[weapon.weapon_id] =
            document["name"] ? document["name"].as<std::string>() : file.stem().string();
    }
    for (std::size_t index = 0; index < seen.size(); ++index) {
        if (!seen[index]) {
            throw std::runtime_error("missing weapon template id " + std::to_string(index));
        }
    }
    apply_default_non_weapon_config(&config);
    config.colliders = load_collider_catalog_from_file(
        default_collider_template_path_for_weapon_dir(directory).string());
    config.weapons.catalog_hash = compute_gameplay_catalog_hash(config);
    const std::vector<std::string> errors = validate_gameplay_config(config);
    if (!errors.empty()) {
        throw std::runtime_error(errors.front());
    }
    return config;
}

GameServerGameplayConfig load_gameplay_config_from_catalog_file(
    const std::string& path) {
    const std::filesystem::path catalog_path = path;
    const std::filesystem::path base_path = catalog_path.parent_path();
    const YAML::Node document = YAML::LoadFile(path);
    if (!document["weapon_template_dir"] || !document["collider_template_file"]) {
        throw std::runtime_error(
            "gameplay catalog requires weapon_template_dir and "
            "collider_template_file: " +
            path);
    }

    const std::filesystem::path weapon_template_dir =
        resolve_catalog_path(base_path, document["weapon_template_dir"]);
    GameServerGameplayConfig config =
        load_gameplay_config_from_weapon_template_directory(
            weapon_template_dir.string());
    config.weapons.catalog_version =
        document["catalog_version"] ? document["catalog_version"].as<std::uint32_t>()
                                    : config.weapons.catalog_version;

    const std::filesystem::path collider_template_file =
        resolve_catalog_path(base_path, document["collider_template_file"]);
    config.colliders =
        load_collider_catalog_from_file(collider_template_file.string());

    if (document["benchmark_projectile_groups"]) {
        const YAML::Node groups = document["benchmark_projectile_groups"];
        config.benchmark_projectile_groups.event_spawn_weapon_id =
            groups["event_spawn_weapon_id"]
                ? static_cast<std::uint8_t>(
                      groups["event_spawn_weapon_id"].as<int>())
                : config.benchmark_projectile_groups.event_spawn_weapon_id;
        config.benchmark_projectile_groups.snapshot_only_weapon_id =
            groups["snapshot_only_weapon_id"]
                ? static_cast<std::uint8_t>(
                      groups["snapshot_only_weapon_id"].as<int>())
                : config.benchmark_projectile_groups.snapshot_only_weapon_id;
        config.benchmark_projectile_groups.hybrid_weapon_id =
            groups["hybrid_weapon_id"]
                ? static_cast<std::uint8_t>(groups["hybrid_weapon_id"].as<int>())
                : config.benchmark_projectile_groups.hybrid_weapon_id;
    }

    config.weapons.catalog_hash = compute_gameplay_catalog_hash(config);
    const std::vector<std::string> errors = validate_gameplay_config(config);
    if (!errors.empty()) {
        throw std::runtime_error(errors.front());
    }
    return config;
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
    if (config.player.health.hp == 0 || config.player.health.max_hp == 0 ||
        config.player.move_speed_meters_per_second <= 0.0f) {
        errors.push_back("player health and move speed must be positive");
    }
    if (config.enemy.health.hp == 0 || config.enemy.health.max_hp == 0 ||
        config.enemy.weapon_id >= kWeaponCount ||
        config.enemy.ai.weapon_id >= kWeaponCount ||
        config.enemy.ai.magazine_size == 0 ||
        config.enemy.ai.fire_interval_seconds <= 0.0f ||
        config.enemy.ai.reload_seconds <= 0.0f) {
        errors.push_back("enemy gameplay config must be valid");
    }
    const auto is_projectile_weapon = [&config](std::uint8_t weapon_id) {
        return weapon_id < kWeaponCount &&
               config.weapons.definitions[weapon_id].fire_mode ==
                   KernelWeaponFireMode_Projectile;
    };
    if (!is_projectile_weapon(
            config.benchmark_projectile_groups.event_spawn_weapon_id) ||
        !is_projectile_weapon(
            config.benchmark_projectile_groups.snapshot_only_weapon_id) ||
        !is_projectile_weapon(config.benchmark_projectile_groups.hybrid_weapon_id)) {
        errors.push_back("benchmark projectile groups must reference projectile weapons");
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

KernelCombatStateDefinition make_player_combat_state(
    const GameServerGameplayConfig& config) {
    KernelCombatStateDefinition combat_state{};
    combat_state.struct_size = sizeof(KernelCombatStateDefinition);
    combat_state.hp = config.player.health.hp;
    combat_state.max_hp = config.player.health.max_hp;
    combat_state.active_weapon_id = kWeaponRifle;
    combat_state.move_speed_meters_per_second =
        config.player.move_speed_meters_per_second;
    combat_state.hitbox_center = config.player.hitbox_center;
    combat_state.hitbox_half_extents = config.player.hitbox_half_extents;
    fill_default_ammo(config.weapons, &combat_state);
    return combat_state;
}

KernelCombatStateDefinition make_enemy_combat_state(
    const GameServerGameplayConfig& config) {
    KernelCombatStateDefinition combat_state{};
    combat_state.struct_size = sizeof(KernelCombatStateDefinition);
    combat_state.hp = config.enemy.health.hp;
    combat_state.max_hp = config.enemy.health.max_hp;
    combat_state.active_weapon_id = config.enemy.weapon_id;
    combat_state.hitbox_center = config.enemy.hitbox_center;
    combat_state.hitbox_half_extents = config.enemy.hitbox_half_extents;
    const KernelWeaponMechanicsDefinition& weapon =
        config.weapons.definitions[config.enemy.weapon_id];
    combat_state.ammo[config.enemy.weapon_id] = weapon.magazine_size;
    combat_state.reserve_ammo[config.enemy.weapon_id] =
        static_cast<std::uint16_t>(weapon.magazine_size * 2u);
    return combat_state;
}

}  // namespace network_example::game_server
