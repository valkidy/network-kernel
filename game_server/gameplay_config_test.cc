#include "game_server/gameplay_config.h"

#include <cassert>
#include <string>
#include <vector>

#include "game_server/enemy.h"
#include "kernel/public/kernel_types.h"

int main() {
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    const std::vector<std::string> errors =
        network_example::game_server::validate_gameplay_config(config);
    assert(errors.empty());

    assert(config.player.entity_type == network_example::game_server::kEntityTypePlayer);
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

    network_example::game_server::GameServerGameplayConfig invalid = config;
    invalid.weapons.definitions[0].damage = 0;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());

    return 0;
}
