#include "game_server/gameplay_config.h"

#include <cassert>
#include <cstdlib>
#include <string>
#include <vector>

#include "game_server/enemy.h"
#include "kernel/public/kernel_types.h"

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

}  // namespace

int main() {
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    const std::vector<std::string> errors =
        network_example::game_server::validate_gameplay_config(config);
    assert(errors.empty());

    assert(config.player.entity_type == network_example::game_server::kEntityTypePlayer);
    require(config.weapons.catalog_version == 1);
    require(config.weapons.catalog_hash != 0);
    require(
        config.weapons.catalog_hash ==
        network_example::game_server::compute_gameplay_catalog_hash(config.weapons));
    network_example::game_server::WeaponCatalogConfig changed_weapons = config.weapons;
    changed_weapons.definitions[network_example::game_server::kWeaponRocket].damage += 1;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(changed_weapons));
    assert(config.player.health.hp == 100);
    assert(config.player.move_speed_meters_per_second == 5.0f);

    assert(config.enemy.entity_type == network_example::game_server::kEntityTypeEnemy);
    assert(config.enemy.health.hp == network_example::game_server::kEnemyInitialHp);
    assert(config.enemy.weapon_id == network_example::game_server::kEnemyRocketWeaponId);
    assert(config.enemy.spawn_position.x == 6.0f);
    assert(config.enemy.ai.move_speed == 2.5f);

    const KernelWeaponMechanicsDefinition& rifle =
        config.weapons.definitions[network_example::game_server::kWeaponRifle];
    assert(rifle.weapon_id == network_example::game_server::kWeaponRifle);
    assert(rifle.fire_mode == KernelWeaponFireMode_Hitscan);
    assert(rifle.damage == 25);
    assert(rifle.magazine_size == 30);
    assert(rifle.max_range == 100.0f);

    const KernelWeaponMechanicsDefinition& rocket =
        config.weapons.definitions[network_example::game_server::kEnemyRocketWeaponId];
    assert(rocket.weapon_id == network_example::game_server::kEnemyRocketWeaponId);
    assert(rocket.fire_mode == KernelWeaponFireMode_Projectile);
    assert(rocket.projectile.struct_size == sizeof(KernelProjectileMechanicsDefinition));
    assert(rocket.projectile.speed == 35.0f);
    assert(rocket.projectile.lifetime_seconds == 2.5f);
    assert(rocket.projectile.explosion_radius == 3.0f);
    assert(rocket.projectile.damage_shape == KernelProjectileDamageShape_Explosion);

    const KernelWeaponMechanicsDefinition& fire_floor =
        config.weapons.definitions[network_example::game_server::kWeaponFireFloor];
    assert(fire_floor.weapon_id == network_example::game_server::kWeaponFireFloor);
    assert(fire_floor.fire_mode == KernelWeaponFireMode_AreaEffect);
    assert(fire_floor.area_effect.radius == 2.0f);
    assert(fire_floor.area_effect.collision_mask == KERNEL_COLLISION_LAYER_ENEMY);
    assert(config.weapons.names[network_example::game_server::kWeaponFireFloor] ==
           "Fire Floor");

    const KernelWeaponMechanicsDefinition& beam_rifle =
        config.weapons.definitions[network_example::game_server::kWeaponBeamRifle];
    assert(beam_rifle.weapon_id == network_example::game_server::kWeaponBeamRifle);
    assert(beam_rifle.fire_mode == KernelWeaponFireMode_Beam);
    assert(beam_rifle.beam.struct_size == sizeof(KernelBeamMechanicsDefinition));
    assert(beam_rifle.beam.length == 8.0f);
    assert(beam_rifle.beam.radius == 0.25f);
    assert(beam_rifle.beam.damage_per_second == 30);
    assert(beam_rifle.beam.collision_mask == KERNEL_COLLISION_LAYER_ENEMY);
    assert(config.weapons.names[network_example::game_server::kWeaponBeamRifle] ==
           "Beam Rifle");

    network_example::game_server::GameServerGameplayConfig invalid = config;
    invalid.weapons.definitions[0].damage = 0;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());

    return 0;
}
