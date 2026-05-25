#include "game_server/gameplay_config.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace network_example::game_server {
namespace {

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
               weapon.projectile.motion_model <= KernelProjectileMotionModel_Parabolic &&
               weapon.projectile.hit_response <= KernelProjectileHitResponse_Attach &&
               weapon.projectile.hit_response != KernelProjectileHitResponse_Bounce &&
               weapon.projectile.hit_response != KernelProjectileHitResponse_Attach &&
               weapon.projectile.damage_shape <= KernelProjectileDamageShape_PiercingSegment &&
               weapon.projectile.collision_mask != 0 &&
               weapon.projectile.max_hit_count > 0 &&
               weapon.projectile.speed > 0.0f &&
               weapon.projectile.lifetime_seconds > 0.0f;
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
        throw std::runtime_error("homing projectiles are not supported in this phase");
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

}  // namespace

GameServerGameplayConfig default_game_server_gameplay_config() {
    GameServerGameplayConfig config;
    config.weapons.definitions = {{
        hitscan_weapon(kWeaponRifle, 30, 25, 3, 30, 100.0f),
        shotgun_weapon(kWeaponShotgun, 8, 10, 20, 45, 40.0f, 5, 0.035f),
        projectile_weapon(
            kWeaponGrenade,
            30,
            40,
            30,
            60,
            15.0f,
            3.0f,
            4.0f,
            KernelProjectileMotionModel_Parabolic,
            KernelVec3{0.0f, -9.8f, 0.0f}),
        projectile_weapon(
            kWeaponRocket,
            6,
            45,
            45,
            75,
            35.0f,
            2.5f,
            3.0f,
            KernelProjectileMotionModel_Linear,
            KernelVec3{0.0f, 0.0f, 0.0f}),
        area_effect_weapon(
            kWeaponFireFloor,
            3,
            12,
            10,
            30,
            2.0f,
            12,
            2,
            6,
            1.0f,
            KERNEL_COLLISION_LAYER_ENEMY),
        beam_weapon(
            kWeaponBeamRifle,
            12,
            30,
            1,
            45,
            8.0f,
            0.25f,
            30,
            2,
            KERNEL_COLLISION_LAYER_ENEMY),
    }};
    config.weapons.names = {{
        "Rifle",
        "Shotgun",
        "Grenade",
        "Rocket",
        "Fire Floor",
        "Beam Rifle",
    }};
    config.enemy.weapon_id = kWeaponRocket;
    config.enemy.ai.weapon_id = kWeaponRocket;
    config.enemy.ai.magazine_size = kEnemyRocketMagazine;
    return config;
}

GameServerGameplayConfig load_gameplay_config_from_weapon_template_directory(
    const std::string& directory) {
    GameServerGameplayConfig config;
    std::array<bool, kWeaponCount> seen{false, false, false, false, false, false};
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
        config.weapons.names[weapon.weapon_id] =
            document["name"] ? document["name"].as<std::string>() : file.stem().string();
    }
    for (std::size_t index = 0; index < seen.size(); ++index) {
        if (!seen[index]) {
            throw std::runtime_error("missing weapon template id " + std::to_string(index));
        }
    }
    config.player = default_game_server_gameplay_config().player;
    config.enemy = default_game_server_gameplay_config().enemy;
    config.enemy.weapon_id = kWeaponRocket;
    config.enemy.ai.weapon_id = kWeaponRocket;
    config.enemy.ai.magazine_size = kEnemyRocketMagazine;
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
    return errors;
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
    combat_state.ammo[config.enemy.weapon_id] = kEnemyRocketMagazine;
    combat_state.reserve_ammo[config.enemy.weapon_id] = kEnemyRocketReserveAmmo;
    return combat_state;
}

}  // namespace network_example::game_server
