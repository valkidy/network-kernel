#include "game_server/src/gameplay_config.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

}  // namespace

#define require(condition) \
    require_impl(static_cast<bool>(condition), #condition, __LINE__)

namespace {

constexpr std::uint16_t kMaxReserveMagazines =
    std::numeric_limits<std::uint16_t>::max();

std::filesystem::path runfiles_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace;
}

std::filesystem::path tmp_dir(const std::string& name) {
    const char* test_tmpdir = std::getenv("TEST_TMPDIR");
    require(test_tmpdir != nullptr);
    const std::filesystem::path root = std::filesystem::path(test_tmpdir) / name;
    std::filesystem::remove_all(root);
    const std::filesystem::path path = root / "weapon_templates";
    std::filesystem::create_directories(path);
    return path;
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    require(file.good());
    file << text;
}

void write_valid_collider_catalog(const std::filesystem::path& weapon_dir) {
    const std::filesystem::path source_dir =
        runfiles_root() / "game_server" / "gameplay_catalog" /
        "collider_templates";
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
        runfiles_root() / "game_server" / "gameplay_catalog" /
        "action_templates";
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
        runfiles_root() / "game_server" / "gameplay_catalog" /
        "action_graph_templates";
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
        dir.parent_path() / "projectile_templates" / "rifle_shot.yaml",
        "id: 10\nname: rifle_shot\ntype: standard\n"
        "collider_template: rifle_segment\n"
        "movement_model: linear\nsync_mode: local_predicted_deterministic\n"
        "hit_response: destroy\ndamage_shape: direct_hit\ndamage: 25\n"
        "collision_mask: actor | limb | terrain | static_obstacle\n"
        "speed: 200.0\nlifetime_ticks: 3\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    write_file(
        dir.parent_path() / "projectile_templates" / "shotgun_shot.yaml",
        "id: 11\nname: shotgun_shot\ntype: standard\n"
        "collider_template: shotgun_segment\n"
        "movement_model: linear\nsync_mode: local_predicted_deterministic\n"
        "hit_response: destroy\ndamage_shape: direct_hit\ndamage: 10\n"
        "collision_mask: actor | terrain | static_obstacle\n"
        "speed: 200.0\nlifetime_ticks: 3\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
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
        "damage: 45\n"
        "lifetime_ticks: 45\n"
        "damage_behavior:\n"
        "  type: area_interval\n"
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
        "damage: 12\n"
        "lifetime_ticks: 6\n"
        "damage_behavior:\n"
        "  type: area_interval\n"
        "  damage_interval_ticks: 2\n"
        "  falloff: none\n"
        "collision_mask: hostile_side\n");
    write_file(
        dir.parent_path() / "projectile_templates" / "beam_rifle_beam.yaml",
        "id: 5\nname: beam_rifle_beam\ntype: beam\ndamage: 1\n"
        "sync_mode: server_snapshot_only\n"
        "collider_template: beam_oriented_box\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 0.0\nlifetime_ticks: 0\n"
        "collision_mask: hostile_side\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n"
        "beam:\n"
        "  length: 8.0\n"
        "  radius: 0.25\n"
        "  lifetime_ticks: 2\n");
    write_file(
        dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "fire_action_template: rifle_fire\nmax_range: 100.0\n"
        "segment_collider: rifle_segment\n"
        "projectile_template: rifle_shot\n");
    write_file(
        dir / "shotgun.yaml",
        "id: 1\nname: Shotgun\nweapon_type: shotgun\nmagazine_size: 8\n"
        "fire_action_template: shotgun_fire\nmax_range: 40.0\n"
        "pellet_count: 5\npellet_spread: 0.035\n"
        "segment_collider: shotgun_segment\n"
        "projectile_template: shotgun_shot\n");
    write_file(
        dir / "spammer.yaml",
        "id: 2\nname: Projectile Spammer\nweapon_type: projectile\n"
        "magazine_size: 120\n"
        "fire_action_template: spammer_fire\n"
        "projectile_template: spammer_projectile\n");
    write_file(
        dir / "rocket.yaml",
        "id: 3\nname: Rocket\nweapon_type: projectile\nmagazine_size: 6\n"
        "fire_action_template: rocket_fire\n"
        "projectile_template: rocket_projectile\n");
    write_file(
        dir / "fire_floor.yaml",
        "id: 4\nname: Fire Floor\nweapon_type: area_effect\nmagazine_size: 3\n"
        "fire_action_template: fire_floor_cast\n"
        "projectile_template: fire_floor_area\n");
    write_file(
        dir / "beam_rifle.yaml",
        "id: 5\nname: Beam Rifle\nweapon_type: beam\nmagazine_size: 12\n"
        "fire_action_template: beam_rifle_fire\n"
        "projectile_template: beam_rifle_beam\n");
    write_file(
        dir / "homing_missile.yaml",
        "id: 6\nname: Homing Missile\nweapon_type: projectile\nmagazine_size: 4\n"
        "fire_action_template: homing_missile_fire\n"
        "projectile_template: homing_missile_projectile\n");
    write_file(
        dir / "grenade_launcher.yaml",
        "id: 99\nname: Grenade Launcher\nweapon_type: projectile\n"
        "magazine_size: 6\n"
        "fire_action_template: grenade_launcher_fire\n"
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
        runfiles_root() / "game_server" / "gameplay_catalog" /
        "weapon_templates";
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::
            load_gameplay_config_from_weapon_template_directory(dir.string());

    require(config.weapons.definitions[network_example::game_server::kWeaponRifle].fire_mode ==
           KernelWeaponFireMode_Hitscan);
    require(config.weapons.definitions[network_example::game_server::kWeaponRifle]
               .reserve_magazines == 6);
    require(config.weapons.definitions[network_example::game_server::kWeaponRifle]
               .segment_collider_template_id == 5);
    require(config.weapons.definitions[network_example::game_server::kWeaponShotgun]
               .segment_collider_template_id == 6);
    require(config.weapons.definitions[network_example::game_server::kWeaponRocket]
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
                network_example::game_server::kWeaponHomingMissile) {
            require(projectile_template.mechanics.collider_template_id == 7);
        }
        // The grenade shell has been a box (projectile_aabb) since ca61488.
        if (projectile_template.weapon_id == network_example::game_server::kWeaponGrenade) {
            require(projectile_template.mechanics.collider_template_id == 16);
        }
        if (projectile_template.weapon_id == network_example::game_server::kWeaponRocket) {
            require(projectile_template.mechanics.collider_template_id == 3);
            require(projectile_template.mechanics
                       .projectile_impact_trigger.action_type ==
                   KernelEntityTriggerActionType_SpawnProjectile);
            require(projectile_template.mechanics.projectile_impact_trigger
                       .spawn_projectile_template_id == 8);
            require(projectile_template.mechanics.collision_query_mode ==
                   KernelProjectileCollisionQueryMode_Auto);
        }
        if (projectile_template.projectile_template_id == 8) {
            found_rocket_explosion_template = true;
            require(projectile_template.mechanics.projectile_type ==
                   KernelProjectileType_AreaEffect);
            require(projectile_template.mechanics.area_effect.damage_interval_ticks == 45);
            require(projectile_template.mechanics.area_effect.lifetime_ticks == 45);
            require(projectile_template.mechanics.damage == 45);
        }
    }
    require(found_rocket_explosion_template);
    require(config.weapons.definitions[network_example::game_server::kWeaponSpammer]
               .damage == 1);
    require(config.weapons.definitions[network_example::game_server::kWeaponSpammer]
               .magazine_size == 3);
    require(config.weapons.definitions[network_example::game_server::kWeaponSpammer]
               .reserve_magazines == kMaxReserveMagazines);
    require(config.weapons.definitions[network_example::game_server::kWeaponSpammer]
               .projectile_template_id == 2);
    require(config.weapons.definitions[network_example::game_server::kWeaponFireFloor]
               .fire_mode == KernelWeaponFireMode_Projectile);
    require(config.weapons.collider_template_ids
               [network_example::game_server::kWeaponFireFloor] == 4);
    require(config.weapons.definitions[network_example::game_server::kWeaponFireFloor]
               .projectile_template_id == 4);
    require(config.weapons.definitions[network_example::game_server::kWeaponBeamRifle]
               .fire_mode == KernelWeaponFireMode_Projectile);
    require(config.weapons.collider_template_ids
               [network_example::game_server::kWeaponBeamRifle] == 8);
    require(config.weapons.definitions[network_example::game_server::kWeaponBeamRifle]
               .projectile_template_id == 5);
    require(config.weapons.definitions[network_example::game_server::kWeaponHomingMissile]
               .projectile_template_id == 6);
    require(config.weapons.configured[network_example::game_server::kWeaponGrenade]);
    require(config.weapons.projectile_sync_modes
               [network_example::game_server::kWeaponGrenade] ==
           KernelProjectileSyncMode_LocalPredictedDeterministic);
    require(config.weapons.names[network_example::game_server::kWeaponGrenade] ==
           "Grenade Launcher");
    require(config.weapons.projectile_sync_modes
               [network_example::game_server::kWeaponRocket] ==
           KernelProjectileSyncMode_HybridDeterministicThenSnapshot);
    require(config.weapons.projectile_sync_modes
               [network_example::game_server::kWeaponHomingMissile] ==
           KernelProjectileSyncMode_HybridDeterministicThenSnapshot);
    require(config.weapons.names[network_example::game_server::kWeaponFireFloor] ==
           "Fire Floor");
    require(config.weapons.names[network_example::game_server::kWeaponBeamRifle] ==
           "Beam Rifle");
    require(config.weapons.names[network_example::game_server::kWeaponHomingMissile] ==
           "Homing Missile");
    // Every action template on disk, plus the catalog's shared reload. Counted
    // rather than hardcoded: the directory grows with every weapon.
    std::size_t action_template_files = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(
             runfiles_root() / "game_server" / "gameplay_catalog" /
             "action_templates")) {
        if (entry.is_regular_file() && entry.path().extension() == ".yaml") {
            ++action_template_files;
        }
    }
    require(action_template_files > 0);
    require(config.action_templates.size() == action_template_files + 1);
    require(config.weapons.definitions[network_example::game_server::kWeaponRifle]
               .fire_action_template_id == 4096);
    require(config.weapons.definitions[network_example::game_server::kWeaponRocket]
               .fire_action_template_id == 4099);
    require(config.weapons.definitions[network_example::game_server::kWeaponBeamRifle]
               .fire_action_template_id == 4101);
    require(storage.definition.action_template_count ==
           config.action_templates.size());
    // By id, not by index: the load order follows the enumerated files.
    const auto action_by_id = [&config](std::uint32_t action_template_id)
        -> const KernelActionTemplateDefinition& {
        for (const network_example::game_server::ActionTemplateConfig& action :
             config.action_templates) {
            if (action.definition.action_template_id == action_template_id) {
                return action.definition;
            }
        }
        std::abort();
    };
    require(action_by_id(4099).commit_offset_ticks == 3);
    require(action_by_id(4099).commit_interval_ticks == 30);
    require(action_by_id(4101).trigger_mode == KernelActionTriggerMode_Hold);
    require(action_by_id(4101).hold_input_timeout_ticks == 6);
    network_example::game_server::GameServerGameplayConfig changed_action = config;
    ++changed_action.action_templates[0].definition.commit_interval_ticks;
    require(network_example::game_server::compute_gameplay_catalog_hash(changed_action) !=
           config.weapons.catalog_hash);
    bool found_segment = false;
    bool found_sphere = false;
    bool found_beam = false;
    for (const network_example::game_server::ColliderTemplateConfig& collider :
         config.colliders.templates) {
        if (collider.definition.template_id == 6) {
            found_segment = true;
            require(collider.definition.shape_type == KernelColliderShapeType_Segment);
            // A weapon segment declares no reach, no scatter, and no lifetime:
            // all three are decided at fire time. Only an optional thickness
            // survives, and shotgun_segment does not author one.
            require(collider.definition.shape_params.x == 0.0f);
            require(collider.definition.shape_params.y == 0.0f);
            require(collider.definition.shape_params.z == 0.0f);
            require(collider.definition.lifetime_ticks == 0);
        }
        if (collider.definition.template_id == 7) {
            found_sphere = true;
            require(collider.definition.shape_type == KernelColliderShapeType_Sphere);
            require(collider.definition.shape_params.x == 0.5f);
        }
        if (collider.definition.template_id == 8) {
            found_beam = true;
            require(collider.definition.shape_type == KernelColliderShapeType_OrientedBox);
            require(collider.definition.shape_params.x == 0.25f);
            require(collider.definition.shape_params.y == 0.25f);
            require(collider.definition.shape_params.z == 4.0f);
        }
    }
    require(found_segment);
    require(found_sphere);
    require(found_beam);
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

    require(projectile_mechanics(config, 2).collision_query_mode ==
           KernelProjectileCollisionQueryMode_Auto);
    require(projectile_mechanics(config, 3).collision_query_mode ==
           KernelProjectileCollisionQueryMode_Overlap);
}

