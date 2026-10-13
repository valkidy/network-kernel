// How the catalog finds and names its action graphs: graphs may sit in
// subdirectories of action_graph_templates (common/, derived/<owner_type>/),
// every id is the file's own name, ids are unique across the whole tree, and
// a reference to a graph that does not exist says who made it. The bundle
// must carry the same graphs the source tree does.
//
// Each case copies the shipped catalog and adds or changes a few files, so a
// refusal can only come from what the case did: the unmodified copy loading
// is the control.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

fs::path runfile(const std::string& relative) {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace / relative;
}

fs::path shipped_catalog() {
    return runfile("game_server/gameplay_catalog");
}

// The copy's files are symlinks into the runfiles tree. Replacing one has to
// drop the link first, or the write would go through it to the shipped file.
void write_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    fs::remove(path);
    std::ofstream file(path);
    require(file.good());
    file << text;
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::string(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string graph_yaml(const std::string& id) {
    return "id: " + id +
        "\n"
        "parameters:\n"
        "  target: null\n"
        "  amount: 1\n"
        "actions:\n"
        "  - type: apply_damage\n"
        "    target: params.target\n"
        "    amount: params.amount\n";
}

// A fresh copy of the shipped catalog, with `files` (relative path -> text)
// written into it. Returns the path of its gameplay_catalog.yaml.
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

// The empty string means "loads".
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

// Every fragment must appear in the error: a refusal for anything else is not
// the rule under test.
bool refused_for(
    const fs::path& catalog,
    const std::vector<std::string>& fragments) {
    const std::string error = load_error(catalog);
    if (error.empty()) {
        std::fprintf(stderr, "expected a refusal, but it loaded\n");
        return false;
    }
    for (const std::string& fragment : fragments) {
        if (error.find(fragment) == std::string::npos) {
            std::fprintf(
                stderr, "expected \"%s\" in \"%s\"\n", fragment.c_str(), error.c_str());
            return false;
        }
    }
    return true;
}

std::vector<std::string> graph_ids(const gs::GameServerGameplayConfig& config) {
    std::vector<std::string> ids;
    for (const gs::ActionGraphTemplateConfig& graph : config.action_graph_templates) {
        ids.push_back(graph.id);
    }
    return ids;
}

bool has_graph(const fs::path& catalog, const std::string& id) {
    const std::vector<std::string> ids = graph_ids(
        gs::load_gameplay_config_from_catalog_file(catalog.string()));
    return std::find(ids.begin(), ids.end(), id) != ids.end();
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

// A graph's text with its id, comments and blank lines dropped: what it does,
// not what it is called.
std::string graph_body(const fs::path& path) {
    std::ifstream file(path);
    require(file.good());
    std::string body;
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t first = line.find_first_not_of(' ');
        if (first == std::string::npos || line[first] == '#' ||
            line.rfind("id:", 0) == 0) {
            continue;
        }
        body += line + "\n";
    }
    return body;
}

// Where a shipped graph sits says what it is: common/ for a flow any owner can
// bind with its own numbers, derived/<owner_type>/ for one owner's own flow.
// Nothing is left loose at the top.
void every_shipped_graph_is_common_or_derived() {
    const fs::path root = shipped_catalog() / "action_graph_templates";
    const std::vector<std::string> owner_types = {
        "projectile", "entity", "prop", "item", "status_effect"};
    std::size_t count = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path relative = entry.path().lexically_relative(root);
        std::vector<std::string> parts;
        for (const fs::path& part : relative) {
            parts.push_back(part.string());
        }
        const bool common = parts.size() == 2u && parts[0] == "common";
        const bool derived = parts.size() == 3u && parts[0] == "derived" &&
            std::find(owner_types.begin(), owner_types.end(), parts[1]) !=
                owner_types.end();
        if (!common && !derived) {
            std::fprintf(stderr, "misplaced graph: %s\n", relative.string().c_str());
            require(false);
        }
        ++count;
    }
    require(count > 0u);
}

void the_unmodified_copy_loads() {
    require(load_error(catalog_with("control", {})).empty());
}

