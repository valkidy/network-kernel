#include "game_server/gameplay_config.h"

#include <cstddef>

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
        return weapon.projectile.struct_size >=
                   sizeof(KernelProjectileMechanicsDefinition) &&
               weapon.projectile.motion_model <= KernelProjectileMotionModel_Parabolic &&
               weapon.projectile.speed > 0.0f &&
               weapon.projectile.lifetime_seconds > 0.0f;
    }
    if (weapon.max_range <= 0.0f) {
        return false;
    }
    return weapon.fire_mode != KernelWeaponFireMode_Shotgun ||
           weapon.pellet_count != 0;
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
    }};
    config.enemy.weapon_id = kWeaponRocket;
    config.enemy.ai.weapon_id = kWeaponRocket;
    config.enemy.ai.magazine_size = kEnemyRocketMagazine;
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
