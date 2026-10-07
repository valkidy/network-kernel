// apply_block_actions in the catalog: accepted in a status on_apply, onto the
// status's own subject, with no fields of its own -- and refused everywhere
// else, by the loader and by the kernel alike. The runtime half lives in
// //engine/src/tests/simulation_tests:status_action_block_test.
//
// Each case copies the shipped catalog and adds one status and one graph, so
// a refusal can only come from what the case added: the unmodified copy
// loading is the control.

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

constexpr std::uint32_t kStatusId = 1901u;

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

const std::string kBlockGraph =
    "id: action_test_block\n"
    "parameters:\n"
    "  target: null\n"
    "actions:\n"
    "  - type: apply_block_actions\n"
    "    target: params.target\n";

std::string status_yaml(const std::string& trigger, const std::string& target) {
    return "id: " + std::to_string(kStatusId) +
        "\n"
        "name: test_block\n"
        "kind: status_effect\n"
        "channel: test_block\n"
        "duration_ticks: 60\n"
        "interval_ticks: 15\n"
        "replace_policy: replace\n"
        "triggers:\n"
        "  " + trigger + ":\n"
        "    action_graph: action_test_block\n"
        "    parameters:\n"
        "      target: " + target + "\n";
}

// A fresh copy of the shipped catalog, with `files` (relative path -> text)
// added to it. Returns the path of its gameplay_catalog.yaml.
fs::path catalog_with(
    const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& files) {
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

bool kernel_accepts(const gs::KernelGameplayCatalogStorage& storage) {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    const bool loaded = Kernel_LoadGameplayCatalog(kernel, &storage.definition, nullptr);
    Kernel_Destroy(kernel);
    return loaded;
}

const KernelStatusEffectDefinition* find_status(
    const gs::KernelGameplayCatalogStorage& storage) {
    for (const KernelStatusEffectDefinition& status : storage.status_effects) {
        if (status.status_effect_id == kStatusId) {
            return &status;
        }
    }
    return nullptr;
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

void the_unmodified_copy_loads() {
    const fs::path catalog = catalog_with("control", {});
    const gs::GameServerGameplayConfig config =
        gs::load_gameplay_config_from_catalog_file(catalog.string());
    const gs::KernelGameplayCatalogStorage built =
        gs::build_kernel_gameplay_catalog(config);
    require(kernel_accepts(built));
}

void on_apply_onto_its_subject_loads_and_the_kernel_takes_it() {
    for (const std::string target : {"event.subject", "self"}) {
        const fs::path catalog = catalog_with(
            "on_apply_" + target,
            {{"action_graph_templates/action_test_block.yaml", kBlockGraph},
             {"status_effect_templates/1901_status_effect_test_block.yaml",
              status_yaml("on_apply", target)}});
        const gs::GameServerGameplayConfig config =
            gs::load_gameplay_config_from_catalog_file(catalog.string());
        const gs::KernelGameplayCatalogStorage built =
            gs::build_kernel_gameplay_catalog(config);
        const KernelStatusEffectDefinition* status = find_status(built);
        require(status != nullptr);
        require(status->on_apply_trigger.action_count == 1u);
        require(status->on_apply_trigger.actions[0].action_type ==
                KernelEntityTriggerActionType_ApplyBlockActions);
        require(kernel_accepts(built));
    }
}

void any_other_lifecycle_trigger_is_refused() {
    for (const std::string trigger : {"on_tick", "on_expire"}) {
        require(refused_for(
            catalog_with(
                trigger,
                {{"action_graph_templates/action_test_block.yaml", kBlockGraph},
                 {"status_effect_templates/1901_status_effect_test_block.yaml",
                  status_yaml(trigger, "event.subject")}}),
            "apply_block_actions is only valid in status on_apply"));
    }
}

void a_target_other_than_the_subject_is_refused() {
    require(refused_for(
        catalog_with(
            "instigator",
            {{"action_graph_templates/action_test_block.yaml", kBlockGraph},
             {"status_effect_templates/1901_status_effect_test_block.yaml",
              status_yaml("on_apply", "event.instigator")}}),
        "status action block target must be self or event.subject"));
}

void a_field_of_its_own_is_refused() {
    // How long is the status's duration_ticks; the action takes nothing.
    require(refused_for(
        catalog_with(
            "extra_field",
            {{"action_graph_templates/action_test_block.yaml",
              kBlockGraph + "    lockout_ticks: 30\n"},
             {"status_effect_templates/1901_status_effect_test_block.yaml",
              status_yaml("on_apply", "event.subject")}}),
        "apply_block_actions takes only target"));
}

void an_entity_trigger_is_refused() {
    // The same graph on a prop's on_collision: no status instance to hold.
    const std::string prop =
        "id: 1902\n"
        "name: test_block_prop\n"
        "entity_type: prop\n"
        "health:\n"
        "  hp: 3\n"
        "  max_hp: 3\n"
        "physics:\n"
        "  collider_template: rocket_aabb\n"
        "triggers:\n"
        "  on_collision:\n"
        "    action_graph: action_test_block\n"
        "    collision_mask: actor\n"
        "    parameters:\n"
        "      target: event.target\n";
    require(refused_for(
        catalog_with(
            "entity_trigger",
            {{"action_graph_templates/action_test_block.yaml", kBlockGraph},
             {"entity_templates/1902_prop_test_block_prop.yaml", prop}}),
        "apply_block_actions is only valid in status on_apply"));
    // Control: the same prop with a damage graph under the same name loads.
    const std::string damage_graph =
        "id: action_test_block\n"
        "parameters:\n"
        "  target: null\n"
        "  amount: 5\n"
        "actions:\n"
        "  - type: apply_damage\n"
        "    target: params.target\n"
        "    amount: params.amount\n";
    require(load_error(catalog_with(
                "entity_trigger_control",
                {{"action_graph_templates/action_test_block.yaml", damage_graph},
                 {"entity_templates/1902_prop_test_block_prop.yaml", prop}}))
                .empty());
}

}  // namespace

int main() {
    the_unmodified_copy_loads();
    on_apply_onto_its_subject_loads_and_the_kernel_takes_it();
    any_other_lifecycle_trigger_is_refused();
    a_target_other_than_the_subject_is_refused();
    a_field_of_its_own_is_refused();
    an_entity_trigger_is_refused();
    std::puts("status_action_block_catalog_test passed");
    return 0;
}