void invalid_templates_are_rejected() {
    const std::filesystem::path legacy_dir = tmp_dir("legacy_cooldown");
    write_valid_templates(legacy_dir);
    write_file(
        legacy_dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "cooldown_ticks: 3\nmax_range: 100.0\n"
        "segment_collider: rifle_segment\n");
    try {
        (void)network_example::game_server::
            load_gameplay_config_from_weapon_template_directory(
                legacy_dir.string());
        require(false);
    } catch (const network_example::game_server::DataLoadError& error) {
        require(
            error.error_code ==
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNKNOWN_FIELD);
        require(error.field == "cooldown_ticks");
        require(error.template_kind ==
               KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_WEAPON);
        require(error.template_id == 0);
        require(error.line > 0);
        require(error.column > 0);
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
    require(!load_fails(finite_press_dir));

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
        require(false);
    } catch (const network_example::game_server::DataLoadError& error) {
        require(
            error.error_code ==
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_NUMERIC_RANGE);
        require(error.path == zero_fire_action.string());
        require(error.field == "commit_interval_ticks");
        require(error.template_kind ==
               KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTION);
        require(error.template_id == 4096);
        require(error.line > 0);
        require(error.column > 0);
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
    require(load_fails(invalid_press_dir));

    const std::filesystem::path missing_policy_dir = tmp_dir("missing_policy");
    write_valid_templates(missing_policy_dir);
    write_file(
        missing_policy_dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\nmax_range: 100.0\n"
        "segment_collider: rifle_segment\n");
    require(load_fails(missing_policy_dir));

    const std::filesystem::path dangling_action_dir = tmp_dir("dangling_action");
    write_valid_templates(dangling_action_dir);
    write_file(
        dangling_action_dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\nfire_action_template: missing_action\n"
        "max_range: 100.0\nsegment_collider: rifle_segment\n");
    require(load_fails(dangling_action_dir));

    const std::filesystem::path duplicate_action_dir = tmp_dir("duplicate_action");
    write_valid_templates(duplicate_action_dir);
    write_file(
        duplicate_action_dir.parent_path() / "action_templates" / "duplicate.yaml",
        "id: 4096\nname: duplicate\ntrigger_mode: press\n"
        "flags: [cancel_on_death]\nammo_cost_per_commit: 1\n"
        "commit_offset_ticks: 0\ncommit_interval_ticks: 1\nmax_commit_count: 1\n"
        "recovery_ticks: 0\nhold_input_timeout_ticks: 0\n");
    require(load_fails(duplicate_action_dir));

    const std::filesystem::path invalid_action_dir = tmp_dir("invalid_action");
    write_valid_templates(invalid_action_dir);
    write_file(
        invalid_action_dir.parent_path() / "action_templates" / "rifle_fire.yaml",
        "id: 4096\nname: rifle_fire\ntrigger_mode: hold\n"
        "flags: [cancel_on_release]\nammo_cost_per_commit: 1\n"
        "commit_offset_ticks: 0\ncommit_interval_ticks: 0\nmax_commit_count: 0\n"
        "recovery_ticks: 4\nhold_input_timeout_ticks: 6\n");
    require(load_fails(invalid_action_dir));

    const std::filesystem::path duplicate_dir = tmp_dir("duplicate");
    write_valid_templates(duplicate_dir);
    write_file(
        duplicate_dir / "duplicate.yaml",
        "id: 4\nname: Duplicate\nweapon_type: hitscan\nmagazine_size: 1\n"
        "damage: 1\nmax_range: 1.0\n");
    require(load_fails(duplicate_dir));

    const std::filesystem::path duplicate_name_dir = tmp_dir("duplicate_name");
    write_valid_templates(duplicate_name_dir);
    write_file(
        duplicate_name_dir / "duplicate_name.yaml",
        "id: 4\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 1\n"
        "damage: 1\nmax_range: 1.0\n"
        "segment_collider: rifle_segment\n");
    require(load_fails(duplicate_name_dir));

    const std::filesystem::path unknown_weapon_field_dir =
        tmp_dir("unknown_weapon_field");
    write_valid_templates(unknown_weapon_field_dir);
    write_file(
        unknown_weapon_field_dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\nmax_range: 100.0\n"
        "segment_collider: rifle_segment\nruntime_instance_id: 9\n");
    require(load_fails(unknown_weapon_field_dir));

    const std::filesystem::path unknown_area_field_dir =
        tmp_dir("unknown_area_field");
    write_valid_templates(unknown_area_field_dir);
    write_file(
        unknown_area_field_dir / "fire_floor.yaml",
        "id: 4\nname: Fire Floor\nweapon_type: area_effect\nmagazine_size: 3\n"
        "area_effect:\n"
        "  collider_template: area_effect_sphere\n"
        "  radius: 2.0\n  damage_per_interval: 12\n  damage_interval_ticks: 2\n"
        "  lifetime_ticks: 6\n  spawn_distance: 1.0\n  collision_mask: hostile_side\n"
        "  current_tick: 123\n");
    require(load_fails(unknown_area_field_dir));

    const std::filesystem::path unknown_beam_field_dir =
        tmp_dir("unknown_beam_field");
    write_valid_templates(unknown_beam_field_dir);
    write_file(
        unknown_beam_field_dir / "beam_rifle.yaml",
        "id: 5\nname: Beam Rifle\nweapon_type: beam\nmagazine_size: 12\n"
        "beam:\n"
        "  collider_template: beam_oriented_box\n"
        "  length: 8.0\n  radius: 0.25\n"
        "  lifetime_ticks: 2\n  collision_mask: hostile_side\n  owner: player\n");
    require(load_fails(unknown_beam_field_dir));

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
    require(load_fails(unknown_homing_field_dir));

    const std::filesystem::path hitscan_projectile_dir = tmp_dir("hitscan_projectile");
    write_valid_templates(hitscan_projectile_dir);
    write_file(
        hitscan_projectile_dir / "rifle.yaml",
        "id: 0\nname: Bad Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\nmax_range: 100.0\n"
        "projectile: {speed: 10.0}\n");
    require(load_fails(hitscan_projectile_dir));

    const std::filesystem::path missing_beam_dir = tmp_dir("missing_beam");
    write_valid_templates(missing_beam_dir);
    write_file(
        missing_beam_dir / "beam_rifle.yaml",
        "id: 5\nname: Beam\nweapon_type: beam\nmagazine_size: 1\n");
    require(load_fails(missing_beam_dir));

    const std::filesystem::path invalid_beam_dir = tmp_dir("invalid_beam");
    write_valid_templates(invalid_beam_dir);
    write_file(
        invalid_beam_dir / "beam_rifle.yaml",
        "id: 5\nname: Beam\nweapon_type: beam\nmagazine_size: 1\n"
        "beam:\n"
        "  length: 0.0\n  radius: 0.25\n"
        "  lifetime_ticks: 2\n");
    require(load_fails(invalid_beam_dir));

    const std::filesystem::path beam_on_hitscan_dir = tmp_dir("beam_on_hitscan");
    write_valid_templates(beam_on_hitscan_dir);
    write_file(
        beam_on_hitscan_dir / "rifle.yaml",
        "id: 0\nname: Bad Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "damage: 25\nmax_range: 100.0\n"
        "beam: {length: 8.0}\n");
    require(load_fails(beam_on_hitscan_dir));

    const std::filesystem::path homing_dir = tmp_dir("homing");
    write_valid_templates(homing_dir);
    write_file(
        homing_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 1\n"
        "sync_mode: server_snapshot_only\ncollider_template: rocket_aabb\n"
        "movement_model: homing\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 1.0\nlifetime_ticks: 30\n"
        "collision_mask: damageable\nmax_hit_count: 1\n");
    require(load_fails(homing_dir));

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
    require(load_fails(invalid_homing_dir));

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
    require(load_fails(homing_on_linear_dir));

    const std::filesystem::path bounce_dir = tmp_dir("bounce");
    write_valid_templates(bounce_dir);
    write_file(
        bounce_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 1\n"
        "sync_mode: server_snapshot_only\ncollider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: bounce\n"
        "damage_shape: direct_hit\nspeed: 1.0\nlifetime_ticks: 30\n"
        "collision_mask: damageable\nmax_hit_count: 1\n");
    require(load_fails(bounce_dir));

    const std::filesystem::path invalid_sync_dir = tmp_dir("invalid_sync");
    write_valid_templates(invalid_sync_dir);
    write_file(
        invalid_sync_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 1\n"
        "sync_mode: remote_magic\ncollider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 1.0\nlifetime_ticks: 30\n"
        "collision_mask: damageable\nmax_hit_count: 1\n");
    require(load_fails(invalid_sync_dir));

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
    require(load_fails(removed_radius_dir));

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
    require(load_fails(area_effect_sphere_shape_dir));

    const std::filesystem::path unknown_projectile_dir =
        tmp_dir("unknown_projectile_template");
    write_valid_templates(unknown_projectile_dir);
    write_file(
        unknown_projectile_dir / "rocket.yaml",
        "id: 3\nname: Rocket\nweapon_type: projectile\nmagazine_size: 6\n"
        "projectile_template: missing_projectile\n");
    require(load_fails(unknown_projectile_dir));

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
    require(load_fails(cone_projectile_dir));
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
    require(projectile_mechanics(config, 3).collision_mask ==
           KERNEL_COLLISION_MASK_NONE);

    const std::filesystem::path zero_dir = tmp_dir("mask_zero");
    write_valid_templates(zero_dir);
    write_file(
        zero_dir.parent_path() / "projectile_templates" / "beam_rifle_beam.yaml",
        "id: 5\nname: beam_rifle_beam\ntype: beam\ndamage: 1\n"
        "sync_mode: server_snapshot_only\n"
        "collider_template: beam_oriented_box\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 0.0\nlifetime_ticks: 0\n"
        "collision_mask: 0\nmax_hit_count: 1\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n"
        "beam:\n"
        "  length: 8.0\n  radius: 0.25\n"
        "  lifetime_ticks: 2\n");
    config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            zero_dir.string());
    require(projectile_mechanics(config, 5).beam.collision_mask ==
           KERNEL_COLLISION_MASK_NONE);

    const std::filesystem::path expression_dir = tmp_dir("mask_expression");
    write_valid_templates(expression_dir);
    write_file(
        expression_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        "id: 4\nname: fire_floor_area\ntype: area_effect\n"
        "collider_template: area_effect_sphere\n"
        "damage: 12\n"
        "lifetime_ticks: 6\n"
        "damage_behavior:\n"
        "  type: area_interval\n"
        "  damage_interval_ticks: 2\n"
        "  falloff: none\n"
        "collision_mask: player_side | hostile_side\n");
    config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            expression_dir.string());
    require(projectile_mechanics(config, 4).area_effect.collision_mask ==
           (KERNEL_COLLISION_LAYER_HOSTILE_SIDE | KERNEL_COLLISION_LAYER_PLAYER_SIDE));
}

