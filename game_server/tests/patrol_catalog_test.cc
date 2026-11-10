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
constexpr const char* kBothIds = "  - ids: [1, 2]\n";

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
    if (start == std::string::npos) {
        std::fprintf(stderr, "needle not found: %s\n", from.c_str());
    }
    require(start != std::string::npos);
    return text.substr(0, start) + to + text.substr(start + from.size());
}

// The catalog with the line after `patrols:` replaced by `ids_line`. Found by
// position, not by content: which ids ship is tuning, and changes.
std::string with_patrol_ids(const std::string& text, const std::string& ids_line) {
    const std::string key = "\npatrols:\n";
    const std::size_t key_at = text.find(key);
    require(key_at != std::string::npos);
    const std::size_t line_at = key_at + key.size();
    const std::size_t line_end = text.find('\n', line_at);
    require(line_end != std::string::npos);
    require(text.compare(line_at, 8, "  - ids:") == 0);
    return text.substr(0, line_at) + ids_line + text.substr(line_end + 1);
}

class CatalogCopy {
public:
    CatalogCopy() {
        const char* tmp = std::getenv("TEST_TMPDIR");
        root_ = fs::path(tmp != nullptr ? tmp : "/tmp") / "patrol_catalog";
        fs::remove_all(root_);
        fs::copy(kCatalogDir, root_, fs::copy_options::recursive);
        // The composed mesh map with its mission and patrols, which these
        // cases were written against (docs/START_SCENE_PLAN.md).
        catalog_ = read_file(root_ / "gameplay_catalog.yaml");
        require(catalog_.find("\nstart_scene: 0\n") != std::string::npos);
        catalog_.replace(catalog_.find("\nstart_scene: 0\n"), 15, "\nstart_scene: 5\n");
        write_file(root_ / "gameplay_catalog.yaml", catalog_);
    }

    const fs::path& root() const { return root_; }
    const std::string& catalog() const { return catalog_; }

    // Loads with the catalog's ids line replaced, then puts the shipping
    // catalog back.
    GameServerGameplayConfig load_with_ids(const std::string& ids_line) {
        write_file(
            root_ / "gameplay_catalog.yaml",
            with_patrol_ids(catalog_, ids_line));
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

void listing_both_runs_both_patrols() {
    CatalogCopy catalog;
    const GameServerGameplayConfig config = catalog.load_with_ids(kBothIds);
    require(config.patrols.size() == 2u);
    require(config.patrols[0].id == 1u);
    require(config.patrols[0].name == "gingerbread_warband");
    require(config.patrols[1].id == 2u);
    require(config.patrols[1].name == "gingerbread_airforce");
    // The warband is tiered by group size; the airforce keeps one mix.
    require(!config.patrols[0].squad_tiers.empty());
    require(!config.patrols[1].composition.empty());
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
        catalog.rejection("  - ids: [99]\n"),
        "patrol id 99 is listed but no file under patrol_template_dir"));
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
        replace(warband, "interval_ticks: 900\n", "interval_ticks: 900\nbogus: 1\n"));
    require(!catalog.rejection("  - ids: [2]\n").empty());
    write_file(catalog.root() / kWarbandFile, warband);

    // A copy whose id was never changed.
    const fs::path copy =
        catalog.root() / "patrol_templates/9_patrol_gingerbread_warband.yaml";
    write_file(copy, warband);
    require(contains(
        catalog.rejection(kBothIds),
        "patrol file must be named 1_patrol_gingerbread_warband.yaml"));
    // Renamed with its id (9: 3 is the shipping giant): another definition, which runs when listed.
    write_file(copy, replace(warband, "id: 1\n", "id: 9\n"));
    const GameServerGameplayConfig three = catalog.load_with_ids("  - ids: [9]\n");
    require(three.patrols.size() == 1u);
    require(three.patrols[0].id == 9u);
    fs::remove(copy);

    // Same id under another name: the file is named correctly, the id is not
    // free.
    const fs::path other = catalog.root() / "patrol_templates/1_patrol_other.yaml";
    write_file(
        other,
        replace(warband, "name: gingerbread_warband\n", "name: other\n"));
    require(contains(catalog.rejection(kBothIds), "duplicate patrol id 1"));
    fs::remove(other);
}

