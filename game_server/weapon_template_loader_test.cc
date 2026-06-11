#include "game_server/gameplay_config.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path runfiles_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    assert(test_srcdir != nullptr);
    assert(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace;
}

std::filesystem::path tmp_dir(const std::string& name) {
    const char* test_tmpdir = std::getenv("TEST_TMPDIR");
    assert(test_tmpdir != nullptr);
    const std::filesystem::path path = std::filesystem::path(test_tmpdir) / name;
    std::filesystem::create_directories(path);
    return path;
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    assert(file.good());
    file << text;
}

void write_valid_collider_catalog(const std::filesystem::path& weapon_dir) {
    write_file(
        weapon_dir.parent_path() / "collider_templates" / "default.yaml",
        "templates:\n"
        "  - id: 1\n"
        "    name: player_hit\n"
        "    shape: aabb\n"
        "    center: {x: 0.0, y: 0.0, z: 0.0}\n"
        "    half_extents: {x: 0.35, y: 0.9, z: 0.35}\n"
        "    radius: 0.0\n"
        "    purpose: hit\n"
        "    layer: player\n"
        "  - id: 2\n"
        "    name: enemy_hit\n"
        "    shape: aabb\n"
        "    center: {x: 0.0, y: 0.0, z: 0.0}\n"
        "    half_extents: {x: 0.4, y: 0.8, z: 0.4}\n"
        "    radius: 0.0\n"
        "    purpose: hit\n"
        "    layer: enemy\n"
        "  - id: 3\n"
        "    name: projectile_damage\n"
        "    shape: aabb\n"
        "    center: {x: 0.0, y: 0.0, z: 0.0}\n"
        "    half_extents: {x: 0.1, y: 0.1, z: 0.1}\n"
        "    radius: 0.0\n"
        "    purpose: damage\n"
        "    layer: projectile\n"
        "  - id: 4\n"
        "    name: explosion_damage\n"
        "    shape: sphere\n"
        "    center: {x: 0.0, y: 0.0, z: 0.0}\n"
        "    half_extents: {x: 1.0, y: 1.0, z: 1.0}\n"
        "    radius: 1.0\n"
        "    purpose: damage\n"
        "    layer: area_effect\n"
        "bindings:\n"
        "  - entity_type: player\n"
        "    collider_template: player_hit\n"
        "    local_position: {x: 0.0, y: 0.9, z: 0.0}\n"
        "  - entity_type: enemy\n"
        "    collider_template: enemy_hit\n"
        "    local_position: {x: 0.0, y: 0.8, z: 0.0}\n"
        "  - entity_type: projectile\n"
        "    collider_template: projectile_damage\n"
        "    local_position: {x: 0.0, y: 0.0, z: 0.0}\n"
        "  - entity_type: area_effect\n"
        "    collider_template: explosion_damage\n"
        "    local_position: {x: 0.0, y: 0.0, z: 0.0}\n");
}

