// weapon_type: lock_instant in the catalog. It authors its cone as
// lock_collider and its lock size as the shot template's max_hit_count, and
// the loader holds it to two rules the runtime depends on:
//
//   - its fire action spends exactly max_hit_count per commit, and the
//     magazine holds whole locks -- every commit spends a full lock whether or
//     not it found that many targets;
//   - its fire action is its own: a client tells which weapon a remote shooter
//     fired from the fire action in the snapshot.
//
// The catalog also carries its weapons into the kernel (ABI 103), so a pure
// client can read the cone back.
//
// Each case copies the shipped catalog and adds one lock weapon, so a refusal
// can only come from what the case added: the plain lock weapon loading is the
// control.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace {

namespace fs = std::filesystem;
namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr std::uint8_t kLockWeapon = 200u;
constexpr std::uint8_t kSecondWeapon = 201u;
constexpr std::uint32_t kConeTemplate = 200u;
constexpr std::uint32_t kShotTemplate = 200u;
constexpr std::uint32_t kFireAction = 4900u;

fs::path shipped_catalog() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace / "game_server" / "gameplay_catalog";
}

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream file(path);
    require(file.good());
    file << text;
}

const std::string kCone =
    "id: 200\n"
    "name: test_lock_cone\n"
    "shape: cone\n"
    "range: 18.0\n"
    "fov_degrees: 50.0\n"
    "purpose: damage\n"
    "layer: projectile\n";

const std::string kShot =
    "id: 200\n"
    "name: test_lock_shot\n"
    "type: standard\n"
    "movement_model: linear\n"
    "sync_mode: server_snapshot_only\n"
    "hit_response: destroy\n"
    "damage_shape: direct_hit\n"
    "damage: 12\n"
    "collision_mask: hostile_side\n"
    "max_hit_count: 6\n"
    "speed: 0.1\n"
    "lifetime_ticks: 3\n"
    "gravity: {x: 0.0, y: 0.0, z: 0.0}\n";

std::string fire_action(std::uint32_t ammo_cost) {
    return "id: 4900\n"
           "name: test_lock_fire\n"
           "trigger_mode: hold\n"
           "flags: [cancel_on_release, cancel_on_death, cancel_on_weapon_change]\n"
           "ammo_cost_per_commit: " + std::to_string(ammo_cost) + "\n"
           "commit_offset_ticks: 0\n"
           "commit_interval_ticks: 6\n"
           "max_commit_count: 0\n"
           "recovery_ticks: 4\n"
           "hold_input_timeout_ticks: 6\n";
}

const std::string kReloadAction =
    "id: 4901\n"
    "name: test_lock_reload\n"
    "trigger_mode: press\n"
    "flags: [cancel_on_death, cancel_on_weapon_change, cancel_before_first_commit]\n"
    "ammo_cost_per_commit: 0\n"
    "commit_offset_ticks: 36\n"
    "commit_interval_ticks: 0\n"
    "max_commit_count: 1\n"
    "recovery_ticks: 0\n"
    "hold_input_timeout_ticks: 0\n";

std::string lock_weapon(std::uint32_t magazine, bool with_cone = true) {
    return "id: 200\n"
           "name: Test Lock\n"
           "weapon_type: lock_instant\n"
           "magazine_size: " + std::to_string(magazine) + "\n"
           "fire_action_template: test_lock_fire\n"
           "reload_action_template: test_lock_reload\n" +
           (with_cone ? std::string("lock_collider: test_lock_cone\n") : std::string()) +
           "projectile_template: test_lock_shot\n";
}

// Any other weapon naming the lock weapon's fire action.
const std::string kSharingWeapon =
    "id: 201\n"
    "name: Test Sharer\n"
    "weapon_type: hitscan\n"
    "magazine_size: 30\n"
    "max_range: 50.0\n"
    "fire_action_template: test_lock_fire\n"
    "reload_action_template: test_lock_reload\n"
    "segment_collider: rifle_segment\n"
    "projectile_template: test_sharer_shot\n";

