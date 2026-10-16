// lock_strike authored in YAML (docs/LOCK_STRIKE_ACTION_PLAN.md, P2): a prop's
// on_collision binds action_lock_strike_and_damage_self and the loader
// compiles it into the kernel's lock_strike, which the kernel then accepts. A
// lock_strike the kernel would refuse is refused here first, naming its graph,
// rather than failing the whole catalog in the kernel.

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

constexpr std::uint32_t kSphere = 200u;
constexpr std::uint32_t kCone = 201u;
constexpr std::uint32_t kShot = 200u;
constexpr std::uint32_t kBottle = 900u;

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

using Files = std::vector<std::pair<std::string, std::string>>;

const std::string kSphereCollider =
    "id: 200\n"
    "name: test_lock_sphere\n"
    "shape: sphere\n"
    "center: {x: 0.0, y: 0.0, z: 0.0}\n"
    "radius: 8.0\n"
    "purpose: damage\n"
    "layer: area_effect\n";

const std::string kConeCollider =
    "id: 201\n"
    "name: test_lock_cone\n"
    "shape: cone\n"
    "range: 18.0\n"
    "fov_degrees: 50.0\n"
    "purpose: damage\n"
    "layer: projectile\n";

std::string shot(std::uint32_t max_hit_count, std::uint32_t lifetime_ticks) {
    return "id: 200\n"
           "name: test_lock_bolt\n"
           "type: standard\n"
           "movement_model: linear\n"
           "sync_mode: server_snapshot_only\n"
           "hit_response: destroy\n"
           "damage_shape: direct_hit\n"
           "damage: 10\n"
           "collision_mask: hostile_side\n"
           "max_hit_count: " + std::to_string(max_hit_count) + "\n"
           "speed: 0.1\n"
           "lifetime_ticks: " + std::to_string(lifetime_ticks) + "\n"
           "gravity: {x: 0.0, y: 0.0, z: 0.0}\n";
}

// A bottle that strikes where it breaks. `extra` is appended to its binding's
// parameters.
std::string bottle(
    const std::string& graph,
    const std::string& collider,
    const std::string& extra) {
    return "id: 900\n"
           "name: test_lock_bottle\n"
           "entity_type: prop\n"
           "health:\n"
           "  hp: 1\n"
           "  max_hp: 1\n"
           "physics:\n"
           "  collider_template: collision_damage_prop_hitbox\n"
           "triggers:\n"
           "  on_collision:\n"
           "    collision_mask: terrain | static_obstacle | actor\n"
           "    action_graph: " + graph + "\n"
           "    parameters:\n"
           "      collider: " + collider + "\n"
           "      template: test_lock_bolt\n"
           "      position: event.position\n"
           "      direction: event.direction\n"
           "      source: self\n" +
           extra;
}

Files files(const std::string& bottle_text, const std::string& shot_text) {
    return {
        {"collider_templates/200_collider_test_lock_sphere.yaml", kSphereCollider},
        {"collider_templates/201_collider_test_lock_cone.yaml", kConeCollider},
        {"projectile_templates/200_projectile_test_lock_bolt.yaml", shot_text},
        {"entity_templates/900_prop_test_lock_bottle.yaml", bottle_text},
    };
}

// A fresh copy of the shipped catalog with `added` written into it. Returns
// the path of its gameplay_catalog.yaml.
fs::path catalog_with(const std::string& name, const Files& added) {
    const char* tmp = std::getenv("TEST_TMPDIR");
    require(tmp != nullptr);
    const fs::path root = fs::path(tmp) / name;
    fs::remove_all(root);
    fs::copy(
        shipped_catalog(),
        root,
        fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    for (const auto& [relative, text] : added) {
        write_file(root / relative, text);
    }
    return root / "gameplay_catalog.yaml";
}

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
        std::fprintf(
            stderr, "expected \"%s\", got \"%s\"\n", reason.c_str(), error.c_str());
        return false;
    }
    return true;
}

const KernelEntityTemplateDefinition* find_bottle(
    const gs::KernelGameplayCatalogStorage& storage) {
    for (std::uint32_t index = 0;
         index < storage.definition.entity_template_count;
         ++index) {
        if (storage.definition.entity_templates[index].entity_template_id ==
            kBottle) {
            return &storage.definition.entity_templates[index];
        }
    }
    return nullptr;
}