void write_valid_templates(const std::filesystem::path& dir) {
    write_valid_collider_catalog(dir);
    write_file(
        dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\ncooldown_ticks: 3\nreload_ticks: 30\nmax_range: 100.0\n");
    write_file(
        dir / "shotgun.yaml",
        "id: 1\nname: Shotgun\nweapon_type: shotgun\nmagazine_size: 8\n"
        "damage: 10\ncooldown_ticks: 20\nreload_ticks: 45\nmax_range: 40.0\n"
        "pellet_count: 5\npellet_spread: 0.035\n");
    write_file(
        dir / "spammer.yaml",
        "id: 2\nname: Projectile Spammer\nweapon_type: projectile\n"
        "magazine_size: 120\n"
        "damage: 1\ncooldown_ticks: 1\nreload_ticks: 30\nprojectile:\n"
        "  sync_mode: local_predicted_deterministic\n"
        "  movement_model: linear\n  hit_response: destroy\n"
        "  damage_shape: direct_hit\n  speed: 30.0\n  lifetime_seconds: 2.0\n"
        "  explosion_radius: 0.0\n  collision_mask: player\n  max_hit_count: 1\n"
        "  gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    write_file(
        dir / "rocket.yaml",
        "id: 3\nname: Rocket\nweapon_type: projectile\nmagazine_size: 6\n"
        "damage: 45\ncooldown_ticks: 45\nreload_ticks: 75\nprojectile:\n"
        "  sync_mode: server_snapshot_only\n"
        "  movement_model: linear\n  hit_response: destroy\n"
        "  damage_shape: explosion\n  speed: 35.0\n  lifetime_seconds: 2.5\n"
        "  explosion_radius: 3.0\n  collision_mask: damageable\n  max_hit_count: 1\n"
        "  gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    write_file(
        dir / "fire_floor.yaml",
        "id: 4\nname: Fire Floor\nweapon_type: area_effect\nmagazine_size: 3\n"
        "damage: 12\ncooldown_ticks: 10\nreload_ticks: 30\narea_effect:\n"
        "  radius: 2.0\n  damage_per_interval: 12\n  damage_interval_ticks: 2\n"
        "  lifetime_ticks: 6\n  spawn_distance: 1.0\n  collision_mask: enemy\n");
    write_file(
        dir / "beam_rifle.yaml",
        "id: 5\nname: Beam Rifle\nweapon_type: beam\nmagazine_size: 12\n"
        "damage: 30\ncooldown_ticks: 1\nreload_ticks: 45\nbeam:\n"
        "  length: 8.0\n  radius: 0.25\n  damage_per_second: 30\n"
        "  lifetime_ticks: 2\n  collision_mask: enemy\n");
    write_file(
        dir / "homing_missile.yaml",
        "id: 6\nname: Homing Missile\nweapon_type: projectile\nmagazine_size: 4\n"
        "damage: 20\ncooldown_ticks: 15\nreload_ticks: 60\nprojectile:\n"
        "  sync_mode: hybrid_deterministic_then_snapshot\n"
        "  movement_model: homing\n  hit_response: destroy\n"
        "  damage_shape: direct_hit\n  speed: 20.0\n  lifetime_seconds: 3.0\n"
        "  explosion_radius: 0.0\n  collision_mask: enemy\n  max_hit_count: 1\n"
        "  gravity: {x: 0.0, y: 0.0, z: 0.0}\n"
        "  homing:\n"
        "    homing_mode: fire_and_forget\n"
        "    sync_mode: hybrid_deterministic_then_snapshot\n"
        "    boost_ticks: 2\n"
        "    lock_on_range: 25.0\n"
        "    lose_target_range: 30.0\n"
        "    lock_cone_degrees: 75.0\n"
        "    max_turn_rate_degrees_per_second: 360.0\n"
        "    acceleration: 20.0\n"
        "    max_speed: 30.0\n");
}

bool load_fails(const std::filesystem::path& dir) {
    try {
        (void)network_example::game_server::
            load_gameplay_config_from_weapon_template_directory(dir.string());
    } catch (...) {
        return true;
    }
    return false;
}

void valid_repo_templates_load_all_slots() {
    const std::filesystem::path dir =
        runfiles_root() / "game_server" / "weapon_templates";
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::
            load_gameplay_config_from_weapon_template_directory(dir.string());

    assert(config.weapons.definitions[network_example::game_server::kWeaponRifle].fire_mode ==
           KernelWeaponFireMode_Hitscan);
    assert(config.weapons.definitions[network_example::game_server::kWeaponRocket]
               .projectile.damage_shape == KernelProjectileDamageShape_Explosion);
    assert(config.weapons.definitions[network_example::game_server::kWeaponGrenade]
               .damage == 1);
    assert(config.weapons.definitions[network_example::game_server::kWeaponGrenade]
               .cooldown_ticks == 1);
    assert(config.weapons.definitions[network_example::game_server::kWeaponGrenade]
               .magazine_size == 120);
    assert(config.weapons.definitions[network_example::game_server::kWeaponGrenade]
               .projectile.motion_model == KernelProjectileMotionModel_Linear);
    assert(config.weapons.definitions[network_example::game_server::kWeaponFireFloor]
               .fire_mode == KernelWeaponFireMode_AreaEffect);
    assert(config.weapons.definitions[network_example::game_server::kWeaponFireFloor]
               .area_effect.collision_mask == KERNEL_COLLISION_LAYER_ENEMY);
    assert(config.weapons.definitions[network_example::game_server::kWeaponBeamRifle]
               .fire_mode == KernelWeaponFireMode_Beam);
    assert(config.weapons.definitions[network_example::game_server::kWeaponBeamRifle]
               .beam.damage_per_second == 30);
    assert(config.weapons.definitions[network_example::game_server::kWeaponHomingMissile]
               .projectile.motion_model == KernelProjectileMotionModel_Homing);
    assert(config.weapons.definitions[network_example::game_server::kWeaponHomingMissile]
               .projectile.homing.lock_on_range == 25.0f);
    assert(config.weapons.projectile_sync_modes
               [network_example::game_server::kWeaponGrenade] ==
           KernelProjectileSyncMode_LocalPredictedDeterministic);
    assert(config.weapons.names[network_example::game_server::kWeaponGrenade] ==
           "Projectile Spammer");
    assert(config.weapons.projectile_sync_modes
               [network_example::game_server::kWeaponRocket] ==
           KernelProjectileSyncMode_ServerSnapshotOnly);
    assert(config.weapons.projectile_sync_modes
               [network_example::game_server::kWeaponHomingMissile] ==
           KernelProjectileSyncMode_HybridDeterministicThenSnapshot);
    assert(config.weapons.names[network_example::game_server::kWeaponFireFloor] ==
           "Fire Floor");
    assert(config.weapons.names[network_example::game_server::kWeaponBeamRifle] ==
           "Beam Rifle");
    assert(config.weapons.names[network_example::game_server::kWeaponHomingMissile] ==
           "Homing Missile");
}

void invalid_templates_are_rejected() {
    const std::filesystem::path duplicate_dir = tmp_dir("duplicate");
    write_valid_templates(duplicate_dir);
    write_file(
        duplicate_dir / "duplicate.yaml",
        "id: 4\nname: Duplicate\nweapon_type: hitscan\nmagazine_size: 1\n"
        "damage: 1\ncooldown_ticks: 1\nreload_ticks: 1\nmax_range: 1.0\n");
    assert(load_fails(duplicate_dir));

    const std::filesystem::path hitscan_projectile_dir = tmp_dir("hitscan_projectile");
    write_valid_templates(hitscan_projectile_dir);
    write_file(
        hitscan_projectile_dir / "rifle.yaml",
        "id: 0\nname: Bad Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\ncooldown_ticks: 3\nreload_ticks: 30\nmax_range: 100.0\n"
        "projectile: {speed: 10.0}\n");
    assert(load_fails(hitscan_projectile_dir));

    const std::filesystem::path missing_beam_dir = tmp_dir("missing_beam");
    write_valid_templates(missing_beam_dir);
    write_file(
        missing_beam_dir / "beam_rifle.yaml",
        "id: 5\nname: Beam\nweapon_type: beam\nmagazine_size: 1\n"
        "damage: 1\ncooldown_ticks: 1\nreload_ticks: 1\n");
    assert(load_fails(missing_beam_dir));

    const std::filesystem::path invalid_beam_dir = tmp_dir("invalid_beam");
    write_valid_templates(invalid_beam_dir);
    write_file(
        invalid_beam_dir / "beam_rifle.yaml",
        "id: 5\nname: Beam\nweapon_type: beam\nmagazine_size: 1\n"
        "damage: 1\ncooldown_ticks: 1\nreload_ticks: 1\nbeam:\n"
        "  length: 0.0\n  radius: 0.25\n  damage_per_second: 30\n"
        "  lifetime_ticks: 2\n  collision_mask: enemy\n");
    assert(load_fails(invalid_beam_dir));

    const std::filesystem::path beam_on_hitscan_dir = tmp_dir("beam_on_hitscan");
    write_valid_templates(beam_on_hitscan_dir);
    write_file(
        beam_on_hitscan_dir / "rifle.yaml",
        "id: 0\nname: Bad Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\ncooldown_ticks: 3\nreload_ticks: 30\nmax_range: 100.0\n"
        "beam: {length: 8.0}\n");
    assert(load_fails(beam_on_hitscan_dir));

    const std::filesystem::path homing_dir = tmp_dir("homing");
    write_valid_templates(homing_dir);
    write_file(
        homing_dir / "rocket.yaml",
        "id: 3\nname: Homing\nweapon_type: projectile\nmagazine_size: 1\n"
        "damage: 1\ncooldown_ticks: 1\nreload_ticks: 1\nprojectile:\n"
        "  movement_model: homing\n  hit_response: destroy\n"
        "  damage_shape: explosion\n  speed: 1.0\n  lifetime_seconds: 1.0\n"
        "  explosion_radius: 1.0\n  collision_mask: damageable\n  max_hit_count: 1\n");
    assert(load_fails(homing_dir));

    const std::filesystem::path invalid_homing_dir = tmp_dir("invalid_homing");
    write_valid_templates(invalid_homing_dir);
    write_file(
        invalid_homing_dir / "homing_missile.yaml",
        "id: 6\nname: Bad Homing\nweapon_type: projectile\nmagazine_size: 1\n"
        "damage: 1\ncooldown_ticks: 1\nreload_ticks: 1\nprojectile:\n"
        "  movement_model: homing\n  hit_response: destroy\n"
        "  damage_shape: direct_hit\n  speed: 1.0\n  lifetime_seconds: 1.0\n"
        "  collision_mask: enemy\n  max_hit_count: 1\n"
        "  homing:\n"
        "    homing_mode: retarget\n"
        "    sync_mode: hybrid_deterministic_then_snapshot\n"
        "    boost_ticks: 1\n"
        "    lock_on_range: 10.0\n"
        "    lose_target_range: 12.0\n"
        "    lock_cone_degrees: 75.0\n"
        "    max_turn_rate_degrees_per_second: 360.0\n"
        "    acceleration: 10.0\n"
        "    max_speed: 20.0\n");
    assert(load_fails(invalid_homing_dir));

    const std::filesystem::path homing_on_linear_dir = tmp_dir("homing_on_linear");
    write_valid_templates(homing_on_linear_dir);
    write_file(
        homing_on_linear_dir / "rocket.yaml",
        "id: 3\nname: Bad Rocket\nweapon_type: projectile\nmagazine_size: 1\n"
        "damage: 1\ncooldown_ticks: 1\nreload_ticks: 1\nprojectile:\n"
        "  movement_model: linear\n  hit_response: destroy\n"
        "  damage_shape: direct_hit\n  speed: 1.0\n  lifetime_seconds: 1.0\n"
        "  collision_mask: damageable\n  max_hit_count: 1\n"
        "  homing: {homing_mode: fire_and_forget}\n");
    assert(load_fails(homing_on_linear_dir));

    const std::filesystem::path bounce_dir = tmp_dir("bounce");
    write_valid_templates(bounce_dir);
    write_file(
        bounce_dir / "rocket.yaml",
        "id: 3\nname: Bounce\nweapon_type: projectile\nmagazine_size: 1\n"
        "damage: 1\ncooldown_ticks: 1\nreload_ticks: 1\nprojectile:\n"
        "  movement_model: linear\n  hit_response: bounce\n"
        "  damage_shape: direct_hit\n  speed: 1.0\n  lifetime_seconds: 1.0\n"
        "  collision_mask: damageable\n  max_hit_count: 1\n");
    assert(load_fails(bounce_dir));

    const std::filesystem::path invalid_sync_dir = tmp_dir("invalid_sync");
    write_valid_templates(invalid_sync_dir);
    write_file(
        invalid_sync_dir / "rocket.yaml",
        "id: 3\nname: Bad Sync\nweapon_type: projectile\nmagazine_size: 1\n"
        "damage: 1\ncooldown_ticks: 1\nreload_ticks: 1\nprojectile:\n"
        "  sync_mode: remote_magic\n  movement_model: linear\n"
        "  hit_response: destroy\n  damage_shape: direct_hit\n  speed: 1.0\n"
        "  lifetime_seconds: 1.0\n  collision_mask: damageable\n"
        "  max_hit_count: 1\n");
    assert(load_fails(invalid_sync_dir));
}

void collision_mask_expressions_are_loaded() {
    const std::filesystem::path none_dir = tmp_dir("mask_none");
    write_valid_templates(none_dir);
    write_file(
        none_dir / "rocket.yaml",
        "id: 3\nname: Rocket\nweapon_type: projectile\nmagazine_size: 6\n"
        "damage: 45\ncooldown_ticks: 45\nreload_ticks: 75\nprojectile:\n"
        "  sync_mode: server_snapshot_only\n"
        "  movement_model: linear\n  hit_response: destroy\n"
        "  damage_shape: explosion\n  speed: 35.0\n  lifetime_seconds: 2.5\n"
        "  explosion_radius: 3.0\n  collision_mask: none\n  max_hit_count: 1\n"
        "  gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            none_dir.string());
    assert(config.weapons.definitions[network_example::game_server::kWeaponRocket]
               .projectile.collision_mask == 0);

    const std::filesystem::path zero_dir = tmp_dir("mask_zero");
    write_valid_templates(zero_dir);
    write_file(
        zero_dir / "beam_rifle.yaml",
        "id: 5\nname: Beam Rifle\nweapon_type: beam\nmagazine_size: 12\n"
        "damage: 30\ncooldown_ticks: 1\nreload_ticks: 45\nbeam:\n"
        "  length: 8.0\n  radius: 0.25\n  damage_per_second: 30\n"
        "  lifetime_ticks: 2\n  collision_mask: 0\n");
    config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            zero_dir.string());
    assert(config.weapons.definitions[network_example::game_server::kWeaponBeamRifle]
               .beam.collision_mask == 0);

    const std::filesystem::path expression_dir = tmp_dir("mask_expression");
    write_valid_templates(expression_dir);
    write_file(
        expression_dir / "fire_floor.yaml",
        "id: 4\nname: Fire Floor\nweapon_type: area_effect\nmagazine_size: 3\n"
        "damage: 12\ncooldown_ticks: 10\nreload_ticks: 30\narea_effect:\n"
        "  radius: 2.0\n  damage_per_interval: 12\n  damage_interval_ticks: 2\n"
        "  lifetime_ticks: 6\n  spawn_distance: 1.0\n"
        "  collision_mask: enemy | player\n");
    config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            expression_dir.string());
    assert(config.weapons.definitions[network_example::game_server::kWeaponFireFloor]
               .area_effect.collision_mask ==
           (KERNEL_COLLISION_LAYER_ENEMY | KERNEL_COLLISION_LAYER_PLAYER));
}

