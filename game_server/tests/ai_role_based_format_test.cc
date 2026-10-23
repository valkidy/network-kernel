// P1a of docs/AI_CONTROLLER_PLAN.md: the role-based `ai:` block
// (default_controller / perception / locomotion / controllers) loads to exactly
// the tuning the legacy block (controller / sentry / chaser / patrol) does.
//
// Every shipped agent template is rewritten into the new shape and the two
// catalogs are loaded side by side, so a field the translation drops shows up
// as a mismatch on whichever template authors it. Templates already in the
// new shape (the garrison walkers, P1c) are left as they are.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include <yaml-cpp/yaml.h>

#include "game_server/src/gameplay_config.h"

namespace {

namespace fs = std::filesystem;
namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}
#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

fs::path catalog_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr && test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace / "game_server" / "gameplay_catalog";
}

fs::path scratch_catalog(const std::string& name) {
    const char* tmp = std::getenv("TEST_TMPDIR");
    require(tmp != nullptr);
    const fs::path root = fs::path(tmp) / name;
    fs::remove_all(root);
    fs::copy(catalog_root(), root, fs::copy_options::recursive);
    return root;
}

gs::GameServerGameplayConfig load(const fs::path& root) {
    return gs::load_gameplay_config_from_catalog_file(
        (root / "gameplay_catalog.yaml").string());
}

// The legacy block in the new shape: the inverse of the loader's translation.
YAML::Node role_based(const YAML::Node& legacy) {
    const std::string controller = legacy["controller"].as<std::string>();
    YAML::Node ai(YAML::NodeType::Map);
    ai["default_controller"] = controller == "sentry" ? "sentry" : "garrison";
    for (const char* key : {"profile", "tick_interval"}) {
        if (legacy[key]) ai[key] = YAML::Clone(legacy[key]);
    }
    if (const YAML::Node sentry = legacy["sentry"]) {
        YAML::Node perception(YAML::NodeType::Map);
        for (const auto& entry : sentry) {
            const std::string key = entry.first.as<std::string>();
            if (key == "patrol_rotation_interval_ticks") {
                perception["idle_rotation_interval_ticks"] = YAML::Clone(entry.second);
            } else if (key == "patrol_rotation_min_degrees") {
                perception["idle_rotation_degrees"]["min"] = YAML::Clone(entry.second);
            } else if (key == "patrol_rotation_max_degrees") {
                perception["idle_rotation_degrees"]["max"] = YAML::Clone(entry.second);
            } else {
                perception[key] = YAML::Clone(entry.second);
            }
        }
        ai["perception"] = perception;
    }
    if (const YAML::Node chaser = legacy["chaser"]) {
        ai["locomotion"] = YAML::Clone(chaser);
    }
    if (const YAML::Node patrol = legacy["patrol"]) {
        ai["controllers"]["patrol"] = YAML::Clone(patrol);
    }
    return ai;
}

void write_yaml(const fs::path& path, const YAML::Node& node) {
    YAML::Emitter out;
    out << node;
    std::ofstream file(path);
    file << out.c_str() << "\n";
    require(file.good());
}

// Rewrites every eligible agent template under `root`; returns how many.
int rewrite_agents(const fs::path& root) {
    int rewritten = 0;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(root / "entity_templates")) {
        YAML::Node node = YAML::LoadFile(entry.path().string());
        const YAML::Node ai = node["ai"];
        if (!ai || !ai["controller"]) continue;
        const std::string controller = ai["controller"].as<std::string>();
        if (controller != "sentry" && controller != "chaser") continue;
        node["ai"] = role_based(ai);
        write_yaml(entry.path(), node);
        ++rewritten;
    }
    return rewritten;
}

bool same(float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); }