// The area_effect branch of the projectile loader returns before the generic
// field parsing, so every field it wants has to be handled inside it. sync_mode
// used to be assigned there unconditionally, which accepted an authored value
// and then discarded it -- the same silent drop `triggers` suffered.
// Reaching one's own blast is off by default and only an area effect can ask
// for it, because only the area effect overlap query implements the filter it
// switches off.
// speed sits below the area_effect branch's early return, so authoring it used
// to be accepted and then discarded -- the effect stayed pinned where it
// spawned. It now means what it says, and still defaults to standing still.
// event.subject_direction is the heading of whatever the graph is running on,
// which only a projectile trigger has. A field that never moves has none, and
// the loader is what keeps that from reaching the runtime as a zero vector.
// The mask that decides what stops a travelling field, kept apart from the one
// that decides who it affects. Absent means nothing stops it and no sweep runs.
void area_effect_motion_collision_mask_is_authored() {
    const std::string area_template =
        "id: 4\nname: fire_floor_area\ntype: area_effect\n"
        "collider_template: area_effect_sphere\n"
        "damage: 12\n"
        "lifetime_ticks: 6\n"
        "damage_behavior:\n"
        "  type: area_interval\n"
        "  damage_interval_ticks: 2\n"
        "  falloff: none\n"
        "collision_mask: hostile_side\n";

    const std::filesystem::path default_dir = tmp_dir("motion_mask_default");
    write_valid_templates(default_dir);
    network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            default_dir.string());
    require(projectile_mechanics(config, 4).area_effect.motion_collision_mask ==
           KERNEL_COLLISION_MASK_NONE);

    const std::filesystem::path authored_dir = tmp_dir("motion_mask_authored");
    write_valid_templates(authored_dir);
    write_file(
        authored_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        area_template + "speed: 6.0\nmotion_collision_mask: terrain | static_obstacle\n");
    config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            authored_dir.string());
    require(projectile_mechanics(config, 4).area_effect.motion_collision_mask ==
           KERNEL_COLLISION_MASK_STATIC_WORLD);
    // The mask that says who it affects is untouched by the one that says what
    // stops it.
    require(projectile_mechanics(config, 4).area_effect.collision_mask ==
           KERNEL_COLLISION_LAYER_HOSTILE_SIDE);

    // Only the static world can stop it: actors and props are what it affects.
    const std::filesystem::path actor_dir = tmp_dir("motion_mask_actor");
    write_valid_templates(actor_dir);
    write_file(
        actor_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        area_template + "speed: 6.0\nmotion_collision_mask: hostile_side\n");
    require(load_fails(actor_dir));

    // A field that never moves has nothing to be stopped.
    const std::filesystem::path still_dir = tmp_dir("motion_mask_still");
    write_valid_templates(still_dir);
    write_file(
        still_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        area_template + "motion_collision_mask: terrain\n");
    require(load_fails(still_dir));

    // And no other projectile type answers this question twice.
    const std::filesystem::path standard_dir = tmp_dir("motion_mask_standard");
    write_valid_templates(standard_dir);
    write_file(
        standard_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\ncollider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 35.0\nlifetime_ticks: 75\n"
        "collision_mask: damageable\nmax_hit_count: 1\n"
        "motion_collision_mask: terrain\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    require(load_fails(standard_dir));
}

