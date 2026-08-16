#include "game_server/gameplay_config.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "game_server/agent_runtime.h"
#include "game_server/game_server.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"
#include "kernel/src/legged_locomotion.h"
#include "kernel/src/skeleton_presentation.h"

// The follower locomotion path is engine-internal: what will eventually drive
// it is a replicated field rather than a call, so it has no C API surface to
// test through yet. Every standard header the kernel pulls in is included ahead
// of the access override, so the override only ever reaches kernel.h itself.
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <thread>
#include <unordered_set>
#include <variant>
#include "protocol/public/network_packets.h"
#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

constexpr std::uint16_t kMaxReserveMagazines =
    std::numeric_limits<std::uint16_t>::max();

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

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(
            stderr,
            "require failed at line %d: %s\n",
            line,
            expression);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

void hash_u64(std::uint64_t* hash, std::uint64_t value) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    for (std::uint32_t byte = 0u; byte < 8u; ++byte) {
        *hash ^= (value >> (byte * 8u)) & 0xffu;
        *hash *= kFnvPrime;
    }
}

void hash_quantized_float(std::uint64_t* hash, float value) {
    const auto quantized = static_cast<std::int64_t>(
        std::llround(static_cast<double>(value) * 10000.0));
    hash_u64(hash, static_cast<std::uint64_t>(quantized));
}

void hash_vec3(std::uint64_t* hash, const glm::vec3& value) {
    hash_quantized_float(hash, value.x);
    hash_quantized_float(hash, value.y);
    hash_quantized_float(hash, value.z);
}

std::uint64_t quantized_locomotion_hash(
    const network_example::LocomotionState& state,
    const glm::vec3& root_position,
    const glm::vec3& root_velocity) {
    constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
    std::uint64_t hash = kFnvOffsetBasis;
    hash_vec3(&hash, root_position);
    hash_vec3(&hash, root_velocity);
    hash_quantized_float(&hash, state.root_yaw_radians);
    hash_u64(&hash, state.pose_valid ? 1u : 0u);
    hash_u64(&hash, state.legs.size());
    for (const network_example::LegLocomotionState& leg : state.legs) {
        hash_u64(&hash, leg.hip_bone_index);
        hash_u64(&hash, leg.knee_bone_index);
        hash_u64(&hash, leg.foot_bone_index);
        hash_u64(&hash, leg.gait_group);
        hash_u64(&hash, leg.swing_tick);
        hash_u64(&hash, static_cast<std::uint8_t>(leg.gait_state));
        hash_u64(&hash, leg.entered_swing ? 1u : 0u);
        hash_u64(&hash, leg.entered_support ? 1u : 0u);
        hash_vec3(&hash, leg.foot_target_world);
        hash_u64(&hash, leg.foot_initialized ? 1u : 0u);
        hash_vec3(&hash, leg.swing_start_world);
        hash_vec3(&hash, leg.landing_target_world);
        hash_vec3(&hash, leg.solved_foot_world);
        hash_vec3(&hash, leg.ground_hit_position);
        hash_vec3(&hash, leg.ground_hit_normal);
        hash_u64(&hash, leg.grounding_candidate_index);
        hash_u64(&hash, leg.supporting_entity_net_id);
        hash_u64(&hash, leg.supporting_collider_id);
        hash_u64(&hash, leg.ground_hit_valid ? 1u : 0u);
        hash_u64(&hash, leg.ik_reach_clamped ? 1u : 0u);
    }
    hash_u64(&hash, state.last_processing_order.size());
    for (const std::uint32_t leg_index : state.last_processing_order) {
        hash_u64(&hash, leg_index);
    }
    hash_u64(&hash, state.local_pose.size());
    for (const KernelBoneLocalTransform& transform : state.local_pose) {
        hash_quantized_float(&hash, transform.local_position.x);
        hash_quantized_float(&hash, transform.local_position.y);
        hash_quantized_float(&hash, transform.local_position.z);
        hash_quantized_float(&hash, transform.local_rotation.x);
        hash_quantized_float(&hash, transform.local_rotation.y);
        hash_quantized_float(&hash, transform.local_rotation.z);
        hash_quantized_float(&hash, transform.local_rotation.w);
        hash_quantized_float(&hash, transform.local_scale.x);
        hash_quantized_float(&hash, transform.local_scale.y);
        hash_quantized_float(&hash, transform.local_scale.z);
    }
    return hash;
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

std::string read_binary_string(const std::string& path) {
    const std::vector<std::uint8_t> bytes = read_binary_file(path);
    return std::string(
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size());
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

void append_collider_template_files(
    std::vector<std::pair<std::string, std::string>>* files) {
    const std::vector<std::string> collider_files = {
        "beam_oriented_box.yaml",
        "sentry_grunt_vision_cone.yaml",
        "sentry_grunt_hit_aabb.yaml",
        "area_effect_sphere.yaml",
        "collision_damage_prop_hitbox.yaml",
        "ice_block_hitbox.yaml",
        "monster_sim_movement_capsule.yaml",
        "player_hit_aabb.yaml",
        "player_movement_capsule.yaml",
        "rocket_aabb.yaml",
        "rifle_segment.yaml",
        "shotgun_segment.yaml",
        "projectile_sphere.yaml",
        "sentry_grunt_movement_capsule.yaml",
    };
    for (const std::string& file : collider_files) {
        files->push_back({
            "collider_templates/" + file,
            read_text_file("game_server/collider_templates/" + file)});
    }
}

std::vector<std::uint8_t> make_gameplay_bundle_zip(
    const std::string& sentry_actor_yaml,
    const std::vector<std::pair<std::string, std::string>>& extra_files = {},
    const std::string& player_actor_yaml = {},
    const std::string& catalog_yaml = {},
    const std::string& monster_actor_yaml = {}) {
    std::vector<std::pair<std::string, std::string>> files;
    files.push_back({
        "gameplay_catalog.yaml",
        catalog_yaml.empty()
            ? read_text_file("game_server/gameplay_catalog.yaml")
            : catalog_yaml});
    append_collider_template_files(&files);
    // Every entity template on disk, with the three the callers override
    // substituted in place. Enumerating keeps the bundle's template set equal
    // to the filesystem's, which the catalog-hash comparison below depends on;
    // a hardcoded list silently drifts as templates are added.
    {
        const std::unordered_map<std::string, std::string> overrides = {
            {"player.yaml", player_actor_yaml},
            {"sentry_grunt.yaml", sentry_actor_yaml},
            {"monster_sim_actor.yaml", monster_actor_yaml},
        };
        std::vector<std::filesystem::path> entity_files;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(
                 "game_server/entity_templates")) {
            if (entry.is_regular_file() &&
                entry.path().extension() == ".yaml") {
                entity_files.push_back(entry.path());
            }
        }
        std::sort(entity_files.begin(), entity_files.end());
        for (const std::filesystem::path& entry : entity_files) {
            const std::string name = entry.filename().string();
            const auto override_entry = overrides.find(name);
            const bool use_override = override_entry != overrides.end() &&
                !override_entry->second.empty();
            files.push_back({
                "entity_templates/" + name,
                use_override ? override_entry->second
                             : read_text_file(entry.string())});
        }
    }
    // Enumerated, not listed: the catalog discovers skeletons by scanning this
    // directory, so a hardcoded subset here would make the bundle and the
    // filesystem disagree on the asset set -- and the two are compared by
    // catalog hash further down.
    {
        std::vector<std::filesystem::path> generated;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(
                 "game_server/skeleton_assets/generated")) {
            if (entry.is_regular_file()) {
                generated.push_back(entry.path());
            }
        }
        std::sort(generated.begin(), generated.end());
        for (const std::filesystem::path& entry : generated) {
            files.push_back({
                "skeleton_assets/generated/" + entry.filename().string(),
                read_binary_string(entry.string())});
        }
    }

    const std::vector<std::string> weapon_files = {
        "beam_rifle.yaml",
        "fire_floor.yaml",
        "homing_missile.yaml",
        "rifle.yaml",
        "rocket.yaml",
        "shotgun.yaml",
        "grenade_launcher.yaml",
        "spammer.yaml",
    };
    for (const std::string& file : weapon_files) {
        files.push_back({
            "weapon_templates/" + file,
            read_text_file("game_server/weapon_templates/" + file)});
    }
    const std::vector<std::string> action_files = {
        "beam_rifle_fire.yaml",
        "fire_floor_cast.yaml",
        "homing_missile_fire.yaml",
        "rifle_fire.yaml",
        "rocket_fire.yaml",
        "shotgun_fire.yaml",
        "grenade_launcher_fire.yaml",
        "spammer_fire.yaml",
    };
    for (const std::string& file : action_files) {
        files.push_back({
            "action_templates/" + file,
            read_text_file("game_server/action_templates/" + file)});
    }
    const std::vector<std::string> action_graph_files = {
        "action_apply_damage_at_activated.yaml",
        "action_apply_damage_at_collision.yaml",
        "action_apply_damage_at_destroy_entity.yaml",
        "action_apply_damage_at_health_depleted.yaml",
        "action_apply_health_change_at_item_used.yaml",
        "action_spawn_entity_at_destroy_entity.yaml",
        "action_spawn_ice_and_damage_self_at_collision.yaml",
        "action_spawn_projectile_at_impact.yaml",
        "action_rocket_explosion_at_target.yaml",
    };
    for (const std::string& file : action_graph_files) {
        files.push_back({
            "action_graph_templates/" + file,
            read_text_file("game_server/action_graph_templates/" + file)});
    }
    const std::vector<std::string> item_files = {
        "activation_token.yaml",
        "fungible_potion.yaml",
        "grenade_consumable.yaml",
        "stateful_magic_bottle.yaml",
        "stateful_potion.yaml",
    };
    for (const std::string& file : item_files) {
        files.push_back({
            "item_templates/" + file,
            read_text_file("game_server/item_templates/" + file)});
    }
    const std::vector<std::string> projectile_files = {
        "beam_rifle_beam.yaml",
        "fire_floor_area.yaml",
        "homing_missile.yaml",
        "rocket.yaml",
        "rocket_explosion.yaml",
        "grenade_shell.yaml",
        "spammer.yaml",
    };
    for (const std::string& file : projectile_files) {
        files.push_back({
            "projectile_templates/" + file,
            read_text_file("game_server/projectile_templates/" + file)});
    }
    files.insert(files.end(), extra_files.begin(), extra_files.end());
    return make_store_zip(files);
}

std::vector<std::uint8_t> make_gameplay_bundle_zip() {
    return make_gameplay_bundle_zip(
        read_text_file("game_server/entity_templates/sentry_grunt.yaml"));
}

std::string replace_once(
    std::string text,
    const std::string& from,
    const std::string& to) {
    const std::size_t position = text.find(from);
    require(position != std::string::npos);
    text.replace(position, from.size(), to);
    return text;
}