// A definition aimed at players, written out whole rather than edited from the
// warband, so the needles below name text this file owns.
constexpr const char* kTargetedFile = "patrol_templates/9_patrol_targeted.yaml";
constexpr const char* kTargetedIds = "  - ids: [9]\n";

const std::string kTargetedWhere =
    "where:\n"
    "  mode: around_players\n"
    "  ring: {min_meters: 50.0, max_meters: 60.0}\n"
    "  pass_within_meters: 15.0\n"
    "  exit_spread_degrees: 30.0\n"
    "  group_radius_meters: 18.0\n"
    "  groups_per_member: 1.0\n"
    "  groups_per_player: 1.5\n"
    "  floor_interval_ticks: 1800\n";

const std::string kTargetedTiers =
    "squad_by_group_size:\n"
    "  - members: {min: 1, max: 1}\n"
    "    count: {min: 4, max: 6}\n"
    "    composition:\n"
    "      - {entity_template: gingerbread_warrior, min: 0, max: 1}\n"
    "      - {entity_template: gingerbread_mage, min: 0, max: 1}\n"
    "      - {entity_template: gingerbread, min: 3, max: 6}\n"
    "  - members: {min: 2, max: 2}\n"
    "    count: {min: 6, max: 9}\n"
    "    composition:\n"
    "      - {entity_template: gingerbread_warrior, min: 1, max: 3}\n"
    "      - {entity_template: gingerbread_mage, min: 0, max: 2}\n"
    "      - {entity_template: gingerbread, min: 4, max: 8}\n"
    "  - members: {min: 3}\n"
    "    count: {min: 8, max: 12}\n"
    "    composition:\n"
    "      - {entity_template: gingerbread_warrior, min: 1, max: 5}\n"
    "      - {entity_template: gingerbread_mage, min: 1, max: 3}\n"
    "      - {entity_template: gingerbread, min: 4, max: 8}\n";

std::string targeted_definition() {
    return "id: 9\nname: targeted\n" + kTargetedWhere + "seed: 11\n" +
        "interval_ticks: 900\n" + kTargetedTiers;
}

// Loads with the targeted definition rewritten to `text` and listed; the
// message, or empty if it loaded.
std::string targeted_rejection(CatalogCopy* catalog, const std::string& text) {
    write_file(catalog->root() / kTargetedFile, text);
    const std::string message = catalog->rejection(kTargetedIds);
    fs::remove(catalog->root() / kTargetedFile);
    return message;
}

void around_players_definition_loads() {
    using network_example::game_server::PatrolPlacementMode;
    CatalogCopy catalog;

    // The control: the airforce is area-placed and untiered, so the checks
    // below are about the new file and not a default.
    const GameServerGameplayConfig shipping = catalog.load_with_ids("  - ids: [2]\n");
    require(shipping.patrols.size() == 1u);
    require(shipping.patrols[0].placement == PatrolPlacementMode::kArea);
    require(shipping.patrols[0].squad_tiers.empty());

    write_file(catalog.root() / kTargetedFile, targeted_definition());
    const GameServerGameplayConfig config = catalog.load_with_ids(kTargetedIds);
    fs::remove(catalog.root() / kTargetedFile);
    require(config.patrols.size() == 1u);
    const auto& patrol = config.patrols[0];
    require(patrol.placement == PatrolPlacementMode::kAroundPlayers);
    require(patrol.around_players.ring_min_meters == 50.0f);
    require(patrol.around_players.ring_max_meters == 60.0f);
    require(patrol.around_players.pass_within_meters == 15.0f);
    require(patrol.around_players.groups_per_player == 1.5f);
    require(patrol.around_players.floor_interval_ticks == 1800u);
    require(patrol.squad_tiers.size() == 3u);
    require(patrol.squad_tiers[0].members_min == 1u);
    require(patrol.squad_tiers[0].members_max == 1u);
    require(patrol.squad_tiers[2].members_min == 3u);
    require(patrol.squad_tiers[2].members_max == 0u);
    require(patrol.squad_tiers[0].count_min == 4u);
    require(patrol.squad_tiers[0].count_max == 6u);
    // Ids resolved, not just refs copied.
    require(patrol.squad_tiers[0].composition.size() == 3u);
    for (const auto& tier : patrol.squad_tiers) {
        for (const auto& entry : tier.composition) {
            require(entry.entity_template_id != 0u);
        }
    }
    // The definition-wide band is the envelope across tiers.
    require(patrol.count_min == 4u);
    require(patrol.count_max == 12u);
    require(patrol.composition.empty());
}