// A tornado's two extras: a ground-following ride and an upright-column
// overlap. Each loads with its defaults, and each refuses the combinations
// that would leave it with nothing to do or a client unable to draw it.
void area_effect_ground_follow_and_cylinder_are_authored() {
    const std::string area_template =
        "id: 4\nname: fire_floor_area\ntype: area_effect\n"
        "collider_template: area_effect_sphere\n"
        "damage: 12\n"
        "lifetime_ticks: 6\n"
        "damage_behavior:\n"
        "  type: area_interval\n"
        "  damage_interval_ticks: 2\n"
        "  falloff: none\n"
        "collision_mask: hostile_side\n";
    const std::string travelling =
        "speed: 6.0\nmotion_collision_mask: terrain\n"
        "sync_mode: local_predicted_deterministic\n";
    const auto load_with = [&](const std::string& name, const std::string& extra) {
        const std::filesystem::path dir = tmp_dir(name);
        write_valid_templates(dir);
        write_file(
            dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
            area_template + extra);
        return dir;
    };

    // Absent: a sphere that flies a straight line, as every template did.
    network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            load_with("tornado_default", "").string());
    KernelAreaEffectMechanicsDefinition area =
        projectile_mechanics(config, 4).area_effect;
    require(area.shape == KernelAreaEffectShape_Sphere);
    require(area.motion == KernelAreaEffectMotion_Linear);
    require(area.half_height == 0.0f && area.hover_height == 0.0f);
    require(area.max_slope_degrees == 0.0f);

    // Authored with only what it must name; the rest takes the character
    // controller's numbers.
    config = network_example::game_server::load_gameplay_config_from_weapon_template_directory(
        load_with(
            "tornado_minimal",
            travelling + "motion:\n  type: ground_follow\n  hover_height: 1.5\n")
            .string());
    area = projectile_mechanics(config, 4).area_effect;
    require(area.motion == KernelAreaEffectMotion_GroundFollow);
    require(area.hover_height == 1.5f);
    require(area.max_slope_degrees == 50.0f);
    require(area.step_up == 0.5f && area.probe_depth == 0.5f);

    config = network_example::game_server::load_gameplay_config_from_weapon_template_directory(
        load_with(
            "tornado_full",
            travelling +
                "area_shape: cylinder\nhalf_height: 1.5\n"
                "motion:\n  type: ground_follow\n  hover_height: 1.5\n"
                "  max_slope_degrees: 40\n  step_up: 0.3\n  probe_depth: 0.8\n")
            .string());
    area = projectile_mechanics(config, 4).area_effect;
    require(area.shape == KernelAreaEffectShape_Cylinder);
    require(area.half_height == 1.5f);
    require(area.max_slope_degrees == 40.0f);
    require(area.step_up == 0.3f && area.probe_depth == 0.8f);

    // Server-only drawing is the other mode a client can live with.
    require(!load_fails(load_with(
        "tornado_snapshot_only",
        "speed: 6.0\nmotion_collision_mask: terrain\n"
        "sync_mode: server_snapshot_only\n"
        "motion:\n  type: ground_follow\n  hover_height: 1.5\n")));

    const std::string ride = "motion:\n  type: ground_follow\n  hover_height: 1.5\n";
    // Nothing to follow without travel.
    require(load_fails(load_with(
        "tornado_still",
        "motion_collision_mask: terrain\nsync_mode: local_predicted_deterministic\n" +
            ride)));
    // Nothing to probe without terrain in the mask.
    require(load_fails(load_with(
        "tornado_no_terrain",
        "speed: 6.0\nmotion_collision_mask: static_obstacle\n"
        "sync_mode: local_predicted_deterministic\n" +
            ride)));
    // Hybrid's correction re-anchors through the straight-line formula.
    require(load_fails(load_with(
        "tornado_hybrid",
        "speed: 6.0\nmotion_collision_mask: terrain\n"
        "sync_mode: hybrid_deterministic_then_snapshot\n" +
            ride)));
    require(load_fails(load_with(
        "tornado_no_hover",
        travelling + "motion:\n  type: ground_follow\n")));
    require(load_fails(load_with(
        "tornado_flat_slope",
        travelling + ride + "  max_slope_degrees: 90\n")));
    require(load_fails(load_with(
        "tornado_unknown_key",
        travelling + ride + "  bounce: true\n")));
    require(load_fails(load_with(
        "tornado_linear_settings",
        travelling + "motion:\n  type: linear\n  hover_height: 1.5\n")));
    require(load_fails(load_with("cylinder_no_height", "area_shape: cylinder\n")));
    require(load_fails(load_with("sphere_with_height", "half_height: 1.0\n")));
    require(load_fails(load_with("unknown_shape", "area_shape: cone\n")));

    // And none of it on anything but an area effect.
    const std::filesystem::path standard_dir = tmp_dir("tornado_standard");
    write_valid_templates(standard_dir);
    write_file(
        standard_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\ncollider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 35.0\nlifetime_ticks: 75\n"
        "collision_mask: damageable\nmax_hit_count: 1\n"
        "area_shape: cylinder\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    require(load_fails(standard_dir));
}

void subject_direction_needs_a_projectile_that_travels() {
    const auto write_impulse_graph = [](const std::filesystem::path& dir,
                                        const std::string& direction) {
        write_file(
            dir.parent_path() / "action_graph_templates" /
                "action_sweep_impulse.yaml",
            "id: action_sweep_impulse\n"
            "parameters:\n"
            "  target: null\n"
            "  strength: 12.0\n"
            "  direction: null\n"
            "actions:\n"
            "  - type: apply_impulse\n"
            "    target: params.target\n"
            "    strength: params.strength\n"
            "    direction: params.direction\n"
            "    collision_mask: actor\n");
        return "triggers:\n"
               "  on_projectile_impact:\n"
               "    action_graph: action_sweep_impulse\n"
               "    parameters:\n"
               "      target: event.target\n"
               "      strength: 12.0\n"
               "      direction: " + direction + "\n";
    };
    const std::string area_template =
        "id: 4\nname: fire_floor_area\ntype: area_effect\n"
        "collider_template: area_effect_sphere\n"
        "damage: 12\n"
        "lifetime_ticks: 6\n"
        "damage_behavior:\n"
        "  type: area_interval\n"
        "  damage_interval_ticks: 2\n"
        "  falloff: none\n"
        "collision_mask: hostile_side\n";

    const std::filesystem::path moving_dir = tmp_dir("subject_direction_moving");
    write_valid_templates(moving_dir);
    const std::string moving_triggers =
        write_impulse_graph(moving_dir, "event.subject_direction");
    write_file(
        moving_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        area_template + "speed: 6.0\n" + moving_triggers);
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            moving_dir.string());
    require(projectile_mechanics(config, 4)
               .projectile_impact_trigger.actions[0]
               .direction_source == KernelEventVec3Source_SubjectDirection);

    const std::filesystem::path still_dir = tmp_dir("subject_direction_still");
    write_valid_templates(still_dir);
    const std::string still_triggers =
        write_impulse_graph(still_dir, "event.subject_direction");
    write_file(
        still_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        area_template + still_triggers);
    require(load_fails(still_dir));
}

