// Patrol definitions as the catalog authors them: one file per definition under
// patrol_template_dir, and `patrols: - ids: [...]` picking which ones run.
//
// Each case loads the shipping catalog with one or two files rewritten, so the
// real loader runs on real templates.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "game_server/src/gameplay_config.h"

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

namespace fs = std::filesystem;
using network_example::game_server::GameServerGameplayConfig;

constexpr const char* kCatalogDir = "game_server/gameplay_catalog";
constexpr const char* kWarbandFile =
    "patrol_templates/1_patrol_gingerbread_warband.yaml";
constexpr const char* kShippedIds = "  - ids: [1, 2]\n";

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good());
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(stream.good());
    stream << text;
}

// `from` replaced by `to`. It has to be there: a needle that has gone would
// otherwise quietly test the shipping file instead.
std::string replace(
    const std::string& text, const std::string& from, const std::string& to) {
    const std::size_t start = text.find(from);
    require(start != std::string::npos);
    return text.substr(0, start) + to + text.substr(start + from.size());
}

class CatalogCopy {
public:
    CatalogCopy() {
        const char* tmp = std::getenv("TEST_TMPDIR");
        root_ = fs::path(tmp != nullptr ? tmp : "/tmp") / "patrol_catalog";
        fs::remove_all(root_);
        fs::copy(kCatalogDir, root_, fs::copy_options::recursive);
        catalog_ = read_file(root_ / "gameplay_catalog.yaml");
    }

    const fs::path& root() const { return root_; }
    const std::string& catalog() const { return catalog_; }

    // Loads with the catalog's ids line replaced, then puts the shipping
    // catalog back.
    GameServerGameplayConfig load_with_ids(const std::string& ids_line) {
        write_file(
            root_ / "gameplay_catalog.yaml",
            replace(catalog_, kShippedIds, ids_line));
        GameServerGameplayConfig config =
            network_example::game_server::load_gameplay_config_from_catalog_file(
                (root_ / "gameplay_catalog.yaml").string());
        write_file(root_ / "gameplay_catalog.yaml", catalog_);
        return config;
    }

    // The message, or empty if it loaded.
    std::string rejection(const std::string& ids_line) {
        try {
            (void)load_with_ids(ids_line);
        } catch (const std::exception& error) {
            write_file(root_ / "gameplay_catalog.yaml", catalog_);
            return error.what();
        }
        return {};
    }

private:
    fs::path root_;
    std::string catalog_;
};

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

void the_shipping_catalog_runs_both_patrols() {
    CatalogCopy catalog;
    const GameServerGameplayConfig config = catalog.load_with_ids(kShippedIds);
    require(config.patrols.size() == 2u);
    require(config.patrols[0].id == 1u);
    require(config.patrols[0].name == "gingerbread_warband");
    require(config.patrols[1].id == 2u);
    require(config.patrols[1].name == "gingerbread_airforce");
    require(!config.patrols[0].composition.empty());
}

void listed_order_is_run_order() {
    CatalogCopy catalog;
    const GameServerGameplayConfig config =
        catalog.load_with_ids("  - ids: [2, 1]\n");
    require(config.patrols.size() == 2u);
    require(config.patrols[0].id == 2u);
    require(config.patrols[1].id == 1u);
}

void only_listed_ids_run() {
    CatalogCopy catalog;
    const GameServerGameplayConfig one = catalog.load_with_ids("  - ids: [2]\n");
    require(one.patrols.size() == 1u);
    require(one.patrols[0].id == 2u);
    const GameServerGameplayConfig none = catalog.load_with_ids("  - ids: []\n");
    require(none.patrols.empty());
    // Several entries add up.
    const GameServerGameplayConfig split =
        catalog.load_with_ids("  - ids: [1]\n  - ids: [2]\n");
    require(split.patrols.size() == 2u);
}

void misauthored_lists_are_refused() {
    CatalogCopy catalog;
    require(contains(
        catalog.rejection("  - ids: [3]\n"),
        "patrol id 3 is listed but no file under patrol_template_dir"));
    require(contains(
        catalog.rejection("  - ids: [1, 1]\n"), "patrol id 1 is listed twice"));
    require(contains(
        catalog.rejection("  - ids: [1]\n  - ids: [1]\n"),
        "patrol id 1 is listed twice"));
    require(contains(
        catalog.rejection("  - id: 1\n    name: gingerbread_warband\n"),
        "patrols lists ids now"));
}

// A definition that does not load fails the catalog even while it is not
// listed, and a file whose name disagrees with what it holds is refused.
void every_file_is_validated_listed_or_not() {
    CatalogCopy catalog;
    const std::string warband = read_file(catalog.root() / kWarbandFile);

    write_file(
        catalog.root() / kWarbandFile,
        replace(warband, "max_live_groups: 2\n", "max_live_groups: 2\nbogus: 1\n"));
    require(!catalog.rejection("  - ids: [2]\n").empty());
    write_file(catalog.root() / kWarbandFile, warband);

    // A copy whose id was never changed.
    const fs::path copy =
        catalog.root() / "patrol_templates/3_patrol_gingerbread_warband.yaml";
    write_file(copy, warband);
    require(contains(
        catalog.rejection(kShippedIds),
        "patrol file must be named 1_patrol_gingerbread_warband.yaml"));
    // Renamed with its id: a third definition, which runs when listed.
    write_file(copy, replace(warband, "id: 1\n", "id: 3\n"));
    const GameServerGameplayConfig three = catalog.load_with_ids("  - ids: [3]\n");
    require(three.patrols.size() == 1u);
    require(three.patrols[0].id == 3u);
    fs::remove(copy);

    // Same id under another name: the file is named correctly, the id is not
    // free.
    const fs::path other = catalog.root() / "patrol_templates/1_patrol_other.yaml";
    write_file(
        other,
        replace(warband, "name: gingerbread_warband\n", "name: other\n"));
    require(contains(catalog.rejection(kShippedIds), "duplicate patrol id 1"));
    fs::remove(other);
}

}  // namespace

int main() {
    the_shipping_catalog_runs_both_patrols();
    listed_order_is_run_order();
    only_listed_ids_run();
    misauthored_lists_are_refused();
    every_file_is_validated_listed_or_not();
    std::printf("patrol_catalog_test passed\n");
    return 0;
}