// The shipped catalog, as the kernel would get it, twice: accepted, and
// hashed the same both times.
void the_shipped_catalog_is_accepted_and_hashes_stably() {
    const fs::path catalog = shipped_catalog() / "gameplay_catalog.yaml";
    const gs::GameServerGameplayConfig first =
        gs::load_gameplay_config_from_catalog_file(catalog.string());
    const gs::GameServerGameplayConfig second =
        gs::load_gameplay_config_from_catalog_file(catalog.string());
    require(gs::compute_gameplay_catalog_hash(first) ==
            gs::compute_gameplay_catalog_hash(second));
    require(kernel_accepts(gs::build_kernel_gameplay_catalog(first)));
}

// Two graphs that differ only in their id are one graph: owners that need
// different numbers say so in their bindings, not with a copy.
void no_two_shipped_graphs_are_the_same_flow() {
    std::vector<std::pair<std::string, fs::path>> bodies;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(
             shipped_catalog() / "action_graph_templates")) {
        if (!entry.is_regular_file() || entry.path().extension() != ".yaml") {
            continue;
        }
        const std::string body = graph_body(entry.path());
        for (const auto& [other_body, other_path] : bodies) {
            if (other_body == body) {
                std::fprintf(
                    stderr,
                    "same flow: %s and %s\n",
                    other_path.string().c_str(),
                    entry.path().string().c_str());
                require(false);
            }
        }
        bodies.emplace_back(body, entry.path());
    }
    require(!bodies.empty());
}

void a_graph_in_a_subdirectory_is_loaded() {
    const fs::path catalog = catalog_with(
        "subdirectory",
        {{"action_graph_templates/common/action_test_common.yaml",
          graph_yaml("action_test_common")},
         {"action_graph_templates/derived/projectile/action_test_derived.yaml",
          graph_yaml("action_test_derived")}});
    require(load_error(catalog).empty());
    require(has_graph(catalog, "action_test_common"));
    require(has_graph(catalog, "action_test_derived"));
}

// Not decided by load order: both files are named, whichever comes first.
void the_same_id_in_two_directories_is_refused() {
    require(refused_for(
        catalog_with(
            "duplicate",
            {{"action_graph_templates/common/action_test_twice.yaml",
              graph_yaml("action_test_twice")},
             {"action_graph_templates/derived/prop/action_test_twice.yaml",
              graph_yaml("action_test_twice")}}),
        {"duplicate action graph id: action_test_twice",
         "common/action_test_twice.yaml",
         "derived/prop/action_test_twice.yaml"}));
}

void an_id_that_is_not_its_file_name_is_refused() {
    require(refused_for(
        catalog_with(
            "id_mismatch",
            {{"action_graph_templates/common/action_test_file.yaml",
              graph_yaml("action_test_other")}}),
        {"action graph id must equal its file name",
         "action_test_other",
         "common/action_test_file.yaml"}));
}

void an_id_without_the_action_prefix_is_refused() {
    require(refused_for(
        catalog_with(
            "no_prefix",
            {{"action_graph_templates/common/test_no_prefix.yaml",
              graph_yaml("test_no_prefix")}}),
        {"action graph id must be action_ followed by lowercase snake_case",
         "test_no_prefix"}));
}

void an_id_that_is_not_snake_case_is_refused() {
    require(refused_for(
        catalog_with(
            "camel_case",
            {{"action_graph_templates/common/action_TestCamel.yaml",
              graph_yaml("action_TestCamel")}}),
        {"action graph id must be action_ followed by lowercase snake_case",
         "action_TestCamel"}));
}

// The rocket's own file, with its impact graph renamed to one that does not
// exist: the error names the graph and the projectile that asked for it.
void an_unknown_graph_names_who_referenced_it() {
    const fs::path rocket =
        shipped_catalog() / "projectile_templates" / "3_projectile_rocket.yaml";
    std::string text = read_file(rocket);
    const std::string from = "action_graph: action_spawn_projectile";
    const std::size_t at = text.find(from);
    require(at != std::string::npos);
    text.replace(at, from.size(), "action_graph: action_test_missing");
    require(refused_for(
        catalog_with(
            "unknown_ref",
            {{"projectile_templates/3_projectile_rocket.yaml", text}}),
        {"unknown action_graph reference: action_test_missing",
         "projectile rocket",
         "on_projectile_impact"}));
}