// Speed 0 is a marker: something that holds a place and fires on_expired
// there. It loads only when it cannot do anything else, so each rejection
// below differs from the accepted template by one field.
void stationary_marker_loads_only_when_inert() {
    const std::string marker =
        "id: 40\nname: strike_marker\ntype: standard\n"
        "collider_template: projectile_sphere\n"
        "damage: 0\ndamage_shape: none\n"
        "sync_mode: server_snapshot_only\n"
        "lifetime_ticks: 20\n";
    const std::string expired_trigger =
        "triggers:\n"
        "  on_expired:\n"
        "    action_graph: action_spawn_projectile_at_impact\n"
        "    parameters:\n"
        "      template: rocket_explosion\n"
        "      position: event.position\n"
        "      direction: event.direction\n";
    const auto marker_dir = [&](const char* name, const std::string& extra) {
        const std::filesystem::path dir = tmp_dir(name);
        write_valid_templates(dir);
        write_file(
            dir.parent_path() / "projectile_templates" / "strike_marker.yaml",
            marker + extra + expired_trigger);
        return dir;
    };

    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            marker_dir("marker_inert", "speed: 0.0\ncollision_mask: none\n")
                .string());
    const KernelProjectileMechanicsDefinition& mechanics =
        projectile_mechanics(config, 40);
    require(mechanics.speed == 0.0f);
    require(mechanics.collision_mask == KERNEL_COLLISION_MASK_NONE);
    require(mechanics.expired_trigger.action_count == 1u);

    const bool hits_terrain = load_fails(marker_dir(
        "marker_hits_terrain", "speed: 0.0\ncollision_mask: terrain\n"));
    require(hits_terrain);
    const bool falls = load_fails(marker_dir(
        "marker_falls",
        "speed: 0.0\ncollision_mask: none\n"
        "gravity: {x: 0.0, y: -9.8, z: 0.0}\n"));
    require(falls);
    const bool backwards = load_fails(marker_dir(
        "marker_negative_speed", "speed: -1.0\ncollision_mask: none\n"));
    require(backwards);
}

// An area effect expires without queuing a trigger, so on_expired on one used
// to load and never fire.
void area_effect_rejects_on_expired() {
    const std::string area_template =
        "id: 4\nname: fire_floor_area\ntype: area_effect\n"
        "collider_template: area_effect_sphere\n"
        "damage: 12\n"
        "lifetime_ticks: 6\n"
        "damage_behavior:\n"
        "  type: area_interval\n"
        "  damage_interval_ticks: 2\n"
        "  falloff: none\n"
        "collision_mask: hostile_side\n";
    const auto binding = [](const char* trigger) {
        return std::string("triggers:\n  ") + trigger +
            ":\n"
            "    action_graph: action_spawn_projectile_at_impact\n"
            "    parameters:\n"
            "      template: rocket_explosion\n"
            "      position: event.position\n"
            "      direction: event.direction\n";
    };

    const std::filesystem::path impact_dir = tmp_dir("area_on_impact");
    write_valid_templates(impact_dir);
    write_file(
        impact_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        area_template + binding("on_projectile_impact"));
    const bool impact_failed = load_fails(impact_dir);
    require(!impact_failed);

    const std::filesystem::path expired_dir = tmp_dir("area_on_expired");
    write_valid_templates(expired_dir);
    write_file(
        expired_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        area_template + binding("on_expired"));
    const bool expired_failed = load_fails(expired_dir);
    require(expired_failed);
}

// launch: descent derives start and speed from a landing target. Each rejected
// variant below differs from the accepted one by one line.
void descent_launch_is_authored() {
    const std::string head =
        "id: 41\nname: meteor_body\ntype: standard\n"
        "collider_template: projectile_sphere\n"
        "damage: 0\ndamage_shape: none\n"
        "collision_mask: terrain | static_obstacle\n";
    const std::string launch =
        "launch:\n"
        "  type: descent\n"
        "  elevation_degrees: [75, 85]\n"
        "  height: 40.0\n"
        "  fall_ticks: 15\n";
    const auto meteor_dir = [&](const char* name, const std::string& body) {
        const std::filesystem::path dir = tmp_dir(name);
        write_valid_templates(dir);
        write_file(
            dir.parent_path() / "projectile_templates" / "meteor_body.yaml",
            head + body);
        return dir;
    };

    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            meteor_dir("descent_valid", "lifetime_ticks: 18\n" + launch)
                .string());
    const KernelProjectileMechanicsDefinition& mechanics =
        projectile_mechanics(config, 41);
    require(mechanics.launch.struct_size == sizeof(KernelProjectileLaunchDefinition));
    require(mechanics.launch.launch_type == KernelProjectileLaunchType_Descent);
    require(mechanics.launch.elevation_min_degrees == 75.0f);
    require(mechanics.launch.elevation_max_degrees == 85.0f);
    require(mechanics.launch.height == 40.0f);
    require(mechanics.launch.fall_ticks == 15u);
    require(mechanics.speed == 0.0f);
    // Not authored, and nothing else is accepted.
    require(mechanics.sync_mode == KernelProjectileSyncMode_ServerSnapshotOnly);

    const network_example::game_server::GameServerGameplayConfig fixed =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            meteor_dir(
                "descent_fixed_elevation",
                "lifetime_ticks: 18\n"
                "launch:\n  type: descent\n  elevation_degrees: 80\n"
                "  height: 40.0\n  fall_ticks: 15\n")
                .string());
    require(projectile_mechanics(fixed, 41).launch.elevation_min_degrees == 80.0f);
    require(projectile_mechanics(fixed, 41).launch.elevation_max_degrees == 80.0f);

    struct Rejected {
        const char* name;
        std::string body;
    };
    const std::vector<Rejected> rejected = {
        {"descent_with_speed", "lifetime_ticks: 18\nspeed: 30.0\n" + launch},
        {"descent_lifetime_short", "lifetime_ticks: 15\n" + launch},
        {"descent_predicted",
         "lifetime_ticks: 18\nsync_mode: hybrid_deterministic_then_snapshot\n" +
             launch},
        {"descent_parabolic", "lifetime_ticks: 18\nmovement_model: parabolic\n" +
             launch},
        {"descent_reversed_range",
         "lifetime_ticks: 18\nlaunch:\n  type: descent\n"
         "  elevation_degrees: [85, 75]\n  height: 40.0\n  fall_ticks: 15\n"},
        {"descent_flat",
         "lifetime_ticks: 18\nlaunch:\n  type: descent\n"
         "  elevation_degrees: 0\n  height: 40.0\n  fall_ticks: 15\n"},
        {"descent_no_height",
         "lifetime_ticks: 18\nlaunch:\n  type: descent\n"
         "  elevation_degrees: 80\n  fall_ticks: 15\n"},
        {"descent_unknown_type",
         "lifetime_ticks: 18\nlaunch:\n  type: orbit\n"
         "  elevation_degrees: 80\n  height: 40.0\n  fall_ticks: 15\n"},
    };
    for (const Rejected& variant : rejected) {
        const bool failed = load_fails(meteor_dir(variant.name, variant.body));
        if (!failed) {
            std::fprintf(stderr, "descent variant loaded: %s\n", variant.name);
        }
        require(failed);
    }

    // A muzzle-fired weapon would drop the projectile onto its own shooter.
    const std::filesystem::path weapon_dir =
        meteor_dir("descent_on_muzzle_weapon", "lifetime_ticks: 18\n" + launch);
    write_file(
        weapon_dir / "rocket.yaml",
        "id: 3\nname: Rocket\nweapon_type: projectile\nmagazine_size: 6\n"
        "fire_action_template: rocket_fire\n"
        "projectile_template: meteor_body\n");
    const bool muzzle_failed = load_fails(weapon_dir);
    require(muzzle_failed);
}

