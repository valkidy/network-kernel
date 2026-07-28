#include "game_server/gameplay_config.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace {

constexpr std::uint16_t kMaxReserveMagazines =
    std::numeric_limits<std::uint16_t>::max();

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
    const std::filesystem::path root = std::filesystem::path(test_tmpdir) / name;
    std::filesystem::remove_all(root);
    const std::filesystem::path path = root / "weapon_templates";
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
    const std::filesystem::path source_dir =
        runfiles_root() / "game_server" / "collider_templates";
    const std::filesystem::path destination_dir =
        weapon_dir.parent_path() / "collider_templates";
    std::filesystem::create_directories(destination_dir);
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(source_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".yaml") {
            std::filesystem::copy_file(
                entry.path(),
                destination_dir / entry.path().filename(),
                std::filesystem::copy_options::overwrite_existing);
        }
    }
}

void write_valid_action_catalog(const std::filesystem::path& weapon_dir) {
    const std::filesystem::path source_dir =
        runfiles_root() / "game_server" / "action_templates";
    const std::filesystem::path destination_dir =
        weapon_dir.parent_path() / "action_templates";
    std::filesystem::create_directories(destination_dir);
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(source_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".yaml") {
            std::filesystem::copy_file(
                entry.path(),
                destination_dir / entry.path().filename(),
                std::filesystem::copy_options::overwrite_existing);
        }
    }
}

void write_valid_action_graph_catalog(const std::filesystem::path& weapon_dir) {
    const std::filesystem::path source_dir =
        runfiles_root() / "game_server" / "action_graph_templates";
    const std::filesystem::path destination_dir =
        weapon_dir.parent_path() / "action_graph_templates";
    std::filesystem::create_directories(destination_dir);
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(source_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".yaml") {
            std::filesystem::copy_file(
                entry.path(),
                destination_dir / entry.path().filename(),
                std::filesystem::copy_options::overwrite_existing);
        }
    }
}