// A prop that runs `graph` with `parameters` (YAML lines, already indented
// for the map) on `trigger`.
std::string prop_yaml(
    std::uint32_t id,
    const std::string& trigger,
    const std::string& graph,
    const std::string& parameters) {
    const bool collision = trigger == "on_collision";
    const bool activated = trigger == "on_activated";
    return "id: " + std::to_string(id) +
        "\n"
        "name: test_prop_" + std::to_string(id) +
        "\n"
        "entity_type: prop\n"
        "health:\n"
        "  hp: 3\n"
        "  max_hp: 3\n"
        "physics:\n"
        "  collider_template: collision_damage_prop_hitbox\n" +
        (activated ? "interaction:\n"
                     "  capabilities: [interactable]\n"
                     "  range: 2.5\n"
                     "  line_of_sight_required: false\n"
                   : "") +
        "triggers:\n"
        "  " + trigger + ":\n" +
        (collision ? "    collision_mask: terrain\n" : "") +
        "    action_graph: " + graph +
        "\n"
        "    parameters:\n" +
        parameters;
}

const KernelEntityTemplateDefinition& compiled_entity(
    const gs::KernelGameplayCatalogStorage& storage,
    std::uint32_t id) {
    for (const KernelEntityTemplateDefinition& entity : storage.entity_templates) {
        if (entity.entity_template_id == id) {
            return entity;
        }
    }
    std::fprintf(stderr, "no compiled entity %u\n", id);
    require(false);
    return storage.entity_templates.front();
}

gs::KernelGameplayCatalogStorage built(const fs::path& catalog) {
    return gs::build_kernel_gameplay_catalog(
        gs::load_gameplay_config_from_catalog_file(catalog.string()));
}

const std::string kPushGraph =
    "id: action_test_push\n"
    "parameters:\n"
    "  target: null\n"
    "  strength: [8.0, 4.0]\n"
    "  direction: null\n"
    "  lockout: 20\n"
    "actions:\n"
    "  - type: apply_impulse\n"
    "    target: params.target\n"
    "    strength: params.strength\n"
    "    direction: params.direction\n"
    "    lockout_ticks: params.lockout\n";

const std::string kPushDefaults =
    "      target: event.target\n"
    "      direction: event.direction\n";

// One graph, two owners, two sets of numbers: neither needs its own copy, and
// the split strength keeps its vertical whichever owner supplies it.
void one_graph_serves_two_owners_with_their_own_numbers() {
    const gs::KernelGameplayCatalogStorage storage = built(catalog_with(
        "two_owners",
        {{"action_graph_templates/common/action_test_push.yaml", kPushGraph},
         {"entity_templates/1901_test_prop.yaml",
          prop_yaml(1901u, "on_collision", "action_test_push", kPushDefaults)},
         {"entity_templates/1902_test_prop.yaml",
          prop_yaml(
              1902u, "on_collision", "action_test_push",
              kPushDefaults +
                  "      strength: [14.0, 6.0]\n"
                  "      lockout: 60\n")}}));
    const KernelActionDefinition& first =
        compiled_entity(storage, 1901u).collision_trigger.actions[0];
    const KernelActionDefinition& second =
        compiled_entity(storage, 1902u).collision_trigger.actions[0];
    require(first.impulse_strength_mode == KERNEL_IMPULSE_STRENGTH_MODE_SPLIT);
    require(first.impulse_strength == 8.0f);
    require(first.impulse_strength_vertical == 4.0f);
    require(first.impulse_lockout_ticks == 20u);
    require(second.impulse_strength_mode == KERNEL_IMPULSE_STRENGTH_MODE_SPLIT);
    require(second.impulse_strength == 14.0f);
    require(second.impulse_strength_vertical == 6.0f);
    require(second.impulse_lockout_ticks == 60u);
}

// A scalar where the graph says [horizontal, vertical] would quietly drop the
// lift; it is refused instead.
void a_scalar_over_a_split_default_is_refused() {
    require(refused_for(
        catalog_with(
            "scalar_over_pair",
            {{"action_graph_templates/common/action_test_push.yaml", kPushGraph},
             {"entity_templates/1901_test_prop.yaml",
              prop_yaml(
                  1901u, "on_collision", "action_test_push",
                  kPushDefaults + "      strength: 12.0\n")}}),
        {"a binding must override it with a pair too"}));
}