void around_players_misauthoring_is_refused() {
    CatalogCopy catalog;
    const std::string good = targeted_definition();
    // The control: the unedited text loads, so each refusal below is the edit.
    require(targeted_rejection(&catalog, good).empty());

    require(contains(
        targeted_rejection(
            &catalog,
            replace(good, "seed: 11\n",
                    "seed: 11\narea: {shape: circle, radius: 10.0}\n")),
        "patrol takes area or where, not both"));
    require(contains(
        targeted_rejection(
            &catalog, replace(good, "seed: 11\n", "seed: 11\nmax_live_groups: 2\n")),
        "replaces max_live_groups with groups_per_player"));
    require(contains(
        targeted_rejection(
            &catalog, replace(good, "mode: around_players", "mode: anywhere")),
        "patrol where mode must be around_players"));
    require(contains(
        targeted_rejection(
            &catalog, replace(good, "seed: 11\n", "seed: 11\ncount: {min: 1, max: 2}\n")),
        "squad_by_group_size or count and composition, not both"));
    require(contains(
        targeted_rejection(
            &catalog,
            replace(good, "ring: {min_meters: 50.0, max_meters: 60.0}",
                    "ring: {min_meters: 60.0, max_meters: 50.0}")),
        "ring needs 0 < min_meters < max_meters"));
    require(contains(
        targeted_rejection(
            &catalog, replace(good, "pass_within_meters: 15.0", "pass_within_meters: 50.0")),
        "pass_within_meters must be above zero and below ring min_meters"));
    require(contains(
        targeted_rejection(
            &catalog, replace(good, "exit_spread_degrees: 30.0", "exit_spread_degrees: 120.0")),
        "exit_spread_degrees must be within 0..90"));
    require(contains(
        targeted_rejection(
            &catalog, replace(good, "groups_per_player: 1.5", "groups_per_player: 0.0")),
        "groups_per_member and groups_per_player must be above zero"));

    // Tiers.
    require(contains(
        targeted_rejection(
            &catalog, replace(good, "members: {min: 1, max: 1}", "members: {min: 2, max: 2}")),
        "tiers must start at members 1 and leave no gap"));
    require(contains(
        targeted_rejection(
            &catalog, replace(good, "members: {min: 3}", "members: {min: 4}")),
        "tiers must start at members 1 and leave no gap, expected min 3"));
    require(contains(
        targeted_rejection(
            &catalog, replace(good, "members: {min: 3}", "members: {min: 3, max: 4}")),
        "last tier must leave members max unbounded"));
    require(contains(
        targeted_rejection(
            &catalog, replace(good, "members: {min: 2, max: 2}", "members: {min: 2}")),
        "only the last tier may leave members max unbounded"));
    require(contains(
        targeted_rejection(
            &catalog, replace(good, "members: {min: 1, max: 1}", "members: {min: 1, max: 0}")),
        "members max must not be zero"));
    // The full warband's floors (1 + 1 + 4) do not fit a lone player's 4-6.
    require(contains(
        targeted_rejection(
            &catalog,
            replace(good,
                    "      - {entity_template: gingerbread_warrior, min: 0, max: 1}\n"
                    "      - {entity_template: gingerbread_mage, min: 0, max: 1}\n"
                    "      - {entity_template: gingerbread, min: 3, max: 6}\n",
                    "      - {entity_template: gingerbread_warrior, min: 1, max: 1}\n"
                    "      - {entity_template: gingerbread_mage, min: 1, max: 1}\n"
                    "      - {entity_template: gingerbread, min: 4, max: 6}\n")),
        "squad_by_group_size members 1 tier"));

    // Tiers on an area-placed definition.
    require(contains(
        targeted_rejection(
            &catalog, replace(good, kTargetedWhere, "area: {shape: circle, radius: 10.0}\n")),
        "squad_by_group_size needs where mode around_players"));
}