void same_tuning(const gs::ActorTemplateConfig& a, const gs::ActorTemplateConfig& b) {
    std::printf("  %s\n", a.name.c_str());
    require(a.ai_controller_type == b.ai_controller_type);
    require(a.default_controller_role == b.default_controller_role);
    require(a.ai_stationary == b.ai_stationary);
    require(a.ai_tick_interval == b.ai_tick_interval);

    const gs::AgentSentryConfig& s = a.sentry;
    const gs::AgentSentryConfig& t = b.sentry;
    require(s.alert_ticks == t.alert_ticks);
    require(s.forget_ticks == t.forget_ticks);
    require(s.ballistic_retry_cooldown_ticks == t.ballistic_retry_cooldown_ticks);
    require(s.patrol_rotation_interval_ticks == t.patrol_rotation_interval_ticks);
    require(same(s.patrol_rotation_min_degrees, t.patrol_rotation_min_degrees));
    require(same(s.patrol_rotation_max_degrees, t.patrol_rotation_max_degrees));
    require(same(s.move_speed_meters_per_second, t.move_speed_meters_per_second));
    require(s.weapon_id == t.weapon_id);
    require(s.animation_idle == t.animation_idle);
    require(s.animation_attack == t.animation_attack);

    const gs::AgentChaseTuning& c = a.chaser;
    const gs::AgentChaseTuning& d = b.chaser;
    require(same(c.stop_distance_meters, d.stop_distance_meters));
    require(same(c.resume_distance_meters, d.resume_distance_meters));
    require(same(c.input_magnitude, d.input_magnitude));
    require(same(c.attack_range_meters, d.attack_range_meters));
    require(same(c.leap.min_distance_meters, d.leap.min_distance_meters));
    require(same(c.leap.max_distance_meters, d.leap.max_distance_meters));
    require(same(c.leap.landing_distance_meters, d.leap.landing_distance_meters));
    require(same(c.leap.apex_meters, d.leap.apex_meters));
    require(same(c.leap.max_horizontal_speed_meters_per_second,
                 d.leap.max_horizontal_speed_meters_per_second));
    require(c.leap.cooldown_ticks == d.leap.cooldown_ticks);
    require(c.leap.attack_lead_ticks == d.leap.attack_lead_ticks);
    require(same(c.charge.min_distance_meters, d.charge.min_distance_meters));
    require(same(c.charge.max_distance_meters, d.charge.max_distance_meters));
    require(c.charge.windup_ticks == d.charge.windup_ticks);
    require(same(c.charge.speed_meters_per_second, d.charge.speed_meters_per_second));
    require(same(c.charge.overshoot_meters, d.charge.overshoot_meters));
    require(c.charge.cooldown_ticks == d.charge.cooldown_ticks);
    require(c.charge.weapon_id == d.charge.weapon_id);
    require(c.charge.windup_animation == d.charge.windup_animation);

    require(same(a.patrol.slot_radius_meters, b.patrol.slot_radius_meters));
    require(same(a.patrol.input_magnitude, b.patrol.input_magnitude));
    require(same(a.patrol.leash_meters, b.patrol.leash_meters));
    require(same(a.patrol.leash_resume_meters, b.patrol.leash_resume_meters));
}

void both_shapes_load_the_same_tuning() {
    const gs::GameServerGameplayConfig legacy = load(catalog_root());
    const fs::path root = scratch_catalog("role_based");
    const int rewritten = rewrite_agents(root);
    std::printf("[format] rewrote %d agent templates\n", rewritten);
    // Not vacuous: sentries, grunts, gingerbread, drones, the airship.
    require(rewritten >= 15);
    const gs::GameServerGameplayConfig role_based_config = load(root);

    int compared = 0;
    int sentries = 0;
    int garrisons = 0;
    for (const gs::ActorTemplateConfig& before : legacy.actor_templates) {
        if (before.actor_type != gs::kActorTypeAgent ||
            before.default_controller_role == gs::AgentControllerRole::kNone) {
            continue;
        }
        const gs::ActorTemplateConfig* after =
            gs::find_actor_template(role_based_config, before.actor_template_id);
        require(after != nullptr);
        same_tuning(before, *after);
        ++compared;
        if (before.default_controller_role == gs::AgentControllerRole::kSentry) ++sentries;
        if (before.default_controller_role == gs::AgentControllerRole::kGarrison) ++garrisons;
    }
    std::printf("[format] compared %d (sentry %d, garrison %d)\n",
                compared, sentries, garrisons);
    require(compared >= rewritten);
    require(sentries >= 4 && garrisons >= 14);
}

