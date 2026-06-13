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
        "  - id: 5\n"
        "    name: rifle_segment_damage\n"
        "    shape: segment\n"
        "    center: {x: 0.0, y: 0.0, z: 0.0}\n"
        "    half_extents: {x: 0.0, y: 0.0, z: 0.0}\n"
        "    radius: 0.0\n"
        "    length: 100.0\n"
        "    scatter_degrees: 0.0\n"
        "    lifetime_ticks: 3\n"
        "    purpose: damage\n"
        "    layer: projectile\n"
        "  - id: 6\n"
        "    name: shotgun_segment_damage\n"
        "    shape: segment\n"
        "    center: {x: 0.0, y: 0.0, z: 0.0}\n"
        "    half_extents: {x: 0.0, y: 0.0, z: 0.0}\n"
        "    radius: 0.0\n"
        "    length: 40.0\n"
        "    scatter_degrees: 6.0\n"
        "    lifetime_ticks: 3\n"
        "    purpose: damage\n"
        "    layer: projectile\n"
        "  - id: 7\n"
        "    name: sphere_damage\n"
        "    shape: sphere\n"
        "    center: {x: 0.0, y: 0.0, z: 0.0}\n"
        "    half_extents: {x: 0.2, y: 0.2, z: 0.2}\n"
        "    radius: 0.2\n"
        "    purpose: damage\n"
        "    layer: projectile\n"
        "  - id: 8\n"
        "    name: beam_damage\n"
        "    shape: oriented_box\n"
        "    center: {x: 0.0, y: 0.0, z: 0.0}\n"
        "    half_extents: {x: 0.1, y: 0.1, z: 2.5}\n"
        "    radius: 0.0\n"
        "    purpose: damage\n"
        "    layer: projectile\n"
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
        dir.parent_path() / "projectile_templates" / "spammer.yaml",
        "id: 2\nname: spammer_projectile\ndamage: 1\n"
        "sync_mode: local_predicted_deterministic\n"
        "collider_template: sphere_damage\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 30.0\nlifetime_seconds: 2.0\n"
        "collision_mask: player\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    write_file(
        dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\n"
        "collider_template: projectile_damage\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 35.0\nlifetime_seconds: 2.5\n"
        "collision_mask: damageable\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n"
        "impact_response:\n"
        "  action: spawn_projectile\n"
        "  projectile_template: rocket_explosion\n"
        "  destroy_self: true\n");
    write_file(
        dir.parent_path() / "projectile_templates" / "rocket_explosion.yaml",
        "id: 8\nname: rocket_explosion\nkind: area_effect\n"
        "collider_template: explosion_damage\n"
        "lifetime_ticks: 45\n"
        "damage_behavior:\n"
        "  type: area_interval\n"
        "  damage_per_interval: 45\n"
        "  damage_interval_ticks: 45\n"
        "  falloff: linear\n"
        "collision_mask: damageable\n");
    write_file(
        dir.parent_path() / "projectile_templates" / "homing_missile.yaml",
        "id: 6\nname: homing_missile_projectile\ndamage: 20\n"
        "sync_mode: hybrid_deterministic_then_snapshot\n"
        "collider_template: sphere_damage\n"
        "movement_model: homing\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 20.0\nlifetime_seconds: 3.0\n"
        "collision_mask: enemy\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n"
        "homing:\n"
        "  homing_mode: fire_and_forget\n"
        "  sync_mode: hybrid_deterministic_then_snapshot\n"
        "  boost_ticks: 2\n"
        "  lock_on_range: 25.0\n"
        "  lose_target_range: 30.0\n"
        "  lock_cone_degrees: 75.0\n"
        "  max_turn_rate_degrees_per_second: 360.0\n"
        "  acceleration: 20.0\n"
        "  max_speed: 30.0\n");
    write_file(
        dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\ncooldown_ticks: 3\nreload_ticks: 30\nmax_range: 100.0\n"
        "segment_collider: rifle_segment_damage\n");
    write_file(
        dir / "shotgun.yaml",
        "id: 1\nname: Shotgun\nweapon_type: shotgun\nmagazine_size: 8\n"
        "damage: 10\ncooldown_ticks: 20\nreload_ticks: 45\nmax_range: 40.0\n"
        "pellet_count: 5\npellet_spread: 0.035\n"
        "segment_collider: shotgun_segment_damage\n");
    write_file(
        dir / "spammer.yaml",
        "id: 2\nname: Projectile Spammer\nweapon_type: projectile\n"
        "magazine_size: 120\n"
        "cooldown_ticks: 1\nreload_ticks: 30\n"
        "projectile_template: spammer_projectile\n");
    write_file(
        dir / "rocket.yaml",
        "id: 3\nname: Rocket\nweapon_type: projectile\nmagazine_size: 6\n"
        "cooldown_ticks: 45\nreload_ticks: 75\n"
        "projectile_template: rocket_projectile\n");
    write_file(
        dir / "fire_floor.yaml",
        "id: 4\nname: Fire Floor\nweapon_type: area_effect\nmagazine_size: 3\n"
        "damage: 12\ncooldown_ticks: 10\nreload_ticks: 30\narea_effect:\n"
        "  collider_template: explosion_damage\n"
        "  radius: 2.0\n  damage_per_interval: 12\n  damage_interval_ticks: 2\n"
        "  lifetime_ticks: 6\n  spawn_distance: 1.0\n  collision_mask: enemy\n");
    write_file(
        dir / "beam_rifle.yaml",
        "id: 5\nname: Beam Rifle\nweapon_type: beam\nmagazine_size: 12\n"
        "damage: 30\ncooldown_ticks: 1\nreload_ticks: 45\nbeam:\n"
        "  collider_template: beam_damage\n"
        "  length: 8.0\n  radius: 0.25\n  damage_per_second: 30\n"
        "  lifetime_ticks: 2\n  collision_mask: enemy\n");
    write_file(
        dir / "homing_missile.yaml",
        "id: 6\nname: Homing Missile\nweapon_type: projectile\nmagazine_size: 4\n"
        "cooldown_ticks: 15\nreload_ticks: 60\n"
        "projectile_template: homing_missile_projectile\n");
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
    assert(config.weapons.definitions[network_example::game_server::kWeaponRifle]
               .segment_collider_template_id == 5);
    assert(config.weapons.definitions[network_example::game_server::kWeaponShotgun]
               .segment_collider_template_id == 6);
    assert(config.weapons.definitions[network_example::game_server::kWeaponRocket]
               .projectile.damage_shape == KernelProjectileDamageShape_DirectHit);
    assert(config.weapons.definitions[network_example::game_server::kWeaponRocket]
               .projectile.projectile_template_id == 3);
    bool found_rocket_explosion_template = false;
    const network_example::game_server::KernelGameplayCatalogStorage storage =
        network_example::game_server::build_kernel_gameplay_catalog(config);
    for (std::uint32_t index = 0; index < storage.definition.projectile_template_count;
         ++index) {
        const KernelProjectileTemplateDefinition& projectile_template =
            storage.definition.projectile_templates[index];
        if (projectile_template.weapon_id == network_example::game_server::kWeaponGrenade ||
            projectile_template.weapon_id ==
                network_example::game_server::kWeaponHomingMissile) {
            assert(projectile_template.collider_template_id == 7);
        }
        if (projectile_template.weapon_id == network_example::game_server::kWeaponRocket) {
            assert(projectile_template.collider_template_id == 3);
            assert(projectile_template.impact_action ==
                   KernelProjectileImpactAction_SpawnProjectile);
            assert(projectile_template.impact_projectile_template_id == 8);
        }
        if (projectile_template.projectile_template_id == 8) {
            found_rocket_explosion_template = true;
            assert(projectile_template.projectile_kind ==
                   KernelProjectileKind_AreaEffect);
            assert(projectile_template.damage_interval_ticks == 45);
            assert(projectile_template.lifetime_ticks == 45);
            assert(projectile_template.damage == 45);
        }
    }
    assert(found_rocket_explosion_template);
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
    assert(config.weapons.collider_template_ids
               [network_example::game_server::kWeaponFireFloor] == 4);
    assert(config.weapons.definitions[network_example::game_server::kWeaponFireFloor]
               .area_effect.collision_mask == KERNEL_COLLISION_LAYER_ENEMY);
    assert(config.weapons.definitions[network_example::game_server::kWeaponBeamRifle]
               .fire_mode == KernelWeaponFireMode_Beam);
    assert(config.weapons.collider_template_ids
               [network_example::game_server::kWeaponBeamRifle] == 8);
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
    bool found_segment = false;
    bool found_sphere = false;
    bool found_beam = false;
    for (const network_example::game_server::ColliderTemplateConfig& collider :
         config.colliders.templates) {
        if (collider.definition.template_id == 6) {
            found_segment = true;
            assert(collider.definition.shape_type == KernelColliderShapeType_Segment);
            assert(collider.definition.segment_length == 40.0f);
            assert(collider.definition.scatter_degrees == 6.0f);
            assert(collider.definition.lifetime_ticks == 3);
        }
        if (collider.definition.template_id == 7) {
            found_sphere = true;
            assert(collider.definition.shape_type == KernelColliderShapeType_Sphere);
            assert(collider.definition.radius == 0.2f);
        }
        if (collider.definition.template_id == 8) {
            found_beam = true;
            assert(collider.definition.shape_type == KernelColliderShapeType_OrientedBox);
            assert(collider.definition.half_extents.x == 0.1f);
            assert(collider.definition.half_extents.y == 0.1f);
            assert(collider.definition.half_extents.z == 2.5f);
        }
    }
    assert(found_segment);
    assert(found_sphere);
    assert(found_beam);
}

