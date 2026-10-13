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
    no_two_shipped_graphs_are_the_same_flow();
    a_graph_in_a_subdirectory_is_loaded();
    the_same_id_in_two_directories_is_refused();
    an_id_that_is_not_its_file_name_is_refused();
    an_id_without_the_action_prefix_is_refused();
    an_id_that_is_not_snake_case_is_refused();
    an_unknown_graph_names_who_referenced_it();
    the_bundle_carries_the_same_graphs();
    return 0;
}