void write_valid_templates(const std::filesystem::path& dir) {
    write_valid_collider_catalog(dir);
    write_valid_action_catalog(dir);
    write_valid_action_graph_catalog(dir);
    write_file(
        dir.parent_path() / "projectile_templates" / "spammer.yaml",
        "id: 2\nname: spammer_projectile\ndamage: 1\n"
        "sync_mode: local_predicted_deterministic\n"
        "collider_template: projectile_sphere\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 30.0\nlifetime_ticks: 60\n"
        "collision_mask: player_side\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    write_file(
        dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\n"
        "collider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 35.0\nlifetime_ticks: 75\n"
        "collision_mask: damageable\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n"
        "triggers:\n"
        "  on_projectile_impact:\n"
        "    action_graph: action_spawn_projectile_at_impact\n"
        "    parameters:\n"
        "      template: rocket_explosion\n"
        "      position: event.position\n"
        "      direction: event.direction\n");
    write_file(
        dir.parent_path() / "projectile_templates" / "rocket_explosion.yaml",
        "id: 8\nname: rocket_explosion\nkind: area_effect\n"
        "collider_template: area_effect_sphere\n"
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
        "collider_template: projectile_sphere\n"
        "movement_model: homing\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 20.0\nlifetime_ticks: 90\n"
        "collision_mask: hostile_side\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n"
        "homing:\n"
        "  homing_mode: fire_and_forget\n"
        "  sync_mode: hybrid_deterministic_then_snapshot\n"
        "  boost_ticks: 2\n"
        "  lock_on_range: 25.0\n"
        "  lose_target_range: 30.0\n"
        "  lock_cone_degrees: 75.0\n"
        "  max_turn_degrees_per_tick: 12.0\n"
        "  acceleration: 20.0\n"
        "  max_speed: 30.0\n");
    write_file(
        dir.parent_path() / "projectile_templates" / "grenade_shell.yaml",
        "id: 7\nname: grenade_shell_projectile\ndamage: 45\n"
        "sync_mode: local_predicted_deterministic\n"
        "collider_template: projectile_sphere\n"
        "movement_model: parabolic\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 24.0\nlifetime_ticks: 180\n"
        "collision_mask: damageable\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: -9.81, z: 0.0}\n"
        "triggers:\n"
        "  on_projectile_impact:\n"
        "    action_graph: action_spawn_projectile_at_impact\n"
        "    parameters:\n"
        "      template: rocket_explosion\n"
        "      position: event.position\n"
        "      direction: event.direction\n");
    write_file(
        dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        "id: 4\nname: fire_floor_area\ntype: area_effect\n"
        "collider_template: area_effect_sphere\n"
        "lifetime_ticks: 6\n"
        "damage_behavior:\n"
        "  type: area_interval\n"
        "  damage_per_interval: 12\n"
        "  damage_interval_ticks: 2\n"
        "  falloff: none\n"
        "collision_mask: hostile_side\n");
    write_file(
        dir.parent_path() / "projectile_templates" / "beam_rifle_beam.yaml",
        "id: 5\nname: beam_rifle_beam\ntype: beam\ndamage: 30\n"
        "sync_mode: server_snapshot_only\n"
        "collider_template: beam_oriented_box\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 0.0\nlifetime_ticks: 0\n"
        "collision_mask: hostile_side\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n"
        "beam:\n"
        "  length: 8.0\n"
        "  radius: 0.25\n"
        "  damage_per_tick: 1\n"
        "  lifetime_ticks: 2\n"
        "  collision_mask: hostile_side\n");
    write_file(
        dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\nfire_action_template: rifle_fire\nreload_ticks: 30\nmax_range: 100.0\n"
        "segment_collider: rifle_segment\n");
    write_file(
        dir / "shotgun.yaml",
        "id: 1\nname: Shotgun\nweapon_type: shotgun\nmagazine_size: 8\n"
        "damage: 10\nfire_action_template: shotgun_fire\nreload_ticks: 45\nmax_range: 40.0\n"
        "pellet_count: 5\npellet_spread: 0.035\n"
        "segment_collider: shotgun_segment\n");
    write_file(
        dir / "spammer.yaml",
        "id: 2\nname: Projectile Spammer\nweapon_type: projectile\n"
        "magazine_size: 120\n"
        "fire_action_template: spammer_fire\nreload_ticks: 30\n"
        "projectile_template: spammer_projectile\n");
    write_file(
        dir / "rocket.yaml",
        "id: 3\nname: Rocket\nweapon_type: projectile\nmagazine_size: 6\n"
        "fire_action_template: rocket_fire\nreload_ticks: 75\n"
        "projectile_template: rocket_projectile\n");
    write_file(
        dir / "fire_floor.yaml",
        "id: 4\nname: Fire Floor\nweapon_type: area_effect\nmagazine_size: 3\n"
        "damage: 12\nfire_action_template: fire_floor_cast\nreload_ticks: 30\n"
        "projectile_template: fire_floor_area\n");
    write_file(
        dir / "beam_rifle.yaml",
        "id: 5\nname: Beam Rifle\nweapon_type: beam\nmagazine_size: 12\n"
        "damage: 30\nfire_action_template: beam_rifle_fire\nreload_ticks: 45\n"
        "projectile_template: beam_rifle_beam\n");
    write_file(
        dir / "homing_missile.yaml",
        "id: 6\nname: Homing Missile\nweapon_type: projectile\nmagazine_size: 4\n"
        "fire_action_template: homing_missile_fire\nreload_ticks: 60\n"
        "projectile_template: homing_missile_projectile\n");
    write_file(
        dir / "grenade_launcher.yaml",
        "id: 99\nname: Grenade Launcher\nweapon_type: projectile\n"
        "magazine_size: 6\n"
        "fire_action_template: grenade_launcher_fire\nreload_ticks: 90\n"
        "projectile_template: grenade_shell_projectile\n");
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

const KernelProjectileMechanicsDefinition& projectile_mechanics(
    const network_example::game_server::GameServerGameplayConfig& config,
    std::uint32_t projectile_template_id) {
    for (const network_example::game_server::ProjectileTemplateConfig& projectile :
         config.projectile_templates) {
        if (projectile.definition.projectile_template_id == projectile_template_id) {
            return projectile.definition.mechanics;
        }
    }
    std::abort();
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
               .reserve_magazines == 6);
    assert(config.weapons.definitions[network_example::game_server::kWeaponRifle]
               .segment_collider_template_id == 5);
    assert(config.weapons.definitions[network_example::game_server::kWeaponShotgun]
               .segment_collider_template_id == 6);
    assert(config.weapons.definitions[network_example::game_server::kWeaponRocket]
               .projectile_template_id == 3);
    bool found_rocket_explosion_template = false;
    const network_example::game_server::KernelGameplayCatalogStorage storage =
        network_example::game_server::build_kernel_gameplay_catalog(config);
    for (std::uint32_t index = 0; index < storage.definition.projectile_template_count;
         ++index) {
        const KernelProjectileTemplateDefinition& projectile_template =
            storage.definition.projectile_templates[index];
        if (projectile_template.weapon_id == network_example::game_server::kWeaponSpammer ||
            projectile_template.weapon_id ==
                network_example::game_server::kWeaponGrenade ||
            projectile_template.weapon_id ==
                network_example::game_server::kWeaponHomingMissile) {
            assert(projectile_template.mechanics.collider_template_id == 7);
        }
        if (projectile_template.weapon_id == network_example::game_server::kWeaponRocket) {
            assert(projectile_template.mechanics.collider_template_id == 3);
            assert(projectile_template.mechanics
                       .projectile_impact_trigger.action_type ==
                   KernelEntityTriggerActionType_SpawnProjectile);
            assert(projectile_template.mechanics.projectile_impact_trigger
                       .spawn_projectile_template_id == 8);
            assert(projectile_template.mechanics.collision_query_mode ==
                   KernelProjectileCollisionQueryMode_Auto);
        }
        if (projectile_template.projectile_template_id == 8) {
            found_rocket_explosion_template = true;
            assert(projectile_template.mechanics.projectile_type ==
                   KernelProjectileType_AreaEffect);
            assert(projectile_template.mechanics.area_effect.damage_interval_ticks == 45);
            assert(projectile_template.mechanics.area_effect.lifetime_ticks == 45);
            assert(projectile_template.mechanics.damage == 45);
        }
    }
    assert(found_rocket_explosion_template);
    assert(config.weapons.definitions[network_example::game_server::kWeaponSpammer]
               .damage == 1);
    assert(config.weapons.definitions[network_example::game_server::kWeaponSpammer]
               .magazine_size == 120);
    assert(config.weapons.definitions[network_example::game_server::kWeaponSpammer]
               .reserve_magazines == kMaxReserveMagazines);
    assert(config.weapons.definitions[network_example::game_server::kWeaponSpammer]
               .projectile_template_id == 2);
    assert(config.weapons.definitions[network_example::game_server::kWeaponFireFloor]
               .fire_mode == KernelWeaponFireMode_Projectile);
    assert(config.weapons.collider_template_ids
               [network_example::game_server::kWeaponFireFloor] == 4);
    assert(config.weapons.definitions[network_example::game_server::kWeaponFireFloor]
               .projectile_template_id == 4);
    assert(config.weapons.definitions[network_example::game_server::kWeaponBeamRifle]
               .fire_mode == KernelWeaponFireMode_Projectile);
    assert(config.weapons.collider_template_ids
               [network_example::game_server::kWeaponBeamRifle] == 8);
    assert(config.weapons.definitions[network_example::game_server::kWeaponBeamRifle]
               .projectile_template_id == 5);
    assert(config.weapons.definitions[network_example::game_server::kWeaponHomingMissile]
               .projectile_template_id == 6);
    assert(config.weapons.configured[network_example::game_server::kWeaponGrenade]);
    assert(config.weapons.projectile_sync_modes
               [network_example::game_server::kWeaponGrenade] ==
           KernelProjectileSyncMode_LocalPredictedDeterministic);
    assert(config.weapons.names[network_example::game_server::kWeaponGrenade] ==
           "Grenade Launcher");
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
    assert(config.action_templates.size() == 9);
    assert(config.weapons.definitions[network_example::game_server::kWeaponRifle]
               .fire_action_template_id == 4096);
    assert(config.weapons.definitions[network_example::game_server::kWeaponRocket]
               .fire_action_template_id == 4099);
    assert(config.weapons.definitions[network_example::game_server::kWeaponBeamRifle]
               .fire_action_template_id == 4101);
    assert(storage.definition.action_template_count == 9);
    assert(config.action_templates[3].definition.commit_offset_ticks == 3);
    assert(config.action_templates[3].definition.commit_interval_ticks == 30);
    assert(config.action_templates[5].definition.trigger_mode ==
           KernelActionTriggerMode_Hold);
    assert(config.action_templates[5].definition.hold_input_timeout_ticks == 6);
    network_example::game_server::GameServerGameplayConfig changed_action = config;
    ++changed_action.action_templates[0].definition.commit_interval_ticks;
    assert(network_example::game_server::compute_gameplay_catalog_hash(changed_action) !=
           config.weapons.catalog_hash);
    bool found_segment = false;
    bool found_sphere = false;
    bool found_beam = false;
    for (const network_example::game_server::ColliderTemplateConfig& collider :
         config.colliders.templates) {
        if (collider.definition.template_id == 6) {
            found_segment = true;
            assert(collider.definition.shape_type == KernelColliderShapeType_Segment);
            assert(collider.definition.shape_params.x == 40.0f);
            assert(collider.definition.shape_params.z == 6.0f);
            assert(collider.definition.lifetime_ticks == 3);
        }
        if (collider.definition.template_id == 7) {
            found_sphere = true;
            assert(collider.definition.shape_type == KernelColliderShapeType_Sphere);
            assert(collider.definition.shape_params.x == 0.2f);
        }
        if (collider.definition.template_id == 8) {
            found_beam = true;
            assert(collider.definition.shape_type == KernelColliderShapeType_OrientedBox);
            assert(collider.definition.shape_params.x == 0.25f);
            assert(collider.definition.shape_params.y == 0.25f);
            assert(collider.definition.shape_params.z == 4.0f);
        }
    }
    assert(found_segment);
    assert(found_sphere);
    assert(found_beam);
}