void a_bound_lockout_past_the_ceiling_is_refused() {
    require(refused_for(
        catalog_with(
            "lockout_ceiling",
            {{"action_graph_templates/common/action_test_push.yaml", kPushGraph},
             {"entity_templates/1901_test_prop.yaml",
              prop_yaml(
                  1901u, "on_collision", "action_test_push",
                  kPushDefaults + "      lockout: 100000\n")}}),
        {"lockout_ticks must not exceed"}));
}

const std::string kPullGraph =
    "id: action_test_pull\n"
    "parameters:\n"
    "  target: null\n"
    "  anchor: null\n"
    "  distance: 1.0\n"
    "  airtime: 18\n"
    "  max_speed: 12.0\n"
    "  strength: 20.0\n"
    "actions:\n"
    "  - type: apply_pull\n"
    "    target: params.target\n"
    "    anchor: params.anchor\n"
    "    distance: params.distance\n"
    "    airtime_ticks: params.airtime\n"
    "    max_speed: params.max_speed\n"
    "    strength: params.strength\n";

const std::string kPullDefaults =
    "      target: event.target\n"
    "      anchor: event.position\n";

// Every pull number from the binding, with the graph's defaults behind them,
// and the same range check a literal gets.
void pull_numbers_come_from_the_binding() {
    const gs::KernelGameplayCatalogStorage storage = built(catalog_with(
        "pull_bound",
        {{"action_graph_templates/common/action_test_pull.yaml", kPullGraph},
         {"entity_templates/1901_test_prop.yaml",
          prop_yaml(1901u, "on_collision", "action_test_pull", kPullDefaults)},
         {"entity_templates/1902_test_prop.yaml",
          prop_yaml(
              1902u, "on_collision", "action_test_pull",
              kPullDefaults +
                  "      distance: 2.5\n"
                  "      airtime: 24\n"
                  "      max_speed: 9.0\n"
                  "      strength: 10.0\n")}}));
    const KernelActionDefinition& defaults =
        compiled_entity(storage, 1901u).collision_trigger.actions[0];
    const KernelActionDefinition& bound =
        compiled_entity(storage, 1902u).collision_trigger.actions[0];
    require(defaults.pull_distance == 1.0f);
    require(defaults.pull_airtime_ticks == 18u);
    require(defaults.pull_max_speed == 12.0f);
    require(defaults.pull_strength == 20.0f);
    require(bound.pull_distance == 2.5f);
    require(bound.pull_airtime_ticks == 24u);
    require(bound.pull_max_speed == 9.0f);
    require(bound.pull_strength == 10.0f);
}

void a_bound_pull_out_of_range_is_refused() {
    require(refused_for(
        catalog_with(
            "pull_zero_airtime",
            {{"action_graph_templates/common/action_test_pull.yaml", kPullGraph},
             {"entity_templates/1901_test_prop.yaml",
              prop_yaml(
                  1901u, "on_collision", "action_test_pull",
                  kPullDefaults + "      airtime: 0\n")}}),
        {"apply_pull needs airtime_ticks"}));
    require(refused_for(
        catalog_with(
            "pull_not_a_number",
            {{"action_graph_templates/common/action_test_pull.yaml", kPullGraph},
             {"entity_templates/1901_test_prop.yaml",
              prop_yaml(
                  1901u, "on_collision", "action_test_pull",
                  kPullDefaults + "      max_speed: fast\n")}}),
        {"max_speed must be a finite number, got fast"}));
}

void ui_id_comes_from_the_binding() {
    const std::string graph =
        "id: action_test_open_ui\n"
        "parameters:\n"
        "  target: null\n"
        "  ui: null\n"
        "actions:\n"
        "  - type: open_ui\n"
        "    target: params.target\n"
        "    ui_id: params.ui\n";
    const std::string parameters = "      target: event.instigator\n";
    const gs::KernelGameplayCatalogStorage storage = built(catalog_with(
        "ui_bound",
        {{"action_graph_templates/common/action_test_open_ui.yaml", graph},
         {"entity_templates/1901_test_prop.yaml",
          prop_yaml(
              1901u, "on_activated", "action_test_open_ui",
              parameters + "      ui: 3\n")}}));
    require(compiled_entity(storage, 1901u).activated_trigger.actions[0].ui_id == 3u);
    require(refused_for(
        catalog_with(
            "ui_zero",
            {{"action_graph_templates/common/action_test_open_ui.yaml", graph},
             {"entity_templates/1901_test_prop.yaml",
              prop_yaml(
                  1901u, "on_activated", "action_test_open_ui",
                  parameters + "      ui: 0\n")}}),
        {"open_ui ui_id must be non-zero"}));
}

