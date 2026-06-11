#include "game_server/gameplay_config.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "game_server/enemy.h"
#include "kernel/public/kernel_types.h"

namespace {

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

std::vector<std::uint8_t> make_gameplay_bundle_zip() {
    std::vector<std::pair<std::string, std::string>> files;
    files.push_back({
        "gameplay_catalog.yaml",
        read_text_file("game_server/gameplay_catalog.yaml")});
    files.push_back({
        "collider_templates/default.yaml",
        read_text_file("game_server/collider_templates/default.yaml")});
    files.push_back({
        "actor_templates/player.yaml",
        read_text_file("game_server/actor_templates/player.yaml")});
    files.push_back({
        "actor_templates/enemy_grunt.yaml",
        read_text_file("game_server/actor_templates/enemy_grunt.yaml")});

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
    return make_store_zip(files);
}

}  // namespace

int main() {
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    const std::vector<std::string> errors =
        network_example::game_server::validate_gameplay_config(config);
    assert(errors.empty());

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
    const KernelCombatStateDefinition player_combat_state =
        network_example::game_server::make_player_combat_state(config);
    assert(player_combat_state.hp == 100);
    assert(player_combat_state.max_hp == 100);
    assert(player_combat_state.move_speed_meters_per_second == 5.0f);

    assert(config.enemy.actor_template_id == 2);
    assert(config.enemy.spawn_position.x == 6.0f);
    const KernelCombatStateDefinition enemy_combat_state =
        network_example::game_server::make_enemy_combat_state(config);
    assert(enemy_combat_state.hp == network_example::game_server::kEnemyInitialHp);
    assert(
        enemy_combat_state.active_weapon_id ==
        network_example::game_server::kEnemyRocketWeaponId);
    const network_example::game_server::ActorTemplateConfig* config_enemy_template =
        network_example::game_server::find_actor_template(
            config,
            config.enemy.actor_template_id);
    assert(config_enemy_template != nullptr);
    assert(config_enemy_template->ai.move_speed == 2.5f);

    const KernelWeaponMechanicsDefinition& rifle =
        config.weapons.definitions[network_example::game_server::kWeaponRifle];
    assert(rifle.weapon_id == network_example::game_server::kWeaponRifle);
    assert(rifle.fire_mode == KernelWeaponFireMode_Hitscan);
    assert(rifle.damage == 25);
    assert(rifle.magazine_size == 30);
    assert(rifle.max_range == 100.0f);

    const KernelWeaponMechanicsDefinition& rocket =
        config.weapons.definitions[network_example::game_server::kWeaponRocket];
    assert(rocket.weapon_id == network_example::game_server::kWeaponRocket);
    assert(rocket.fire_mode == KernelWeaponFireMode_Projectile);
    assert(rocket.projectile.struct_size == sizeof(KernelProjectileMechanicsDefinition));
    assert(rocket.projectile.speed == 35.0f);
    assert(rocket.projectile.lifetime_seconds == 2.5f);
    assert(rocket.projectile.explosion_radius == 3.0f);
    assert(rocket.projectile.damage_shape == KernelProjectileDamageShape_Explosion);
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
    assert(
        projectile_spammer.projectile.motion_model ==
        KernelProjectileMotionModel_Linear);
    assert(
        projectile_spammer.projectile.damage_shape ==
        KernelProjectileDamageShape_DirectHit);
    assert(projectile_spammer.projectile.explosion_radius == 0.0f);
    assert(projectile_spammer.projectile.collision_mask == KERNEL_COLLISION_LAYER_PLAYER);
    assert(projectile_spammer.pellet_count == 3);
    assert(projectile_spammer.pellet_spread == 15.0f);
    assert(config.weapons.names[network_example::game_server::kWeaponGrenade] ==
           "Projectile Spammer");
    assert(
        network_example::game_server::active_weapon_id(*config_enemy_template) ==
        network_example::game_server::kWeaponGrenade);
    assert(config_enemy_template->ai.weapon_id ==
           network_example::game_server::kWeaponGrenade);
    assert(config_enemy_template->ai.magazine_size == 120);
    assert(
        config.weapons
            .projectile_sync_modes[network_example::game_server::kWeaponRocket] ==
        KernelProjectileSyncMode_ServerSnapshotOnly);
    assert(config.colliders.templates.size() == 4);
    assert(config.colliders.bindings.size() == 4);
    assert(config.actor_templates.size() == 2);
    const network_example::game_server::ActorTemplateConfig& player_template =
        config.actor_templates[0];
    assert(player_template.actor_template_id == 1);
    assert(player_template.name == "player");
    assert(player_template.entity_type == network_example::game_server::kEntityTypePlayer);
    assert(player_template.weapon_slot_count == 2);
    assert(player_template.weapon_slots[0] == network_example::game_server::kWeaponRifle);
    assert(player_template.weapon_slots[1] == network_example::game_server::kWeaponShotgun);
    assert(player_template.active_weapon_slot == 0);
    const network_example::game_server::ActorTemplateConfig& enemy_template =
        config.actor_templates[1];
    assert(enemy_template.actor_template_id == 2);
    assert(enemy_template.name == "enemy_grunt");
    assert(enemy_template.entity_type == network_example::game_server::kEntityTypeEnemy);
    assert(enemy_template.weapon_slot_count == 1);
    assert(enemy_template.weapon_slots[0] == network_example::game_server::kWeaponGrenade);

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

    const std::vector<std::uint8_t> gameplay_bundle = make_gameplay_bundle_zip();
    const network_example::game_server::GameServerGameplayConfig bundle_config =
        network_example::game_server::load_gameplay_config_from_bundle_memory(
            gameplay_bundle.data(),
            static_cast<std::uint32_t>(gameplay_bundle.size()),
            "gameplay_catalog.yaml");
    assert(bundle_config.weapons.catalog_hash == config.weapons.catalog_hash);
    assert(bundle_config.colliders.templates.size() == config.colliders.templates.size());
    assert(bundle_config.colliders.bindings.size() == config.colliders.bindings.size());
    assert(bundle_config.actor_templates.size() == config.actor_templates.size());
    assert(bundle_config.enemy.actor_template_id == config.enemy.actor_template_id);

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

    return 0;
}