// The legacy reading, pinned: sentry -> kSentry (always stationary), chaser ->
// kGarrison; and the walkers, authored in the new shape, are garrisons.
void legacy_roles_are_what_the_plan_says(const gs::GameServerGameplayConfig& config) {
    const auto role = [&](std::uint32_t id) {
        const gs::ActorTemplateConfig* t = gs::find_actor_template(config, id);
        require(t != nullptr);
        return t;
    };
    require(role(2)->default_controller_role == gs::AgentControllerRole::kSentry);
    require(role(2)->ai_stationary);
    require(role(21)->default_controller_role == gs::AgentControllerRole::kGarrison);
    require(!role(21)->ai_stationary);
    require(role(28)->default_controller_role == gs::AgentControllerRole::kGarrison);
    require(!role(28)->ai_stationary);
}

// One edit to gingerbread's (28) rewritten ai block must make the catalog
// refuse to load.
void refuses(const char* label, const std::function<void(YAML::Node&)>& edit) {
    const fs::path root = scratch_catalog("refuse");
    const fs::path file = root / "entity_templates" / "28_gingerbread.yaml";
    YAML::Node node = YAML::LoadFile(file.string());
    node["ai"] = role_based(node["ai"]);
    YAML::Node ai = node["ai"];  // a handle onto node's block
    edit(ai);
    write_yaml(file, node);
    bool threw = false;
    try {
        (void)load(root);
    } catch (const std::exception& error) {
        threw = true;
        std::printf("[refused] %s: %s\n", label, error.what());
    }
    require(threw);
}

void malformed_blocks_are_refused() {
    // Control: the unedited rewrite loads, so each refusal below is the edit's.
    {
        const fs::path root = scratch_catalog("control");
        const fs::path file = root / "entity_templates" / "28_gingerbread.yaml";
        YAML::Node node = YAML::LoadFile(file.string());
        node["ai"] = role_based(node["ai"]);
        node["ai"]["default_controller"] = "patrol";
        write_yaml(file, node);
        const gs::GameServerGameplayConfig config = load(root);
        require(gs::find_actor_template(config, 28u)->default_controller_role ==
                gs::AgentControllerRole::kPatrol);
    }
    refuses("mixed", [](YAML::Node& ai) { ai["controller"] = "chaser"; });
    refuses("missing", [](YAML::Node& ai) { ai.remove("default_controller"); });
    refuses("hunter", [](YAML::Node& ai) { ai["default_controller"] = "hunter"; });
    refuses("unknown role", [](YAML::Node& ai) { ai["default_controller"] = "chaser"; });
    refuses("moving stationary", [](YAML::Node& ai) { ai["locomotion"]["stationary"] = true; });
    refuses("sentry with chase", [](YAML::Node& ai) { ai["default_controller"] = "sentry"; });
    refuses("unknown perception key", [](YAML::Node& ai) {
        ai["perception"]["patrol_rotation_interval_ticks"] = 30;
    });
    refuses("garrison key", [](YAML::Node& ai) {
        ai["controllers"]["garrison"]["leash_meters"] = 10.0;
    });
}

}  // namespace

int main() {
    const gs::GameServerGameplayConfig legacy = load(catalog_root());
    legacy_roles_are_what_the_plan_says(legacy);
    both_shapes_load_the_same_tuning();
    malformed_blocks_are_refused();
    std::printf("ai_role_based_format_test passed\n");
    return 0;
}