void placement_comes_from_the_binding() {
    const std::string graph =
        "id: action_test_spawn_entity\n"
        "parameters:\n"
        "  template: null\n"
        "  position: null\n"
        "  direction: null\n"
        "  owner: null\n"
        "  placement: exact\n"
        "actions:\n"
        "  - type: spawn_entity\n"
        "    entity_template: params.template\n"
        "    position: params.position\n"
        "    direction: params.direction\n"
        "    owner: params.owner\n"
        "    placement: params.placement\n";
    const std::string parameters =
        "      template: glyph_block\n"
        "      position: event.position\n"
        "      direction: event.direction\n"
        "      owner: self\n";
    const gs::KernelGameplayCatalogStorage storage = built(catalog_with(
        "placement_bound",
        {{"action_graph_templates/common/action_test_spawn_entity.yaml", graph},
         {"entity_templates/1901_test_prop.yaml",
          prop_yaml(1901u, "on_collision", "action_test_spawn_entity", parameters)},
         {"entity_templates/1902_test_prop.yaml",
          prop_yaml(
              1902u, "on_collision", "action_test_spawn_entity",
              parameters + "      placement: clear\n")}}));
    require(compiled_entity(storage, 1901u).collision_trigger.actions[0].spawn_placement ==
            KERNEL_SPAWN_PLACEMENT_EXACT);
    require(compiled_entity(storage, 1902u).collision_trigger.actions[0].spawn_placement ==
            KERNEL_SPAWN_PLACEMENT_CLEAR);
    require(refused_for(
        catalog_with(
            "placement_bad",
            {{"action_graph_templates/common/action_test_spawn_entity.yaml", graph},
             {"entity_templates/1901_test_prop.yaml",
              prop_yaml(
                  1901u, "on_collision", "action_test_spawn_entity",
                  parameters + "      placement: nearby\n")}}),
        {"placement must be exact or clear, got nearby"}));
}

// A graph does not choose who it is tested against or who it moves: the
// owner's collision_mask and the target's force_resistance do. Refused by
// name, not ignored.
void a_graph_collision_mask_is_refused() {
    std::string graph = kPushGraph;
    graph += "    collision_mask: actor\n";
    require(refused_for(
        catalog_with(
            "graph_mask",
            {{"action_graph_templates/common/action_test_push.yaml", graph}}),
        {"action graph actions have no collision_mask", "action_test_push"}));
}

// impulse_resistance became force_resistance (ABI 105); the old key is
// refused by name rather than as an anonymous unknown field.
void the_old_resistance_key_names_the_new_one() {
    std::string tent = read_file(
        shipped_catalog() / "entity_templates" / "216_prop_tent.yaml");
    const std::string from = "force_resistance:";
    const std::size_t at = tent.find(from);
    require(at != std::string::npos);
    tent.replace(at, from.size(), "impulse_resistance:");
    require(refused_for(
        catalog_with(
            "old_resistance_key",
            {{"entity_templates/216_prop_tent.yaml", tent}}),
        {"impulse_resistance", "renamed force_resistance"}));
}

const KernelProjectileMechanicsDefinition& compiled_projectile(
    const gs::KernelGameplayCatalogStorage& storage,
    std::uint32_t id) {
    for (const KernelProjectileTemplateDefinition& projectile :
         storage.projectile_templates) {
        if (projectile.projectile_template_id == id) {
            return projectile.mechanics;
        }
    }
    std::fprintf(stderr, "no compiled projectile %u\n", id);
    require(false);
    return storage.projectile_templates.front().mechanics;
}