void projectile_collision_query_modes_are_loaded() {
    const std::filesystem::path dir = tmp_dir("collision_query_mode");
    write_valid_templates(dir);
    write_file(
        dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\n"
        "collider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 35.0\nlifetime_ticks: 75\n"
        "collision_query_mode: overlap\n"
        "collision_mask: damageable\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");

    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            dir.string());

    assert(projectile_mechanics(config, 2).collision_query_mode ==
           KernelProjectileCollisionQueryMode_Auto);
    assert(projectile_mechanics(config, 3).collision_query_mode ==
           KernelProjectileCollisionQueryMode_Overlap);
}

void invalid_templates_are_rejected() {
    const std::filesystem::path legacy_dir = tmp_dir("legacy_cooldown");
    write_valid_templates(legacy_dir);
    write_file(
        legacy_dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\ncooldown_ticks: 3\nreload_ticks: 30\nmax_range: 100.0\n"
        "segment_collider: rifle_segment\n");
    try {
        (void)network_example::game_server::
            load_gameplay_config_from_weapon_template_directory(
                legacy_dir.string());
        assert(false);
    } catch (const network_example::game_server::DataLoadError& error) {
        assert(
            error.error_code ==
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNKNOWN_FIELD);
        assert(error.field == "cooldown_ticks");
        assert(error.template_kind ==
               KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_WEAPON);
        assert(error.template_id == 0);
        assert(error.line > 0);
        assert(error.column > 0);
    }

    const std::filesystem::path finite_press_dir = tmp_dir("finite_press");
    write_valid_templates(finite_press_dir);
    write_file(
        finite_press_dir.parent_path() /
            "action_templates" / "rifle_fire.yaml",
        "id: 4096\nname: rifle_fire\ntrigger_mode: press\n"
        "flags: [cancel_on_death]\nammo_cost_per_commit: 1\n"
        "commit_offset_ticks: 0\ncommit_interval_ticks: 2\n"
        "max_commit_count: 3\nrecovery_ticks: 0\n"
        "hold_input_timeout_ticks: 0\n");
    assert(!load_fails(finite_press_dir));

    const std::filesystem::path zero_fire_interval_dir =
        tmp_dir("zero_fire_interval");
    write_valid_templates(zero_fire_interval_dir);
    const std::filesystem::path zero_fire_action =
        zero_fire_interval_dir.parent_path() /
        "action_templates" / "rifle_fire.yaml";
    write_file(
        zero_fire_action,
        "id: 4096\nname: rifle_fire\ntrigger_mode: press\n"
        "flags: [cancel_on_death]\nammo_cost_per_commit: 1\n"
        "commit_offset_ticks: 0\ncommit_interval_ticks: 0\n"
        "max_commit_count: 1\nrecovery_ticks: 0\n"
        "hold_input_timeout_ticks: 0\n");
    try {
        (void)network_example::game_server::
            load_gameplay_config_from_weapon_template_directory(
                zero_fire_interval_dir.string());
        assert(false);
    } catch (const network_example::game_server::DataLoadError& error) {
        assert(
            error.error_code ==
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_NUMERIC_RANGE);
        assert(error.path == zero_fire_action.string());
        assert(error.field == "commit_interval_ticks");
        assert(error.template_kind ==
               KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTION);
        assert(error.template_id == 4096);
        assert(error.line > 0);
        assert(error.column > 0);
    }

    const std::filesystem::path invalid_press_dir = tmp_dir("invalid_press");
    write_valid_templates(invalid_press_dir);
    write_file(
        invalid_press_dir.parent_path() /
            "action_templates" / "rifle_fire.yaml",
        "id: 4096\nname: rifle_fire\ntrigger_mode: press\n"
        "flags: []\nammo_cost_per_commit: 1\n"
        "commit_offset_ticks: 0\ncommit_interval_ticks: 2\n"
        "max_commit_count: 0\nrecovery_ticks: 0\n"
        "hold_input_timeout_ticks: 1\n");
    assert(load_fails(invalid_press_dir));

    const std::filesystem::path missing_policy_dir = tmp_dir("missing_policy");
    write_valid_templates(missing_policy_dir);
    write_file(
        missing_policy_dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\nreload_ticks: 30\nmax_range: 100.0\n"
        "segment_collider: rifle_segment\n");
    assert(load_fails(missing_policy_dir));

    const std::filesystem::path dangling_action_dir = tmp_dir("dangling_action");
    write_valid_templates(dangling_action_dir);
    write_file(
        dangling_action_dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\nfire_action_template: missing_action\nreload_ticks: 30\n"
        "max_range: 100.0\nsegment_collider: rifle_segment\n");
    assert(load_fails(dangling_action_dir));

    const std::filesystem::path duplicate_action_dir = tmp_dir("duplicate_action");
    write_valid_templates(duplicate_action_dir);
    write_file(
        duplicate_action_dir.parent_path() / "action_templates" / "duplicate.yaml",
        "id: 4096\nname: duplicate\ntrigger_mode: press\n"
        "flags: [cancel_on_death]\nammo_cost_per_commit: 1\n"
        "commit_offset_ticks: 0\ncommit_interval_ticks: 1\nmax_commit_count: 1\n"
        "recovery_ticks: 0\nhold_input_timeout_ticks: 0\n");
    assert(load_fails(duplicate_action_dir));

    const std::filesystem::path invalid_action_dir = tmp_dir("invalid_action");
    write_valid_templates(invalid_action_dir);
    write_file(
        invalid_action_dir.parent_path() / "action_templates" / "rifle_fire.yaml",
        "id: 4096\nname: rifle_fire\ntrigger_mode: hold\n"
        "flags: [cancel_on_release]\nammo_cost_per_commit: 1\n"
        "commit_offset_ticks: 0\ncommit_interval_ticks: 0\nmax_commit_count: 0\n"
        "recovery_ticks: 4\nhold_input_timeout_ticks: 6\n");
    assert(load_fails(invalid_action_dir));

    const std::filesystem::path duplicate_dir = tmp_dir("duplicate");
    write_valid_templates(duplicate_dir);
    write_file(
        duplicate_dir / "duplicate.yaml",
        "id: 4\nname: Duplicate\nweapon_type: hitscan\nmagazine_size: 1\n"
        "damage: 1\nreload_ticks: 1\nmax_range: 1.0\n");
    assert(load_fails(duplicate_dir));

    const std::filesystem::path duplicate_name_dir = tmp_dir("duplicate_name");
    write_valid_templates(duplicate_name_dir);
    write_file(
        duplicate_name_dir / "duplicate_name.yaml",
        "id: 4\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 1\n"
        "damage: 1\nreload_ticks: 1\nmax_range: 1.0\n"
        "segment_collider: rifle_segment\n");
    assert(load_fails(duplicate_name_dir));

    const std::filesystem::path unknown_weapon_field_dir =
        tmp_dir("unknown_weapon_field");
    write_valid_templates(unknown_weapon_field_dir);
    write_file(
        unknown_weapon_field_dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\nreload_ticks: 30\nmax_range: 100.0\n"
        "segment_collider: rifle_segment\nruntime_instance_id: 9\n");
    assert(load_fails(unknown_weapon_field_dir));

    const std::filesystem::path unknown_area_field_dir =
        tmp_dir("unknown_area_field");
    write_valid_templates(unknown_area_field_dir);
    write_file(
        unknown_area_field_dir / "fire_floor.yaml",
        "id: 4\nname: Fire Floor\nweapon_type: area_effect\nmagazine_size: 3\n"
        "damage: 12\nreload_ticks: 30\narea_effect:\n"
        "  collider_template: area_effect_sphere\n"
        "  radius: 2.0\n  damage_per_interval: 12\n  damage_interval_ticks: 2\n"
        "  lifetime_ticks: 6\n  spawn_distance: 1.0\n  collision_mask: hostile_side\n"
        "  current_tick: 123\n");
    assert(load_fails(unknown_area_field_dir));

    const std::filesystem::path unknown_beam_field_dir =
        tmp_dir("unknown_beam_field");
    write_valid_templates(unknown_beam_field_dir);
    write_file(
        unknown_beam_field_dir / "beam_rifle.yaml",
        "id: 5\nname: Beam Rifle\nweapon_type: beam\nmagazine_size: 12\n"
        "damage: 30\nreload_ticks: 45\nbeam:\n"
        "  collider_template: beam_oriented_box\n"
        "  length: 8.0\n  radius: 0.25\n  damage_per_tick: 1\n"
        "  lifetime_ticks: 2\n  collision_mask: hostile_side\n  owner: player\n");
    assert(load_fails(unknown_beam_field_dir));

    const std::filesystem::path unknown_homing_field_dir =
        tmp_dir("unknown_homing_field");
    write_valid_templates(unknown_homing_field_dir);
    write_file(
        unknown_homing_field_dir.parent_path() /
            "projectile_templates" / "homing_missile.yaml",
        "id: 6\nname: homing_missile_projectile\ndamage: 20\n"
        "sync_mode: hybrid_deterministic_then_snapshot\n"
        "collider_template: projectile_sphere\n"
        "movement_model: homing\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 20.0\nlifetime_ticks: 90\n"
        "collision_mask: hostile_side\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n"
        "homing:\n"
        "  homing_mode: fire_and_forget\n"
        "  sync_mode: hybrid_deterministic_then_snapshot\n"
        "  boost_ticks: 2\n"
        "  lock_on_range: 25.0\n"
        "  lose_target_range: 30.0\n"
        "  lock_cone_degrees: 75.0\n"
        "  max_turn_degrees_per_tick: 12.0\n"
        "  acceleration: 20.0\n"
        "  max_speed: 30.0\n"
        "  owner_entity_id: 99\n");
    assert(load_fails(unknown_homing_field_dir));

    const std::filesystem::path hitscan_projectile_dir = tmp_dir("hitscan_projectile");
    write_valid_templates(hitscan_projectile_dir);
    write_file(
        hitscan_projectile_dir / "rifle.yaml",
        "id: 0\nname: Bad Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\nreload_ticks: 30\nmax_range: 100.0\n"
        "projectile: {speed: 10.0}\n");
    assert(load_fails(hitscan_projectile_dir));

    const std::filesystem::path missing_beam_dir = tmp_dir("missing_beam");
    write_valid_templates(missing_beam_dir);
    write_file(
        missing_beam_dir / "beam_rifle.yaml",
        "id: 5\nname: Beam\nweapon_type: beam\nmagazine_size: 1\n"
        "damage: 1\nreload_ticks: 1\n");
    assert(load_fails(missing_beam_dir));

    const std::filesystem::path invalid_beam_dir = tmp_dir("invalid_beam");
    write_valid_templates(invalid_beam_dir);
    write_file(
        invalid_beam_dir / "beam_rifle.yaml",
        "id: 5\nname: Beam\nweapon_type: beam\nmagazine_size: 1\n"
        "damage: 1\nreload_ticks: 1\nbeam:\n"
        "  length: 0.0\n  radius: 0.25\n  damage_per_tick: 1\n"
        "  lifetime_ticks: 2\n  collision_mask: hostile_side\n");
    assert(load_fails(invalid_beam_dir));

    const std::filesystem::path beam_on_hitscan_dir = tmp_dir("beam_on_hitscan");
    write_valid_templates(beam_on_hitscan_dir);
    write_file(
        beam_on_hitscan_dir / "rifle.yaml",
        "id: 0\nname: Bad Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\nreload_ticks: 30\nmax_range: 100.0\n"
        "beam: {length: 8.0}\n");
    assert(load_fails(beam_on_hitscan_dir));

    const std::filesystem::path homing_dir = tmp_dir("homing");
    write_valid_templates(homing_dir);
    write_file(
        homing_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 1\n"
        "sync_mode: server_snapshot_only\ncollider_template: rocket_aabb\n"
        "movement_model: homing\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 1.0\nlifetime_ticks: 30\n"
        "collision_mask: damageable\nmax_hit_count: 1\n");
    assert(load_fails(homing_dir));

    const std::filesystem::path invalid_homing_dir = tmp_dir("invalid_homing");
    write_valid_templates(invalid_homing_dir);
    write_file(
        invalid_homing_dir.parent_path() /
            "projectile_templates" / "homing_missile.yaml",
        "id: 6\nname: homing_missile_projectile\ndamage: 1\n"
        "sync_mode: hybrid_deterministic_then_snapshot\n"
        "collider_template: projectile_sphere\n"
        "movement_model: homing\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 1.0\nlifetime_ticks: 30\n"
        "collision_mask: hostile_side\nmax_hit_count: 1\n"
        "homing:\n"
        "  homing_mode: retarget\n"
        "  sync_mode: hybrid_deterministic_then_snapshot\n"
        "  boost_ticks: 1\n"
        "  lock_on_range: 10.0\n"
        "  lose_target_range: 12.0\n"
        "  lock_cone_degrees: 75.0\n"
        "  max_turn_degrees_per_tick: 12.0\n"
        "  acceleration: 10.0\n"
        "  max_speed: 20.0\n");
    assert(load_fails(invalid_homing_dir));

    const std::filesystem::path homing_on_linear_dir = tmp_dir("homing_on_linear");
    write_valid_templates(homing_on_linear_dir);
    write_file(
        homing_on_linear_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 1\n"
        "sync_mode: server_snapshot_only\ncollider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 1.0\nlifetime_ticks: 30\n"
        "collision_mask: damageable\nmax_hit_count: 1\n"
        "homing: {homing_mode: fire_and_forget}\n");
    assert(load_fails(homing_on_linear_dir));

    const std::filesystem::path bounce_dir = tmp_dir("bounce");
    write_valid_templates(bounce_dir);
    write_file(
        bounce_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 1\n"
        "sync_mode: server_snapshot_only\ncollider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: bounce\n"
        "damage_shape: direct_hit\nspeed: 1.0\nlifetime_ticks: 30\n"
        "collision_mask: damageable\nmax_hit_count: 1\n");
    assert(load_fails(bounce_dir));

    const std::filesystem::path invalid_sync_dir = tmp_dir("invalid_sync");
    write_valid_templates(invalid_sync_dir);
    write_file(
        invalid_sync_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 1\n"
        "sync_mode: remote_magic\ncollider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 1.0\nlifetime_ticks: 30\n"
        "collision_mask: damageable\nmax_hit_count: 1\n");
    assert(load_fails(invalid_sync_dir));

    const std::filesystem::path removed_radius_dir = tmp_dir("removed_radius");
    write_valid_templates(removed_radius_dir);
    const std::string removed_radius_key = std::string("explosion_") + "radius";
    write_file(
        removed_radius_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        std::string("id: 3\nname: rocket_projectile\ndamage: 45\n")
            + "sync_mode: server_snapshot_only\ncollider_template: rocket_aabb\n"
              "movement_model: linear\nhit_response: destroy\n"
              "damage_shape: direct_hit\nspeed: 35.0\nlifetime_ticks: 75\n"
            + removed_radius_key
            + ": 3.0\ncollision_mask: damageable\nmax_hit_count: 1\n"
              "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    assert(load_fails(removed_radius_dir));

    const std::filesystem::path area_effect_sphere_shape_dir =
        tmp_dir("area_effect_sphere_shape");
    write_valid_templates(area_effect_sphere_shape_dir);
    write_file(
        area_effect_sphere_shape_dir.parent_path() /
            "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\ncollider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: explosion\nspeed: 35.0\nlifetime_ticks: 75\n"
        "collision_mask: damageable\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    assert(load_fails(area_effect_sphere_shape_dir));

    const std::filesystem::path unknown_projectile_dir =
        tmp_dir("unknown_projectile_template");
    write_valid_templates(unknown_projectile_dir);
    write_file(
        unknown_projectile_dir / "rocket.yaml",
        "id: 3\nname: Rocket\nweapon_type: projectile\nmagazine_size: 6\n"
        "reload_ticks: 75\n"
        "projectile_template: missing_projectile\n");
    assert(load_fails(unknown_projectile_dir));

    const std::filesystem::path cone_projectile_dir =
        tmp_dir("cone_projectile_collider");
    write_valid_templates(cone_projectile_dir);
    write_file(
        cone_projectile_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\ncollider_template: sentry_grunt_vision_cone\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 35.0\nlifetime_ticks: 75\n"
        "collision_mask: damageable\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    assert(load_fails(cone_projectile_dir));
}

void collision_mask_expressions_are_loaded() {
    const std::filesystem::path none_dir = tmp_dir("mask_none");
    write_valid_templates(none_dir);
    write_file(
        none_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\ncollider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 35.0\nlifetime_ticks: 75\n"
        "collision_mask: none\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            none_dir.string());
    assert(projectile_mechanics(config, 3).collision_mask == KERNEL_COLLISION_MASK_NONE);

    const std::filesystem::path zero_dir = tmp_dir("mask_zero");
    write_valid_templates(zero_dir);
    write_file(
        zero_dir.parent_path() / "projectile_templates" / "beam_rifle_beam.yaml",
        "id: 5\nname: beam_rifle_beam\ntype: beam\ndamage: 30\n"
        "sync_mode: server_snapshot_only\n"
        "collider_template: beam_oriented_box\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 0.0\nlifetime_ticks: 0\n"
        "collision_mask: 0\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n"
        "beam:\n"
        "  length: 8.0\n  radius: 0.25\n  damage_per_tick: 1\n"
        "  lifetime_ticks: 2\n  collision_mask: 0\n");
    config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            zero_dir.string());
    assert(projectile_mechanics(config, 5).beam.collision_mask ==
           KERNEL_COLLISION_MASK_NONE);

    const std::filesystem::path expression_dir = tmp_dir("mask_expression");
    write_valid_templates(expression_dir);
    write_file(
        expression_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        "id: 4\nname: fire_floor_area\ntype: area_effect\n"
        "collider_template: area_effect_sphere\n"
        "lifetime_ticks: 6\n"
        "damage_behavior:\n"
        "  type: area_interval\n"
        "  damage_per_interval: 12\n"
        "  damage_interval_ticks: 2\n"
        "  falloff: none\n"
        "collision_mask: player_side | hostile_side\n");
    config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            expression_dir.string());
    assert(projectile_mechanics(config, 4).area_effect.collision_mask ==
           (KERNEL_COLLISION_LAYER_HOSTILE_SIDE | KERNEL_COLLISION_LAYER_PLAYER_SIDE));
}

void malformed_collision_masks_are_rejected() {
    const std::filesystem::path unknown_dir = tmp_dir("mask_unknown");
    write_valid_templates(unknown_dir);
    write_file(
        unknown_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\ncollider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 35.0\nlifetime_ticks: 75\n"
        "collision_mask: ghost\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    assert(load_fails(unknown_dir));

    const std::filesystem::path empty_token_dir = tmp_dir("mask_empty_token");
    write_valid_templates(empty_token_dir);
    write_file(
        empty_token_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\ncollider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 35.0\nlifetime_ticks: 75\n"
        "collision_mask: hostile_side |\n"
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
    assert(config.weapons.catalog_version == 5);
    assert(config.weapons.catalog_hash != 0);
    assert(config.colliders.templates.size() == 12);
    assert(config.colliders.bindings.empty());
}

}  // namespace

int main() {
    valid_repo_templates_load_all_slots();
    projectile_collision_query_modes_are_loaded();
    invalid_templates_are_rejected();
    collision_mask_expressions_are_loaded();
    malformed_collision_masks_are_rejected();
    catalog_file_loads_colliders();
    return 0;
}