void map_playable_bounds() {
    CatalogCopy catalog;
    // Shipping leaves the bounds to the map: a terrain fills them in from
    // map_size, and a mesh map's patrols fall back to the navmesh's extent.
    const auto shipped = catalog.load_with_ids("  - ids: [1]\n").map.playable_bounds;
    require(!shipped.authored);

    const std::string shipping = catalog.catalog();
    const std::string unmapped = shipping;
    const auto with_map = [&](const std::string& map_block) {
        return replace(unmapped, "\npatrols:\n", "\n" + map_block + "patrols:\n");
    };
    const auto load = [&](const std::string& text) {
        write_file(catalog.root() / "gameplay_catalog.yaml", text);
        GameServerGameplayConfig config =
            network_example::game_server::load_gameplay_config_from_catalog_file(
                (catalog.root() / "gameplay_catalog.yaml").string());
        write_file(catalog.root() / "gameplay_catalog.yaml", shipping);
        return config;
    };
    const auto refusal = [&](const std::string& text) {
        try {
            (void)load(text);
        } catch (const std::exception& error) {
            write_file(catalog.root() / "gameplay_catalog.yaml", shipping);
            return std::string(error.what());
        }
        return std::string();
    };

    // The control: without the block, nothing is authored.
    require(!load(unmapped).map.playable_bounds.authored);

    const GameServerGameplayConfig config = load(with_map(
        "map:\n"
        "  playable_bounds: {min: {x: -108.0, z: -100.0}, max: {x: 108.0, z: 116.0}}\n"));
    const auto& bounds = config.map.playable_bounds;
    require(bounds.authored);
    require(bounds.min_x == -108.0f);
    require(bounds.min_z == -100.0f);
    require(bounds.max_x == 108.0f);
    require(bounds.max_z == 116.0f);

    require(contains(
        refusal(with_map(
            "map:\n"
            "  playable_bounds: {min: {x: 10.0, z: -10.0}, max: {x: 10.0, z: 10.0}}\n")),
        "map playable_bounds min must be below max"));
    require(contains(
        refusal(with_map(
            "map:\n"
            "  playable_bounds: {min: {x: -10.0, z: -10.0}}\n")),
        "map playable_bounds requires min and max"));
    // A height is refused rather than silently dropped.
    require(!refusal(with_map(
                 "map:\n"
                 "  playable_bounds: {min: {x: -10.0, y: 0.0, z: -10.0}, "
                 "max: {x: 10.0, z: 10.0}}\n"))
                 .empty());
}

}  // namespace

int main() {
    listing_both_runs_both_patrols();
    listed_order_is_run_order();
    only_listed_ids_run();
    misauthored_lists_are_refused();
    every_file_is_validated_listed_or_not();
    around_players_definition_loads();
    around_players_misauthoring_is_refused();
    map_playable_bounds();
    std::printf("patrol_catalog_test passed\n");
    return 0;
}
