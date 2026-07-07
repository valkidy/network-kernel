#include "game_server/gameplay_config.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "game_server/agent_runtime.h"
#include "kernel/public/kernel_types.h"

namespace {

template <typename T, typename = void>
struct HasSentryMagazineSize : std::false_type {};

template <typename T>
struct HasSentryMagazineSize<
    T,
    std::void_t<decltype(std::declval<T&>().magazine_size)>>
    : std::true_type {};

static_assert(
    !HasSentryMagazineSize<
        network_example::game_server::AgentSentryConfig>::value,
    "AgentSentryConfig must not duplicate weapon template magazine_size.");

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

std::string read_text_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> read_binary_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

std::filesystem::path runfiles_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace;
}

void append_u16(std::vector<std::uint8_t>* out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xffu));
    out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void append_u32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    append_u16(out, static_cast<std::uint16_t>(value & 0xffffu));
    append_u16(out, static_cast<std::uint16_t>((value >> 16u) & 0xffffu));
}

std::uint32_t crc32(const std::string& text) {
    std::uint32_t crc = 0xffffffffu;
    for (const unsigned char byte : text) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

std::vector<std::uint8_t> make_store_zip(
    const std::vector<std::pair<std::string, std::string>>& files) {
    struct CentralEntry {
        std::string path;
        std::string data;
        std::uint32_t crc = 0;
        std::uint32_t local_offset = 0;
    };

    std::vector<CentralEntry> central_entries;
    std::vector<std::uint8_t> zip;
    for (const auto& [path, data] : files) {
        const std::uint32_t entry_crc = crc32(data);
        const std::uint32_t local_offset =
            static_cast<std::uint32_t>(zip.size());
        append_u32(&zip, 0x04034b50u);
        append_u16(&zip, 20);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u32(&zip, entry_crc);
        append_u32(&zip, static_cast<std::uint32_t>(data.size()));
        append_u32(&zip, static_cast<std::uint32_t>(data.size()));
        append_u16(&zip, static_cast<std::uint16_t>(path.size()));
        append_u16(&zip, 0);
        zip.insert(zip.end(), path.begin(), path.end());
        zip.insert(zip.end(), data.begin(), data.end());
        central_entries.push_back(CentralEntry{path, data, entry_crc, local_offset});
    }

    const std::uint32_t central_offset = static_cast<std::uint32_t>(zip.size());
    for (const CentralEntry& entry : central_entries) {
        append_u32(&zip, 0x02014b50u);
        append_u16(&zip, 20);
        append_u16(&zip, 20);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u32(&zip, entry.crc);
        append_u32(&zip, static_cast<std::uint32_t>(entry.data.size()));
        append_u32(&zip, static_cast<std::uint32_t>(entry.data.size()));
        append_u16(&zip, static_cast<std::uint16_t>(entry.path.size()));
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u16(&zip, 0);
        append_u32(&zip, 0);
        append_u32(&zip, entry.local_offset);
        zip.insert(zip.end(), entry.path.begin(), entry.path.end());
    }
    const std::uint32_t central_size =
        static_cast<std::uint32_t>(zip.size()) - central_offset;

    append_u32(&zip, 0x06054b50u);
    append_u16(&zip, 0);
    append_u16(&zip, 0);
    append_u16(&zip, static_cast<std::uint16_t>(central_entries.size()));
    append_u16(&zip, static_cast<std::uint16_t>(central_entries.size()));
    append_u32(&zip, central_size);
    append_u32(&zip, central_offset);
    append_u16(&zip, 0);
    return zip;
}

std::vector<std::uint8_t> make_gameplay_bundle_zip(
    const std::string& sentry_actor_yaml) {
    std::vector<std::pair<std::string, std::string>> files;
    files.push_back({
        "gameplay_catalog.yaml",
        read_text_file("game_server/gameplay_catalog.yaml")});
    files.push_back({
        "collider_templates/default.yaml",
        read_text_file("game_server/collider_templates/default.yaml")});
    files.push_back({
        "entity_templates/player.yaml",
        read_text_file("game_server/entity_templates/player.yaml")});
    files.push_back({
        "entity_templates/sentry_grunt.yaml",
        sentry_actor_yaml});
    files.push_back({
        "entity_templates/earth_mother.yaml",
        read_text_file("game_server/entity_templates/earth_mother.yaml")});

    const std::vector<std::string> weapon_files = {
        "beam_rifle.yaml",
        "fire_floor.yaml",
        "homing_missile.yaml",
        "rifle.yaml",
        "rocket.yaml",
        "shotgun.yaml",
        "spammer.yaml",
    };
    for (const std::string& file : weapon_files) {
        files.push_back({
            "weapon_templates/" + file,
            read_text_file("game_server/weapon_templates/" + file)});
    }
    const std::vector<std::string> projectile_files = {
        "beam_rifle_beam.yaml",
        "fire_floor_area.yaml",
        "homing_missile.yaml",
        "rocket.yaml",
        "rocket_explosion.yaml",
        "spammer.yaml",
    };
    for (const std::string& file : projectile_files) {
        files.push_back({
            "projectile_templates/" + file,
            read_text_file("game_server/projectile_templates/" + file)});
    }
    return make_store_zip(files);
}

std::vector<std::uint8_t> make_gameplay_bundle_zip() {
    return make_gameplay_bundle_zip(
        read_text_file("game_server/entity_templates/sentry_grunt.yaml"));
}

std::vector<std::uint8_t> make_entity_template_bundle_zip(
    const std::string& sentry_template_yaml) {
    std::vector<std::pair<std::string, std::string>> files;
    files.push_back({
        "gameplay_catalog.yaml",
        "catalog_version: 1\n"
        "weapon_template_dir: weapon_templates\n"
        "projectile_template_dir: projectile_templates\n"
        "entity_template_dir: entity_templates\n"
        "collider_template_file: collider_templates/default.yaml\n"
        "player:\n"
        "  entity_template: player\n"});
    files.push_back({
        "collider_templates/default.yaml",
        read_text_file("game_server/collider_templates/default.yaml")});
    files.push_back({
        "entity_templates/player.yaml",
        "id: 1\n"
        "name: player\n"
        "entity_type: actor\n"
        "actor_type: player\n"
        "camp: player_side\n"
        "collider_template: player_hit\n"
        "health:\n"
        "  hp: 1000\n"
        "  max_hp: 1000\n"
        "movement:\n"
        "  move_speed_meters_per_second: 5.0\n"
        "hitbox:\n"
        "  center: {x: 0.0, y: 0.9, z: 0.0}\n"
        "  half_extents: {x: 0.35, y: 0.9, z: 0.35}\n"
        "weapon_slots:\n"
        "  - 3\n"
        "  - 1\n"
        "active_weapon_slot: 0\n"
        "animations:\n"
        "  idle: 0\n"
        "  chasing: 1\n"});
    files.push_back({
        "entity_templates/sentry_grunt.yaml",
        sentry_template_yaml});
    files.push_back({
        "entity_templates/earth_mother.yaml",
        "id: 100\n"
        "name: earth_mother\n"
        "entity_type: director\n"
        "server_only: true\n"
        "transform:\n"
        "  position: {x: 0.0, y: 0.0, z: 0.0}\n"
        "ai:\n"
        "  controller: director\n"
        "  profile: earth_mother\n"
        "  tick_interval: 10\n"
        "director:\n"
        "  kind: world_rule\n"
        "  spawn:\n"
        "    target_count: 10\n"
        "    entity_template: sentry_grunt\n"
        "    radius: 5.0\n"
        "    seed: 4242\n"});

    const std::vector<std::string> weapon_files = {
        "beam_rifle.yaml",
        "fire_floor.yaml",
        "homing_missile.yaml",
        "rifle.yaml",
        "rocket.yaml",
        "shotgun.yaml",
        "spammer.yaml",
    };
    for (const std::string& file : weapon_files) {
        files.push_back({
            "weapon_templates/" + file,
            read_text_file("game_server/weapon_templates/" + file)});
    }
    const std::vector<std::string> projectile_files = {
        "beam_rifle_beam.yaml",
        "fire_floor_area.yaml",
        "homing_missile.yaml",
        "rocket.yaml",
        "rocket_explosion.yaml",
        "spammer.yaml",
    };
    for (const std::string& file : projectile_files) {
        files.push_back({
            "projectile_templates/" + file,
            read_text_file("game_server/projectile_templates/" + file)});
    }
    return make_store_zip(files);
}

std::string sentry_grunt_entity_template_yaml(std::string entity_type) {
    return
        "id: 2\n"
        "name: sentry_grunt\n"
        "entity_type: " + entity_type + "\n"
        "actor_type: agent\n"
        "camp: enemy_side\n"
        "collider_template: enemy_hit\n"
        "health:\n"
        "  hp: 500\n"
        "  max_hp: 500\n"
        "movement:\n"
        "  move_speed_meters_per_second: 2.5\n"
        "hitbox:\n"
        "  center: {x: 0.0, y: 0.8, z: 0.0}\n"
        "  half_extents: {x: 0.4, y: 0.8, z: 0.4}\n"
        "weapon_slots:\n"
        "  - 2\n"
        "active_weapon_slot: 0\n"
        "animations:\n"
        "  idle: 0\n"
        "  chasing: 1\n"
        "vision:\n"
        "  collider_template: cone_vision\n"
        "ai:\n"
        "  controller: sentry\n"
        "  profile: default\n";
}

}  // namespace

