// The role-based `ai:` block (docs/AI_CONTROLLER_PLAN.md §6.1, P1d):
// default_controller / perception / locomotion / controllers is the only shape
// an agent template may use. The legacy keys it replaced (controller / sentry /
// chaser / patrol) are refused with a pointer to their replacements; a
// director may still say `controller: director`.
//
// P1a-P1c proved the two shapes load to the same tuning, field by field, over
// every shipped agent; P1d converted the catalog with that check passing and
// the catalog hash unchanged, then removed the legacy shape.

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

gs::GameServerGameplayConfig load(const fs::path& root) {
    return gs::load_gameplay_config_from_catalog_file(
        (root / "gameplay_catalog.yaml").string());
}

void write_yaml(const fs::path& path, const YAML::Node& node) {
    YAML::Emitter out;
    out << node;
    std::ofstream file(path);
    file << out.c_str() << "\n";
    require(file.good());
}

// Every shipped template with an ai block is either a director or authored
// in the role-based shape.
void every_agent_template_is_role_based() {
    int agents = 0;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(catalog_root() / "entity_templates")) {
        const YAML::Node node = YAML::LoadFile(entry.path().string());
        const YAML::Node ai = node["ai"];
        if (!ai) continue;
        if (ai["controller"]) {
            require(ai["controller"].as<std::string>() == "director");
            continue;
        }
        require(ai["default_controller"]);
        ++agents;
    }
    std::printf("[format] %d role-based agent templates\n", agents);
    require(agents >= 21);
}

void roles_are_what_the_plan_says(const gs::GameServerGameplayConfig& config) {
    const auto find = [&](std::uint32_t id) {
        const gs::ActorTemplateConfig* t = gs::find_actor_template(config, id);
        require(t != nullptr);
        return t;
    };
    require(find(2)->default_controller_role == gs::AgentControllerRole::kSentry);
    require(find(2)->ai_stationary);
    require(find(2)->ai_controller_type == KernelAiControllerType_Sentry);
    require(find(21)->default_controller_role == gs::AgentControllerRole::kGarrison);
    require(!find(21)->ai_stationary);
    require(find(28)->default_controller_role == gs::AgentControllerRole::kGarrison);
    require(find(28)->ai_controller_type == KernelAiControllerType_Chaser);
    // gingerbread's controllers.patrol and perception's idle rotation, read
    // through their new names.
    require(find(28)->patrol.input_magnitude == 0.45f);
    require(find(28)->sentry.patrol_rotation_min_degrees == 15.0f);
    require(find(28)->sentry.patrol_rotation_max_degrees == 30.0f);
}

// One edit to gingerbread's (28) ai block; reports whether the catalog loads.
bool loads_after(const char* label, const std::function<void(YAML::Node&)>& edit) {
    const char* tmp = std::getenv("TEST_TMPDIR");
    require(tmp != nullptr);
    const fs::path root = fs::path(tmp) / "edited";
    fs::remove_all(root);
    fs::copy(catalog_root(), root, fs::copy_options::recursive);
    const fs::path file = root / "entity_templates" / "28_gingerbread.yaml";
    YAML::Node node = YAML::LoadFile(file.string());
    YAML::Node ai = node["ai"];  // a handle onto node's block
    edit(ai);
    write_yaml(file, node);
    try {
        (void)load(root);
        std::printf("[loads] %s\n", label);
        return true;
    } catch (const std::exception& error) {
        std::printf("[refused] %s: %s\n", label, error.what());
        return false;
    }
}

void malformed_blocks_are_refused() {
    // Control: an edit that keeps it valid loads, so each refusal below is the
    // edit's and not the copy's.
    require(loads_after("default patrol", [](YAML::Node& ai) {
        ai["default_controller"] = "patrol";
    }));
    for (const char* legacy : {"controller", "sentry", "chaser", "patrol"}) {
        const std::string label = std::string("legacy key ") + legacy;
        require(!loads_after(label.c_str(), [legacy](YAML::Node& ai) {
            if (std::string(legacy) == "controller") {
                ai[legacy] = "chaser";
            } else {
                ai[legacy]["alert_ticks"] = 90;
            }
        }));
    }
    require(!loads_after("missing", [](YAML::Node& ai) { ai.remove("default_controller"); }));
    require(!loads_after("hunter", [](YAML::Node& ai) { ai["default_controller"] = "hunter"; }));
    require(!loads_after("unknown role", [](YAML::Node& ai) { ai["default_controller"] = "chaser"; }));
    require(!loads_after("moving stationary", [](YAML::Node& ai) {
        ai["locomotion"]["stationary"] = true;
    }));
    require(!loads_after("sentry with chase", [](YAML::Node& ai) {
        ai["default_controller"] = "sentry";
        ai.remove("controllers");
    }));
    require(!loads_after("old perception key", [](YAML::Node& ai) {
        ai["perception"]["patrol_rotation_interval_ticks"] = 30;
    }));
    require(!loads_after("unknown rotation key", [](YAML::Node& ai) {
        ai["perception"]["idle_rotation_degrees"]["step"] = 5.0;
    }));
    require(!loads_after("garrison key", [](YAML::Node& ai) {
        ai["controllers"]["garrison"]["leash_meters"] = 10.0;
    }));
}

}  // namespace

int main() {
    every_agent_template_is_role_based();
    roles_are_what_the_plan_says(load(catalog_root()));
    malformed_blocks_are_refused();
    std::printf("ai_role_based_format_test passed\n");
    return 0;
}