// A hit's damage and stagger are the projectile's (ABI 105). The shipped
// numbers, now authored where the damage is: the shockwave and the slam
// stagger on their own, the frag never does, the rocket derives it, and the
// two shoves deal no damage at all.
void hit_damage_and_stagger_live_on_the_projectile() {
    const gs::KernelGameplayCatalogStorage storage =
        built(shipped_catalog() / "gameplay_catalog.yaml");
    const KernelProjectileMechanicsDefinition& rocket = compiled_projectile(storage, 8u);
    require(rocket.damage == 45u);
    require(rocket.damage_falloff == KernelProjectileDamageFalloff_Linear);
    require(rocket.stagger_authored == 0u);
    const KernelProjectileMechanicsDefinition& shockwave = compiled_projectile(storage, 16u);
    require(shockwave.stagger_authored == 1u && shockwave.stagger == 150.0f);
    const KernelProjectileMechanicsDefinition& slam = compiled_projectile(storage, 14u);
    require(slam.damage == 40u);
    require(slam.stagger_authored == 1u && slam.stagger == 120.0f);
    const KernelProjectileMechanicsDefinition& frag = compiled_projectile(storage, 17u);
    require(frag.stagger_authored == 1u && frag.stagger == 0.0f);
    require(frag.projectile_impact_trigger.action_count == 0u);
    for (const std::uint32_t shove : {27u, 29u}) {
        const KernelProjectileMechanicsDefinition& field = compiled_projectile(storage, shove);
        require(field.damage == 0u);
        require(field.damage_shape == KernelProjectileDamageShape_None);
    }
    // The impulse the rocket's explosion binds keeps its lift.
    const KernelActionDefinition& rocket_push = rocket.projectile_impact_trigger.actions[0];
    require(rocket_push.action_type == KernelEntityTriggerActionType_ApplyImpulse);
    require(rocket_push.impulse_strength_mode == KERNEL_IMPULSE_STRENGTH_MODE_SPLIT);
    require(rocket_push.impulse_strength == 12.0f);
    require(rocket_push.impulse_strength_vertical == 5.0f);
    require(rocket_push.impulse_lockout_ticks == 40u);
}

void a_negative_projectile_stagger_is_refused() {
    std::string shockwave = read_file(
        shipped_catalog() / "projectile_templates" / "16_projectile_shockwave_blast.yaml");
    const std::string from = "stagger: 150";
    const std::size_t at = shockwave.find(from);
    require(at != std::string::npos);
    shockwave.replace(at, from.size(), "stagger: -1");
    require(refused_for(
        catalog_with(
            "negative_stagger",
            {{"projectile_templates/16_projectile_shockwave_blast.yaml", shockwave}}),
        {"projectile stagger must be finite and >= 0"}));
}

// apply_status's strength and refill_weapon_reserve's percent come from the
// binding: the water bubble's 10, from the bullet and the balloon alike, and
// the MP potion's 50%.
void status_strength_and_refill_percent_come_from_the_binding() {
    const gs::KernelGameplayCatalogStorage storage =
        built(shipped_catalog() / "gameplay_catalog.yaml");
    const KernelActionDefinition& bullet =
        compiled_projectile(storage, 31u).projectile_impact_trigger.actions[0];
    require(bullet.action_type == KernelEntityTriggerActionType_ApplyStatus);
    require(bullet.status_strength == 10.0f);
    const KernelActionDefinition& balloon =
        compiled_entity(storage, 224u).collision_trigger.actions[0];
    require(balloon.action_type == KernelEntityTriggerActionType_ApplyStatus);
    require(balloon.status_strength == 10.0f);
    bool found_potion = false;
    for (const KernelItemTemplateDefinition& item : storage.item_templates) {
        if (item.item_template_id != 3013u) {
            continue;
        }
        found_potion = true;
        const KernelActionDefinition& refill = item.item_used_trigger.actions[0];
        require(refill.action_type == KernelEntityTriggerActionType_RefillWeaponReserve);
        require(refill.reserve_refill_percent == 50u);
        require(refill.reserve_refill_count == 0u);
    }
    require(found_potion);
}

