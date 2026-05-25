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
    std::ofstream file(path);
    assert(file.good());
    file << text;
}

void write_valid_templates(const std::filesystem::path& dir) {
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
        dir / "grenade.yaml",
        "id: 2\nname: Grenade\nweapon_type: projectile\nmagazine_size: 30\n"
        "damage: 40\ncooldown_ticks: 30\nreload_ticks: 60\nprojectile:\n"
        "  movement_model: parabolic\n  hit_response: destroy\n"
        "  damage_shape: explosion\n  speed: 15.0\n  lifetime_seconds: 3.0\n"
        "  explosion_radius: 4.0\n  collision_mask: damageable\n  max_hit_count: 1\n"
        "  gravity: {x: 0.0, y: -9.8, z: 0.0}\n");
    write_file(
        dir / "rocket.yaml",
        "id: 3\nname: Rocket\nweapon_type: projectile\nmagazine_size: 6\n"
        "damage: 45\ncooldown_ticks: 45\nreload_ticks: 75\nprojectile:\n"
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
    assert(config.weapons.definitions[network_example::game_server::kWeaponFireFloor]
               .fire_mode == KernelWeaponFireMode_AreaEffect);
    assert(config.weapons.definitions[network_example::game_server::kWeaponFireFloor]
               .area_effect.collision_mask == KERNEL_COLLISION_LAYER_ENEMY);
    assert(config.weapons.definitions[network_example::game_server::kWeaponBeamRifle]
               .fire_mode == KernelWeaponFireMode_Beam);
    assert(config.weapons.definitions[network_example::game_server::kWeaponBeamRifle]
               .beam.damage_per_second == 30);
    assert(config.weapons.names[network_example::game_server::kWeaponFireFloor] ==
           "Fire Floor");
    assert(config.weapons.names[network_example::game_server::kWeaponBeamRifle] ==
           "Beam Rifle");
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
}

}  // namespace

int main() {
    valid_repo_templates_load_all_slots();
    invalid_templates_are_rejected();
    return 0;
}