const std::string kSharerShot =
    "id: 201\n"
    "name: test_sharer_shot\n"
    "type: standard\n"
    "movement_model: linear\n"
    "sync_mode: server_snapshot_only\n"
    "hit_response: destroy\n"
    "damage_shape: direct_hit\n"
    "damage: 5\n"
    "collision_mask: hostile_side\n"
    "speed: 0.1\n"
    "lifetime_ticks: 3\n"
    "gravity: {x: 0.0, y: 0.0, z: 0.0}\n";

using Files = std::vector<std::pair<std::string, std::string>>;

Files lock_files(std::uint32_t ammo_cost, std::uint32_t magazine, bool with_cone = true) {
    return {
        {"collider_templates/200_collider_test_lock_cone.yaml", kCone},
        {"projectile_templates/200_projectile_test_lock_shot.yaml", kShot},
        {"action_templates/test_lock_fire.yaml", fire_action(ammo_cost)},
        {"action_templates/test_lock_reload.yaml", kReloadAction},
        {"weapon_templates/200_weapon_test_lock.yaml", lock_weapon(magazine, with_cone)},
    };
}

// A fresh copy of the shipped catalog, with `files` (relative path -> text)
// added to it. Returns the path of its gameplay_catalog.yaml.
fs::path catalog_with(const std::string& name, const Files& files) {
    const char* tmp = std::getenv("TEST_TMPDIR");
    require(tmp != nullptr);
    const fs::path root = fs::path(tmp) / name;
    fs::remove_all(root);
    fs::copy(
        shipped_catalog(),
        root,
        fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    for (const auto& [relative, text] : files) {
        write_file(root / relative, text);
    }
    return root / "gameplay_catalog.yaml";
}

// The empty string means "loads". Otherwise the load must fail, and for the
// reason given: a refusal for anything else is not the rule under test.
std::string load_error(const fs::path& catalog) {
    try {
        const gs::GameServerGameplayConfig config =
            gs::load_gameplay_config_from_catalog_file(catalog.string());
        (void)gs::build_kernel_gameplay_catalog(config);
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

bool refused_for(const fs::path& catalog, const std::string& reason) {
    const std::string error = load_error(catalog);
    if (error.find(reason) == std::string::npos) {
        std::fprintf(stderr, "expected \"%s\", got \"%s\"\n", reason.c_str(), error.c_str());
        return false;
    }
    return true;
}

void a_lock_weapon_loads_and_a_pure_client_reads_its_cone() {
    const fs::path catalog = catalog_with("plain", lock_files(6u, 24u));
    const gs::GameServerGameplayConfig config =
        gs::load_gameplay_config_from_catalog_file(catalog.string());
    require(config.weapons.configured[kLockWeapon]);
    const KernelWeaponMechanicsDefinition& authored =
        config.weapons.definitions[kLockWeapon];
    require(authored.fire_mode == KernelWeaponFireMode_LockInstant);
    require(authored.melee_collider_template_id == kConeTemplate);
    require(authored.projectile_template_id == kShotTemplate);
    require(authored.fire_action_template_id == kFireAction);
    require(authored.damage == 12);
    require(authored.collision_mask == KERNEL_COLLISION_LAYER_HOSTILE_SIDE);

    // The whole shipped catalog's weapons go to the kernel with it, so this is
    // also the check that every one of them passes the kernel's validation.
    const gs::KernelGameplayCatalogStorage storage =
        gs::build_kernel_gameplay_catalog(config);
    require(storage.definition.weapon_definition_count > 1u);

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_Client;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    KernelHandle* client = Kernel_Create(&kernel_config);
    require(client != nullptr);
    require(Kernel_LoadGameplayCatalog(client, &storage.definition, nullptr));
    KernelWeaponMechanicsDefinition read{};
    read.struct_size = sizeof(read);
    require(Kernel_GetCatalogWeaponDefinition(client, kLockWeapon, &read));
    require(read.fire_mode == KernelWeaponFireMode_LockInstant);
    require(read.melee_collider_template_id == kConeTemplate);
    Kernel_Destroy(client);
}

void a_lock_weapon_must_spend_its_whole_lock_per_commit() {
    require(refused_for(
        catalog_with("cost", lock_files(1u, 24u)),
        "ammo_cost_per_commit must equal the shot template's max_hit_count"));
}

void a_lock_weapon_magazine_must_hold_whole_locks() {
    require(refused_for(
        catalog_with("magazine", lock_files(6u, 20u)),
        "magazine_size must be a multiple of the shot template's max_hit_count"));
}

void a_lock_weapon_needs_its_cone() {
    require(refused_for(
        catalog_with("no_cone", lock_files(6u, 24u, false)),
        "lock_instant weapon requires lock_collider"));
}

// The missile variant: the same cone and shot, whose lifetime_ticks is the
// longest flight and travels in a byte.
Files missile_files(std::uint32_t lifetime) {
    Files files = lock_files(1u, 24u);
    std::string weapon = lock_weapon(24u);
    weapon.replace(
        weapon.find("weapon_type: lock_instant"),
        std::string("weapon_type: lock_instant").size(),
        "weapon_type: lock_missile");
    files[4].second = weapon;
    std::string shot = kShot;
    shot.replace(
        shot.find("lifetime_ticks: 3"),
        std::string("lifetime_ticks: 3").size(),
        "lifetime_ticks: " + std::to_string(lifetime));
    files[1].second = shot;
    return files;
}

void a_lock_missile_loads_with_its_flight_on_the_shot() {
    const fs::path catalog = catalog_with("missile", missile_files(30u));
    const gs::GameServerGameplayConfig config =
        gs::load_gameplay_config_from_catalog_file(catalog.string());
    const KernelWeaponMechanicsDefinition& authored =
        config.weapons.definitions[kLockWeapon];
    require(authored.fire_mode == KernelWeaponFireMode_LockMissile);
    require(authored.melee_collider_template_id == kConeTemplate);
    require(authored.projectile_template_id == kShotTemplate);
    const gs::KernelGameplayCatalogStorage storage =
        gs::build_kernel_gameplay_catalog(config);
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_Client;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    KernelHandle* client = Kernel_Create(&kernel_config);
    require(client != nullptr);
    require(Kernel_LoadGameplayCatalog(client, &storage.definition, nullptr));
    KernelWeaponMechanicsDefinition read{};
    read.struct_size = sizeof(read);
    require(Kernel_GetCatalogWeaponDefinition(client, kLockWeapon, &read));
    require(read.fire_mode == KernelWeaponFireMode_LockMissile);
    Kernel_Destroy(client);
}

void a_lock_missile_flight_must_fit_a_byte() {
    require(refused_for(
        catalog_with("missile_long", missile_files(256u)),
        "lifetime_ticks is its longest flight and must be 1 to 255"));
}

void a_lock_weapon_fire_action_cannot_be_shared() {
    Files files = lock_files(6u, 24u);
    files.emplace_back("weapon_templates/201_weapon_test_sharer.yaml", kSharingWeapon);
    files.emplace_back(
        "projectile_templates/201_projectile_test_sharer_shot.yaml", kSharerShot);
    require(refused_for(
        catalog_with("shared", files),
        "a lock-on weapon's fire_action_template cannot be shared"));
    (void)kSecondWeapon;
}

}  // namespace

int main() {
    a_lock_weapon_loads_and_a_pure_client_reads_its_cone();
    a_lock_weapon_must_spend_its_whole_lock_per_commit();
    a_lock_weapon_magazine_must_hold_whole_locks();
    a_lock_weapon_needs_its_cone();
    a_lock_weapon_fire_action_cannot_be_shared();
    a_lock_missile_loads_with_its_flight_on_the_shot();
    a_lock_missile_flight_must_fit_a_byte();
    std::puts("lock_weapon_catalog_test passed");
    return 0;
}