void invalid_templates_are_rejected() {
    const std::filesystem::path duplicate_dir = tmp_dir("duplicate");
    write_valid_templates(duplicate_dir);
    write_file(
        duplicate_dir / "duplicate.yaml",
        "id: 4\nname: Duplicate\nweapon_type: hitscan\nmagazine_size: 1\n"
        "damage: 1\ncooldown_ticks: 1\nreload_ticks: 1\nmax_range: 1.0\n");
    assert(load_fails(duplicate_dir));

    const std::filesystem::path duplicate_name_dir = tmp_dir("duplicate_name");
    write_valid_templates(duplicate_name_dir);
    write_file(
        duplicate_name_dir / "duplicate_name.yaml",
        "id: 4\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 1\n"
        "damage: 1\ncooldown_ticks: 1\nreload_ticks: 1\nmax_range: 1.0\n"
        "segment_collider: rifle_segment_damage\n");
    assert(load_fails(duplicate_name_dir));

    const std::filesystem::path unknown_weapon_field_dir =
        tmp_dir("unknown_weapon_field");
    write_valid_templates(unknown_weapon_field_dir);
    write_file(
        unknown_weapon_field_dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\ncooldown_ticks: 3\nreload_ticks: 30\nmax_range: 100.0\n"
        "segment_collider: rifle_segment_damage\nruntime_instance_id: 9\n");
    assert(load_fails(unknown_weapon_field_dir));

    const std::filesystem::path unknown_area_field_dir =
        tmp_dir("unknown_area_field");
    write_valid_templates(unknown_area_field_dir);
    write_file(
        unknown_area_field_dir / "fire_floor.yaml",
        "id: 4\nname: Fire Floor\nweapon_type: area_effect\nmagazine_size: 3\n"
        "damage: 12\ncooldown_ticks: 10\nreload_ticks: 30\narea_effect:\n"
        "  collider_template: explosion_damage\n"
        "  radius: 2.0\n  damage_per_interval: 12\n  damage_interval_ticks: 2\n"
        "  lifetime_ticks: 6\n  spawn_distance: 1.0\n  collision_mask: enemy\n"
        "  current_tick: 123\n");
    assert(load_fails(unknown_area_field_dir));

    const std::filesystem::path unknown_beam_field_dir =
        tmp_dir("unknown_beam_field");
    write_valid_templates(unknown_beam_field_dir);
    write_file(
        unknown_beam_field_dir / "beam_rifle.yaml",
        "id: 5\nname: Beam Rifle\nweapon_type: beam\nmagazine_size: 12\n"
        "damage: 30\ncooldown_ticks: 1\nreload_ticks: 45\nbeam:\n"
        "  collider_template: beam_damage\n"
        "  length: 8.0\n  radius: 0.25\n  damage_per_second: 30\n"
        "  lifetime_ticks: 2\n  collision_mask: enemy\n  owner: player\n");
    assert(load_fails(unknown_beam_field_dir));

    const std::filesystem::path unknown_homing_field_dir =
        tmp_dir("unknown_homing_field");
    write_valid_templates(unknown_homing_field_dir);
    write_file(
        unknown_homing_field_dir.parent_path() /
            "projectile_templates" / "homing_missile.yaml",
        "id: 6\nname: homing_missile_projectile\ndamage: 20\n"
        "sync_mode: hybrid_deterministic_then_snapshot\n"
        "collider_template: sphere_damage\n"
        "movement_model: homing\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 20.0\nlifetime_seconds: 3.0\n"
        "collision_mask: enemy\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n"
        "homing:\n"
        "  homing_mode: fire_and_forget\n"
        "  sync_mode: hybrid_deterministic_then_snapshot\n"
        "  boost_ticks: 2\n"
        "  lock_on_range: 25.0\n"
        "  lose_target_range: 30.0\n"
        "  lock_cone_degrees: 75.0\n"
        "  max_turn_rate_degrees_per_second: 360.0\n"
        "  acceleration: 20.0\n"
        "  max_speed: 30.0\n"
        "  owner_entity_id: 99\n");
    assert(load_fails(unknown_homing_field_dir));

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
        homing_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 1\n"
        "sync_mode: server_snapshot_only\ncollider_template: projectile_damage\n"
        "movement_model: homing\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 1.0\nlifetime_seconds: 1.0\n"
        "collision_mask: damageable\nmax_hit_count: 1\n");
    assert(load_fails(homing_dir));

    const std::filesystem::path invalid_homing_dir = tmp_dir("invalid_homing");
    write_valid_templates(invalid_homing_dir);
    write_file(
        invalid_homing_dir.parent_path() /
            "projectile_templates" / "homing_missile.yaml",
        "id: 6\nname: homing_missile_projectile\ndamage: 1\n"
        "sync_mode: hybrid_deterministic_then_snapshot\n"
        "collider_template: sphere_damage\n"
        "movement_model: homing\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 1.0\nlifetime_seconds: 1.0\n"
        "collision_mask: enemy\nmax_hit_count: 1\n"
        "homing:\n"
        "  homing_mode: retarget\n"
        "  sync_mode: hybrid_deterministic_then_snapshot\n"
        "  boost_ticks: 1\n"
        "  lock_on_range: 10.0\n"
        "  lose_target_range: 12.0\n"
        "  lock_cone_degrees: 75.0\n"
        "  max_turn_rate_degrees_per_second: 360.0\n"
        "  acceleration: 10.0\n"
        "  max_speed: 20.0\n");
    assert(load_fails(invalid_homing_dir));

    const std::filesystem::path homing_on_linear_dir = tmp_dir("homing_on_linear");
    write_valid_templates(homing_on_linear_dir);
    write_file(
        homing_on_linear_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 1\n"
        "sync_mode: server_snapshot_only\ncollider_template: projectile_damage\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 1.0\nlifetime_seconds: 1.0\n"
        "collision_mask: damageable\nmax_hit_count: 1\n"
        "homing: {homing_mode: fire_and_forget}\n");
    assert(load_fails(homing_on_linear_dir));

    const std::filesystem::path bounce_dir = tmp_dir("bounce");
    write_valid_templates(bounce_dir);
    write_file(
        bounce_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 1\n"
        "sync_mode: server_snapshot_only\ncollider_template: projectile_damage\n"
        "movement_model: linear\nhit_response: bounce\n"
        "damage_shape: direct_hit\nspeed: 1.0\nlifetime_seconds: 1.0\n"
        "collision_mask: damageable\nmax_hit_count: 1\n");
    assert(load_fails(bounce_dir));

    const std::filesystem::path invalid_sync_dir = tmp_dir("invalid_sync");
    write_valid_templates(invalid_sync_dir);
    write_file(
        invalid_sync_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 1\n"
        "sync_mode: remote_magic\ncollider_template: projectile_damage\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 1.0\nlifetime_seconds: 1.0\n"
        "collision_mask: damageable\nmax_hit_count: 1\n");
    assert(load_fails(invalid_sync_dir));

    const std::filesystem::path removed_radius_dir = tmp_dir("removed_radius");
    write_valid_templates(removed_radius_dir);
    const std::string removed_radius_key = std::string("explosion_") + "radius";
    write_file(
        removed_radius_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        std::string("id: 3\nname: rocket_projectile\ndamage: 45\n")
            + "sync_mode: server_snapshot_only\ncollider_template: projectile_damage\n"
              "movement_model: linear\nhit_response: destroy\n"
              "damage_shape: direct_hit\nspeed: 35.0\nlifetime_seconds: 2.5\n"
            + removed_radius_key
            + ": 3.0\ncollision_mask: damageable\nmax_hit_count: 1\n"
              "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    assert(load_fails(removed_radius_dir));

    const std::filesystem::path explosion_damage_shape_dir =
        tmp_dir("explosion_damage_shape");
    write_valid_templates(explosion_damage_shape_dir);
    write_file(
        explosion_damage_shape_dir.parent_path() /
            "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\ncollider_template: projectile_damage\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: explosion\nspeed: 35.0\nlifetime_seconds: 2.5\n"
        "collision_mask: damageable\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    assert(load_fails(explosion_damage_shape_dir));

    const std::filesystem::path unknown_projectile_dir =
        tmp_dir("unknown_projectile_template");
    write_valid_templates(unknown_projectile_dir);
    write_file(
        unknown_projectile_dir / "rocket.yaml",
        "id: 3\nname: Rocket\nweapon_type: projectile\nmagazine_size: 6\n"
        "cooldown_ticks: 45\nreload_ticks: 75\n"
        "projectile_template: missing_projectile\n");
    assert(load_fails(unknown_projectile_dir));
}

void collision_mask_expressions_are_loaded() {
    const std::filesystem::path none_dir = tmp_dir("mask_none");
    write_valid_templates(none_dir);
    write_file(
        none_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\ncollider_template: projectile_damage\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 35.0\nlifetime_seconds: 2.5\n"
        "collision_mask: none\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            none_dir.string());
    assert(config.weapons.definitions[network_example::game_server::kWeaponRocket]
               .projectile.collision_mask == KERNEL_COLLISION_MASK_NONE);

    const std::filesystem::path zero_dir = tmp_dir("mask_zero");
    write_valid_templates(zero_dir);
    write_file(
        zero_dir / "beam_rifle.yaml",
        "id: 5\nname: Beam Rifle\nweapon_type: beam\nmagazine_size: 12\n"
        "damage: 30\ncooldown_ticks: 1\nreload_ticks: 45\nbeam:\n"
        "  collider_template: beam_damage\n"
        "  length: 8.0\n  radius: 0.25\n  damage_per_second: 30\n"
        "  lifetime_ticks: 2\n  collision_mask: 0\n");
    config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            zero_dir.string());
    assert(config.weapons.definitions[network_example::game_server::kWeaponBeamRifle]
               .beam.collision_mask == KERNEL_COLLISION_MASK_NONE);

    const std::filesystem::path expression_dir = tmp_dir("mask_expression");
    write_valid_templates(expression_dir);
    write_file(
        expression_dir / "fire_floor.yaml",
        "id: 4\nname: Fire Floor\nweapon_type: area_effect\nmagazine_size: 3\n"
        "damage: 12\ncooldown_ticks: 10\nreload_ticks: 30\narea_effect:\n"
        "  collider_template: explosion_damage\n"
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
        unknown_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\ncollider_template: projectile_damage\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 35.0\nlifetime_seconds: 2.5\n"
        "collision_mask: ghost\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    assert(load_fails(unknown_dir));

    const std::filesystem::path empty_token_dir = tmp_dir("mask_empty_token");
    write_valid_templates(empty_token_dir);
    write_file(
        empty_token_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\ncollider_template: projectile_damage\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 35.0\nlifetime_seconds: 2.5\n"
        "collision_mask: enemy |\n"
        "max_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
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
    assert(config.colliders.templates.size() == 8);
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