std::string with_replaced(
    const fs::path& file,
    const std::string& from,
    const std::string& to) {
    std::string text = read_file(file);
    const std::size_t at = text.find(from);
    require(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

void bound_status_strength_and_refill_percent_are_range_checked() {
    require(refused_for(
        catalog_with(
            "status_strength_negative",
            {{"entity_templates/224_prop_water_balloon.yaml",
              with_replaced(
                  shipped_catalog() / "entity_templates" / "224_prop_water_balloon.yaml",
                  "strength: 10.0", "strength: -1.0")}}),
        {"apply_status strength must be finite and >= 0"}));
    for (const char* percent : {"percent: 0", "percent: 101"}) {
        require(refused_for(
            catalog_with(
                "refill_percent_out_of_range",
                {{"item_templates/3013_fungible_mp_potion.yaml",
                  with_replaced(
                      shipped_catalog() / "item_templates" / "3013_fungible_mp_potion.yaml",
                      "percent: 50", percent)}}),
            {"refill_weapon_reserve percent must be 1 to 100"}));
    }
}

// spawn_projectile's lifetime and repeat from the binding: the meteor storm
// marker's 10-15 fuses, 6 m scatter, 10 + 60 ticks.
void spawn_repeat_comes_from_the_binding() {
    const gs::KernelGameplayCatalogStorage storage =
        built(shipped_catalog() / "gameplay_catalog.yaml");
    const KernelActionDefinition& spawn =
        compiled_projectile(storage, 21u).expired_trigger.actions[0];
    require(spawn.action_type == KernelEntityTriggerActionType_SpawnProjectile);
    require(spawn.spawn_lifetime_ticks == 10u);
    require(spawn.repeat_count_min == 10u && spawn.repeat_count_max == 15u);
    require(spawn.repeat_scatter_radius == 6.0f);
    require(spawn.repeat_stagger_lifetime_ticks == 60u);

    const fs::path marker =
        shipped_catalog() / "projectile_templates" / "21_projectile_meteor_storm_marker.yaml";
    for (const char* count : {"count: [15, 10]", "count: 0", "count: 17"}) {
        require(refused_for(
            catalog_with(
                "repeat_count_out_of_range",
                {{"projectile_templates/21_projectile_meteor_storm_marker.yaml",
                  with_replaced(marker, "count: [10, 15]", count)}}),
            {"spawn_projectile repeat count must satisfy"}));
    }
    require(refused_for(
        catalog_with(
            "lifetime_zero",
            {{"projectile_templates/21_projectile_meteor_storm_marker.yaml",
              with_replaced(marker, "lifetime: 10", "lifetime: 0")}}),
        {"spawn_projectile lifetime_ticks must be at least 1"}));
}

// The archive the client loads must hold every graph the source tree does,
// subdirectories included: the bundle rule copies files by path.
void the_bundle_carries_the_same_graphs() {
    const std::string bytes =
        read_file(runfile("game_server/gameplay_catalog_bundle/bundle.zip"));
    const gs::GameServerGameplayConfig from_bundle =
        gs::load_gameplay_config_from_bundle_memory(
            reinterpret_cast<const std::uint8_t*>(bytes.data()),
            static_cast<std::uint32_t>(bytes.size()),
            "gameplay_catalog.yaml");
    const gs::GameServerGameplayConfig from_tree =
        gs::load_gameplay_config_from_catalog_file(
            (shipped_catalog() / "gameplay_catalog.yaml").string());
    require(!graph_ids(from_tree).empty());
    require(graph_ids(from_bundle) == graph_ids(from_tree));
}

}  // namespace

int main() {
    the_unmodified_copy_loads();
    the_shipped_catalog_is_accepted_and_hashes_stably();
    every_shipped_graph_is_common_or_derived();
    no_two_shipped_graphs_are_the_same_flow();
    a_graph_in_a_subdirectory_is_loaded();
    the_same_id_in_two_directories_is_refused();
    an_id_that_is_not_its_file_name_is_refused();
    an_id_without_the_action_prefix_is_refused();
    an_id_that_is_not_snake_case_is_refused();
    an_unknown_graph_names_who_referenced_it();
    one_graph_serves_two_owners_with_their_own_numbers();
    a_scalar_over_a_split_default_is_refused();
    a_bound_lockout_past_the_ceiling_is_refused();
    pull_numbers_come_from_the_binding();
    a_bound_pull_out_of_range_is_refused();
    ui_id_comes_from_the_binding();
    placement_comes_from_the_binding();
    the_old_resistance_key_names_the_new_one();
    a_graph_collision_mask_is_refused();
    hit_damage_and_stagger_live_on_the_projectile();
    a_negative_projectile_stagger_is_refused();
    status_strength_and_refill_percent_come_from_the_binding();
    bound_status_strength_and_refill_percent_are_range_checked();
    spawn_repeat_comes_from_the_binding();
    the_bundle_carries_the_same_graphs();
    return 0;
}