// weapon_type: targeted_strike lands its projectile template on a point. It
// needs a max_range, and what it lands must be something only the server
// places: server_snapshot_only, and not a beam.
void targeted_strike_weapon_is_authored() {
    const std::string marker =
        "id: 40\nname: strike_marker\ntype: standard\n"
        "collider_template: projectile_sphere\n"
        "damage: 0\ndamage_shape: none\n"
        "speed: 0.0\ncollision_mask: none\n"
        "lifetime_ticks: 20\n";
    const std::string meteor =
        "id: 41\nname: meteor_body\ntype: standard\n"
        "collider_template: projectile_sphere\n"
        "damage: 0\ndamage_shape: none\n"
        "collision_mask: terrain | static_obstacle\n"
        "lifetime_ticks: 18\n"
        "launch:\n  type: descent\n  elevation_degrees: [75, 85]\n"
        "  height: 40.0\n  fall_ticks: 15\n";
    const auto strike_dir = [&](const char* name,
                                const std::string& marker_sync,
                                const std::string& weapon_body) {
        const std::filesystem::path dir = tmp_dir(name);
        write_valid_templates(dir);
        write_file(
            dir.parent_path() / "projectile_templates" / "strike_marker.yaml",
            marker + marker_sync);
        write_file(
            dir.parent_path() / "projectile_templates" / "meteor_body.yaml",
            meteor);
        write_file(
            dir / "rocket.yaml",
            "id: 3\nname: Meteor Staff\nweapon_type: targeted_strike\n"
            "magazine_size: 2\nfire_action_template: rocket_fire\n" +
                weapon_body);
        return dir;
    };

    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            strike_dir(
                "strike_marker_weapon",
                "sync_mode: server_snapshot_only\n",
                "max_range: 35.0\nprojectile_template: strike_marker\n")
                .string());
    const KernelWeaponMechanicsDefinition& weapon = config.weapons.definitions[3];
    require(config.weapons.configured[3]);
    require(weapon.fire_mode == KernelWeaponFireMode_TargetedStrike);
    require(weapon.max_range == 35.0f);
    require(weapon.projectile_template_id == 40u);

    // A descent template may be landed directly; the muzzle rule does not
    // apply to a weapon that never fires from the muzzle.
    const network_example::game_server::GameServerGameplayConfig direct =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            strike_dir(
                "strike_meteor_weapon",
                "sync_mode: server_snapshot_only\n",
                "max_range: 35.0\nprojectile_template: meteor_body\n")
                .string());
    require(direct.weapons.definitions[3].projectile_template_id == 41u);

    const bool no_range = load_fails(strike_dir(
        "strike_no_range",
        "sync_mode: server_snapshot_only\n",
        "projectile_template: strike_marker\n"));
    require(no_range);
    const bool predicted = load_fails(strike_dir(
        "strike_predicted_marker",
        "sync_mode: hybrid_deterministic_then_snapshot\n",
        "max_range: 35.0\nprojectile_template: strike_marker\n"));
    require(predicted);
    const bool beam = load_fails(strike_dir(
        "strike_beam",
        "sync_mode: server_snapshot_only\n",
        "max_range: 35.0\nprojectile_template: beam_rifle_beam\n"));
    require(beam);
}

// repeat and lifetime_ticks on a spawn_projectile graph action reach the
// compiled trigger; out-of-range repeats and misplaced keys are refused.
void spawn_repeat_is_authored() {
    const std::string marker =
        "id: 40\nname: storm_marker\ntype: standard\n"
        "collider_template: projectile_sphere\n"
        "damage: 0\ndamage_shape: none\n"
        "speed: 0.0\ncollision_mask: none\n"
        "sync_mode: server_snapshot_only\n"
        "lifetime_ticks: 20\n"
        "triggers:\n"
        "  on_expired:\n"
        "    action_graph: action_meteor_storm\n"
        "    parameters:\n"
        "      template: rocket_explosion\n"
        "      position: event.position\n"
        "      direction: event.direction\n";
    const auto storm_dir = [&](const char* name, const std::string& action) {
        const std::filesystem::path dir = tmp_dir(name);
        write_valid_templates(dir);
        write_file(
            dir.parent_path() / "projectile_templates" / "storm_marker.yaml",
            marker);
        write_file(
            dir.parent_path() / "action_graph_templates" /
                "action_meteor_storm.yaml",
            "id: action_meteor_storm\n"
            "parameters:\n"
            "  template: null\n"
            "  position: null\n"
            "  direction: null\n"
            "actions:\n" + action);
        return dir;
    };
    const std::string spawn =
        "  - type: spawn_projectile\n"
        "    projectile_template: params.template\n"
        "    position: params.position\n"
        "    direction: params.direction\n";

    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            storm_dir(
                "repeat_valid",
                spawn +
                    "    lifetime_ticks: 10\n"
                    "    repeat:\n"
                    "      count: [10, 15]\n"
                    "      scatter_radius: 6.0\n"
                    "      stagger_lifetime_ticks: 60\n")
                .string());
    const KernelActionDefinition& action =
        projectile_mechanics(config, 40).expired_trigger.actions[0];
    require(action.spawn_lifetime_ticks == 10u);
    require(action.repeat_count_min == 10u);
    require(action.repeat_count_max == 15u);
    require(action.repeat_scatter_radius == 6.0f);
    require(action.repeat_stagger_lifetime_ticks == 60u);

    struct Rejected {
        const char* name;
        std::string action;
    };
    const std::vector<Rejected> rejected = {
        {"repeat_over_cap", spawn + "    repeat:\n      count: 17\n"},
        {"repeat_reversed", spawn + "    repeat:\n      count: [5, 4]\n"},
        {"repeat_zero", spawn + "    repeat:\n      count: 0\n"},
        {"repeat_no_count", spawn + "    repeat:\n      scatter_radius: 2.0\n"},
        {"repeat_negative_scatter",
         spawn + "    repeat:\n      count: 3\n      scatter_radius: -1.0\n"},
        {"spawn_lifetime_zero", spawn + "    lifetime_ticks: 0\n"},
    };
    for (const Rejected& variant : rejected) {
        const bool failed = load_fails(storm_dir(variant.name, variant.action));
        if (!failed) {
            std::fprintf(stderr, "repeat variant loaded: %s\n", variant.name);
        }
        require(failed);
    }

    // On any other action it is refused, not ignored. The graph is unbound,
    // so the only thing that can fail it is the key; the same graph without
    // the key is the control.
    const auto damage_graph_dir = [&](const char* name, const std::string& extra) {
        const std::filesystem::path dir = tmp_dir(name);
        write_valid_templates(dir);
        write_file(
            dir.parent_path() / "action_graph_templates" /
                "action_repeat_damage.yaml",
            "id: action_repeat_damage\n"
            "parameters:\n"
            "  target: null\n"
            "  amount: 1\n"
            "actions:\n"
            "  - type: apply_damage\n"
            "    target: params.target\n"
            "    amount: params.amount\n" + extra);
        return dir;
    };
    const bool plain_damage_failed =
        load_fails(damage_graph_dir("damage_graph_plain", ""));
    require(!plain_damage_failed);
    const bool repeated_damage_failed = load_fails(damage_graph_dir(
        "damage_graph_repeat", "    repeat:\n      count: 3\n"));
    require(repeated_damage_failed);
}

// replication: derived is only accepted where a client can derive it: below a
// stationary marker a targeted_strike weapon lands, ending on the static
// world. The accepted chain is the control for each refusal.
void derived_replication_is_authored() {
    const std::string marker =
        "id: 40\nname: strike_marker\ntype: standard\n"
        "collider_template: projectile_sphere\n"
        "damage: 0\ndamage_shape: none\n"
        "speed: 0.0\ncollision_mask: none\n"
        "sync_mode: server_snapshot_only\n"
        "lifetime_ticks: 20\n"
        "triggers:\n"
        "  on_expired:\n"
        "    action_graph: action_spawn_projectile_at_impact\n"
        "    parameters:\n"
        "      template: meteor_body\n"
        "      position: event.position\n"
        "      direction: event.direction\n";
    const std::string meteor_head =
        "id: 41\nname: meteor_body\ntype: standard\n"
        "collider_template: projectile_sphere\n"
        "damage: 0\ndamage_shape: none\n"
        "lifetime_ticks: 18\n"
        "launch:\n  type: descent\n  elevation_degrees: 80\n"
        "  height: 40.0\n  fall_ticks: 15\n";
    const auto chain_dir = [&](const char* name,
                               const std::string& meteor_tail,
                               const std::string& weapon_template) {
        const std::filesystem::path dir = tmp_dir(name);
        write_valid_templates(dir);
        write_file(
            dir.parent_path() / "projectile_templates" / "strike_marker.yaml",
            marker);
        write_file(
            dir.parent_path() / "projectile_templates" / "meteor_body.yaml",
            meteor_head + meteor_tail);
        write_file(
            dir / "rocket.yaml",
            "id: 3\nname: Meteor Staff\nweapon_type: targeted_strike\n"
            "magazine_size: 2\nmax_range: 35.0\n"
            "fire_action_template: rocket_fire\n"
            "projectile_template: " + weapon_template + "\n");
        return dir;
    };

    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            chain_dir(
                "derived_valid",
                "collision_mask: terrain | static_obstacle\nreplication: derived\n",
                "strike_marker")
                .string());
    require(projectile_mechanics(config, 41).replication ==
           KernelProjectileReplication_Derived);
    require(projectile_mechanics(config, 40).replication ==
           KernelProjectileReplication_Replicated);

    // Its end would depend on actors only the server sees move.
    const bool hits_actors = load_fails(chain_dir(
        "derived_hits_actors",
        "collision_mask: terrain | hostile_side\nreplication: derived\n",
        "strike_marker"));
    require(hits_actors);
    // Fired directly, it has no root for a client to hold.
    const bool fired_directly = load_fails(chain_dir(
        "derived_fired_directly",
        "collision_mask: terrain | static_obstacle\nreplication: derived\n",
        "meteor_body"));
    require(fired_directly);
    const bool bad_value = load_fails(chain_dir(
        "derived_bad_value",
        "collision_mask: terrain | static_obstacle\nreplication: sometimes\n",
        "strike_marker"));
    require(bad_value);

    // A rocket is not a root: its impact point is decided in flight.
    const std::filesystem::path rocket_dir = chain_dir(
        "derived_under_rocket",
        "collision_mask: terrain | static_obstacle\n",
        "strike_marker");
    write_file(
        rocket_dir.parent_path() / "projectile_templates" / "rocket_explosion.yaml",
        "id: 8\nname: rocket_explosion\nkind: area_effect\n"
        "collider_template: area_effect_sphere\n"
        "damage: 45\nlifetime_ticks: 45\n"
        "damage_behavior:\n  type: area_interval\n"
        "  damage_interval_ticks: 45\n  falloff: linear\n"
        "collision_mask: damageable\n"
        "replication: derived\n");
    const bool under_rocket = load_fails(rocket_dir);
    require(under_rocket);
}