std::vector<std::uint8_t> make_entity_template_bundle_zip(
    const std::string& sentry_template_yaml) {
    std::vector<std::pair<std::string, std::string>> files;
    files.push_back({
        "gameplay_catalog.yaml",
        "catalog_version: 8\n"
        "action_template_dir: action_templates\n"
        "action_graph_template_dir: action_graph_templates\n"
        "reload_action_template:\n"
        "  id: 4199\n"
        "  name: shared_reload\n"
        "  trigger_mode: press\n"
        "  flags: [cancel_on_death, cancel_on_weapon_change, cancel_before_first_commit]\n"
        "  ammo_cost_per_commit: 0\n"
        "  commit_offset_ticks: 30\n"
        "  commit_interval_ticks: 0\n"
        "  max_commit_count: 1\n"
        "  recovery_ticks: 0\n"
        "  hold_input_timeout_ticks: 0\n"
        "weapon_template_dir: weapon_templates\n"
        "projectile_template_dir: projectile_templates\n"
        "entity_template_dir: entity_templates\n"
        "collider_template_dir: collider_templates\n"
        "player:\n"
        "  entity_template: player\n"});
    append_collider_template_files(&files);
    files.push_back({
        "entity_templates/player.yaml",
        "id: 1\n"
        "name: player\n"
        "entity_type: actor\n"
        "actor_type: player\n"
        "camp: player_side\n"
        "collider_template: player_hit_aabb\n"
        "health:\n"
        "  hp: 1000\n"
        "  max_hp: 1000\n"
        "movement:\n"
        "  controller: character\n"
        "  move_speed_meters_per_second: 5.0\n"
        "  collider_template: player_movement_capsule\n"
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
        "grenade_launcher.yaml",
        "spammer.yaml",
    };
    for (const std::string& file : weapon_files) {
        files.push_back({
            "weapon_templates/" + file,
            read_text_file("game_server/weapon_templates/" + file)});
    }
    const std::vector<std::string> action_files = {
        "beam_rifle_fire.yaml",
        "fire_floor_cast.yaml",
        "homing_missile_fire.yaml",
        "rifle_fire.yaml",
        "rocket_fire.yaml",
        "shotgun_fire.yaml",
        "grenade_launcher_fire.yaml",
        "spammer_fire.yaml",
    };
    for (const std::string& file : action_files) {
        files.push_back({
            "action_templates/" + file,
            read_text_file("game_server/action_templates/" + file)});
    }
    files.push_back({
        "action_graph_templates/action_spawn_projectile_at_impact.yaml",
        read_text_file(
            "game_server/action_graph_templates/"
            "action_spawn_projectile_at_impact.yaml")});
    const std::vector<std::string> projectile_files = {
        "beam_rifle_beam.yaml",
        "fire_floor_area.yaml",
        "homing_missile.yaml",
        "rocket.yaml",
        "rocket_explosion.yaml",
        "grenade_shell.yaml",
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
        "collider_template: sentry_grunt_hit_aabb\n"
        "health:\n"
        "  hp: 500\n"
        "  max_hp: 500\n"
        "movement:\n"
        "  controller: grounded\n"
        "  move_speed_meters_per_second: 2.5\n"
        "  collider_template: sentry_grunt_movement_capsule\n"
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
        "  collider_template: sentry_grunt_vision_cone\n"
        "ai:\n"
        "  controller: sentry\n"
        "  profile: default\n";
}

}  // namespace

int main() {
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    assert(network_example::game_server::AgentSentryConfig{}.weapon_id ==
           UINT16_MAX);
    const std::vector<std::string> errors =
        network_example::game_server::validate_gameplay_config(config);
    assert(errors.empty());

    const std::string production_player_yaml =
        read_text_file("game_server/entity_templates/player.yaml");
    const std::string production_sentry_yaml =
        read_text_file("game_server/entity_templates/sentry_grunt.yaml");
    const std::string production_monster_yaml =
        read_text_file("game_server/entity_templates/monster_sim_actor.yaml");
    bool invalid_leg_hierarchy_rejected = false;
    try {
        const std::vector<std::uint8_t> invalid_leg_bundle =
            make_gameplay_bundle_zip(
                production_sentry_yaml,
                {},
                {},
                {},
                replace_once(
                    production_monster_yaml,
                    "foot_bone: JNT_LegFrontLeft_Foot",
                    "foot_bone: JNT_LegFrontLeft_Hip"));
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            invalid_leg_bundle.data(),
            static_cast<std::uint32_t>(invalid_leg_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const network_example::game_server::DataLoadError& error) {
        require(error.error_code ==
                KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_YAML);
        require(error.source_kind ==
                KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE);
        require(error.template_kind ==
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR);
        require(error.template_id == 20u);
        require(error.field == "skeleton");
        require(error.path.find("monster_sim_actor.yaml") != std::string::npos);
        require(std::string(error.what()).find("simplified_monster_sim_v4") !=
                std::string::npos);
        require(std::string(error.what()).find("JNT_LegFrontLeft_Hip") !=
                std::string::npos);
        require(std::string(error.what()).find("JNT_LegFrontLeft_Knee") !=
                std::string::npos);
        invalid_leg_hierarchy_rejected =
            std::string(error.what()).find("invalid two-bone hierarchy") !=
            std::string::npos;
    }
    require(invalid_leg_hierarchy_rejected);
    bool invalid_gait_threshold_rejected = false;
    try {
        const std::vector<std::uint8_t> invalid_gait_bundle =
            make_gameplay_bundle_zip(
                production_sentry_yaml,
                {},
                {},
                {},
                replace_once(
                    production_monster_yaml,
                    "step_threshold_meters: 1.95",
                    "step_threshold_meters: 0.0"));
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            invalid_gait_bundle.data(),
            static_cast<std::uint32_t>(invalid_gait_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        invalid_gait_threshold_rejected =
            std::string(error.what()).find("invalid locomotion gait values") !=
            std::string::npos;
    }
    require(invalid_gait_threshold_rejected);
    bool fixed_walk_rejected = false;
    try {
        const std::vector<std::uint8_t> fixed_walk_bundle =
            make_gameplay_bundle_zip(
                production_sentry_yaml,
                {},
                {},
                {},
                replace_once(
                    production_monster_yaml,
                    "type: displacement_threshold",
                    "type: fixed_walk"));
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            fixed_walk_bundle.data(),
            static_cast<std::uint32_t>(fixed_walk_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        fixed_walk_rejected =
            std::string(error.what()).find(
                "requires displacement_threshold parameters") !=
            std::string::npos;
    }
    require(fixed_walk_rejected);
    const auto rejects_monster_locomotion_yaml =
        [&](const std::string& monster_yaml, const std::string& expected_error) {
            try {
                const std::vector<std::uint8_t> bundle =
                    make_gameplay_bundle_zip(
                        production_sentry_yaml, {}, {}, {}, monster_yaml);
                (void)network_example::game_server::
                    load_gameplay_config_from_bundle_memory(
                        bundle.data(),
                        static_cast<std::uint32_t>(bundle.size()),
                        "gameplay_catalog.yaml");
                return false;
            } catch (const std::exception& error) {
                return std::string(error.what()).find(expected_error) !=
                    std::string::npos;
            }
        };
    require(rejects_monster_locomotion_yaml(
        replace_once(
            production_monster_yaml,
            "step_duration_ticks: 18",
            "step_duration_ticks: 0"),
        "invalid locomotion gait values"));
    require(rejects_monster_locomotion_yaml(
        replace_once(
            production_monster_yaml,
            "gait_group: 0",
            "gait_group: 4"),
        "gait_group is out of range"));
    require(rejects_monster_locomotion_yaml(
        replace_once(
            production_monster_yaml,
            "processing_order: [front_left, front_right, rear_left, rear_right]",
            "processing_order: [front_left, front_left, rear_left, rear_right]"),
        "processing_order repeats leg"));
    // movement.collision_mask names geometry, never a gameplay side. It shares a
    // key name with a projectile's collision_mask but not its vocabulary, so a
    // token borrowed from the other one has to fail rather than resolve to
    // whatever bit happens to share its name.
    require(rejects_monster_locomotion_yaml(
        replace_once(
            production_monster_yaml,
            "collision_mask: terrain | static_obstacle",
            "collision_mask: terrain | hostile_side"),
        "unsupported movement collision_mask"));
    // The old key is gone rather than quietly tolerated: leaving it accepted
    // would leave the capsule blocking players with nothing to show for it.
    require(rejects_monster_locomotion_yaml(
        replace_once(
            production_monster_yaml,
            "collision_mask: terrain | static_obstacle",
            "collides_with: terrain | static_obstacle"),
        "unknown field: collides_with"));
    require(rejects_monster_locomotion_yaml(
        replace_once(
            production_monster_yaml,
            "stance_crouch_meters: 4.0",
            "stance_crouch_meters: -1.0"),
        "invalid locomotion body follow values"));
    bool invalid_foothold_distance_rejected = false;
    try {
        const std::vector<std::uint8_t> invalid_foothold_bundle =
            make_gameplay_bundle_zip(
                production_sentry_yaml,
                {},
                {},
                {},
                replace_once(
                    production_monster_yaml,
                    "query_distance_meters: 90.0",
                    "query_distance_meters: 0.0"));
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            invalid_foothold_bundle.data(),
            static_cast<std::uint32_t>(invalid_foothold_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        invalid_foothold_distance_rejected =
            std::string(error.what()).find("invalid locomotion foothold") !=
            std::string::npos;
    }
    require(invalid_foothold_distance_rejected);
    const auto load_player_yaml = [&](const std::string& player_yaml) {
        const std::vector<std::uint8_t> bundle = make_gameplay_bundle_zip(
            production_sentry_yaml, {}, player_yaml);
        return network_example::game_server::load_gameplay_config_from_bundle_memory(
            bundle.data(),
            static_cast<std::uint32_t>(bundle.size()),
            "gameplay_catalog.yaml");
    };
    const auto rejects_player_yaml = [&](const std::string& player_yaml) {
        try {
            (void)load_player_yaml(player_yaml);
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    const network_example::game_server::GameServerGameplayConfig numeric_item_config =
        load_player_yaml(replace_once(
            production_player_yaml,
            "item_template: fungible_potion",
            "item_template: 3002"));
    assert(numeric_item_config.actor_templates[0]
               .inventory_slots[0]
               .item_template_id == 3002);
    assert(rejects_player_yaml(replace_once(
        production_player_yaml,
        "inventory_slot_capacity: 8\n",
        "")));
    assert(rejects_player_yaml(replace_once(
        production_player_yaml,
        "inventory_slots:\n"
        "  - item_template: fungible_potion\n"
        "    quantity: 5\n"
        "  - item_template: stateful_potion\n"
        "    quantity: 1\n"
        "  - item_template: stateful_magic_bottle\n"
        "    quantity: 1\n",
        "")));
    assert(rejects_player_yaml(replace_once(
        production_player_yaml,
        "inventory_slots:\n",
        "inventory_slots: {}\n")));
    assert(rejects_player_yaml(replace_once(
        production_player_yaml,
        "  - item_template: fungible_potion\n    quantity: 5\n",
        "  - null\n")));
    assert(rejects_player_yaml(replace_once(
        production_player_yaml,
        "    quantity: 5\n",
        "    quantity: 5\n    unsupported: true\n")));
    assert(rejects_player_yaml(replace_once(
        production_player_yaml,
        "    quantity: 5\n",
        "")));
    assert(rejects_player_yaml(replace_once(
        production_player_yaml,
        "item_template: fungible_potion",
        "item_template: missing_item")));
    assert(rejects_player_yaml(replace_once(
        production_player_yaml,
        "quantity: 5",
        "quantity: 0")));
    assert(rejects_player_yaml(replace_once(
        production_player_yaml,
        "quantity: 5",
        "quantity: 6")));
    assert(rejects_player_yaml(replace_once(
        production_player_yaml,
        "item_template: stateful_potion\n    quantity: 1",
        "item_template: stateful_potion\n    quantity: 2")));
    assert(rejects_player_yaml(replace_once(
        production_player_yaml,
        "inventory_slot_capacity: 8",
        "inventory_slot_capacity: 2")));
    const std::vector<std::uint8_t> agent_inventory_bundle =
        make_gameplay_bundle_zip(
            production_sentry_yaml +
                "\ninventory_slot_capacity: 1\n"
                "inventory_slots:\n"
                "  - item_template: fungible_potion\n"
                "    quantity: 1\n");
    bool agent_inventory_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            agent_inventory_bundle.data(),
            static_cast<std::uint32_t>(agent_inventory_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception&) {
        agent_inventory_rejected = true;
    }
    assert(agent_inventory_rejected);

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
        "    passive_patrol: true\n"
        "    patrol_extent_x_meters: 8.0\n"
        "    patrol_input_magnitude: 0.6\n"
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
    assert(data_driven_sentry.sentry.passive_patrol);
    assert(data_driven_sentry.sentry.patrol_extent_x_meters == 8.0f);
    assert(data_driven_sentry.sentry.patrol_input_magnitude == 0.6f);
    assert(data_driven_sentry.sentry.move_speed_meters_per_second == 2.5f);
    assert(data_driven_sentry.sentry.weapon_id == 2);
    assert(data_driven_sentry.sentry.animation_idle ==
           data_driven_sentry.animation_idle);
    assert(data_driven_sentry.sentry.animation_attack ==
           data_driven_sentry.animation_chasing);

    for (const std::pair<std::string, std::string>& invalid_patrol_value : {
             std::pair<std::string, std::string>{
                 "patrol_extent_x_meters: 8.0",
                 "patrol_extent_x_meters: 0.0"},
             std::pair<std::string, std::string>{
                 "patrol_input_magnitude: 0.6",
                 "patrol_input_magnitude: .nan"},
         }) {
        bool invalid_passive_patrol_rejected = false;
        try {
            const std::vector<std::uint8_t> invalid_passive_patrol_bundle =
                make_entity_template_bundle_zip(replace_once(
                    data_driven_sentry_yaml,
                    invalid_patrol_value.first,
                    invalid_patrol_value.second));
            (void)network_example::game_server::
                load_gameplay_config_from_bundle_memory(
                    invalid_passive_patrol_bundle.data(),
                    static_cast<std::uint32_t>(
                        invalid_passive_patrol_bundle.size()),
                    "gameplay_catalog.yaml");
        } catch (const std::exception&) {
            invalid_passive_patrol_rejected = true;
        }
        assert(invalid_passive_patrol_rejected);
    }

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
    assert(
        config.static_collision_scene.entry_path ==
        "mesh_assets/jolt/undulating.joltmesh");
    assert(config.static_collision_scene.scene_id == 1u);
    assert(config.static_collision_scene.collider_id == 0x80000001u);
    assert(
        config.static_collision_scene.collision_layer ==
        KERNEL_STATIC_COLLISION_LAYER_TERRAIN);
    require(config.weapons.catalog_version == 11);
    require(config.prop_population_rules.size() == 1u);
    require(config.prop_population_rules[0].name == "temporary_deployable");
    require(
        config.prop_population_rules[0].definition.population_group_id == 1u);
    require(config.prop_population_rules[0].definition.max_alive == 256u);
    const auto ice_block = std::find_if(
        config.entity_templates.begin(),
        config.entity_templates.end(),
        [](const network_example::game_server::EntityTemplateConfig& entity) {
            return entity.name == "ice_block";
        });
    require(ice_block != config.entity_templates.end());
    require(ice_block->prop.lifetime_ticks == 900u);
    require(ice_block->prop.population_group_id == 1u);
    const auto ice_block_collider = std::find_if(
        config.colliders.templates.begin(),
        config.colliders.templates.end(),
        [](const network_example::game_server::ColliderTemplateConfig& collider) {
            return collider.name == "ice_block_hitbox";
        });
    require(ice_block_collider != config.colliders.templates.end());
    require(ice_block->collider_template_id == 13u);
    require(
        ice_block_collider->definition.shape_type ==
        KernelColliderShapeType_OrientedBox);
    require(ice_block_collider->definition.center.y == 1.5f);
    require(ice_block_collider->definition.shape_params.x == 1.0f);
    require(ice_block_collider->definition.shape_params.y == 1.5f);
    require(ice_block_collider->definition.shape_params.z == 0.3f);
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
    changed_config = config;
    changed_config.static_collision_scene.scene_id += 1u;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(changed_config));
    changed_config = config;
    const auto changed_monster_sentry = std::find_if(
        changed_config.entity_templates.begin(),
        changed_config.entity_templates.end(),
        [](const network_example::game_server::EntityTemplateConfig& entity) {
            return entity.name == "monster_sim_actor";
        });
    require(changed_monster_sentry != changed_config.entity_templates.end());
    changed_monster_sentry->sentry.patrol_extent_x_meters += 1.0f;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(changed_config));
    changed_config = config;
    changed_config.agent.override_director_spawn = true;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(changed_config));
    changed_config = config;
    changed_config.prop_population_rules[0].definition.max_alive -= 1u;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(changed_config));
    changed_config = config;
    const auto changed_ice = std::find_if(
        changed_config.entity_templates.begin(),
        changed_config.entity_templates.end(),
        [](const network_example::game_server::EntityTemplateConfig& entity) {
            return entity.name == "ice_block";
        });
    require(changed_ice != changed_config.entity_templates.end());
    changed_ice->prop.lifetime_ticks += 1u;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(changed_config));
    require(!config.item_templates.empty());
    changed_config = config;
    changed_config.item_templates.front()
        .definition.throw_policy.trajectory_projectile_template_id += 1u;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(changed_config));
    changed_config = config;
    changed_config.item_templates.front()
        .definition.portable_state_fields[0]
        .uint32_default += 1u;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(changed_config));
    const KernelCombatStateDefinition player_combat_state =
        network_example::game_server::make_player_combat_state(config);
    assert(player_combat_state.hp == 1000);
    assert(player_combat_state.max_hp == 1000);
    assert(player_combat_state.move_speed_meters_per_second == 5.0f);
    assert(player_combat_state.collider_template_id == 1);

    // earth_mother.yaml populates the map with a legged rig, not the sentry
    // grunt; they carry the same combat block, so only the template id and the
    // spawn shape differ. Which rig is a deliberate choice in that file, so this
    // id tracks it: monster_sim_actor (20) until f280528 made tripod_actor (22)
    // the default test model.
    assert(config.agent.actor_template_id == 22);
    assert(config.agent.spawn_position.x == 0.0f);
    assert(config.agent.spawn_count == 1);
    assert(config.agent.spawn_radius == 10.0f);
    assert(config.agent.spawn_seed == 4242);
    const KernelCombatStateDefinition enemy_combat_state =
        network_example::game_server::make_agent_combat_state(config);
    assert(enemy_combat_state.hp == 500);
    assert(enemy_combat_state.max_hp == 500);
    assert(enemy_combat_state.collider_template_id == 2);
    assert(
        enemy_combat_state.active_weapon_slot == 0);
    assert(
        enemy_combat_state.weapon_ids[0] ==
        network_example::game_server::kWeaponSpammer);
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
    // Discovered by scanning skeleton_assets/generated, not by a list in the
    // catalog: the three base locomotion rigs are present here only because
    // their build targets are data deps of this test.
    require(config.skeleton_assets.size() == 5u);
    for (const std::uint32_t expected_asset_id : {1u, 2u, 3u, 4u, 5u}) {
        require(
            std::any_of(
                config.skeleton_assets.begin(),
                config.skeleton_assets.end(),
                [expected_asset_id](
                    const network_example::game_server::SkeletonAssetConfig&
                        asset) {
                    return asset.skeleton_asset_id == expected_asset_id;
                }));
    }
    const auto quadruped_asset_config = std::find_if(
        config.skeleton_assets.begin(),
        config.skeleton_assets.end(),
        [](const network_example::game_server::SkeletonAssetConfig& asset) {
            return asset.skeleton_asset_id == 1u;
        });
    const auto biped_asset_config = std::find_if(
        config.skeleton_assets.begin(),
        config.skeleton_assets.end(),
        [](const network_example::game_server::SkeletonAssetConfig& asset) {
            return asset.skeleton_asset_id == 2u;
        });
    require(quadruped_asset_config != config.skeleton_assets.end());
    require(quadruped_asset_config->bones.size() == 41u);
    require(biped_asset_config != config.skeleton_assets.end());
    require(biped_asset_config->bones.size() == 18u);
    const auto monster_template = std::find_if(
        config.entity_templates.begin(),
        config.entity_templates.end(),
        [](const network_example::game_server::EntityTemplateConfig& entity) {
            return entity.name == "monster_sim_actor";
        });
    require(monster_template != config.entity_templates.end());
    require(monster_template->skeleton.enabled);
    require(monster_template->sentry.passive_patrol);
    require(monster_template->sentry.patrol_extent_x_meters == 30.0f);
    require(monster_template->sentry.patrol_input_magnitude == 1.0f);
    require(monster_template->movement_controller_type ==
            KernelMovementControllerType_Character);
    require(monster_template->movement_collider_template_id == 14u);
    require(monster_template->skeleton.legs.size() == 4u);
    require(monster_template->skeleton.processing_order.size() == 4u);
    require(monster_template->movement_max_yaw_degrees_per_second == 45.0f);
    require(monster_template->skeleton.input_deadzone == 0.01f);
    require(monster_template->skeleton.step_threshold_meters == 1.95f);
    require(monster_template->skeleton.step_duration_ticks == 18u);
    require(monster_template->skeleton.max_swinging_legs == 2u);
    // Body height comes from the legs, seated 4 m below the bind pose, and the
    // movement capsule sweeps the static world only -- terrain and props -- so
    // the open space under the belly does not block the walk.
    require(monster_template->skeleton.body_follow_speed == 10.0f);
    require(monster_template->skeleton.slope_alignment == 0.0f);
    require(monster_template->skeleton.stance_crouch_meters == 4.0f);
    require(monster_template->movement_collision_mask ==
            (KERNEL_MOVEMENT_LAYER_TERRAIN |
             KERNEL_MOVEMENT_LAYER_STATIC_OBSTACLE));
    require(monster_template->skeleton.foothold_query_type ==
            KernelFootholdQueryType_Raycast);
    require(monster_template->skeleton.foothold_query_start_height_meters ==
            30.0f);
    require(monster_template->skeleton.foothold_query_distance_meters ==
            90.0f);
    require(monster_template->skeleton.foothold_candidate_offsets.size() == 5u);

    // Limb colliders: one per GEO_ bone the rig carries, and the count is a
    // property of the rig rather than of this template, so it is worth pinning.
    // monster_sim_actor authors none, which is what keeps this opt-in.
    require(monster_template->skeleton.limb_colliders.empty());
    const auto limb_collider_count =
        [&config](const std::string& template_name) -> std::size_t {
        const auto found = std::find_if(
            config.entity_templates.begin(),
            config.entity_templates.end(),
            [&template_name](
                const network_example::game_server::EntityTemplateConfig&
                    entity) { return entity.name == template_name; });
        require(found != config.entity_templates.end());
        return found->skeleton.limb_colliders.size();
    };
    require(limb_collider_count("biped_actor") == 12u);
    require(limb_collider_count("quadruped_actor") == 9u);
    require(limb_collider_count("tripod_actor") == 7u);

    const auto tripod_template = std::find_if(
        config.entity_templates.begin(),
        config.entity_templates.end(),
        [](const network_example::game_server::EntityTemplateConfig& entity) {
            return entity.name == "tripod_actor";
        });
    require(tripod_template != config.entity_templates.end());
    for (const network_example::game_server::LimbColliderConfig& limb :
         tripod_template->skeleton.limb_colliders) {
        require(
            (limb.purpose_flags & KernelColliderPurpose_Limb) != 0u);
        require((limb.purpose_flags & KernelColliderPurpose_Hit) != 0u);
        require(limb.layer_mask == KERNEL_COLLISION_LAYER_HOSTILE_SIDE);
        // Bone names resolve to real indices, never the 0 a failed lookup would
        // leave behind -- GEO_ bones are never the root.
        require(limb.bone_index != 0u);
        const bool is_body = limb.bone == "GEO_Body";
        // The body is the rig's one sphere; everything else is a box. A leg
        // limb names its leg, the body does not belong to one.
        require(
            limb.shape_type ==
            (is_body ? KernelColliderShapeType_Sphere
                     : KernelColliderShapeType_OrientedBox));
        require(
            (limb.leg_index == KERNEL_MAX_SKELETON_LEGS) == is_body);
    }
    // The biped's arms are limbs that belong to no leg, so leg membership must
    // be optional rather than implied by being a limb.
    const auto biped_template = std::find_if(
        config.entity_templates.begin(),
        config.entity_templates.end(),
        [](const network_example::game_server::EntityTemplateConfig& entity) {
            return entity.name == "biped_actor";
        });
    require(biped_template != config.entity_templates.end());
    require(
        std::count_if(
            biped_template->skeleton.limb_colliders.begin(),
            biped_template->skeleton.limb_colliders.end(),
            [](const network_example::game_server::LimbColliderConfig& limb) {
                return limb.leg_index == KERNEL_MAX_SKELETON_LEGS;
            }) == 8);

    require(catalog.definition.skeleton_asset_count == 5u);
    const auto quadruped_asset = std::find_if(
        catalog.skeleton_assets.begin(),
        catalog.skeleton_assets.end(),
        [](const KernelSkeletonAssetDefinition& asset) {
            return asset.skeleton_asset_id == 1u;
        });
    const auto biped_asset = std::find_if(
        catalog.skeleton_assets.begin(),
        catalog.skeleton_assets.end(),
        [](const KernelSkeletonAssetDefinition& asset) {
            return asset.skeleton_asset_id == 2u;
        });
    require(quadruped_asset != catalog.skeleton_assets.end());
    require(quadruped_asset->bone_count == 41u);
    require(biped_asset != catalog.skeleton_assets.end());
    require(biped_asset->bone_count == 18u);
    const auto monster_definition = std::find_if(
        catalog.entity_templates.begin(),
        catalog.entity_templates.end(),
        [](const KernelEntityTemplateDefinition& entity) {
            return entity.entity_template_id == 20u;
        });
    require(monster_definition != catalog.entity_templates.end());
    require(
        (monster_definition->component_flags &
         KERNEL_ENTITY_COMPONENT_SKELETON) != 0u);
    require(monster_definition->skeleton.leg_count == 4u);
    require(monster_definition->movement.max_yaw_degrees_per_second == 45.0f);
    require(monster_definition->skeleton.step_threshold_meters == 1.95f);
    require(monster_definition->skeleton.step_duration_ticks == 18u);
    require(monster_definition->skeleton.stance_crouch_meters == 4.0f);
    require(monster_definition->movement.movement_collision_mask ==
            (KERNEL_MOVEMENT_LAYER_TERRAIN |
             KERNEL_MOVEMENT_LAYER_STATIC_OBSTACLE));
    // Everything else keeps the engine default, which zero selects.
    const auto sentry_definition = std::find_if(
        catalog.entity_templates.begin(),
        catalog.entity_templates.end(),
        [](const KernelEntityTemplateDefinition& entity) {
            return entity.entity_template_id == 2u;
        });
    require(sentry_definition != catalog.entity_templates.end());
    require(sentry_definition->movement.movement_collision_mask == 0u);
    require(monster_definition->skeleton.legs[0].gait_group == 0u);
    require(monster_definition->skeleton.legs[1].gait_group == 0u);
    require(monster_definition->skeleton.legs[2].gait_group == 1u);
    require(monster_definition->skeleton.legs[3].gait_group == 1u);
    require(monster_definition->skeleton.foothold_query_start_height_meters ==
            30.0f);
    require(monster_definition->skeleton.foothold_query_distance_meters ==
            90.0f);
    require(monster_definition->skeleton.foothold_candidate_count == 5u);
    require(monster_definition->skeleton.foothold_candidate_offsets[1].x ==
            0.2f);

    network_example::RuntimeSkeletonAsset locomotion_asset;
    require(network_example::load_runtime_skeleton_asset(
        *quadruped_asset, &locomotion_asset));
    network_example::LocomotionState grounded_locomotion;
    require(network_example::initialize_locomotion_state(
        monster_definition->skeleton, 0.0f, &grounded_locomotion));
    require(network_example::advance_locomotion_state(
        monster_definition->skeleton,
        KernelVec2{0.0f, 1.0f},
        monster_definition->movement.max_yaw_degrees_per_second,
        1.0f / 30.0f,
        &grounded_locomotion));
    // Ground every foothold ray at the root plane. Keyed off the ray origin
    // rather than a call counter so the expectations do not depend on how many
    // candidates the fan-out ends up trying.
    std::vector<glm::vec3> grounding_origins;
    const network_example::LocomotionGroundingQuery ordered_grounding =
        [&grounding_origins](
            const glm::vec3& origin,
            float,
            network_example::LocomotionGroundingHit* hit) {
            grounding_origins.push_back(origin);
            hit->position = glm::vec3{origin.x, 0.0f, origin.z};
            hit->normal = glm::vec3{0.0f, 1.0f, 0.0f};
            hit->supporting_entity_net_id = 77u;
            hit->supporting_collider_id = 9u;
            return true;
        };
    require(network_example::solve_legged_locomotion_pose(
        locomotion_asset.skeleton,
        locomotion_asset.bind_pose,
        monster_definition->skeleton,
        glm::vec3{0.0f},
        monster_definition->movement.max_slope_degrees,
        1.0f / 30.0f,
        ordered_grounding,
        &grounded_locomotion));
    // Each leg casts at least one ray and at most one per authored candidate:
    // the fan-out stops at the first walkable hit it can actually reach, and
    // this rig's near-straight bind stance leaves some legs needing a candidate
    // past the first.
    require(grounding_origins.size() >= monster_definition->skeleton.leg_count);
    require(grounding_origins.size() <=
            monster_definition->skeleton.leg_count *
                monster_definition->skeleton.foothold_candidate_count);
    require(grounded_locomotion.pose_valid);
    require(grounded_locomotion.local_pose.size() == 41u);
    std::vector<glm::vec3> initial_grounded_anchors;
    for (const network_example::LegLocomotionState& leg :
         grounded_locomotion.legs) {
        require(leg.ground_hit_valid);
        require(leg.grounding_candidate_index <
                monster_definition->skeleton.foothold_candidate_count);
        // A reachable foothold was found, so nothing needed clamping.
        require(!leg.ik_reach_clamped);
        require(leg.supporting_entity_net_id == 77u);
        require(leg.supporting_collider_id == 9u);
        require(leg.foot_initialized);
        require(leg.gait_state == network_example::LegGaitState::kSupport);
        initial_grounded_anchors.push_back(leg.foot_target_world);
    }
    const std::size_t rays_after_first_solve = grounding_origins.size();
    // A root move smaller than the authored step_threshold keeps every foot
    // planted where it is. Derived from the threshold rather than fixed, so
    // retuning the gait cannot silently turn this into a stepping test.
    require(network_example::advance_locomotion_state(
        monster_definition->skeleton,
        KernelVec2{0.0f, 1.0f},
        monster_definition->movement.max_yaw_degrees_per_second,
        1.0f / 30.0f,
        &grounded_locomotion));
    require(network_example::solve_legged_locomotion_pose(
        locomotion_asset.skeleton,
        locomotion_asset.bind_pose,
        monster_definition->skeleton,
        glm::vec3{
            monster_definition->skeleton.step_threshold_meters * 0.5f,
            0.0f,
            0.0f},
        monster_definition->movement.max_slope_degrees,
        1.0f / 30.0f,
        ordered_grounding,
        &grounded_locomotion));
    // The second solve re-sampled every leg's home rather than reusing the old
    // hit, so more rays went out.
    require(grounding_origins.size() > rays_after_first_solve);
    for (std::size_t index = 0u;
         index < grounded_locomotion.legs.size();
         ++index) {
        const network_example::LegLocomotionState& leg =
            grounded_locomotion.legs[index];
        require(leg.gait_state == network_example::LegGaitState::kSupport);
        require(!leg.entered_swing);
        require(glm::length(
            leg.foot_target_world - initial_grounded_anchors[index]) <
            0.0001f);
    }

    const network_example::LocomotionGroundingQuery flat_grounding =
        [](const glm::vec3& origin, float,
           network_example::LocomotionGroundingHit* hit) {
            hit->position = origin - glm::vec3{0.0f, 2.0f, 0.0f};
            hit->normal = glm::vec3{0.0f, 1.0f, 0.0f};
            return true;
        };

    // A leg with no foothold under it never initializes and keeps the bind pose
    // for that limb (the solve is decoupled from any body grounded/landed flag).
    network_example::LocomotionState ungrounded_locomotion;
    require(network_example::initialize_locomotion_state(
        monster_definition->skeleton, 0.0f, &ungrounded_locomotion));
    require(network_example::advance_locomotion_state(
        monster_definition->skeleton,
        KernelVec2{0.0f, 1.0f},
        monster_definition->movement.max_yaw_degrees_per_second,
        1.0f / 30.0f,
        &ungrounded_locomotion));
    require(network_example::solve_legged_locomotion_pose(
        locomotion_asset.skeleton,
        locomotion_asset.bind_pose,
        monster_definition->skeleton,
        glm::vec3{3.0f, 8.0f, 0.0f},
        monster_definition->movement.max_slope_degrees,
        1.0f / 30.0f,
        [](const glm::vec3&, float,
           network_example::LocomotionGroundingHit*) { return false; },
        &ungrounded_locomotion));
    require(ungrounded_locomotion.pose_valid);
    for (const network_example::LegLocomotionState& leg :
         ungrounded_locomotion.legs) {
        require(leg.gait_state == network_example::LegGaitState::kSupport);
        require(!leg.ground_hit_valid);
        require(!leg.foot_initialized);
        require(!leg.entered_swing);
    }
    // Once a foothold appears the foot seeds to it and holds.
    require(network_example::advance_locomotion_state(
        monster_definition->skeleton,
        KernelVec2{0.0f, 1.0f},
        monster_definition->movement.max_yaw_degrees_per_second,
        1.0f / 30.0f,
        &ungrounded_locomotion));
    require(network_example::solve_legged_locomotion_pose(
        locomotion_asset.skeleton,
        locomotion_asset.bind_pose,
        monster_definition->skeleton,
        glm::vec3{3.0f, 0.0f, 0.0f},
        monster_definition->movement.max_slope_degrees,
        1.0f / 30.0f,
        flat_grounding,
        &ungrounded_locomotion));
    for (const network_example::LegLocomotionState& leg :
         ungrounded_locomotion.legs) {
        require(leg.foot_initialized);
        require(leg.gait_state == network_example::LegGaitState::kSupport);
        require(!leg.entered_swing);
    }

    network_example::LocomotionState idle_locomotion;
    require(network_example::initialize_locomotion_state(
        monster_definition->skeleton, 0.0f, &idle_locomotion));
    require(network_example::advance_locomotion_state(
        monster_definition->skeleton,
        KernelVec2{},
        monster_definition->movement.max_yaw_degrees_per_second,
        1.0f / 30.0f,
        &idle_locomotion));
    require(network_example::solve_legged_locomotion_pose(
        locomotion_asset.skeleton,
        locomotion_asset.bind_pose,
        monster_definition->skeleton,
        glm::vec3{0.0f},
        monster_definition->movement.max_slope_degrees,
        1.0f / 30.0f,
        flat_grounding,
        &idle_locomotion));
    std::vector<glm::vec3> idle_anchors;
    for (const network_example::LegLocomotionState& leg :
         idle_locomotion.legs) {
        idle_anchors.push_back(leg.foot_target_world);
    }
    // With no movement input the actor is idle: even though the (physics) body
    // could be nudged, the legs never step and every foot stays planted.
    for (std::uint32_t tick = 0u; tick < 300u; ++tick) {
        require(network_example::advance_locomotion_state(
            monster_definition->skeleton,
            KernelVec2{},
            monster_definition->movement.max_yaw_degrees_per_second,
            1.0f / 30.0f,
            &idle_locomotion));
        require(!idle_locomotion.locomotion_active);
        require(network_example::solve_legged_locomotion_pose(
            locomotion_asset.skeleton,
            locomotion_asset.bind_pose,
            monster_definition->skeleton,
            glm::vec3{2.0f, 0.0f, 0.0f},
            monster_definition->movement.max_slope_degrees,
            1.0f / 30.0f,
            flat_grounding,
            &idle_locomotion));
        for (std::size_t index = 0u;
             index < idle_locomotion.legs.size();
             ++index) {
            const network_example::LegLocomotionState& leg =
                idle_locomotion.legs[index];
            require(leg.gait_state ==
                    network_example::LegGaitState::kSupport);
            require(!leg.entered_swing);
            require(glm::length(
                leg.foot_target_world - idle_anchors[index]) < 0.0001f);
        }
    }

    require(network_example::advance_locomotion_state(
        monster_definition->skeleton,
        KernelVec2{0.0f, 1.0f},
        monster_definition->movement.max_yaw_degrees_per_second,
        1.0f / 30.0f,
        &idle_locomotion));
    require(network_example::solve_legged_locomotion_pose(
        locomotion_asset.skeleton,
        locomotion_asset.bind_pose,
        monster_definition->skeleton,
        glm::vec3{6.0f, 0.0f, 0.0f},
        monster_definition->movement.max_slope_degrees,
        1.0f / 30.0f,
        flat_grounding,
        &idle_locomotion));
    std::uint32_t group_zero_swing_count = 0u;
    for (const network_example::LegLocomotionState& leg :
         idle_locomotion.legs) {
        if (leg.gait_group == 0u) {
            require(leg.gait_state == network_example::LegGaitState::kSwing);
            require(leg.entered_swing);
            ++group_zero_swing_count;
        } else {
            require(leg.gait_state == network_example::LegGaitState::kSupport);
        }
    }
    require(group_zero_swing_count == 2u);
    // The landing spot is committed once, at lift-off, and held for the rest of
    // the swing -- a step is meant to be replayable from the single event that
    // started it. Here the body jumps 4 m on the trigger tick and then crawls at
    // 0.1 m/tick, so the extrapolation that normally aims a step ahead of the
    // body lands out of the leg's reach and is rejected: this pins the frozen
    // fallback, which is the stance under the body at lift-off.
    std::vector<glm::vec3> committed_landing_targets;
    for (const network_example::LegLocomotionState& leg :
         idle_locomotion.legs) {
        committed_landing_targets.push_back(leg.landing_target_world);
    }
    // Derived from the authored gait rather than hard-coded: a step spans
    // step_duration_ticks ticks counting the one that triggered it, so the foot
    // lands on the last of them.
    const std::uint32_t monster_swing_ticks =
        monster_definition->skeleton.step_duration_ticks;
    for (std::uint32_t swing_tick = 1u;
         swing_tick < monster_swing_ticks;
         ++swing_tick) {
        require(network_example::advance_locomotion_state(
            monster_definition->skeleton,
            KernelVec2{0.0f, 1.0f},
            monster_definition->movement.max_yaw_degrees_per_second,
            1.0f / 30.0f,
            &idle_locomotion));
        require(network_example::solve_legged_locomotion_pose(
            locomotion_asset.skeleton,
            locomotion_asset.bind_pose,
            monster_definition->skeleton,
            glm::vec3{6.0f + 0.1f * static_cast<float>(swing_tick),
                      0.0f,
                      0.0f},
            monster_definition->movement.max_slope_degrees,
            1.0f / 30.0f,
            flat_grounding,
            &idle_locomotion));
        for (std::size_t index = 0u;
             index < idle_locomotion.legs.size();
             ++index) {
            const network_example::LegLocomotionState& leg =
                idle_locomotion.legs[index];
            if (leg.gait_group != 0u) {
                require(leg.gait_state ==
                        network_example::LegGaitState::kSupport);
                continue;
            }
            require(leg.landing_target_world ==
                    committed_landing_targets[index]);
            if (swing_tick < monster_swing_ticks - 1u) {
                require(leg.gait_state ==
                        network_example::LegGaitState::kSwing);
            } else {
                require(leg.gait_state ==
                        network_example::LegGaitState::kSupport);
                require(leg.entered_support);
                require(glm::length(
                    leg.foot_target_world - leg.landing_target_world) <
                    0.0001f);
            }
        }
    }

    network_example::LocomotionState rotating_locomotion;
    require(network_example::initialize_locomotion_state(
        monster_definition->skeleton, 0.0f, &rotating_locomotion));
    require(network_example::advance_locomotion_state(
        monster_definition->skeleton,
        KernelVec2{},
        90.0f,
        1.0f,
        &rotating_locomotion));
    require(network_example::solve_legged_locomotion_pose(
        locomotion_asset.skeleton,
        locomotion_asset.bind_pose,
        monster_definition->skeleton,
        glm::vec3{0.0f},
        monster_definition->movement.max_slope_degrees,
        1.0f / 30.0f,
        flat_grounding,
        &rotating_locomotion));
    // Turning in place changes the heading, which sweeps every foot's home
    // stance around the body. Even with the root stationary, that drift past the
    // step threshold makes the legs re-step (requirement: legs react to movement
    // OR yaw change).
    require(network_example::advance_locomotion_state(
        monster_definition->skeleton,
        KernelVec2{1.0f, 0.0f},
        90.0f,
        1.0f,
        &rotating_locomotion));
    require(rotating_locomotion.locomotion_active);
    require(network_example::solve_legged_locomotion_pose(
        locomotion_asset.skeleton,
        locomotion_asset.bind_pose,
        monster_definition->skeleton,
        glm::vec3{0.0f},
        monster_definition->movement.max_slope_degrees,
        1.0f / 30.0f,
        flat_grounding,
        &rotating_locomotion));
    require(std::any_of(
        rotating_locomotion.legs.begin(),
        rotating_locomotion.legs.end(),
        [](const network_example::LegLocomotionState& leg) {
            return leg.entered_swing;
        }));

    network_example::LocomotionState missed_locomotion;
    require(network_example::initialize_locomotion_state(
        monster_definition->skeleton, 0.0f, &missed_locomotion));
    require(network_example::advance_locomotion_state(
        monster_definition->skeleton,
        KernelVec2{},
        monster_definition->movement.max_yaw_degrees_per_second,
        1.0f / 30.0f,
        &missed_locomotion));
    require(network_example::solve_legged_locomotion_pose(
        locomotion_asset.skeleton,
        locomotion_asset.bind_pose,
        monster_definition->skeleton,
        glm::vec3{0.0f},
        monster_definition->movement.max_slope_degrees,
        1.0f / 30.0f,
        [](const glm::vec3&, float,
           network_example::LocomotionGroundingHit*) { return false; },
        &missed_locomotion));
    for (const network_example::LegLocomotionState& leg :
         missed_locomotion.legs) {
        require(!leg.ground_hit_valid);
        require(!leg.foot_initialized);
        require(!leg.ik_reach_clamped);
    }
    require(network_example::advance_locomotion_state(
        monster_definition->skeleton,
        KernelVec2{},
        monster_definition->movement.max_yaw_degrees_per_second,
        1.0f / 30.0f,
        &missed_locomotion));
    require(network_example::solve_legged_locomotion_pose(
        locomotion_asset.skeleton,
        locomotion_asset.bind_pose,
        monster_definition->skeleton,
        glm::vec3{20.0f, 0.0f, 0.0f},
        monster_definition->movement.max_slope_degrees,
        1.0f / 30.0f,
        [](const glm::vec3&, float,
           network_example::LocomotionGroundingHit*) { return false; },
        &missed_locomotion));
    for (const network_example::LegLocomotionState& leg :
         missed_locomotion.legs) {
        require(leg.gait_state == network_example::LegGaitState::kSupport);
        require(!leg.foot_initialized);
        require(!leg.ik_reach_clamped);
    }
    // First frame a foothold appears the foot seeds to it and holds.
    require(network_example::advance_locomotion_state(
        monster_definition->skeleton,
        KernelVec2{0.0f, 1.0f},
        monster_definition->movement.max_yaw_degrees_per_second,
        1.0f / 30.0f,
        &missed_locomotion));
    require(network_example::solve_legged_locomotion_pose(
        locomotion_asset.skeleton,
        locomotion_asset.bind_pose,
        monster_definition->skeleton,
        glm::vec3{20.0f, 0.0f, 0.0f},
        monster_definition->movement.max_slope_degrees,
        1.0f / 30.0f,
        flat_grounding,
        &missed_locomotion));
    for (const network_example::LegLocomotionState& leg :
         missed_locomotion.legs) {
        require(leg.gait_state == network_example::LegGaitState::kSupport);
        require(leg.foot_initialized);
        require(!leg.entered_swing);
    }
    // Once the body drives the home past the step threshold, a leg swings.
    require(network_example::advance_locomotion_state(
        monster_definition->skeleton,
        KernelVec2{0.0f, 1.0f},
        monster_definition->movement.max_yaw_degrees_per_second,
        1.0f / 30.0f,
        &missed_locomotion));
    require(network_example::solve_legged_locomotion_pose(
        locomotion_asset.skeleton,
        locomotion_asset.bind_pose,
        monster_definition->skeleton,
        glm::vec3{26.0f, 0.0f, 0.0f},
        monster_definition->movement.max_slope_degrees,
        1.0f / 30.0f,
        flat_grounding,
        &missed_locomotion));
    require(std::any_of(
        missed_locomotion.legs.begin(),
        missed_locomotion.legs.end(),
        [](const network_example::LegLocomotionState& leg) {
            return leg.entered_swing;
        }));

    network_example::LocomotionState clamped_locomotion;
    require(network_example::initialize_locomotion_state(
        monster_definition->skeleton, 0.0f, &clamped_locomotion));
    require(network_example::advance_locomotion_state(
        monster_definition->skeleton,
        KernelVec2{},
        monster_definition->movement.max_yaw_degrees_per_second,
        1.0f / 30.0f,
        &clamped_locomotion));
    require(network_example::solve_legged_locomotion_pose(
        locomotion_asset.skeleton,
        locomotion_asset.bind_pose,
        monster_definition->skeleton,
        glm::vec3{0.0f},
        monster_definition->movement.max_slope_degrees,
        1.0f / 30.0f,
        [](const glm::vec3& origin, float,
           network_example::LocomotionGroundingHit* hit) {
            hit->position = origin + glm::vec3{100.0f, 0.0f, 0.0f};
            hit->normal = glm::vec3{0.0f, 1.0f, 0.0f};
            return true;
        },
        &clamped_locomotion));
    // The foothold is grounded but far out of reach: the foot seeds to it and
    // the IK stage clamps the target to the leg's reach, keeping a finite pose.
    for (const network_example::LegLocomotionState& leg :
         clamped_locomotion.legs) {
        require(leg.ground_hit_valid);
        require(leg.foot_initialized);
        require(leg.ik_reach_clamped);
    }
    for (const KernelBoneLocalTransform& transform :
         clamped_locomotion.local_pose) {
        require(std::isfinite(transform.local_position.x));
        require(std::isfinite(transform.local_position.y));
        require(std::isfinite(transform.local_position.z));
        require(std::isfinite(transform.local_rotation.x));
        require(std::isfinite(transform.local_rotation.y));
        require(std::isfinite(transform.local_rotation.z));
        require(std::isfinite(transform.local_rotation.w));
        require(std::isfinite(transform.local_scale.x));
        require(std::isfinite(transform.local_scale.y));
        require(std::isfinite(transform.local_scale.z));
    }

    const auto replay_locomotion = [&]() {
        network_example::LocomotionState state;
        require(network_example::initialize_locomotion_state(
            monster_definition->skeleton, 0.0f, &state));
        glm::vec3 root_position{0.0f};
        std::vector<std::uint64_t> hashes;
        hashes.reserve(90u);
        for (std::uint32_t tick = 0u; tick < 90u; ++tick) {
            const KernelVec2 input = tick < 30u
                ? KernelVec2{0.0f, 1.0f}
                : tick < 60u ? KernelVec2{1.0f, 0.0f} : KernelVec2{};
            const glm::vec3 root_velocity{
                input.x * 2.0f,
                0.0f,
                input.y * 2.0f,
            };
            root_position += root_velocity * (1.0f / 30.0f);
            require(network_example::advance_locomotion_state(
                monster_definition->skeleton,
                input,
                monster_definition->movement.max_yaw_degrees_per_second,
                1.0f / 30.0f,
                &state));
            require(network_example::solve_legged_locomotion_pose(
                locomotion_asset.skeleton,
                locomotion_asset.bind_pose,
                monster_definition->skeleton,
                root_position,
                monster_definition->movement.max_slope_degrees,
                1.0f / 30.0f,
                [](const glm::vec3& origin, float,
                   network_example::LocomotionGroundingHit* hit) {
                    hit->position = glm::vec3{origin.x, 0.0f, origin.z};
                    hit->normal = glm::vec3{0.0f, 1.0f, 0.0f};
                    hit->supporting_entity_net_id = 77u;
                    hit->supporting_collider_id = 9u;
                    return true;
                },
                &state));
            hashes.push_back(quantized_locomotion_hash(
                state, root_position, root_velocity));
        }
        return hashes;
    };
    const std::vector<std::uint64_t> first_replay = replay_locomotion();
    const std::vector<std::uint64_t> second_replay = replay_locomotion();
    require(first_replay == second_replay);
    require(first_replay.front() != first_replay.back());
    const auto magic_bottle = std::find_if(
        catalog.entity_templates.begin(),
        catalog.entity_templates.end(),
        [](const KernelEntityTemplateDefinition& entity) {
            return entity.entity_template_id == 203u;
        });
    require(magic_bottle != catalog.entity_templates.end());
    require(magic_bottle->collision_trigger.action_count == 2u);
    require(
        magic_bottle->collision_trigger.actions[0].action_type ==
        KernelEntityTriggerActionType_SpawnEntity);
    require(
        magic_bottle->collision_trigger.actions[0].direction_source ==
        KernelEventVec3Source_Direction);
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
    assert(rifle.reserve_magazines == 6);
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
    const KernelWeaponMechanicsDefinition& grenade_launcher =
        config.weapons.definitions[network_example::game_server::kWeaponGrenade];
    assert(grenade_launcher.fire_mode == KernelWeaponFireMode_Projectile);
    assert(grenade_launcher.damage == 0);
    assert(grenade_launcher.magazine_size == 6);
    assert(grenade_launcher.projectile_template_id == 7);
    assert(config.weapons.collider_template_ids
               [network_example::game_server::kWeaponGrenade] == 7);
    assert(config.weapons.names[network_example::game_server::kWeaponGrenade] ==
           "Grenade Launcher");
    assert(
        network_example::game_server::active_weapon_id(*config_enemy_template) ==
        network_example::game_server::kWeaponSpammer);
    assert(config_enemy_template->sentry.weapon_id ==
           network_example::game_server::kWeaponSpammer);
    const KernelWeaponMechanicsDefinition& projectile_spammer =
        config.weapons.definitions[network_example::game_server::kWeaponSpammer];
    assert(projectile_spammer.damage == 1);
    assert(projectile_spammer.magazine_size == 120);
    assert(projectile_spammer.reserve_magazines == kMaxReserveMagazines);
    assert(projectile_spammer.projectile_template_id == 2);
    assert(config.weapons.names[network_example::game_server::kWeaponSpammer] ==
           "Projectile Spammer");
    assert(config_enemy_template->sentry.alert_ticks == 90);
    assert(config_enemy_template->sentry.forget_ticks == 150);
    assert(config_enemy_template->sentry.patrol_rotation_interval_ticks == 30);
    assert(config_enemy_template->sentry.patrol_rotation_min_degrees == 15.0f);
    assert(config_enemy_template->sentry.patrol_rotation_max_degrees == 30.0f);
    assert(
        config.weapons
            .projectile_sync_modes[network_example::game_server::kWeaponRocket] ==
        KernelProjectileSyncMode_ServerSnapshotOnly);
    assert(config.colliders.templates.size() == 14);
    assert(config.colliders.bindings.empty());
    assert(config.actor_templates.size() == 6);
    const network_example::game_server::ActorTemplateConfig& player_template =
        config.actor_templates[0];
    assert(player_template.actor_template_id == 1);
    assert(player_template.name == "player");
    assert(player_template.entity_type == network_example::game_server::kEntityTypeActor);
    assert(player_template.actor_type == network_example::game_server::kActorTypePlayer);
    assert(player_template.collider_template_id == 1);
    assert(player_template.weapon_slot_count == 3);
    assert(player_template.weapon_ids[0] == network_example::game_server::kWeaponRocket);
    assert(player_template.weapon_ids[1] == network_example::game_server::kWeaponShotgun);
    assert(player_template.weapon_ids[2] == network_example::game_server::kWeaponGrenade);
    assert(player_template.active_weapon_slot == 0);
    assert(player_template.inventory_slot_capacity == 8);
    assert(player_template.inventory_slots.size() == 3);
    assert(player_template.inventory_slots[0].item_template_id == 3002);
    assert(player_template.inventory_slots[0].quantity == 5);
    assert(player_template.inventory_slots[1].item_template_id == 3003);
    assert(player_template.inventory_slots[1].quantity == 1);
    assert(player_template.inventory_slots[2].item_template_id == 3004);
    assert(player_template.inventory_slots[2].quantity == 1);
    assert(player_template.vision.camp == KernelAgentCamp_PlayerSide);
    assert(player_template.vision.vision_collider_template_id == 0);
    assert(player_template.movement_controller_type ==
           KernelMovementControllerType_Character);
    assert(player_template.movement_collider_template_id == 10);
    const network_example::game_server::ActorTemplateConfig& enemy_template =
        config.actor_templates[1];
    assert(enemy_template.actor_template_id == 2);
    assert(enemy_template.name == "sentry_grunt");
    assert(enemy_template.entity_type == network_example::game_server::kEntityTypeActor);
    assert(enemy_template.actor_type == network_example::game_server::kActorTypeAgent);
    assert(enemy_template.collider_template_id == 2);
    assert(enemy_template.weapon_slot_count == 1);
    assert(enemy_template.weapon_ids[0] == network_example::game_server::kWeaponSpammer);
    assert(enemy_template.movement_controller_type ==
           KernelMovementControllerType_Grounded);
    assert(enemy_template.movement_collider_template_id == 11);

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
    bool found_player_movement_capsule = false;
    bool found_sentry_grunt_movement_capsule = false;
    for (const network_example::game_server::ColliderTemplateConfig& collider :
         config.colliders.templates) {
        if (collider.definition.template_id == 9) {
            found_vision_collider = true;
            assert(collider.name == "sentry_grunt_vision_cone");
            assert(collider.definition.shape_type == KernelColliderShapeType_Cone);
            assert(collider.definition.purpose_flags == KernelColliderPurpose_Vision);
            assert(collider.definition.layer_mask == KERNEL_COLLISION_LAYER_AGENT_VISION);
            assert(collider.definition.shape_params.x == 12.0f);
            assert(collider.definition.shape_params.y == 90.0f);
        } else if (collider.definition.template_id == 10) {
            found_player_movement_capsule = true;
            assert(collider.definition.shape_type ==
                   KernelColliderShapeType_Capsule);
            assert(collider.definition.purpose_flags ==
                   KernelColliderPurpose_Movement);
            assert(collider.definition.shape_params.x == 0.55f);
            assert(collider.definition.shape_params.y == 0.35f);
        } else if (collider.definition.template_id == 11) {
            found_sentry_grunt_movement_capsule = true;
            assert(collider.definition.shape_type ==
                   KernelColliderShapeType_Capsule);
            assert(collider.definition.purpose_flags ==
                   KernelColliderPurpose_Movement);
        }
    }
    assert(found_vision_collider);
    assert(found_player_movement_capsule);
    assert(found_sentry_grunt_movement_capsule);

    const KernelWeaponMechanicsDefinition& homing_missile =
        config.weapons.definitions[network_example::game_server::kWeaponHomingMissile];
    assert(homing_missile.projectile_template_id == 6);
    assert(config.weapons.collider_template_ids
               [network_example::game_server::kWeaponHomingMissile] == 7);
    assert(config.projectile_templates.size() == 7);
    bool found_homing_projectile = false;
    bool found_rocket_projectile = false;
    bool found_rocket_explosion = false;
    bool found_grenade_shell = false;
    bool found_spammer_projectile = false;
    bool found_fire_floor_area = false;
    bool found_beam_rifle_beam = false;
    for (const network_example::game_server::ProjectileTemplateConfig& projectile :
         config.projectile_templates) {
        if (projectile.name == "grenade_shell_projectile") {
            found_grenade_shell = true;
            assert(projectile.definition.mechanics.collider_template_id == 7);
            assert(projectile.definition.mechanics.damage == 0);
            assert(projectile.definition.mechanics.damage_shape ==
                   KernelProjectileDamageShape_None);
            assert(projectile.definition.mechanics
                       .projectile_impact_trigger.action_type ==
                   KernelEntityTriggerActionType_SpawnProjectile);
            assert(projectile.definition.mechanics.projectile_impact_trigger
                       .spawn_projectile_template_id == 8);
        }
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
            assert(projectile.definition.mechanics.damage == 45);
            assert(projectile.definition.mechanics
                       .projectile_impact_trigger.action_type ==
                   KernelEntityTriggerActionType_SpawnProjectile);
            assert(projectile.definition.mechanics.projectile_impact_trigger
                       .spawn_projectile_template_id == 8);
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
            assert(projectile.definition.mechanics.area_effect.collision_mask &
                   KERNEL_COLLISION_MASK_PROP);
            assert(projectile.definition.mechanics.projectile_impact_trigger.action_count ==
                   2u);
            assert(projectile.definition.mechanics.projectile_impact_trigger.actions[1].action_type ==
                   KernelEntityTriggerActionType_ApplyImpulse);
            assert(projectile.definition.mechanics.projectile_impact_trigger.actions[1].impulse_strength ==
                   12.0f);
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
    assert(found_grenade_shell);
    assert(found_spammer_projectile);
    assert(found_rocket_projectile);
    assert(found_rocket_explosion);
    assert(found_homing_projectile);
    assert(found_fire_floor_area);
    assert(found_beam_rifle_beam);

    const auto player_entity_template = std::find_if(
        config.entity_templates.begin(),
        config.entity_templates.end(),
        [](const auto& entity) { return entity.name == "player"; });
    require(player_entity_template != config.entity_templates.end());
    require(player_entity_template->impulse_resistance == 0.0f);

    const std::string impulse_graph =
        "id: action_apply_impulse_test\n"
        "parameters:\n"
        "  target: null\n"
        "  strength: 4.5\n"
        "  direction: {x: 1.0, y: 0.0, z: 0.0}\n"
        "actions:\n"
        "  - type: apply_impulse\n"
        "    target: params.target\n"
        "    strength: params.strength\n"
        "    direction: params.direction\n";
    const std::string impulse_prop =
        "id: 302\n"
        "name: impulse_prop\n"
        "entity_type: prop\n"
        "impulse_resistance: 9.0\n"
        "health:\n"
        "  hp: 3\n"
        "  max_hp: 3\n"
        "physics:\n"
        "  collider_template: rocket_aabb\n"
        "triggers:\n"
        "  on_collision:\n"
        "    action_graph: action_apply_impulse_test\n"
        "    parameters:\n"
        "      target: event.target\n"
        "      strength: 4.5\n";
    const auto impulse_bundle = make_gameplay_bundle_zip(
        read_text_file("game_server/entity_templates/sentry_grunt.yaml"),
        {{"action_graph_templates/action_apply_impulse_test.yaml", impulse_graph},
         {"entity_templates/impulse_prop.yaml", impulse_prop}});
    const auto impulse_config =
        network_example::game_server::load_gameplay_config_from_bundle_memory(
            impulse_bundle.data(),
            static_cast<std::uint32_t>(impulse_bundle.size()),
            "gameplay_catalog.yaml");
    const auto impulse_catalog =
        network_example::game_server::build_kernel_gameplay_catalog(impulse_config);
    const auto impulse_entity = std::find_if(
        impulse_catalog.entity_templates.begin(),
        impulse_catalog.entity_templates.end(),
        [](const auto& entity) { return entity.entity_template_id == 302u; });
    require(impulse_entity != impulse_catalog.entity_templates.end());
    require(impulse_entity->impulse_resistance == 9.0f);
    require(impulse_entity->collision_trigger.actions[0].action_type ==
            KernelEntityTriggerActionType_ApplyImpulse);
    require(impulse_entity->collision_trigger.actions[0].impulse_collision_mask ==
            KERNEL_COLLISION_MASK_ACTOR);
    require(impulse_entity->collision_trigger.actions[0].impulse_strength == 4.5f);
    require(impulse_entity->collision_trigger.actions[0].direction_source ==
            KernelEventVec3Source_Literal);
    require(impulse_entity->collision_trigger.actions[0].impulse_direction.x ==
            1.0f);
    require(impulse_entity->collision_trigger.actions[0].impulse_direction.y ==
            0.0f);
    require(impulse_entity->collision_trigger.actions[0].impulse_direction.z ==
            0.0f);

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

    const std::string health_change_graph =
        "id: action_apply_health_change\n"
        "parameters:\n"
        "  target: null\n"
        "  amount: 1\n"
        "actions:\n"
        "  - type: apply_health_change\n"
        "    target: params.target\n"
        "    amount: params.amount\n"
        "    when: event.has_target\n";
    const std::string collision_prop =
        "id: 300\n"
        "name: health_change_collision_prop\n"
        "entity_type: prop\n"
        "health:\n"
        "  hp: 3\n"
        "  max_hp: 3\n"
        "physics:\n"
        "  collider_template: rocket_aabb\n"
        "triggers:\n"
        "  on_collision:\n"
        "    collision_mask: actor | terrain | obstacle\n"
        "    action_graph: action_apply_health_change\n"
        "    parameters:\n"
        "      target: event.target\n"
        "      amount: 30\n";
    const std::vector<std::uint8_t> health_change_bundle =
        make_gameplay_bundle_zip(
            read_text_file("game_server/entity_templates/sentry_grunt.yaml"),
            {
                {"action_graph_templates/action_apply_health_change.yaml",
                 health_change_graph},
                {"entity_templates/health_change_collision_prop.yaml",
                 collision_prop},
            });
    const network_example::game_server::GameServerGameplayConfig
        health_change_config =
            network_example::game_server::load_gameplay_config_from_bundle_memory(
                health_change_bundle.data(),
                static_cast<std::uint32_t>(health_change_bundle.size()),
                "gameplay_catalog.yaml");
    const network_example::game_server::KernelGameplayCatalogStorage
        health_change_catalog =
            network_example::game_server::build_kernel_gameplay_catalog(
                health_change_config);
    const auto health_prop = std::find_if(
        health_change_catalog.entity_templates.begin(),
        health_change_catalog.entity_templates.end(),
        [](const KernelEntityTemplateDefinition& entity) {
            return entity.entity_template_id == 300u;
        });
    require(health_prop != health_change_catalog.entity_templates.end());
    require(health_prop->collision_trigger.action_count == 1u);
    require(
        health_prop->collision_trigger.action_type ==
        KernelEntityTriggerActionType_ApplyHealthChange);
    require(health_prop->collision_trigger.health_change_amount == 30);
    require(
        health_prop->collision_trigger.condition_type ==
        KernelActionConditionType_EventHasTarget);
    require(
        health_prop->collision_trigger_mask ==
        (KERNEL_COLLISION_MASK_ACTOR |
         KERNEL_COLLISION_MASK_STATIC_WORLD));
    require(
        health_prop->collision_trigger.actions[0].action_type ==
        KernelEntityTriggerActionType_ApplyHealthChange);
    require(health_prop->collision_trigger.actions[0].health_change_amount == 30);
    require(
        health_prop->collision_trigger.actions[0].condition_type ==
        KernelActionConditionType_EventHasTarget);
    auto health_change_hash_config = health_change_config;
    const auto authored_health_prop = std::find_if(
        health_change_hash_config.entity_templates.begin(),
        health_change_hash_config.entity_templates.end(),
        [](const network_example::game_server::EntityTemplateConfig& entity) {
            return entity.actor_template_id == 300u;
        });
    require(
        authored_health_prop != health_change_hash_config.entity_templates.end());
    const auto amount_parameter = std::find_if(
        authored_health_prop->collision_trigger.parameters.begin(),
        authored_health_prop->collision_trigger.parameters.end(),
        [](const auto& parameter) { return parameter.first == "amount"; });
    require(
        amount_parameter !=
        authored_health_prop->collision_trigger.parameters.end());
    amount_parameter->second = "31";
    require(
        network_example::game_server::compute_gameplay_catalog_hash(
            health_change_config) !=
        network_example::game_server::compute_gameplay_catalog_hash(
            health_change_hash_config));

    const std::string collision_projectile_graph =
        "id: action_spawn_projectile_at_collision\n"
        "parameters:\n"
        "  template: null\n"
        "  position: null\n"
        "  direction: null\n"
        "actions:\n"
        "  - type: spawn_projectile\n"
        "    projectile_template: params.template\n"
        "    position: params.position\n"
        "    direction: params.direction\n";
    const std::string collision_projectile_prop =
        "id: 301\n"
        "name: collision_projectile_prop\n"
        "entity_type: prop\n"
        "health:\n"
        "  hp: 1\n"
        "  max_hp: 1\n"
        "physics:\n"
        "  collider_template: rocket_aabb\n"
        "triggers:\n"
        "  on_collision:\n"
        "    collision_mask: terrain\n"
        "    action_graph: action_spawn_projectile_at_collision\n"
        "    parameters:\n"
        "      template: fire_floor_area\n"
        "      position: event.position\n"
        "      direction: event.direction\n";
    const std::vector<std::uint8_t> collision_projectile_bundle =
        make_gameplay_bundle_zip(
            read_text_file("game_server/entity_templates/sentry_grunt.yaml"),
            {
                {"action_graph_templates/"
                 "action_spawn_projectile_at_collision.yaml",
                 collision_projectile_graph},
                {"entity_templates/collision_projectile_prop.yaml",
                 collision_projectile_prop},
            });
    const network_example::game_server::GameServerGameplayConfig
        collision_projectile_config =
            network_example::game_server::load_gameplay_config_from_bundle_memory(
                collision_projectile_bundle.data(),
                static_cast<std::uint32_t>(collision_projectile_bundle.size()),
                "gameplay_catalog.yaml");
    const network_example::game_server::KernelGameplayCatalogStorage
        collision_projectile_catalog =
            network_example::game_server::build_kernel_gameplay_catalog(
                collision_projectile_config);
    const auto collision_projectile = std::find_if(
        collision_projectile_catalog.entity_templates.begin(),
        collision_projectile_catalog.entity_templates.end(),
        [](const KernelEntityTemplateDefinition& entity) {
            return entity.entity_template_id == 301u;
        });
    require(
        collision_projectile !=
        collision_projectile_catalog.entity_templates.end());
    require(collision_projectile->collision_trigger.action_count == 1u);
    require(
        collision_projectile->collision_trigger.action_type ==
        KernelEntityTriggerActionType_SpawnProjectile);
    require(
        collision_projectile->collision_trigger.spawn_projectile_template_id ==
        4u);
    require(
        collision_projectile->collision_trigger.position_source ==
        KernelEventVec3Source_Position);
    require(
        collision_projectile->collision_trigger.direction_source ==
        KernelEventVec3Source_Direction);

    for (const char* invalid_amount : {"0", "65536", "-65536"}) {
        const std::string invalid_prop =
            "id: 300\n"
            "name: health_change_collision_prop\n"
            "entity_type: prop\n"
            "health:\n"
            "  hp: 3\n"
            "  max_hp: 3\n"
            "physics:\n"
            "  collider_template: rocket_aabb\n"
            "triggers:\n"
            "  on_collision:\n"
            "    collision_mask: actor\n"
            "    action_graph: action_apply_health_change\n"
            "    parameters:\n"
            "      target: self\n"
            "      amount: " + std::string(invalid_amount) + "\n";
        const std::vector<std::uint8_t> invalid_bundle =
            make_gameplay_bundle_zip(
                read_text_file("game_server/entity_templates/sentry_grunt.yaml"),
                {
                    {"action_graph_templates/action_apply_health_change.yaml",
                     health_change_graph},
                    {"entity_templates/health_change_collision_prop.yaml",
                     invalid_prop},
                });
        bool rejected = false;
        try {
            const auto invalid_config = network_example::game_server::
                load_gameplay_config_from_bundle_memory(
                    invalid_bundle.data(),
                    static_cast<std::uint32_t>(invalid_bundle.size()),
                    "gameplay_catalog.yaml");
            (void)network_example::game_server::build_kernel_gameplay_catalog(
                invalid_config);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected);
    }

    for (const std::string& invalid_trigger : {
             std::string(
                 "  on_collision:\n"
                 "    action_graph: action_apply_health_change\n"
                 "    parameters:\n"
                 "      target: self\n"
                 "      amount: 30\n"),
             std::string(
                 "  on_world_impact:\n"
                 "    action_graph: action_apply_health_change\n"
                 "    parameters:\n"
                 "      target: self\n"
                 "      amount: 30\n"),
         }) {
        const std::string invalid_prop =
            "id: 300\n"
            "name: invalid_collision_prop\n"
            "entity_type: prop\n"
            "health:\n"
            "  hp: 3\n"
            "  max_hp: 3\n"
            "physics:\n"
            "  collider_template: rocket_aabb\n"
            "triggers:\n" + invalid_trigger;
        const std::vector<std::uint8_t> invalid_bundle =
            make_gameplay_bundle_zip(
                read_text_file("game_server/entity_templates/sentry_grunt.yaml"),
                {
                    {"action_graph_templates/action_apply_health_change.yaml",
                     health_change_graph},
                    {"entity_templates/invalid_collision_prop.yaml",
                     invalid_prop},
                });
        bool rejected = false;
        try {
            (void)network_example::game_server::
                load_gameplay_config_from_bundle_memory(
                    invalid_bundle.data(),
                    static_cast<std::uint32_t>(invalid_bundle.size()),
                    "gameplay_catalog.yaml");
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected);
    }

    const std::vector<std::uint8_t> bundle_with_large_binary =
        make_gameplay_bundle_zip(
            read_text_file("game_server/entity_templates/sentry_grunt.yaml"),
            {{
                "mesh_assets/jolt/oversized.joltmesh",
                std::string(1024 * 1024 + 1, 'x'),
            }});
    const network_example::game_server::GameServerGameplayConfig
        binary_bundle_config =
            network_example::game_server::load_gameplay_config_from_bundle_memory(
                bundle_with_large_binary.data(),
                static_cast<std::uint32_t>(bundle_with_large_binary.size()),
                "gameplay_catalog.yaml");
    assert(binary_bundle_config.weapons.catalog_hash == config.weapons.catalog_hash);

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

    const network_example::game_server::GameServerGameplayConfig
        monster_observer_config =
            network_example::game_server::load_gameplay_config_from_bundle_memory(
                generated_bundle.data(),
                static_cast<std::uint32_t>(generated_bundle.size()),
                "monster_observer_gameplay_catalog.yaml");
    require(monster_observer_config.weapons.catalog_version == 11u);
    require(monster_observer_config.agent.override_director_spawn);
    require(monster_observer_config.agent.actor_template_id == 20u);
    require(monster_observer_config.agent.spawn_count == 1u);
    require(monster_observer_config.agent.spawn_radius == 0.0f);
    require(monster_observer_config.agent.spawn_seed == 4242u);
    require(monster_observer_config.agent.spawn_position.x == 0.0f);
    require(monster_observer_config.agent.spawn_position.y == 10.0f);
    const network_example::game_server::ActorTemplateConfig*
        monster_observer_actor =
            network_example::game_server::find_actor_template(
                monster_observer_config,
                monster_observer_config.agent.actor_template_id);
    require(monster_observer_actor != nullptr);
    require(monster_observer_actor->name == "monster_sim_actor");
    require(monster_observer_actor->sentry.passive_patrol);
    require(monster_observer_actor->sentry.patrol_input_magnitude == 1.0f);
    require(monster_observer_actor->movement_controller_type ==
            KernelMovementControllerType_Character);
    require(
        monster_observer_config.weapons.catalog_hash !=
        generated_bundle_config.weapons.catalog_hash);
    const network_example::game_server::KernelGameplayCatalogStorage
        monster_observer_catalog =
            network_example::game_server::build_kernel_gameplay_catalog(
                monster_observer_config);
    const auto monster_observer_director = std::find_if(
        monster_observer_catalog.entity_templates.begin(),
        monster_observer_catalog.entity_templates.end(),
        [](const KernelEntityTemplateDefinition& definition) {
            return definition.entity_type == KernelEntityType_Director;
        });
    require(
        monster_observer_director !=
        monster_observer_catalog.entity_templates.end());
    require(monster_observer_director->ai.spawn_target_count == 1u);
    require(monster_observer_director->ai.spawn_entity_template_id == 20u);
    require(monster_observer_director->ai.spawn_actor_template_id == 20u);

    const std::string conflicting_enemy_catalog =
        read_text_file("game_server/gameplay_catalog.yaml") +
        "enemy:\n"
        "  actor_template: sentry_grunt\n"
        "  entity_template: monster_sim_actor\n";
    bool conflicting_enemy_rejected = false;
    try {
        const std::vector<std::uint8_t> conflicting_enemy_bundle =
            make_gameplay_bundle_zip(
                production_sentry_yaml,
                {},
                {},
                conflicting_enemy_catalog);
        (void)network_example::game_server::
            load_gameplay_config_from_bundle_memory(
                conflicting_enemy_bundle.data(),
                static_cast<std::uint32_t>(conflicting_enemy_bundle.size()),
                "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        conflicting_enemy_rejected =
            std::string(error.what()).find("both actor_template and entity_template") !=
            std::string::npos;
    }
    require(conflicting_enemy_rejected);

    const std::vector<std::uint8_t> unsupported_version_bundle = make_store_zip({
        {"gameplay_catalog.yaml", "catalog_version: 7\n"},
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
        {"gameplay_catalog.yaml", "catalog_version: 8\nsurprise: true\n"},
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

    // catalog_version 10 replaced the skeleton_manifests list with a directory
    // scan. The old key is still in the allowlist purely so it can be reported
    // as a migration rather than as an anonymous unknown field.
    const std::vector<std::uint8_t> legacy_manifest_list_bundle = make_store_zip({
        {"gameplay_catalog.yaml",
         "catalog_version: 10\n"
         "weapon_template_dir: weapon_templates\n"
         "projectile_template_dir: projectile_templates\n"
         "collider_template_dir: collider_templates\n"
         "skeleton_manifests:\n"
         "  - skeleton_assets/generated/simplified_monster_sim_v4"
         ".skeleton_manifest.json\n"},
    });
    bool legacy_manifest_list_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            legacy_manifest_list_bundle.data(),
            static_cast<std::uint32_t>(legacy_manifest_list_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        legacy_manifest_list_rejected =
            std::string(error.what()).find("skeleton_manifests_dir") !=
            std::string::npos;
    }
    assert(legacy_manifest_list_rejected);

    // A directory that exists but holds no manifests is a typo, not an empty
    // rig set -- catalogs without skeletons omit the key entirely.
    const std::vector<std::uint8_t> empty_manifest_dir_bundle = make_store_zip({
        {"gameplay_catalog.yaml",
         "catalog_version: 10\n"
         "weapon_template_dir: weapon_templates\n"
         "projectile_template_dir: projectile_templates\n"
         "collider_template_dir: collider_templates\n"
         "skeleton_manifests_dir: skeleton_assets/nowhere\n"},
    });
    bool empty_manifest_dir_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            empty_manifest_dir_bundle.data(),
            static_cast<std::uint32_t>(empty_manifest_dir_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        empty_manifest_dir_rejected =
            std::string(error.what()).find("contains no") != std::string::npos;
    }
    assert(empty_manifest_dir_rejected);

    const std::string production_catalog =
        read_text_file("game_server/gameplay_catalog.yaml");
    const auto rejects_catalog = [&](const std::string& catalog_yaml) {
        const std::vector<std::uint8_t> bundle = make_gameplay_bundle_zip(
            production_sentry_yaml, {}, {}, catalog_yaml);
        try {
            (void)network_example::game_server::
                load_gameplay_config_from_bundle_memory(
                    bundle.data(),
                    static_cast<std::uint32_t>(bundle.size()),
                    "gameplay_catalog.yaml");
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    require(rejects_catalog(replace_once(
        production_catalog,
        "    max_alive: 256\n",
        "    max_alive: 256\n"
        "    overflow: despawn_oldest\n")));
    require(rejects_catalog(replace_once(
        production_catalog,
        "    max_alive: 256\n",
        "    max_alive: 257\n")));
    require(rejects_catalog(replace_once(
        production_catalog,
        "    max_alive: 256\n",
        "    max_alive: 256\n"
        "  - id: 1\n"
        "    name: duplicate\n"
        "    max_alive: 1\n")));

    const std::vector<std::uint8_t> legacy_item_input_bundle =
        make_gameplay_bundle_zip(
            read_text_file("game_server/entity_templates/sentry_grunt.yaml"),
            {{"item_templates/legacy.yaml",
              "id: 9900\n"
              "name: legacy\n"
              "mode: fungible\n"
              "max_stack: 1\n"
              "capabilities: []\n"
              "input:\n"
              "  inventory:\n"
              "    use: none\n"}});
    bool legacy_item_input_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            legacy_item_input_bundle.data(),
            static_cast<std::uint32_t>(legacy_item_input_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        legacy_item_input_rejected =
            std::string(error.what()).find("unknown field") != std::string::npos;
    }
    assert(legacy_item_input_rejected);

    const std::vector<std::uint8_t> legacy_throw_speed_bundle =
        make_gameplay_bundle_zip(
            read_text_file("game_server/entity_templates/sentry_grunt.yaml"),
            {{"item_templates/legacy_throw.yaml",
              "id: 9902\n"
              "name: legacy_throw\n"
              "mode: stateful\n"
              "max_stack: 1\n"
              "capabilities: [throwable]\n"
              "throw:\n"
              "  mode: identity_preserving\n"
              "  speed: 10.0\n"}});
    bool legacy_throw_speed_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            legacy_throw_speed_bundle.data(),
            static_cast<std::uint32_t>(legacy_throw_speed_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        legacy_throw_speed_rejected =
            std::string(error.what()).find("unknown field") !=
            std::string::npos;
    }
    assert(legacy_throw_speed_rejected);

    const std::vector<std::uint8_t> legacy_prop_mapping_bundle =
        make_gameplay_bundle_zip(
            read_text_file("game_server/entity_templates/sentry_grunt.yaml"),
            {{"entity_templates/legacy.yaml",
              "id: 9901\n"
              "name: legacy_prop\n"
              "entity_type: prop\n"
              "interaction:\n"
              "  capabilities: [interactable]\n"
              "  interact_tap: activate\n"
              "  range: 3.0\n"}});
    bool legacy_prop_mapping_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            legacy_prop_mapping_bundle.data(),
            static_cast<std::uint32_t>(legacy_prop_mapping_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        legacy_prop_mapping_rejected =
            std::string(error.what()).find("unknown field") != std::string::npos;
    }
    assert(legacy_prop_mapping_rejected);

    const std::vector<std::uint8_t> unknown_nested_catalog_field_bundle = make_store_zip({
        {"gameplay_catalog.yaml",
         "catalog_version: 8\n"
         "weapon_template_dir: weapon_templates\n"
         "projectile_template_dir: projectile_templates\n"
         "collider_template_dir: collider_templates\n"
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

    const std::vector<std::uint8_t> legacy_collider_field_bundle = make_store_zip({
        {"gameplay_catalog.yaml",
         "catalog_version: 8\n"
         "weapon_template_dir: weapon_templates\n"
         "projectile_template_dir: projectile_templates\n"
         "collider_template_file: collider_templates/default.yaml\n"},
    });
    bool legacy_collider_field_rejected = false;
    try {
        (void)network_example::game_server::load_gameplay_config_from_bundle_memory(
            legacy_collider_field_bundle.data(),
            static_cast<std::uint32_t>(legacy_collider_field_bundle.size()),
            "gameplay_catalog.yaml");
    } catch (const std::exception& error) {
        legacy_collider_field_rejected =
            std::string(error.what()).find("unknown field") != std::string::npos;
    }
    assert(legacy_collider_field_rejected);

    std::vector<std::pair<std::string, std::string>> duplicate_collider_files;
    duplicate_collider_files.push_back({
        "gameplay_catalog.yaml",
        "catalog_version: 8\n"
        "reload_action_template:\n"
        "  id: 4199\n"
        "  name: shared_reload\n"
        "  trigger_mode: press\n"
        "  flags: []\n"
        "  ammo_cost_per_commit: 0\n"
        "  commit_offset_ticks: 1\n"
        "  commit_interval_ticks: 0\n"
        "  max_commit_count: 1\n"
        "  recovery_ticks: 0\n"
        "  hold_input_timeout_ticks: 0\n"
        "weapon_template_dir: weapon_templates\n"
        "projectile_template_dir: projectile_templates\n"
        "collider_template_dir: collider_templates\n"});
    const std::vector<std::string> duplicate_collider_weapon_files = {
        "beam_rifle.yaml",
        "fire_floor.yaml",
        "homing_missile.yaml",
        "rifle.yaml",
        "rocket.yaml",
        "shotgun.yaml",
        "grenade_launcher.yaml",
        "spammer.yaml",
    };
    for (const std::string& file : duplicate_collider_weapon_files) {
        duplicate_collider_files.push_back({
            "weapon_templates/" + file,
            read_text_file("game_server/weapon_templates/" + file)});
    }
    duplicate_collider_files.push_back({
        "collider_templates/a.yaml",
        "id: 1\n"
        "name: a\n"
        "shape: aabb\n"
        "center: {x: 0.0, y: 0.0, z: 0.0}\n"
        "half_extents: {x: 1.0, y: 1.0, z: 1.0}\n"
        "radius: 0.0\n"
        "purpose: hit\n"
        "layer: player_side\n"});
    duplicate_collider_files.push_back({
        "collider_templates/b.yaml",
        "id: 1\n"
        "name: b\n"
        "shape: sphere\n"
        "center: {x: 0.0, y: 0.0, z: 0.0}\n"
        "half_extents: {x: 1.0, y: 1.0, z: 1.0}\n"
        "radius: 1.0\n"
        "purpose: damage\n"
        "layer: hostile_side\n"});
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
        {"gameplay_catalog.yaml", "catalog_version: 8\n"},
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
    actor_hash_changed = config;
    actor_hash_changed.actor_templates[0].inventory_slot_capacity += 1;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(
            actor_hash_changed));
    actor_hash_changed = config;
    actor_hash_changed.actor_templates[0].inventory_slots[0].quantity -= 1;
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(
            actor_hash_changed));
    actor_hash_changed = config;
    std::swap(
        actor_hash_changed.actor_templates[0].inventory_slots[0],
        actor_hash_changed.actor_templates[0].inventory_slots[1]);
    require(
        config.weapons.catalog_hash !=
        network_example::game_server::compute_gameplay_catalog_hash(
            actor_hash_changed));

    invalid = config;
    invalid.actor_templates[0].inventory_slot_capacity = 2;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());
    invalid = config;
    invalid.actor_templates[0].inventory_slots[0].quantity = 0;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());
    invalid = config;
    invalid.actor_templates[0].inventory_slots[0].quantity = 6;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());
    invalid = config;
    invalid.actor_templates[0].inventory_slots[1].quantity = 2;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());
    invalid = config;
    invalid.actor_templates[0].inventory_slots[0].item_template_id = 999999;
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());
    invalid = config;
    invalid.actor_templates[1].inventory_slot_capacity = 1;
    invalid.actor_templates[1].inventory_slots = {
        config.actor_templates[0].inventory_slots[0]};
    assert(!network_example::game_server::validate_gameplay_config(invalid).empty());

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

    const auto create_player_entity = [](KernelHandle* kernel) {
        KernelServerEntityCreateInfo create_info{};
        create_info.struct_size = sizeof(create_info);
        create_info.entity_type = network_example::game_server::kEntityTypeActor;
        create_info.actor_type = KernelActorType_Player;
        create_info.owner_peer = 7;
        create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t net_id = 0;
        assert(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
        return net_id;
    };
    const auto make_server_kernel = []() {
        KernelConfig kernel_config{};
        kernel_config.mode = KernelMode_DedicatedServer;
        kernel_config.tick.server_tick_rate = 30;
        kernel_config.tick.snapshot_rate = 30;
        KernelHandle* kernel = Kernel_Create(&kernel_config);
        assert(kernel != nullptr);
        return kernel;
    };

    KernelHandle* skeleton_kernel = make_server_kernel();
    require(network_example::game_server::load_kernel_gameplay_catalog(
        skeleton_kernel,
        config));
    require(Kernel_GetSkeletonBindPose(
                nullptr,
                1u,
                UINT64_C(0x1c171165d9bb479b),
                nullptr,
                0u) == 0u);
    require(Kernel_GetSkeletonBindPose(
                skeleton_kernel,
                999u,
                UINT64_C(0x1c171165d9bb479b),
                nullptr,
                0u) == 0u);
    require(Kernel_GetSkeletonBindPose(
                skeleton_kernel,
                1u,
                UINT64_C(0xdeadbeef),
                nullptr,
                0u) == 0u);
    require(Kernel_GetSkeletonBindPose(
                skeleton_kernel,
                1u,
                UINT64_C(0x1c171165d9bb479b),
                nullptr,
                0u) == 41u);
    std::array<KernelBoneLocalTransform, 10> truncated_bind_pose{};
    require(Kernel_GetSkeletonBindPose(
                skeleton_kernel,
                1u,
                UINT64_C(0x1c171165d9bb479b),
                truncated_bind_pose.data(),
                truncated_bind_pose.size()) == 41u);
    require(truncated_bind_pose[0].local_scale.x == 1.0f);
    std::array<KernelBoneLocalTransform, 39> complete_bind_pose{};
    require(Kernel_GetSkeletonBindPose(
                skeleton_kernel,
                1u,
                UINT64_C(0x1c171165d9bb479b),
                complete_bind_pose.data(),
                complete_bind_pose.size()) == 41u);
    require(complete_bind_pose.back().local_scale.z == 1.0f);
    require(Kernel_StartDedicatedServer(skeleton_kernel, 7900));
    KernelServerEntityCreateInfo skeleton_create{};
    skeleton_create.struct_size = sizeof(skeleton_create);
    skeleton_create.entity_template_id = 20u;
    skeleton_create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t skeleton_entity_id = 0u;
    require(Kernel_ServerCreateEntity(
        skeleton_kernel,
        &skeleton_create,
        &skeleton_entity_id));
    KernelPlayerInput locomotion_input{};
    locomotion_input.input_seq = 1u;
    locomotion_input.move = KernelVec2{1.0f, 0.0f};
    require(Kernel_ServerSubmitEntityInput(
        skeleton_kernel,
        skeleton_entity_id,
        &locomotion_input));
    Kernel_Update(skeleton_kernel, 1.0f / 30.0f);
    KernelSkeletonRenderStateResult skeleton_result{};
    skeleton_result.struct_size = sizeof(skeleton_result);
    require(Kernel_GetSkeletonRenderStates(
               skeleton_kernel,
               nullptr,
               0u,
               nullptr,
               0u,
               &skeleton_result) == 0u);
    require(skeleton_result.status ==
           KERNEL_SKELETON_RENDER_STATUS_INSUFFICIENT_CAPACITY);
    require(skeleton_result.required_state_count == 1u);
    require(skeleton_result.required_bone_transform_count == 41u);
    std::array<KernelSkeletonRenderState, 1> skeleton_states{};
    std::array<KernelBoneLocalTransform, 41> bone_transforms{};
    skeleton_result.struct_size = sizeof(skeleton_result);
    require(Kernel_GetSkeletonRenderStates(
               skeleton_kernel,
               skeleton_states.data(),
               skeleton_states.size(),
               bone_transforms.data(),
               bone_transforms.size() - 1u,
               &skeleton_result) == 0u);
    require(skeleton_result.status ==
           KERNEL_SKELETON_RENDER_STATUS_INSUFFICIENT_CAPACITY);
    skeleton_result.struct_size = sizeof(skeleton_result);
    require(Kernel_GetSkeletonRenderStates(
               skeleton_kernel,
               skeleton_states.data(),
               skeleton_states.size(),
               bone_transforms.data(),
               bone_transforms.size(),
               &skeleton_result) == 1u);
    require(skeleton_result.status == KERNEL_SKELETON_RENDER_STATUS_SUCCESS);
    require(skeleton_states[0].entity_net_id == skeleton_entity_id);
    require(skeleton_states[0].skeleton_asset_id == 1u);
    require(skeleton_states[0].bone_count == 41u);
    require((skeleton_states[0].pose_flags &
            KERNEL_SKELETON_POSE_FLAG_PROCEDURAL) != 0u);
    require(bone_transforms[0].local_scale.x == 1.0f);
    std::array<RenderEntityState, 256> locomotion_render_states{};
    const std::uint32_t locomotion_render_count = Kernel_GetRenderStates(
        skeleton_kernel,
        locomotion_render_states.data(),
        locomotion_render_states.size());
    const auto locomotion_render_state = std::find_if(
        locomotion_render_states.begin(),
        locomotion_render_states.begin() + locomotion_render_count,
        [skeleton_entity_id](const RenderEntityState& state) {
            return state.net_id == skeleton_entity_id;
        });
    require(locomotion_render_state !=
            locomotion_render_states.begin() + locomotion_render_count);
    const float expected_half_yaw_radians =
        45.0f * 3.14159265358979323846f / 180.0f / 30.0f / 2.0f;
    require(std::abs(
        locomotion_render_state->rotation.y -
        std::sin(expected_half_yaw_radians)) < 0.0001f);
    require(std::abs(
        locomotion_render_state->rotation.w -
        std::cos(expected_half_yaw_radians)) < 0.0001f);
    Kernel_Destroy(skeleton_kernel);

    // The follower path end to end: a kernel that never simulates the monster --
    // it is not in its registry, and no grounding query is ever made for it --
    // reconstructs the monster's legs from replicated steps alone, and the
    // presentation reports them as procedural rather than falling back to the
    // bind pose. This is the client-side half of replicating steps instead of
    // bones; only the transport that fills the step queue is still missing.
    {
        network_example::game_server::KernelGameplayCatalogStorage
            follower_catalog =
                network_example::game_server::build_kernel_gameplay_catalog(
                    config);
        KernelConfig follower_config{};
        follower_config.mode = KernelMode_Client;
        follower_config.tick.server_tick_rate = 30;
        follower_config.tick.snapshot_rate = 15;
        follower_config.max_render_states = 64;
        follower_config.max_events = 128;
        network_example::KernelEngine follower(follower_config);
        follower.reset_runtime_state(KernelMode_Client);
        require(follower.load_gameplay_catalog(follower_catalog.definition));

        constexpr network_example::NetId kMonsterNetId = 4242u;
        constexpr std::uint32_t kMonsterTemplateId = 20u;
        // A baseline is sent beside the spawn packet, so it can reach the
        // client before that entity's first snapshot. It must survive that.
        network_example::LocomotionStepBatchPacket early_baseline{};
        early_baseline.server_tick = 99u;
        early_baseline.records.push_back(network_example::LocomotionStepRecord{
            kMonsterNetId, 2u, UINT8_MAX, glm::vec3{5.0f, 0.0f, -5.0f}});
        follower.handle_client_locomotion_step_batch(early_baseline);
        follower.update_follower_locomotion();
        require(follower.pending_follower_steps_.size() == 1u);
        // REPRO: the client ticks at 30 Hz but snapshots arrive at 15 Hz, so
        // update_follower_locomotion is routinely called with no new tick to
        // step. A held step must survive those calls.
        follower.update_follower_locomotion();
        follower.update_follower_locomotion();
        require(follower.pending_follower_steps_.size() == 1u);

        network_example::EntitySpawnPacket spawn{};
        spawn.net_id = kMonsterNetId;
        spawn.entity_type = network_example::EntityType::kActor;
        spawn.actor_type = network_example::ActorType::kAgent;
        spawn.owner_peer = 0u;
        spawn.actor_template_id = kMonsterTemplateId;
        spawn.entity_template_id = kMonsterTemplateId;
        follower.handle_client_spawn(spawn);

        const auto push_snapshot = [&](std::uint32_t server_tick, float x) {
            network_example::WorldSnapshot snapshot;
            snapshot.header.server_tick = server_tick;
            network_example::EntitySnapshot entity;
            entity.net_id = kMonsterNetId;
            entity.type = network_example::EntityType::kActor;
            entity.actor_type = network_example::ActorType::kAgent;
            entity.position = glm::vec3{x, 0.0f, 0.0f};
            entity.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
            snapshot.entities.push_back(entity);
            follower.handle_client_snapshot(snapshot);
        };

        // Two snapshots so the buffer can interpolate between server ticks.
        push_snapshot(100u, 0.0f);
        push_snapshot(102u, 0.5f);
        follower.update_follower_locomotion();
        require(follower.follower_locomotion_states_.count(kMonsterNetId) == 1u);
        // Only the leg the early baseline named has committed to a world
        // position; nothing has stepped, so no other foot has.
        for (std::size_t leg_index = 0u;
             leg_index <
                 follower.follower_locomotion_states_[kMonsterNetId].legs.size();
             ++leg_index) {
            const network_example::LegLocomotionState& leg =
                follower.follower_locomotion_states_[kMonsterNetId]
                    .legs[leg_index];
            require(leg.foot_initialized == (leg_index == 2u));
        }

        // One replicated step, for one leg, landing somewhere the authority
        // chose. The follower is told nothing else: no terrain, no lift-off
        // point, no gait.
        // Routed through the real wire format rather than injected, so this
        // covers the encode/decode the server will actually use.
        const glm::vec3 landing{1.25f, 0.0f, 9.5f};
        network_example::LocomotionStepBatchPacket step_batch{};
        step_batch.server_tick = 106u;
        step_batch.records.push_back(network_example::LocomotionStepRecord{
            kMonsterNetId, 0u, 2u, landing});
        const std::vector<std::uint8_t> encoded_steps =
            network_example::encode_locomotion_step_batch_packet(
                step_batch, 1u);
        require(!encoded_steps.empty());
        network_example::LocomotionStepBatchPacket decoded_steps;
        require(network_example::decode_locomotion_step_batch_packet(
            encoded_steps.data(), encoded_steps.size(), &decoded_steps));
        follower.handle_client_locomotion_step_batch(decoded_steps);
        // server_tick 106 minus a 2 tick delta is the tick the swing began.
        require(follower.pending_follower_steps_.size() == 1u);
        require(follower.pending_follower_steps_[0].event.start_tick == 104u);

        // A step in the future is held, not applied early.
        follower.update_follower_locomotion();
        require(!follower.follower_locomotion_states_[kMonsterNetId]
                     .legs[0]
                     .foot_initialized);
        require(follower.pending_follower_steps_.size() == 1u);

        // Walk the snapshot buffer past the step, then past its whole swing.
        // The end tick is derived from the authored gait: the swing begins on
        // tick 104 and spans step_duration_ticks ticks counting that one, with
        // a couple of spare ticks so the assertions below are not sitting on
        // the exact touchdown tick.
        const std::uint32_t swing_end_tick =
            104u + monster_definition->skeleton.step_duration_ticks + 3u;
        float monster_x = 0.5f;
        for (std::uint32_t server_tick = 104u; server_tick <= swing_end_tick;
             server_tick += 2u) {
            monster_x += 0.25f;
            push_snapshot(server_tick, monster_x);
            follower.update_follower_locomotion();
        }
        require(follower.pending_follower_steps_.empty());

        const network_example::LegLocomotionState& stepped =
            follower.follower_locomotion_states_[kMonsterNetId].legs[0];
        require(stepped.foot_initialized);
        require(stepped.gait_state == network_example::LegGaitState::kSupport);
        // Landed exactly where the authority said, with no resync and no
        // knowledge of the terrain under it.
        require(glm::length(stepped.foot_target_world - landing) < 0.0001f);
        // Legs that were never told to step stay uncommitted rather than
        // guessing a foothold of their own.
        require(!follower.follower_locomotion_states_[kMonsterNetId]
                     .legs[1]
                     .foot_initialized);
        // The pre-snapshot baseline was kept and applied, so its leg is planted
        // rather than waiting for a step of its own.
        const network_example::LegLocomotionState& early_planted =
            follower.follower_locomotion_states_[kMonsterNetId].legs[2];
        require(early_planted.foot_initialized);
        require(glm::length(
                    early_planted.foot_target_world -
                    glm::vec3{5.0f, 0.0f, -5.0f}) < 0.0001f);

        // Poses were recorded per server tick, so presentation can sample them
        // at the same instant it samples the roots.
        require(follower.skeleton_pose_history_.count(kMonsterNetId) == 1u);
        require(follower.skeleton_pose_history_[kMonsterNetId].size > 1u);

        // And they survive the authoritative pass. simulate_tick runs
        // update_legged_locomotion first, which knows only the entities this
        // kernel simulates -- on a client, none of the replicated ones. A tick
        // that steps nothing (about half of them, since the client ticks faster
        // than snapshots arrive) must still leave a history for presentation to
        // sample, or the pose falls back to the newest solved tick while the
        // root stays interpolated, and every foot jumps between two placements.
        const std::size_t history_before =
            follower.skeleton_pose_history_[kMonsterNetId].size;
        follower.update_legged_locomotion({}, 1.0f / 30.0f);
        follower.update_follower_locomotion();  // no new snapshot: steps 0 ticks
        require(follower.skeleton_pose_history_.count(kMonsterNetId) == 1u);
        require(follower.skeleton_pose_history_[kMonsterNetId].size ==
                history_before);
        std::vector<KernelBoneLocalTransform> sampled;
        std::uint32_t sampled_tick = 0u;
        require(network_example::sample_skeleton_pose_history(
            follower.skeleton_pose_history_[kMonsterNetId],
            follower.skeleton_pose_history_[kMonsterNetId]
                .samples[0]
                .time_us,
            &sampled,
            &sampled_tick));

        // And presentation reports procedural legs, not the bind-pose fallback
        // a client used to be stuck with.
        follower.rebuild_skeleton_presentation_at_time(
            follower.client_local_time_us_);
        const auto presented = std::find_if(
            follower.skeleton_presentation_poses_.begin(),
            follower.skeleton_presentation_poses_.end(),
            [](const network_example::SkeletonPresentationPose& pose) {
                return pose.entity_net_id == kMonsterNetId;
            });
        require(presented != follower.skeleton_presentation_poses_.end());
        require(presented->pose_flags ==
                KERNEL_SKELETON_POSE_FLAG_PROCEDURAL);

        // And the pose survives the AT_TIME query, which is the one a renderer
        // actually makes. It filters on pose_time_us <= requested time, so a
        // pose stamped with the SERVER's clock while the request carries the
        // client's is discarded outright -- a server that has been running
        // longer than the client makes every pose look like the future, and the
        // query returns nothing at all while the presentation list still holds
        // one. Asserted through the public entry point rather than the internal
        // list so the filter is actually exercised.
        // Reproduce the ordinary deployment: the server has been running longer
        // than the client, so its clock is ahead. The snapshots above sit around
        // tick 125, i.e. ~4.2 s of server time, so a 2 s offset puts the client
        // at ~2.5 s of its own clock for the same instant.
        follower.client_clock_offset_us_ = 2000000;
        follower.has_client_clock_sync_ = true;
        follower.client_local_time_us_ = 2500000;
        follower.rebuild_skeleton_presentation_at_time(
            follower.client_local_time_us_);

        std::array<KernelSkeletonRenderState, 8> at_time_states{};
        std::array<KernelBoneLocalTransform, 512> at_time_bones{};
        KernelSkeletonRenderStateResult at_time_result{};
        at_time_result.struct_size = sizeof(at_time_result);
        const std::uint32_t at_time_count =
            follower.get_skeleton_render_states_at_time(
                follower.client_local_time_us_,
                at_time_states.data(),
                static_cast<std::uint32_t>(at_time_states.size()),
                at_time_bones.data(),
                static_cast<std::uint32_t>(at_time_bones.size()),
                &at_time_result);
        require(at_time_count == 1u);
        require(at_time_states[0].entity_net_id == kMonsterNetId);
        require(at_time_states[0].pose_flags ==
                KERNEL_SKELETON_POSE_FLAG_PROCEDURAL);
        require(at_time_states[0].bone_count ==
                static_cast<std::uint32_t>(presented->local_transforms.size()));

        // A baseline is carried by the same record type: a step old enough that
        // its whole swing is in the past, which the follower resolves by
        // planting the foot outright. That is what stops a newly relevant
        // entity's legs from appearing one at a time as each happens to step.
        const glm::vec3 anchored{-3.0f, 0.0f, -7.5f};
        network_example::LocomotionStepBatchPacket baseline{};
        baseline.server_tick = swing_end_tick + 2u;
        baseline.records.push_back(network_example::LocomotionStepRecord{
            kMonsterNetId, 1u, UINT8_MAX, anchored});
        follower.handle_client_locomotion_step_batch(baseline);
        push_snapshot(swing_end_tick + 2u, monster_x + 0.25f);
        follower.update_follower_locomotion();
        // REPRO: a baseline that arrives while snapshots are already flowing --
        // which is exactly when one is sent, on relevance enter -- and lands on
        // an update that has no new tick to step. The client ticks at 30 Hz
        // against 15 Hz snapshots, so that is about half of all updates.
        network_example::LocomotionStepBatchPacket late_baseline{};
        late_baseline.server_tick = swing_end_tick + 2u;
        late_baseline.records.push_back(network_example::LocomotionStepRecord{
            kMonsterNetId, 3u, UINT8_MAX, glm::vec3{2.0f, 0.0f, 2.0f}});
        follower.handle_client_locomotion_step_batch(late_baseline);
        follower.update_follower_locomotion();  // no new snapshot: steps 0 ticks
        require(follower.pending_follower_steps_.size() == 1u);
        // It survives until a tick is actually stepped, and then plants.
        push_snapshot(swing_end_tick + 4u, monster_x + 0.5f);
        follower.update_follower_locomotion();
        const network_example::LegLocomotionState& late_planted =
            follower.follower_locomotion_states_[kMonsterNetId].legs[3];
        require(late_planted.foot_initialized);
        require(glm::length(
                    late_planted.foot_target_world -
                    glm::vec3{2.0f, 0.0f, 2.0f}) < 0.0001f);

        const network_example::LegLocomotionState& planted =
            follower.follower_locomotion_states_[kMonsterNetId].legs[1];
        require(planted.foot_initialized);
        require(planted.gait_state ==
                network_example::LegGaitState::kSupport);
        require(glm::length(planted.foot_target_world - anchored) < 0.0001f);
    }


    KernelConfig observer_kernel_config{};
    observer_kernel_config.mode = KernelMode_ListenServer;
    observer_kernel_config.tick.server_tick_rate = 30;
    observer_kernel_config.tick.snapshot_rate = 30;
    observer_kernel_config.max_events = 128;
    observer_kernel_config.max_render_states = 128;
    KernelHandle* observer_kernel = Kernel_Create(&observer_kernel_config);
    require(observer_kernel != nullptr);
    KernelGameplayCatalogLoadResult observer_load_result{};
    observer_load_result.struct_size = sizeof(observer_load_result);
    require(Kernel_LoadGameplayCatalogFromMemory(
        observer_kernel,
        generated_bundle.data(),
        static_cast<std::uint32_t>(generated_bundle.size()),
        "monster_observer_gameplay_catalog.yaml",
        &observer_load_result));
    require(
        observer_load_result.status ==
        KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_SUCCESS);
    require(Kernel_StartListenServer(observer_kernel, 7897));
    network_example::game_server::GameServer observer_server(
        observer_kernel,
        monster_observer_config);

    std::uint32_t observer_monster_net_id = 0u;
    float observer_monster_min_x = std::numeric_limits<float>::infinity();
    float observer_monster_max_x = -std::numeric_limits<float>::infinity();
    bool observer_monster_moved_positive = false;
    bool observer_monster_moved_negative = false;
    KernelVec3 observer_monster_last_position{};
    KernelVec3 observer_monster_last_velocity{};
    KernelVec3 observer_monster_start_position{};
    KernelVec3 observer_monster_position_after_300_ticks{};
    std::uint32_t observer_monster_observed_frames = 0u;
    std::uint16_t observer_player_hp = 0u;
    for (std::uint32_t frame = 0u; frame < 720u; ++frame) {
        Kernel_Update(observer_kernel, 1.0f / 30.0f);

        std::array<KernelEvent, 128> events{};
        const std::uint32_t event_count = Kernel_PollEvents(
            observer_kernel,
            events.data(),
            static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t event_index = 0u;
             event_index < event_count;
             ++event_index) {
            observer_server.handle_event(events[event_index]);
        }
        observer_server.tick(1.0f / 30.0f);

        std::array<KernelServerEntityState, 8> actor_states{};
        for (KernelServerEntityState& state : actor_states) {
            state.struct_size = sizeof(KernelServerEntityState);
        }
        const std::uint32_t actor_state_count = Kernel_ServerQueryEntities(
            observer_kernel,
            network_example::game_server::kEntityTypeActor,
            actor_states.data(),
            static_cast<std::uint32_t>(actor_states.size()));
        std::uint32_t observer_agent_count = 0u;
        for (std::uint32_t state_index = 0u;
             state_index < actor_state_count;
             ++state_index) {
            const KernelServerEntityState& state = actor_states[state_index];
            if (state.actor_type == network_example::game_server::kActorTypePlayer) {
                observer_player_hp = state.hp;
                continue;
            }
            if (state.actor_type != network_example::game_server::kActorTypeAgent) {
                continue;
            }
            ++observer_agent_count;
            require(state.actor_template_id == 20u);
            observer_monster_net_id = state.net_id;
            observer_monster_min_x =
                std::min(observer_monster_min_x, state.position.x);
            observer_monster_max_x =
                std::max(observer_monster_max_x, state.position.x);
            observer_monster_moved_positive =
                observer_monster_moved_positive || state.velocity.x > 0.1f;
            observer_monster_moved_negative =
                observer_monster_moved_negative || state.velocity.x < -0.1f;
            observer_monster_last_position = state.position;
            observer_monster_last_velocity = state.velocity;
            if (observer_monster_observed_frames == 0u) {
                observer_monster_start_position = state.position;
            } else if (observer_monster_observed_frames == 300u) {
                observer_monster_position_after_300_ticks = state.position;
            }
            ++observer_monster_observed_frames;
            require(state.position.x >= -30.5f);
            require(state.position.x <= 30.5f);
        }
        require(observer_agent_count <= 1u);
    }

    require(observer_monster_net_id != 0u);
    require(observer_monster_observed_frames > 300u);
    const float observer_monster_300_tick_distance =
        observer_monster_position_after_300_ticks.x -
        observer_monster_start_position.x;
    if (observer_monster_300_tick_distance <= 23.75f ||
        observer_monster_300_tick_distance >= 25.5f) {
        std::fprintf(
            stderr,
            "observer 300-tick diagnostic start=(%f,%f,%f) "
            "end=(%f,%f,%f) distance_x=%f\n",
            observer_monster_start_position.x,
            observer_monster_start_position.y,
            observer_monster_start_position.z,
            observer_monster_position_after_300_ticks.x,
            observer_monster_position_after_300_ticks.y,
            observer_monster_position_after_300_ticks.z,
            observer_monster_300_tick_distance);
    }
    require(observer_monster_300_tick_distance > 23.75f);
    require(observer_monster_300_tick_distance < 25.5f);
    require(std::abs(
        observer_monster_position_after_300_ticks.z -
        observer_monster_start_position.z) < 0.25f);
    if (observer_monster_max_x - observer_monster_min_x <= 8.0f ||
        !observer_monster_moved_positive ||
        !observer_monster_moved_negative) {
        std::fprintf(
            stderr,
            "observer patrol diagnostic min_x=%f max_x=%f positive=%d negative=%d "
            "position=(%f,%f,%f) velocity=(%f,%f,%f) next_input_seq=%u\n",
            observer_monster_min_x,
            observer_monster_max_x,
            observer_monster_moved_positive ? 1 : 0,
            observer_monster_moved_negative ? 1 : 0,
            observer_monster_last_position.x,
            observer_monster_last_position.y,
            observer_monster_last_position.z,
            observer_monster_last_velocity.x,
            observer_monster_last_velocity.y,
            observer_monster_last_velocity.z,
            observer_server.agent_runtime_manager().agents().empty()
                ? 0u
                : observer_server.agent_runtime_manager().agents()[0]
                      .next_input_seq);
    }
    require(observer_monster_max_x - observer_monster_min_x > 8.0f);
    require(observer_monster_moved_positive);
    require(observer_monster_moved_negative);
    require(observer_player_hp == 1000u);
    require(observer_server.agent_runtime_manager().agent_count() == 1u);
    require(
        observer_server.agent_runtime_manager().agents()[0].next_input_seq > 300u);
    require(
        observer_server.agent_runtime_manager().agents()[0].target_player_net_id == 0u);
    require(
        observer_server.agent_runtime_manager().agents()[0].sentry.state ==
        network_example::game_server::AgentSentryState::kIdle);

    std::array<RenderEntityState, 128> observer_render_states{};
    const std::uint32_t observer_render_count = Kernel_GetRenderStates(
        observer_kernel,
        observer_render_states.data(),
        static_cast<std::uint32_t>(observer_render_states.size()));
    const auto observer_monster_render_state = std::find_if(
        observer_render_states.begin(),
        observer_render_states.begin() + observer_render_count,
        [observer_monster_net_id](const RenderEntityState& state) {
            return state.net_id == observer_monster_net_id;
        });
    require(
        observer_monster_render_state !=
        observer_render_states.begin() + observer_render_count);
    require(observer_monster_render_state->template_id == 20u);

    std::array<KernelSkeletonRenderState, 4> observer_skeleton_states{};
    std::array<KernelBoneLocalTransform, 164> observer_bone_transforms{};
    KernelSkeletonRenderStateResult observer_skeleton_result{};
    observer_skeleton_result.struct_size = sizeof(observer_skeleton_result);
    require(Kernel_GetSkeletonRenderStates(
                observer_kernel,
                observer_skeleton_states.data(),
                static_cast<std::uint32_t>(observer_skeleton_states.size()),
                observer_bone_transforms.data(),
                static_cast<std::uint32_t>(observer_bone_transforms.size()),
                &observer_skeleton_result) == 1u);
    const auto observer_monster_skeleton_state = std::find_if(
        observer_skeleton_states.begin(),
        observer_skeleton_states.begin() + observer_skeleton_result.written_state_count,
        [observer_monster_net_id](const KernelSkeletonRenderState& state) {
            return state.entity_net_id == observer_monster_net_id;
        });
    require(
        observer_monster_skeleton_state !=
        observer_skeleton_states.begin() +
            observer_skeleton_result.written_state_count);
    require(observer_monster_skeleton_state->skeleton_asset_id == 1u);
    require(
        observer_monster_skeleton_state->skeleton_content_hash ==
        UINT64_C(0x1c171165d9bb479b));
    require(observer_monster_skeleton_state->bone_count == 41u);
    require(
        (observer_monster_skeleton_state->pose_flags &
         KERNEL_SKELETON_POSE_FLAG_PROCEDURAL) != 0u);
    const std::uint32_t observer_first_bone =
        observer_monster_skeleton_state->first_bone_transform;
    for (std::uint32_t bone_index = 0u;
         bone_index < observer_monster_skeleton_state->bone_count;
         ++bone_index) {
        const KernelBoneLocalTransform& transform =
            observer_bone_transforms[observer_first_bone + bone_index];
        require(std::isfinite(transform.local_position.x));
        require(std::isfinite(transform.local_position.y));
        require(std::isfinite(transform.local_position.z));
        require(std::isfinite(transform.local_rotation.x));
        require(std::isfinite(transform.local_rotation.y));
        require(std::isfinite(transform.local_rotation.z));
        require(std::isfinite(transform.local_rotation.w));
        require(std::isfinite(transform.local_scale.x));
        require(std::isfinite(transform.local_scale.y));
        require(std::isfinite(transform.local_scale.z));
    }
    // The pose must be evaluated at the instant the root transform it will be
    // composed onto was evaluated at -- the snapshot-derived time, which lags
    // the newest simulated tick by the interpolation delay -- rather than being
    // stamped with the caller's render time and snapped to the freshest solve.
    // Composing a pose built for tick N onto a root rendered at N-delay is what
    // made a planted foot drift with the body.
    //
    const auto observer_pose_at = [&](std::uint64_t render_time_us,
                                      std::vector<KernelBoneLocalTransform>* out,
                                      KernelSkeletonRenderState* out_state) {
        std::array<KernelSkeletonRenderState, 4> states{};
        std::array<KernelBoneLocalTransform, 164> bones{};
        KernelSkeletonRenderStateResult result{};
        result.struct_size = sizeof(result);
        require(Kernel_GetSkeletonRenderStatesAtTime(
                    observer_kernel,
                    render_time_us,
                    states.data(),
                    static_cast<std::uint32_t>(states.size()),
                    bones.data(),
                    static_cast<std::uint32_t>(bones.size()),
                    &result) == 1u);
        const auto found = std::find_if(
            states.begin(),
            states.begin() + result.written_state_count,
            [observer_monster_net_id](const KernelSkeletonRenderState& state) {
                return state.entity_net_id == observer_monster_net_id;
            });
        require(found != states.begin() + result.written_state_count);
        *out_state = *found;
        out->assign(
            bones.begin() + found->first_bone_transform,
            bones.begin() + found->first_bone_transform + found->bone_count);
    };
    const std::uint64_t observer_tick_us =
        static_cast<std::uint64_t>(1000000.0 / 30.0);
    // Deliberately not tick-aligned, so a pose still stamped with the caller's
    // render time is distinguishable from one stamped with the evaluated time.
    const std::uint64_t observer_requested_render_time_us =
        observer_monster_skeleton_state->pose_time_us + observer_tick_us + 7777u;
    std::vector<KernelBoneLocalTransform> observer_pose_at_time;
    KernelSkeletonRenderState observer_state_at_time{};
    observer_pose_at(
        observer_requested_render_time_us,
        &observer_pose_at_time,
        &observer_state_at_time);
    require(observer_pose_at_time.size() == observer_monster_skeleton_state->bone_count);
    require(
        observer_state_at_time.pose_time_us !=
        observer_requested_render_time_us);
    require(
        observer_state_at_time.pose_time_us < observer_requested_render_time_us);
    for (const KernelBoneLocalTransform& transform : observer_pose_at_time) {
        require(std::isfinite(transform.local_rotation.w));
        require(std::isfinite(transform.local_position.y));
    }

    // ... and it has to be *continuous* in render time, not quantised to the
    // snapshot stream. A listen server shares the server's clock, so advancing
    // the client clock by a fraction of a tick -- too little to simulate a new
    // tick -- must still move the pose. It did not before the loopback was
    // treated as clock-synced: the target was pinned to `newest_tick - delay`,
    // so the body glided while the legs stepped 30 times a second.
    const auto observer_live_pose = [&](std::vector<KernelBoneLocalTransform>* out,
                                        KernelSkeletonRenderState* out_state) {
        std::array<KernelSkeletonRenderState, 4> states{};
        std::array<KernelBoneLocalTransform, 164> bones{};
        KernelSkeletonRenderStateResult result{};
        result.struct_size = sizeof(result);
        require(Kernel_GetSkeletonRenderStates(
                    observer_kernel,
                    states.data(),
                    static_cast<std::uint32_t>(states.size()),
                    bones.data(),
                    static_cast<std::uint32_t>(bones.size()),
                    &result) == 1u);
        const auto found = std::find_if(
            states.begin(),
            states.begin() + result.written_state_count,
            [observer_monster_net_id](const KernelSkeletonRenderState& state) {
                return state.entity_net_id == observer_monster_net_id;
            });
        require(found != states.begin() + result.written_state_count);
        *out_state = *found;
        out->assign(
            bones.begin() + found->first_bone_transform,
            bones.begin() + found->first_bone_transform + found->bone_count);
    };
    std::vector<KernelBoneLocalTransform> observer_pose_before_subtick;
    std::vector<KernelBoneLocalTransform> observer_pose_after_subtick;
    KernelSkeletonRenderState observer_state_before_subtick{};
    KernelSkeletonRenderState observer_state_after_subtick{};
    observer_live_pose(
        &observer_pose_before_subtick, &observer_state_before_subtick);
    // A third of a tick: advances the client clock without simulating a tick.
    Kernel_Update(observer_kernel, 1.0f / 90.0f);
    observer_live_pose(
        &observer_pose_after_subtick, &observer_state_after_subtick);
    require(
        observer_state_after_subtick.pose_time_us >
        observer_state_before_subtick.pose_time_us);
    require(
        observer_pose_before_subtick.size() ==
        observer_pose_after_subtick.size());
    bool observer_pose_moved_within_tick = false;
    for (std::size_t bone = 0u; bone < observer_pose_before_subtick.size();
         ++bone) {
        const KernelQuat& before =
            observer_pose_before_subtick[bone].local_rotation;
        const KernelQuat& after =
            observer_pose_after_subtick[bone].local_rotation;
        if (std::abs(before.x - after.x) > 1e-7f ||
            std::abs(before.y - after.y) > 1e-7f ||
            std::abs(before.z - after.z) > 1e-7f ||
            std::abs(before.w - after.w) > 1e-7f) {
            observer_pose_moved_within_tick = true;
            break;
        }
    }
    require(observer_pose_moved_within_tick);
    const auto patrol_velocity_from_synthetic_position =
        [&](float position_x) {
            KernelVec3 position{
                position_x,
                observer_monster_last_position.y,
                observer_monster_last_position.z,
            };
            KernelQuat rotation{0.0f, 0.0f, 0.0f, 1.0f};
            require(Kernel_ServerSetEntityTransform(
                observer_kernel,
                observer_monster_net_id,
                &position,
                &rotation));
            Kernel_Update(observer_kernel, 1.0f / 30.0f);
            observer_server.tick(1.0f / 30.0f);
            Kernel_Update(observer_kernel, 1.0f / 30.0f);
            KernelServerEntityState state{};
            state.struct_size = sizeof(state);
            require(Kernel_ServerGetEntityState(
                observer_kernel, observer_monster_net_id, &state));
            return state.velocity.x;
        };
    require(patrol_velocity_from_synthetic_position(30.0f) < -0.1f);
    require(patrol_velocity_from_synthetic_position(-30.0f) > 0.1f);
    Kernel_Destroy(observer_kernel);

    KernelHandle* inventory_kernel = make_server_kernel();
    network_example::game_server::GameServer inventory_server(
        inventory_kernel, config);
    assert(Kernel_StartDedicatedServer(inventory_kernel, 7898));
    const std::uint32_t inventory_player = create_player_entity(inventory_kernel);
    KernelEvent player_joined{};
    player_joined.type = KernelEventType_PlayerJoined;
    player_joined.net_id = inventory_player;
    inventory_server.handle_event(player_joined);
    std::array<KernelInventoryContainerView, 2> containers{};
    for (KernelInventoryContainerView& container : containers) {
        container.struct_size = sizeof(KernelInventoryContainerView);
    }
    assert(Kernel_CopyOwnedInventoryContainers(
               inventory_kernel,
               inventory_player,
               containers.data(),
               static_cast<std::uint32_t>(containers.size())) == 1);
    assert(containers[0].slot_capacity == 8);
    assert(containers[0].occupied_slot_count == 3);
    std::array<KernelItemInstanceView, 8> items{};
    for (KernelItemInstanceView& item : items) {
        item.struct_size = sizeof(KernelItemInstanceView);
    }
    assert(Kernel_CopyInventorySlots(
               inventory_kernel,
               containers[0].inventory_container_id,
               items.data(),
               static_cast<std::uint32_t>(items.size())) == 3);
    assert(items[0].slot == 0 && items[0].item_template_id == 3002 &&
           items[0].quantity == 5);
    assert(items[1].slot == 1 && items[1].item_template_id == 3003 &&
           items[1].quantity == 1);
    assert(items[1].portable_state_field_count == 1);
    assert(items[1].portable_state_fields[0].uint32_default == 3);
    assert(items[2].slot == 2 && items[2].item_template_id == 3004 &&
           items[2].quantity == 1);
    assert(items[2].portable_state_field_count == 1);
    assert(items[2].portable_state_fields[0].uint32_default == 1);
    inventory_server.handle_event(player_joined);
    assert(Kernel_CopyOwnedInventoryContainers(
               inventory_kernel,
               inventory_player,
               containers.data(),
               static_cast<std::uint32_t>(containers.size())) == 1);
    assert(Kernel_CopyInventorySlots(
               inventory_kernel,
               containers[0].inventory_container_id,
               items.data(),
               static_cast<std::uint32_t>(items.size())) == 3);
    Kernel_Destroy(inventory_kernel);

    network_example::game_server::GameServerGameplayConfig no_inventory_config =
        config;
    no_inventory_config.actor_templates[0].inventory_slot_capacity = 0;
    no_inventory_config.actor_templates[0].inventory_slots.clear();
    no_inventory_config.entity_templates[0].inventory_slot_capacity = 0;
    no_inventory_config.entity_templates[0].inventory_slots.clear();
    no_inventory_config.weapons.catalog_hash =
        network_example::game_server::compute_gameplay_catalog_hash(
            no_inventory_config);
    assert(network_example::game_server::validate_gameplay_config(
               no_inventory_config).empty());
    KernelHandle* no_inventory_kernel = make_server_kernel();
    network_example::game_server::GameServer no_inventory_server(
        no_inventory_kernel, no_inventory_config);
    assert(Kernel_StartDedicatedServer(no_inventory_kernel, 7899));
    const std::uint32_t no_inventory_player =
        create_player_entity(no_inventory_kernel);
    player_joined.net_id = no_inventory_player;
    no_inventory_server.handle_event(player_joined);
    assert(Kernel_CopyOwnedInventoryContainers(
               no_inventory_kernel,
               no_inventory_player,
               nullptr,
               0) == 0);
    Kernel_Destroy(no_inventory_kernel);

    return 0;
}