// The shared graph compiles to a lock_strike on the sphere and the shot, with
// the default lift and the mode the binding names, followed by the bottle
// spending itself -- and the kernel takes the catalog.
void a_bottle_compiles_to_a_lock_strike_the_kernel_accepts() {
    const fs::path catalog = catalog_with(
        "plain",
        files(
            bottle(
                "action_lock_strike_and_damage_self",
                "test_lock_sphere",
                "      mode: missile\n"),
            shot(4u, 20u)));
    const gs::GameServerGameplayConfig config =
        gs::load_gameplay_config_from_catalog_file(catalog.string());
    const gs::KernelGameplayCatalogStorage storage =
        gs::build_kernel_gameplay_catalog(config);
    const KernelEntityTemplateDefinition* compiled = find_bottle(storage);
    require(compiled != nullptr);
    const KernelActionTriggerDefinition& trigger = compiled->collision_trigger;
    require(trigger.action_count == 2u);
    const KernelActionDefinition& strike = trigger.actions[0];
    require(strike.action_type == KernelEntityTriggerActionType_LockStrike);
    require(strike.lock_collider_template_id == kSphere);
    require(strike.spawn_projectile_template_id == kShot);
    require(strike.lock_mode == KernelLockStrikeMode_Missile);
    require(strike.lock_origin_lift == 1.0f);
    require(strike.lock_repeat_count == 0u);
    require(strike.position_source == KernelEventVec3Source_Position);
    require(strike.direction_source == KernelEventVec3Source_Direction);
    require(trigger.actions[1].action_type ==
            KernelEntityTriggerActionType_ApplyDamage);

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(Kernel_LoadGameplayCatalog(kernel, &storage.definition, nullptr));
    Kernel_Destroy(kernel);
}

// A cone aims along the event's direction; the default mode is instant; a
// lift may be authored.
void a_cone_and_a_lift_compile() {
    const fs::path catalog = catalog_with(
        "cone",
        files(
            bottle(
                "action_lock_strike_and_damage_self",
                "test_lock_cone",
                "      origin_lift: 2.5\n"),
            shot(4u, 20u)));
    const gs::KernelGameplayCatalogStorage storage =
        gs::build_kernel_gameplay_catalog(
            gs::load_gameplay_config_from_catalog_file(catalog.string()));
    const KernelActionDefinition& strike =
        find_bottle(storage)->collision_trigger.actions[0];
    require(strike.lock_collider_template_id == kCone);
    require(strike.lock_mode == KernelLockStrikeMode_Instant);
    require(strike.lock_origin_lift == 2.5f);
}

void what_the_kernel_would_refuse_is_refused_here() {
    require(refused_for(
        catalog_with(
            "mode",
            files(
                bottle(
                    "action_lock_strike_and_damage_self",
                    "test_lock_sphere",
                    "      mode: sometimes\n"),
                shot(4u, 20u))),
        "lock_strike mode must be instant or missile"));
    require(refused_for(
        catalog_with(
            "lift",
            files(
                bottle(
                    "action_lock_strike_and_damage_self",
                    "test_lock_sphere",
                    "      origin_lift: 6.0\n"),
                shot(4u, 20u))),
        "lock_strike origin_lift must be 0 to 5 m"));
    // Not a lock volume: the bottle's own hit box.
    require(refused_for(
        catalog_with(
            "volume",
            files(
                bottle(
                    "action_lock_strike_and_damage_self",
                    "collision_damage_prop_hitbox",
                    ""),
                shot(4u, 20u))),
        "lock_strike collider must be a damage cone or sphere"));
    require(refused_for(
        catalog_with(
            "wide",
            files(
                bottle(
                    "action_lock_strike_and_damage_self",
                    "test_lock_sphere",
                    ""),
                shot(17u, 20u))),
        "must lock at most 16"));
    require(refused_for(
        catalog_with(
            "flight",
            files(
                bottle(
                    "action_lock_strike_and_damage_self",
                    "test_lock_sphere",
                    "      mode: missile\n"),
                shot(4u, 300u))),
        "lifetime_ticks must be 1..255"));
    // A cone with no heading: a graph that never binds direction.
    Files headless = files(
        bottle("action_test_lock_strike_headless", "test_lock_cone", ""),
        shot(4u, 20u));
    headless.emplace_back(
        "action_graph_templates/common/action_test_lock_strike_headless.yaml",
        "id: action_test_lock_strike_headless\n"
        "parameters:\n"
        "  collider: null\n"
        "  template: null\n"
        "  position: null\n"
        "  direction: null\n"
        "  source: null\n"
        "actions:\n"
        "  - type: lock_strike\n"
        "    collider: params.collider\n"
        "    projectile_template: params.template\n"
        "    mode: instant\n"
        "    position: params.position\n");
    require(refused_for(
        catalog_with("headless", headless),
        "lock_strike with a cone needs a direction"));
}

}  // namespace

int main() {
    a_bottle_compiles_to_a_lock_strike_the_kernel_accepts();
    a_cone_and_a_lift_compile();
    what_the_kernel_would_refuse_is_refused_here();
    std::puts("lock_strike_catalog_test passed");
    return 0;
}