void malformed_collision_masks_are_rejected() {
    const std::filesystem::path unknown_dir = tmp_dir("mask_unknown");
    write_valid_templates(unknown_dir);
    write_file(
        unknown_dir / "rocket.yaml",
        "id: 3\nname: Rocket\nweapon_type: projectile\nmagazine_size: 6\n"
        "damage: 45\ncooldown_ticks: 45\nreload_ticks: 75\nprojectile:\n"
        "  sync_mode: server_snapshot_only\n"
        "  movement_model: linear\n  hit_response: destroy\n"
        "  damage_shape: explosion\n  speed: 35.0\n  lifetime_seconds: 2.5\n"
        "  explosion_radius: 3.0\n  collision_mask: ghost\n  max_hit_count: 1\n"
        "  gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    assert(load_fails(unknown_dir));

    const std::filesystem::path empty_token_dir = tmp_dir("mask_empty_token");
    write_valid_templates(empty_token_dir);
    write_file(
        empty_token_dir / "rocket.yaml",
        "id: 3\nname: Rocket\nweapon_type: projectile\nmagazine_size: 6\n"
        "damage: 45\ncooldown_ticks: 45\nreload_ticks: 75\nprojectile:\n"
        "  sync_mode: server_snapshot_only\n"
        "  movement_model: linear\n  hit_response: destroy\n"
        "  damage_shape: explosion\n  speed: 35.0\n  lifetime_seconds: 2.5\n"
        "  explosion_radius: 3.0\n  collision_mask: enemy |\n"
        "  max_hit_count: 1\n"
        "  gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    assert(load_fails(empty_token_dir));
}

void catalog_file_loads_colliders() {
    const std::filesystem::path catalog_file =
        runfiles_root() / "game_server" / "gameplay_catalog.yaml";
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_catalog_file(
            catalog_file.string());
    assert(config.weapons.catalog_version == 1);
    assert(config.weapons.catalog_hash != 0);
    assert(config.colliders.templates.size() == 4);
    assert(config.colliders.bindings.size() == 4);
}

}  // namespace

int main() {
    valid_repo_templates_load_all_slots();
    invalid_templates_are_rejected();
    collision_mask_expressions_are_loaded();
    malformed_collision_masks_are_rejected();
    catalog_file_loads_colliders();
    return 0;
}