int main() {
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    assert(network_example::game_server::AgentSentryConfig{}.weapon_id ==
           KERNEL_MAX_WEAPONS);
    const std::vector<std::string> errors =
        network_example::game_server::validate_gameplay_config(config);
    assert(errors.empty());

    const std::vector<std::uint8_t> entity_bundle =
        make_entity_template_bundle_zip(
            sentry_grunt_entity_template_yaml("actor"));
    const network_example::game_server::GameServerGameplayConfig entity_config =
        network_example::game_server::load_gameplay_config_from_bundle_memory(
            entity_bundle.data(),
            static_cast<std::uint32_t>(entity_bundle.size()),
            "gameplay_catalog.yaml");
    assert(entity_config.entity_templates.size() == 3);
    assert(entity_config.entity_templates[1].name == "sentry_grunt");
    assert(entity_config.entity_templates[1].actor_type ==
           network_example::game_server::kActorTypeAgent);
    assert(entity_config.entity_templates[1].vision.camp == KernelAgentCamp_EnemySide);
    assert(entity_config.entity_templates[2].name == "earth_mother");
    assert(entity_config.entity_templates[2].entity_type == KernelEntityType_Director);
    const network_example::game_server::KernelGameplayCatalogStorage
        entity_catalog =
            network_example::game_server::build_kernel_gameplay_catalog(entity_config);
    assert(entity_catalog.definition.entity_template_count == 3);
    assert(entity_catalog.entity_templates[2].entity_type == KernelEntityType_Director);
    assert(
        (entity_catalog.entity_templates[2].component_flags &
         KERNEL_ENTITY_COMPONENT_SERVER_ONLY) != 0u);
    assert(
        entity_catalog.entity_templates[2].ai.controller_type ==
        KernelAiControllerType_Director);
    assert(entity_catalog.entity_templates[2].ai.tick_interval == 10);
    assert(entity_catalog.entity_templates[2].ai.spawn_target_count == 10);
    assert(entity_catalog.entity_templates[2].ai.spawn_entity_template_id == 2);

    const std::string data_driven_sentry_yaml =
        sentry_grunt_entity_template_yaml("actor") +
        "  sentry:\n"
        "    alert_ticks: 4\n"
        "    forget_ticks: 6\n"
        "    patrol_rotation_interval_ticks: 2\n"
        "    patrol_rotation_min_degrees: 10.5\n"
        "    patrol_rotation_max_degrees: 22.5\n"
        "    weapon_id: 2\n"
        "    animation_idle: idle\n"
        "    animation_attack: chasing\n";
    const std::vector<std::uint8_t> data_driven_sentry_bundle =
        make_entity_template_bundle_zip(data_driven_sentry_yaml);
    const network_example::game_server::GameServerGameplayConfig
        data_driven_sentry_config =
            network_example::game_server::load_gameplay_config_from_bundle_memory(
                data_driven_sentry_bundle.data(),
                static_cast<std::uint32_t>(data_driven_sentry_bundle.size()),
                "gameplay_catalog.yaml");
    const network_example::game_server::ActorTemplateConfig&
        data_driven_sentry =
            data_driven_sentry_config.entity_templates[1];
    assert(data_driven_sentry.sentry.alert_ticks == 4);
    assert(data_driven_sentry.sentry.forget_ticks == 6);
    assert(data_driven_sentry.sentry.patrol_rotation_interval_ticks == 2);
    assert(data_driven_sentry.sentry.patrol_rotation_min_degrees == 10.5f);
    assert(data_driven_sentry.sentry.patrol_rotation_max_degrees == 22.5f);
    assert(data_driven_sentry.sentry.weapon_id == 2);
    assert(data_driven_sentry.sentry.animation_idle ==
           data_driven_sentry.animation_idle);
    assert(data_driven_sentry.sentry.animation_attack ==
           data_driven_sentry.animation_chasing);

    const std::vector<std::uint8_t> invalid_enemy_entity_bundle =
        make_entity_template_bundle_zip(
            sentry_grunt_entity_template_yaml("enemy"));
    bool enemy_entity_type_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            invalid_enemy_entity_bundle.data(),
            static_cast<std::uint32_t>(invalid_enemy_entity_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        enemy_entity_type_rejected =
            std::string(error.what()).find("unsupported entity_type: enemy") !=
            std::string::npos;
    }
    assert(enemy_entity_type_rejected);

    assert(config.player.actor_template_id == 1);
    require(config.weapons.catalog_version == 1);
    require(config.weapons.catalog_hash != 0);
    require(
        config.weapons.catalog_hash ==
        network_example::game_server::compute_gameplay_catalog_hash(config));
    network_example::game_server::GameServerGameplayConfig changed_config = config;
    changed_config.weapons
        .definitions[network_example::game_server::kWeaponRocket]
        .damage += 1;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(changed_config));
    changed_config = config;
    changed_config.projectile_templates[1].definition.mechanics.damage += 1;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(changed_config));
    const KernelCombatStateDefinition player_combat_state =
        network_example::game_server::make_player_combat_state(config);
    assert(player_combat_state.hp == 1000);
    assert(player_combat_state.max_hp == 1000);
    assert(player_combat_state.move_speed_meters_per_second == 5.0f);
    assert(player_combat_state.collider_template_id == 1);

    assert(config.agent.actor_template_id == 2);
    assert(config.agent.spawn_position.x == 6.0f);
    assert(config.agent.spawn_count == 10);
    assert(config.agent.spawn_radius == 5.0f);
    assert(config.agent.spawn_seed == 4242);
    const KernelCombatStateDefinition enemy_combat_state =
        network_example::game_server::make_agent_combat_state(config);
    assert(enemy_combat_state.hp == 500);
    assert(enemy_combat_state.max_hp == 500);
    assert(enemy_combat_state.collider_template_id == 2);
    assert(
        enemy_combat_state.active_weapon_id ==
        network_example::game_server::kWeaponGrenade);
    const network_example::game_server::ActorTemplateConfig* config_enemy_template =
        network_example::game_server::find_actor_template(
            config,
            config.agent.actor_template_id);
    assert(config_enemy_template != nullptr);
    assert(config_enemy_template->move_speed_meters_per_second == 2.5f);
    assert(config_enemy_template->vision.camp == KernelAgentCamp_EnemySide);
    assert(config_enemy_template->vision.vision_collider_template_id == 9);
    const network_example::game_server::KernelGameplayCatalogStorage catalog =
        network_example::game_server::build_kernel_gameplay_catalog(config);
    assert(catalog.definition.actor_template_count == config.actor_templates.size());
    assert(catalog.actor_templates.size() == config.actor_templates.size());
    assert(catalog.actor_templates[1].actor_template_id == 2);
    assert(catalog.actor_templates[1].collider_template_id == 2);
    assert(catalog.actor_templates[1].vision.vision_collider_template_id == 9);

    const KernelWeaponMechanicsDefinition& rifle =
        config.weapons.definitions[network_example::game_server::kWeaponRifle];
    assert(rifle.weapon_id == network_example::game_server::kWeaponRifle);
    assert(rifle.fire_mode == KernelWeaponFireMode_Hitscan);
    assert(rifle.damage == 25);
    assert(rifle.magazine_size == 30);
    assert(rifle.max_range == 100.0f);
    assert(rifle.segment_collider_template_id == 5);

    const KernelWeaponMechanicsDefinition& rocket =
        config.weapons.definitions[network_example::game_server::kWeaponRocket];
    assert(rocket.weapon_id == network_example::game_server::kWeaponRocket);
    assert(rocket.fire_mode == KernelWeaponFireMode_Projectile);
    assert(rocket.projectile_template_id == 3);
    assert(
        config.weapons
            .projectile_sync_modes[network_example::game_server::kWeaponGrenade] ==
        KernelProjectileSyncMode_LocalPredictedDeterministic);
    const KernelWeaponMechanicsDefinition& projectile_spammer =
        config.weapons.definitions[network_example::game_server::kWeaponGrenade];
    assert(projectile_spammer.fire_mode == KernelWeaponFireMode_Projectile);
    assert(projectile_spammer.damage == 1);
    assert(projectile_spammer.magazine_size == 120);
    assert(projectile_spammer.cooldown_ticks == 1);
    assert(projectile_spammer.projectile_template_id == 2);
    assert(projectile_spammer.pellet_count == 3);
    assert(projectile_spammer.pellet_spread == 15.0f);
    assert(config.weapons.collider_template_ids
               [network_example::game_server::kWeaponGrenade] == 7);
    assert(config.weapons.names[network_example::game_server::kWeaponGrenade] ==
           "Projectile Spammer");
    assert(
        network_example::game_server::active_weapon_id(*config_enemy_template) ==
        network_example::game_server::kWeaponGrenade);
    assert(config_enemy_template->sentry.weapon_id ==
           network_example::game_server::kWeaponGrenade);
    assert(config_enemy_template->sentry.alert_ticks == 90);
    assert(config_enemy_template->sentry.forget_ticks == 150);
    assert(config_enemy_template->sentry.patrol_rotation_interval_ticks == 30);
    assert(config_enemy_template->sentry.patrol_rotation_min_degrees == 15.0f);
    assert(config_enemy_template->sentry.patrol_rotation_max_degrees == 30.0f);
    assert(
        config.weapons
            .projectile_sync_modes[network_example::game_server::kWeaponRocket] ==
        KernelProjectileSyncMode_ServerSnapshotOnly);
    assert(config.colliders.templates.size() == 9);
    assert(config.colliders.bindings.empty());
    assert(config.actor_templates.size() == 2);
    const network_example::game_server::ActorTemplateConfig& player_template =
        config.actor_templates[0];
    assert(player_template.actor_template_id == 1);
    assert(player_template.name == "player");
    assert(player_template.entity_type == network_example::game_server::kEntityTypeActor);
    assert(player_template.actor_type == network_example::game_server::kActorTypePlayer);
    assert(player_template.collider_template_id == 1);
    assert(player_template.weapon_slot_count == 2);
    assert(player_template.weapon_slots[0] == network_example::game_server::kWeaponRocket);
    assert(player_template.weapon_slots[1] == network_example::game_server::kWeaponShotgun);
    assert(player_template.active_weapon_slot == 0);
    assert(player_template.vision.camp == KernelAgentCamp_PlayerSide);
    assert(player_template.vision.vision_collider_template_id == 0);
    const network_example::game_server::ActorTemplateConfig& enemy_template =
        config.actor_templates[1];
    assert(enemy_template.actor_template_id == 2);
    assert(enemy_template.name == "sentry_grunt");
    assert(enemy_template.entity_type == network_example::game_server::kEntityTypeActor);
    assert(enemy_template.actor_type == network_example::game_server::kActorTypeAgent);
    assert(enemy_template.collider_template_id == 2);
    assert(enemy_template.weapon_slot_count == 1);
    assert(enemy_template.weapon_slots[0] == network_example::game_server::kWeaponGrenade);

    const KernelWeaponMechanicsDefinition& fire_floor =
        config.weapons.definitions[network_example::game_server::kWeaponFireFloor];
    assert(fire_floor.weapon_id == network_example::game_server::kWeaponFireFloor);
    assert(fire_floor.fire_mode == KernelWeaponFireMode_Projectile);
    assert(fire_floor.projectile_template_id == 4);
    assert(config.weapons.names[network_example::game_server::kWeaponFireFloor] ==
           "Fire Floor");

    const KernelWeaponMechanicsDefinition& beam_rifle =
        config.weapons.definitions[network_example::game_server::kWeaponBeamRifle];
    assert(beam_rifle.weapon_id == network_example::game_server::kWeaponBeamRifle);
    assert(beam_rifle.fire_mode == KernelWeaponFireMode_Projectile);
    assert(beam_rifle.projectile_template_id == 5);
    assert(config.weapons.collider_template_ids
               [network_example::game_server::kWeaponBeamRifle] == 8);
    assert(config.weapons.names[network_example::game_server::kWeaponBeamRifle] ==
           "Beam Rifle");

    bool found_vision_collider = false;
    for (const network_example::game_server::ColliderTemplateConfig& collider :
         config.colliders.templates) {
        if (collider.definition.template_id == 9) {
            found_vision_collider = true;
            assert(collider.name == "cone_vision");
            assert(collider.definition.shape_type == KernelColliderShapeType_Cone);
            assert(collider.definition.purpose_flags == KernelColliderPurpose_Vision);
            assert(collider.definition.layer_mask == KERNEL_COLLISION_LAYER_AGENT_VISION);
            assert(collider.definition.shape_params.x == 12.0f);
            assert(collider.definition.shape_params.y == 90.0f);
        }
    }
    assert(found_vision_collider);

    const KernelWeaponMechanicsDefinition& homing_missile =
        config.weapons.definitions[network_example::game_server::kWeaponHomingMissile];
    assert(homing_missile.projectile_template_id == 6);
    assert(config.weapons.collider_template_ids
               [network_example::game_server::kWeaponHomingMissile] == 7);
    assert(config.projectile_templates.size() == 6);
    bool found_homing_projectile = false;
    bool found_rocket_projectile = false;
    bool found_rocket_explosion = false;
    bool found_spammer_projectile = false;
    bool found_fire_floor_area = false;
    bool found_beam_rifle_beam = false;
    for (const network_example::game_server::ProjectileTemplateConfig& projectile :
         config.projectile_templates) {
        if (projectile.name == "spammer_projectile") {
            found_spammer_projectile = true;
            assert(projectile.definition.mechanics.collider_template_id == 7);
            assert(projectile.definition.mechanics.damage == 1);
        }
        if (projectile.name == "rocket_projectile") {
            found_rocket_projectile = true;
            assert(projectile.definition.mechanics.collider_template_id == 3);
            assert(projectile.definition.mechanics.damage_shape ==
                   KernelProjectileDamageShape_DirectHit);
            assert(projectile.definition.mechanics
                       .impact_spawn_projectile_template_id == 8);
            assert((projectile.definition.mechanics.flags & 1u) != 0u);
        }
        if (projectile.name == "rocket_explosion") {
            found_rocket_explosion = true;
            assert(projectile.definition.mechanics.projectile_type ==
                   KernelProjectileType_AreaEffect);
            assert(projectile.definition.mechanics.damage == 45);
            assert(projectile.definition.mechanics.area_effect.damage_interval_ticks == 45);
            assert(projectile.definition.mechanics.area_effect.lifetime_ticks == 45);
            assert(projectile.definition.mechanics.damage_falloff ==
                   KernelProjectileDamageFalloff_Linear);
        }
        if (projectile.name == "homing_missile_projectile") {
            found_homing_projectile = true;
            assert(projectile.definition.mechanics.collider_template_id == 7);
            assert(projectile.definition.mechanics.homing.lock_on_range == 25.0f);
        }
        if (projectile.name == "fire_floor_area") {
            found_fire_floor_area = true;
            assert(projectile.definition.mechanics.projectile_type ==
                   KernelProjectileType_AreaEffect);
            assert(projectile.definition.mechanics.area_effect.damage_per_interval == 12);
        }
        if (projectile.name == "beam_rifle_beam") {
            found_beam_rifle_beam = true;
            assert(projectile.definition.mechanics.projectile_type ==
                   KernelProjectileType_Beam);
            assert(projectile.definition.mechanics.beam.damage_per_tick == 1);
        }
    }
    assert(found_spammer_projectile);
    assert(found_rocket_projectile);
    assert(found_rocket_explosion);
    assert(found_homing_projectile);
    assert(found_fire_floor_area);
    assert(found_beam_rifle_beam);

    const std::vector<std::uint8_t> gameplay_bundle = make_gameplay_bundle_zip();
    const network_example::game_server::GameServerGameplayConfig bundle_config =
        network_example::game_server::load_gameplay_config_from_bundle_memory(
            gameplay_bundle.data(),
            static_cast<std::uint32_t>(gameplay_bundle.size()),
            "gameplay_catalog.yaml");
    assert(bundle_config.weapons.catalog_hash == config.weapons.catalog_hash);
    assert(bundle_config.colliders.templates.size() == config.colliders.templates.size());
    assert(bundle_config.colliders.bindings.empty());
    assert(bundle_config.projectile_templates.size() == config.projectile_templates.size());
    assert(bundle_config.actor_templates.size() == config.actor_templates.size());
    assert(bundle_config.agent.actor_template_id == config.agent.actor_template_id);

    const std::vector<std::uint8_t> generated_bundle = read_binary_file(
        (runfiles_root() / "game_server" / "gameplay_catalog_bundle" / "bundle.zip")
            .string());
    const network_example::game_server::GameServerGameplayConfig generated_bundle_config =
        network_example::game_server::load_gameplay_config_from_bundle_memory(
            generated_bundle.data(),
            static_cast<std::uint32_t>(generated_bundle.size()),
            "gameplay_catalog.yaml");
    assert(generated_bundle_config.weapons.catalog_hash == config.weapons.catalog_hash);
    assert(
        generated_bundle_config.projectile_templates.size() ==
        config.projectile_templates.size());

    const std::vector<std::uint8_t> unsupported_version_bundle = make_store_zip({
        {"gameplay_catalog.yaml", "catalog_version: 2\n"},
    });
    bool unsupported_version_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            unsupported_version_bundle.data(),
            static_cast<std::uint32_t>(unsupported_version_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        unsupported_version_rejected =
            std::string(error.what()).find("unsupported catalog_version") !=
            std::string::npos;
    }
    assert(unsupported_version_rejected);

    const std::vector<std::uint8_t> unknown_catalog_field_bundle = make_store_zip({
        {"gameplay_catalog.yaml", "catalog_version: 1\nsurprise: true\n"},
    });
    bool unknown_catalog_field_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            unknown_catalog_field_bundle.data(),
            static_cast<std::uint32_t>(unknown_catalog_field_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        unknown_catalog_field_rejected =
            std::string(error.what()).find("unknown field") != std::string::npos;
    }
    assert(unknown_catalog_field_rejected);

    const std::vector<std::uint8_t> unknown_nested_catalog_field_bundle = make_store_zip({
        {"gameplay_catalog.yaml",
         "catalog_version: 1\n"
         "weapon_template_dir: weapon_templates\n"
         "projectile_template_dir: projectile_templates\n"
         "collider_template_file: collider_templates/default.yaml\n"
         "player:\n"
         "  actor_template: player\n"
         "  current_hp: 12\n"},
    });
    bool unknown_nested_catalog_field_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            unknown_nested_catalog_field_bundle.data(),
            static_cast<std::uint32_t>(unknown_nested_catalog_field_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        unknown_nested_catalog_field_rejected =
            std::string(error.what()).find("unknown field") != std::string::npos;
    }
    assert(unknown_nested_catalog_field_rejected);

    std::vector<std::pair<std::string, std::string>> duplicate_collider_files;
    duplicate_collider_files.push_back({
        "gameplay_catalog.yaml",
        "catalog_version: 1\n"
        "weapon_template_dir: weapon_templates\n"
        "projectile_template_dir: projectile_templates\n"
        "collider_template_file: collider_templates/default.yaml\n"});
    const std::vector<std::string> duplicate_collider_weapon_files = {
        "beam_rifle.yaml",
        "fire_floor.yaml",
        "homing_missile.yaml",
        "rifle.yaml",
        "rocket.yaml",
        "shotgun.yaml",
        "spammer.yaml",
    };
    for (const std::string& file : duplicate_collider_weapon_files) {
        duplicate_collider_files.push_back({
            "weapon_templates/" + file,
            read_text_file("game_server/weapon_templates/" + file)});
    }
    duplicate_collider_files.push_back({
        "collider_templates/default.yaml",
        "templates:\n"
        "  - id: 1\n"
        "    name: a\n"
        "    shape: aabb\n"
        "    center: {x: 0.0, y: 0.0, z: 0.0}\n"
        "    half_extents: {x: 1.0, y: 1.0, z: 1.0}\n"
        "    radius: 0.0\n"
        "    purpose: hit\n"
        "    layer: player_side\n"
        "  - id: 1\n"
        "    name: b\n"
        "    shape: sphere\n"
        "    center: {x: 0.0, y: 0.0, z: 0.0}\n"
        "    half_extents: {x: 1.0, y: 1.0, z: 1.0}\n"
        "    radius: 1.0\n"
        "    purpose: damage\n"
        "    layer: hostile_side\n"});
    const std::vector<std::uint8_t> duplicate_collider_id_bundle =
        make_store_zip(duplicate_collider_files);
    bool duplicate_collider_id_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            duplicate_collider_id_bundle.data(),
            static_cast<std::uint32_t>(duplicate_collider_id_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        duplicate_collider_id_rejected =
            std::string(error.what()).find("duplicate collider template id") !=
            std::string::npos;
    }
    assert(duplicate_collider_id_rejected);

    const std::vector<std::uint8_t> invalid_path_bundle = make_store_zip({
        {"../x.yaml", "catalog_version: 1\n"},
    });
    bool invalid_path_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            invalid_path_bundle.data(),
            static_cast<std::uint32_t>(invalid_path_bundle.size()),
            "../x.yaml");
    } catch (const std::exception& error) {
        invalid_path_rejected =
            std::string(error.what()).find("invalid archive path") !=
            std::string::npos;
    }
    assert(invalid_path_rejected);

    const std::vector<std::uint8_t> duplicate_path_bundle = make_store_zip({
        {"gameplay_catalog.yaml", "catalog_version: 1\n"},
        {"gameplay_catalog.yaml", "catalog_version: 2\n"},
    });
    bool duplicate_path_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            duplicate_path_bundle.data(),
            static_cast<std::uint32_t>(duplicate_path_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        duplicate_path_rejected =
            std::string(error.what()).find("duplicate archive entry") !=
            std::string::npos;
    }
    assert(duplicate_path_rejected);

    network_example::game_server::GameServerGameplayConfig invalid = config;
    invalid.weapons.definitions[0].damage = 0;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());

    network_example::game_server::GameServerGameplayConfig actor_hash_changed =
        config;
    actor_hash_changed.actor_templates[0].health.hp += 1;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(
            actor_hash_changed));
    actor_hash_changed = config;
    actor_hash_changed.actor_templates[1].sentry.patrol_rotation_interval_ticks += 1;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(
            actor_hash_changed));
    actor_hash_changed = config;
    actor_hash_changed.actor_templates[1].sentry.patrol_rotation_max_degrees += 1.0f;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(
            actor_hash_changed));

    invalid = config;
    invalid.actor_templates[1].sentry.weapon_id =
        network_example::game_server::kWeaponRocket;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());
    invalid = config;
    invalid.actor_templates[1].sentry.alert_ticks = 0;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());
    invalid = config;
    invalid.actor_templates[1].sentry.forget_ticks = 0;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());
    invalid = config;
    invalid.actor_templates[1].sentry.patrol_rotation_interval_ticks = 0;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());
    invalid = config;
    invalid.actor_templates[1].sentry.patrol_rotation_min_degrees = 0.0f;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());
    invalid = config;
    invalid.actor_templates[1].sentry.patrol_rotation_max_degrees =
        invalid.actor_templates[1].sentry.patrol_rotation_min_degrees - 1.0f;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());

    return 0;
}