void area_effect_speed_is_authored() {
    const std::filesystem::path default_dir = tmp_dir("area_speed_default");
    write_valid_templates(default_dir);
    network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            default_dir.string());
    require(projectile_mechanics(config, 4).speed == 0.0f);

    const std::filesystem::path moving_dir = tmp_dir("area_speed_moving");
    write_valid_templates(moving_dir);
    const std::string moving_template =
        "id: 4\nname: fire_floor_area\ntype: area_effect\n"
        "collider_template: area_effect_sphere\n"
        "damage: 12\n"
        "lifetime_ticks: 6\n"
        "damage_behavior:\n"
        "  type: area_interval\n"
        "  damage_interval_ticks: 2\n"
        "  falloff: none\n"
        "collision_mask: hostile_side\n";
    write_file(
        moving_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        moving_template + "speed: 6.0\n");
    config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            moving_dir.string());
    require(projectile_mechanics(config, 4).speed == 6.0f);
    // The motion model stays linear whatever the speed: homing is still a
    // standard-projectile-only model.
    require(projectile_mechanics(config, 4).motion_model ==
           KernelProjectileMotionModel_Linear);

    const std::filesystem::path negative_dir = tmp_dir("area_speed_negative");
    write_valid_templates(negative_dir);
    write_file(
        negative_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        moving_template + "speed: -1.0\n");
    require(load_fails(negative_dir));
}

void area_effect_hit_instigator_is_authored() {
    const std::filesystem::path default_dir = tmp_dir("hit_instigator_default");
    write_valid_templates(default_dir);
    network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            default_dir.string());
    require(projectile_mechanics(config, 4).area_effect.hit_instigator == 0u);

    const std::filesystem::path authored_dir = tmp_dir("hit_instigator_authored");
    write_valid_templates(authored_dir);
    write_file(
        authored_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        "id: 4\nname: fire_floor_area\ntype: area_effect\n"
        "collider_template: area_effect_sphere\n"
        "damage: 12\n"
        "lifetime_ticks: 6\n"
        "damage_behavior:\n"
        "  type: area_interval\n"
        "  damage_interval_ticks: 2\n"
        "  falloff: none\n"
        "collision_mask: hostile_side\n"
        "hit_instigator: true\n");
    config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            authored_dir.string());
    require(projectile_mechanics(config, 4).area_effect.hit_instigator == 1u);

    const std::filesystem::path standard_dir = tmp_dir("hit_instigator_standard");
    write_valid_templates(standard_dir);
    write_file(
        standard_dir.parent_path() / "projectile_templates" / "rocket.yaml",
        "id: 3\nname: rocket_projectile\ndamage: 45\n"
        "sync_mode: server_snapshot_only\ncollider_template: rocket_aabb\n"
        "movement_model: linear\nhit_response: destroy\n"
        "damage_shape: direct_hit\nspeed: 35.0\nlifetime_ticks: 75\n"
        "collision_mask: damageable\nmax_hit_count: 1\n"
        "hit_instigator: true\n"
        "gravity: {x: 0.0, y: 0.0, z: 0.0}\n");
    require(load_fails(standard_dir));
}

void area_effect_sync_mode_is_authored_not_forced() {
    const auto area_effect_template = [](const std::string& extra_fields) {
        return "id: 4\nname: fire_floor_area\ntype: area_effect\n"
               "collider_template: area_effect_sphere\n"
               "damage: 12\n"
               "lifetime_ticks: 6\n"
               "damage_behavior:\n"
               "  type: area_interval\n"
               "  damage_interval_ticks: 2\n"
               "  falloff: none\n"
               "collision_mask: hostile_side\n" +
            extra_fields;
    };

    const std::filesystem::path default_dir = tmp_dir("area_sync_default");
    write_valid_templates(default_dir);
    write_file(
        default_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        area_effect_template(""));
    network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            default_dir.string());
    require(projectile_mechanics(config, 4).sync_mode ==
           KernelProjectileSyncMode_ServerSnapshotOnly);

    const std::filesystem::path predicted_dir = tmp_dir("area_sync_predicted");
    write_valid_templates(predicted_dir);
    write_file(
        predicted_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        area_effect_template("sync_mode: local_predicted_deterministic\n"));
    config =
        network_example::game_server::load_gameplay_config_from_weapon_template_directory(
            predicted_dir.string());
    require(projectile_mechanics(config, 4).sync_mode ==
           KernelProjectileSyncMode_LocalPredictedDeterministic);

    const std::filesystem::path invalid_dir = tmp_dir("area_sync_invalid");
    write_valid_templates(invalid_dir);
    write_file(
        invalid_dir.parent_path() / "projectile_templates" / "fire_floor_area.yaml",
        area_effect_template("sync_mode: remote_magic\n"));
    require(load_fails(invalid_dir));

    // The three the area effect really does own are rejected rather than
    // accepted and overwritten.
    int overridden_index = 0;
    for (const char* overridden_field :
         {"movement_model: parabolic\n",
          "hit_response: bounce\n",
          "damage_shape: none\n"}) {
        const std::filesystem::path overridden_dir = tmp_dir(
            "area_overridden_" + std::to_string(overridden_index++));
        write_valid_templates(overridden_dir);
        write_file(
            overridden_dir.parent_path() / "projectile_templates" /
                "fire_floor_area.yaml",
            area_effect_template(overridden_field));
        require(load_fails(overridden_dir));
    }
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
    require(load_fails(unknown_dir));

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
    require(load_fails(empty_token_dir));

    const std::filesystem::path area_static_dir =
        tmp_dir("area_static_mask");
    write_valid_templates(area_static_dir);
    write_file(
        area_static_dir.parent_path() /
            "projectile_templates" / "fire_floor_area.yaml",
        "id: 4\nname: fire_floor_area\ntype: area_effect\n"
        "collider_template: area_effect_sphere\n"
        "damage: 12\n"
        "lifetime_ticks: 6\n"
        "damage_behavior:\n"
        "  type: area_interval\n"
        "  damage_interval_ticks: 2\n"
        "  falloff: none\n"
        "collision_mask: hostile_side | terrain\n");
    require(load_fails(area_static_dir));
}

void catalog_file_loads_colliders() {
    const std::filesystem::path catalog_file =
        runfiles_root() / "game_server" / "gameplay_catalog" /
        "gameplay_catalog.yaml";
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_catalog_file(
            catalog_file.string());
    require(config.weapons.catalog_version == 16);
    require(config.weapons.catalog_hash != 0);
    // Every collider_templates/*.yaml; counted, since the directory grows.
    std::size_t collider_files = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(
             catalog_file.parent_path() / "collider_templates")) {
        if (entry.is_regular_file() && entry.path().extension() == ".yaml") {
            ++collider_files;
        }
    }
    require(collider_files > 0);
    require(config.colliders.templates.size() == collider_files);
    require(config.colliders.bindings.empty());
}

// An instant weapon's shot template describes the shot without spawning it, so
// what this pins is that naming one does not drag the projectile branch's
// behaviour along with it: the weapon is still resolved by raycast.
void instant_weapon_keeps_its_fire_mode_and_segment() {
    const std::filesystem::path dir =
        runfiles_root() / "game_server" / "gameplay_catalog" /
        "weapon_templates";
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::
            load_gameplay_config_from_weapon_template_directory(dir.string());

    const KernelWeaponMechanicsDefinition& rifle =
        config.weapons.definitions[network_example::game_server::kWeaponRifle];
    const KernelWeaponMechanicsDefinition& shotgun =
        config.weapons.definitions[network_example::game_server::kWeaponShotgun];
    require(rifle.projectile_template_id == 10);
    require(shotgun.projectile_template_id == 11);

    // Everything the projectile branch would have changed, and did not.
    require(rifle.fire_mode == KernelWeaponFireMode_Hitscan);
    require(shotgun.fire_mode == KernelWeaponFireMode_Shotgun);
    require(rifle.segment_collider_template_id == 5);
    require(shotgun.segment_collider_template_id == 6);
    // collider_template_ids keeps the segment, not the shot template's collider.
    require(
        config.weapons.collider_template_ids[
            network_example::game_server::kWeaponRifle] == 5);
    require(
        config.weapons.collider_template_ids[
            network_example::game_server::kWeaponShotgun] == 6);
    require(
        config.weapons.projectile_sync_modes[
            network_example::game_server::kWeaponRifle] ==
        KernelProjectileSyncMode_HybridDeterministicThenSnapshot);
    require(shotgun.pellet_count == 5);

    // Both shot templates reach the catalog, which is what lets a client find
    // the asset it draws a tracer from.
    const network_example::game_server::KernelGameplayCatalogStorage storage =
        network_example::game_server::build_kernel_gameplay_catalog(config);
    std::uint32_t found_shots = 0;
    for (std::uint32_t index = 0;
         index < storage.definition.projectile_template_count;
         ++index) {
        const std::uint32_t id =
            storage.definition.projectile_templates[index].projectile_template_id;
        if (id == 10 || id == 11) {
            ++found_shots;
        }
    }
    require(found_shots == 2);
}

// Reload timing is per weapon, and it lives in an action template rather than a
// bare tick count so a weapon can change the *shape* of its reload -- a
// shell-at-a-time refill the player can interrupt is a different trigger_mode,
// flag set, and commit count, none of which a scalar could carry.
void weapons_name_their_own_reload_action() {
    const std::filesystem::path dir =
        runfiles_root() / "game_server" / "gameplay_catalog" /
        "weapon_templates";
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::
            load_gameplay_config_from_weapon_template_directory(dir.string());

    const auto reload_of = [&config](std::uint8_t weapon_id) {
        return config.weapons.definitions[weapon_id].reload_action_template_id;
    };
    const auto reload_offset_of = [&config](std::uint32_t action_template_id) {
        for (const network_example::game_server::ActionTemplateConfig& action :
             config.action_templates) {
            if (action.definition.action_template_id == action_template_id) {
                return action.definition.commit_offset_ticks;
            }
        }
        std::abort();
    };

    // Each weapon reaches a different template, and the durations are the ones
    // the weapon templates used to state in a `reload_ticks` nothing read.
    require(reload_offset_of(reload_of(network_example::game_server::kWeaponRifle)) == 30);
    require(reload_offset_of(reload_of(network_example::game_server::kWeaponShotgun)) == 45);
    require(reload_offset_of(reload_of(network_example::game_server::kWeaponRocket)) == 75);
    require(reload_offset_of(reload_of(network_example::game_server::kWeaponGrenade)) == 90);
    require(reload_offset_of(
               reload_of(network_example::game_server::kWeaponHomingMissile)) == 60);

    // Distinct templates, and none of them the catalog's shared fallback --
    // except the grunts' claw (10) and slam (11), which name `shared_reload`
    // themselves: a bottomless AI weapon has no reload of its own to author.
    require(reload_of(network_example::game_server::kWeaponRifle) !=
           reload_of(network_example::game_server::kWeaponShotgun));
    for (std::size_t id = 0; id < config.weapons.definitions.size(); ++id) {
        if (!config.weapons.configured[id]) {
            continue;
        }
        const bool names_shared_reload = id == 10u || id == 11u;
        require(
            (config.weapons.definitions[id].reload_action_template_id == 4199u) ==
            names_shared_reload);
    }
}

// Naming one is optional: a weapon that says nothing still reloads, through the
// catalog's shared action. The fixture's weapons author no reload reference.
void weapons_without_a_reload_action_fall_back_to_the_shared_one() {
    const std::filesystem::path dir = tmp_dir("shared_reload_fallback");
    write_valid_templates(dir);

    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::
            load_gameplay_config_from_weapon_template_directory(dir.string());

    for (std::size_t id = 0; id < config.weapons.definitions.size(); ++id) {
        if (config.weapons.configured[id]) {
            require(config.weapons.definitions[id].reload_action_template_id == 4199u);
        }
    }
}

// Damage is authored in exactly one place per projectile template -- the
// top-level `damage` -- whatever the type. The beam block and the area-effect
// block used to carry their own copies, which the loader then overwrote or
// silently lost to.
void damage_is_authored_once_per_projectile_template() {
    const std::filesystem::path dir =
        runfiles_root() / "game_server" / "gameplay_catalog" /
        "weapon_templates";
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::
            load_gameplay_config_from_weapon_template_directory(dir.string());

    // Beam: `damage` read as per-tick, copied into the block the runtime uses.
    const KernelProjectileMechanicsDefinition& beam_rifle =
        projectile_mechanics(config, 5);
    require(beam_rifle.damage == 1);
    require(beam_rifle.beam.damage_per_tick == beam_rifle.damage);

    // Area effect: `damage` read as per-interval, likewise.
    const KernelProjectileMechanicsDefinition& fire_floor =
        projectile_mechanics(config, 4);
    require(fire_floor.damage == 12);
    require(fire_floor.area_effect.damage_per_interval == fire_floor.damage);

    // An instant weapon is no different: its shot template owns the number, and
    // the weapon definition mirrors it rather than authoring a second one.
    const KernelProjectileMechanicsDefinition& rifle_shot =
        projectile_mechanics(config, 10);
    require(rifle_shot.damage == 45);
    require(config.weapons.definitions[network_example::game_server::kWeaponRifle]
               .damage == rifle_shot.damage);
    const KernelProjectileMechanicsDefinition& shotgun_shot =
        projectile_mechanics(config, 11);
    require(shotgun_shot.damage == 10);
    require(config.weapons
               .definitions[network_example::game_server::kWeaponShotgun]
               .damage == shotgun_shot.damage);

    // collision_mask travels the same way, and the shotgun still does not ask
    // for limbs while the rifle does.
    require(config.weapons.definitions[network_example::game_server::kWeaponRifle]
               .collision_mask == rifle_shot.collision_mask);
    require((config.weapons.definitions[network_example::game_server::kWeaponRifle]
                .collision_mask & KERNEL_COLLISION_LAYER_LIMB) != 0u);
    require((config.weapons
                .definitions[network_example::game_server::kWeaponShotgun]
                .collision_mask & KERNEL_COLLISION_LAYER_LIMB) == 0u);

    // An instant weapon without a shot template has nowhere to put either
    // number, so it is rejected rather than defaulting to zero damage.
    const std::filesystem::path no_shot_dir = tmp_dir("instant_without_shot");
    write_valid_templates(no_shot_dir);
    write_file(
        no_shot_dir / "rifle.yaml",
        "id: 0\nname: Rifle\nweapon_type: hitscan\nmagazine_size: 30\n"
        "fire_action_template: rifle_fire\nmax_range: 100.0\n"
        "segment_collider: rifle_segment\n");
    require(load_fails(no_shot_dir));
}

}  // namespace

int main() {
    weapons_name_their_own_reload_action();
    weapons_without_a_reload_action_fall_back_to_the_shared_one();
    damage_is_authored_once_per_projectile_template();
    instant_weapon_keeps_its_fire_mode_and_segment();
    valid_repo_templates_load_all_slots();
    projectile_collision_query_modes_are_loaded();
    invalid_templates_are_rejected();
    collision_mask_expressions_are_loaded();
    malformed_collision_masks_are_rejected();
    area_effect_sync_mode_is_authored_not_forced();
    area_effect_hit_instigator_is_authored();
    area_effect_speed_is_authored();
    stationary_marker_loads_only_when_inert();
    area_effect_rejects_on_expired();
    descent_launch_is_authored();
    targeted_strike_weapon_is_authored();
    spawn_repeat_is_authored();
    derived_replication_is_authored();
    subject_direction_needs_a_projectile_that_travels();
    area_effect_motion_collision_mask_is_authored();
    area_effect_ground_follow_and_cylinder_are_authored();
    catalog_file_loads_colliders();
    return 0;
}
