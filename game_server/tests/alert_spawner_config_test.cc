// Authoring for spawners that call for help: `trigger: on_alert` on an agent,
// and the two catalog ceilings that bound them.
//
// Each case loads the shipping catalog with one or two files rewritten, so what
// is exercised is the real loader on real templates rather than a hand-built
// config -- and a rejected case is refused for the reason it names, not for
// whichever other check happened to run first.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/spawner_director.h"

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
using network_example::game_server::SpawnerCarrierConfig;
using network_example::game_server::SpawnerTrigger;

constexpr const char* kCatalogDir = "game_server/gameplay_catalog";
constexpr const char* kGruntFile = "entity_templates/26_chaser_grunt.yaml";
constexpr const char* kNestFile = "entity_templates/208_prop_gingerbread_nest.yaml";

constexpr const char* kBudgets =
    "\nreinforce_budget:\n"
    "  max_live_agents: 64\n"
    "agent_budget:\n"
    "  max_live_agents: 256\n";

// Unbounded calls, spaced and capped, dropped from under the carrier.
constexpr const char* kGruntCallsForHelp =
    "\nspawner:\n"
    "  trigger: on_alert\n"
    "  calls_per_alert: 0\n"
    "  interval_ticks: 120\n"
    "  max_live_agents: 4\n"
    "  radius: 4.0\n"
    "  offset: {x: 0.0, y: -3.0, z: 1.0}\n"
    "  count: {min: 2, max: 2}\n"
    "  composition:\n"
    "    - entity_template: gingerbread\n"
    "      min: 2\n"
    "      max: 2\n";

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

// One writable copy of the catalog; every case rewrites what it changes and
// puts the originals back before the next.
class CatalogCopy {
public:
    CatalogCopy() {
        const char* tmp = std::getenv("TEST_TMPDIR");
        root_ = fs::path(tmp != nullptr ? tmp : "/tmp") / "alert_spawner_catalog";
        fs::remove_all(root_);
        fs::copy(kCatalogDir, root_, fs::copy_options::recursive);
        catalog_ = read_file(root_ / "gameplay_catalog.yaml");
        grunt_ = read_file(root_ / kGruntFile);
        nest_ = read_file(root_ / kNestFile);
    }

    const std::string& grunt() const { return grunt_; }
    const std::string& nest() const { return nest_; }

    GameServerGameplayConfig load(
        const std::string& catalog_tail,
        const std::string& grunt,
        const std::string& nest) {
        write_file(root_ / "gameplay_catalog.yaml", catalog_ + catalog_tail);
        write_file(root_ / kGruntFile, grunt);
        write_file(root_ / kNestFile, nest);
        return network_example::game_server::load_gameplay_config_from_catalog_file(
            (root_ / "gameplay_catalog.yaml").string());
    }

    // The message, or empty if it loaded.
    std::string rejection(
        const std::string& catalog_tail,
        const std::string& grunt,
        const std::string& nest) {
        try {
            (void)load(catalog_tail, grunt, nest);
        } catch (const std::exception& error) {
            return error.what();
        }
        return {};
    }

private:
    fs::path root_;
    std::string catalog_;
    std::string grunt_;
    std::string nest_;
};

bool names(const std::string& message, const std::string& needle) {
    if (message.find(needle) != std::string::npos) {
        return true;
    }
    std::fprintf(
        stderr,
        "expected \"%s\" in: %s\n",
        needle.c_str(),
        message.empty() ? "(loaded)" : message.c_str());
    return false;
}

std::string replace_once(
    std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    require(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

// The nest without its door, so a case about the trigger is not refused for
// the entry first.
std::string doorless(const std::string& nest) {
    const std::size_t at = nest.find("\n  entry:\n");
    require(at != std::string::npos);
    // Back up over the comment block that introduces it.
    const std::size_t comment = nest.rfind("\n  # Coming out of the door", at);
    return nest.substr(0, comment != std::string::npos ? comment : at) + "\n";
}

const SpawnerCarrierConfig* carrier_named(
    const GameServerGameplayConfig& config, const std::string& name) {
    for (const SpawnerCarrierConfig& carrier : config.spawner_carriers) {
        if (carrier.name == name) {
            return &carrier;
        }
    }
    return nullptr;
}

// An agent carries an on_alert spawner, and every new field reaches the
// carrier as authored. The control is the shipping catalog itself: it loads,
// nothing in it is on_alert, and the budgets default to unbounded.
void an_agent_carries_an_on_alert_spawner(CatalogCopy* catalog) {
    const GameServerGameplayConfig baseline =
        catalog->load({}, catalog->grunt(), catalog->nest());
    require(baseline.reinforce_budget.max_live_agents == 0u);
    require(baseline.agent_budget.max_live_agents == 0u);
    require(carrier_named(baseline, "chaser_grunt") == nullptr);
    const SpawnerCarrierConfig* nest = carrier_named(baseline, "gingerbread_nest");
    require(nest != nullptr);
    require(nest->spawner.trigger == SpawnerTrigger::kInterval);
    require(nest->spawner.calls_per_alert == 1u);

    const GameServerGameplayConfig config = catalog->load(
        kBudgets, catalog->grunt() + kGruntCallsForHelp, catalog->nest());
    require(config.reinforce_budget.max_live_agents == 64u);
    require(config.agent_budget.max_live_agents == 256u);
    const SpawnerCarrierConfig* grunt = carrier_named(config, "chaser_grunt");
    require(grunt != nullptr);
    require(grunt->entity_type == KernelEntityType_Actor);
    require(grunt->spawner.trigger == SpawnerTrigger::kOnAlert);
    require(grunt->spawner.calls_per_alert == 0u);
    require(grunt->spawner.interval_ticks == 120u);
    require(grunt->spawner.max_live_agents == 4u);
    require(grunt->spawner.offset.y == -3.0f);
    require(grunt->spawner.offset.z == 1.0f);
    require(grunt->spawner.composition.size() == 1u);
    require(grunt->spawner.composition[0].entity_template_id != 0u);

    // The new authoring is part of what a client has to agree on.
    require(config.weapons.catalog_hash != baseline.weapons.catalog_hash);
    const GameServerGameplayConfig once_per_alert = catalog->load(
        kBudgets,
        catalog->grunt() +
            replace_once(kGruntCallsForHelp, "calls_per_alert: 0", "calls_per_alert: 1"),
        catalog->nest());
    require(once_per_alert.weapons.catalog_hash != config.weapons.catalog_hash);
}

void misauthored_on_alert_is_refused(CatalogCopy* catalog) {
    const std::string grunt = catalog->grunt() + kGruntCallsForHelp;
    const std::string nest = catalog->nest();

    require(names(
        catalog->rejection(
            kBudgets,
            catalog->grunt() +
                replace_once(kGruntCallsForHelp, "on_alert", "on_sight"),
            nest),
        "spawner trigger must be interval or on_alert"));

    // A limit on calls means nothing on a clock.
    require(names(
        catalog->rejection(
            kBudgets,
            catalog->grunt() +
                replace_once(kGruntCallsForHelp, "  trigger: on_alert\n", ""),
            nest),
        "calls_per_alert requires trigger: on_alert"));

    // Unbounded calls need a ceiling of their own.
    require(names(
        catalog->rejection(
            kBudgets,
            catalog->grunt() +
                replace_once(kGruntCallsForHelp, "  max_live_agents: 4\n", ""),
            nest),
        "unbounded calls_per_alert requires max_live_agents"));

    // No budget, or one too small for a single wave.
    require(names(catalog->rejection({}, grunt, nest), "requires reinforce_budget"));
    require(names(
        catalog->rejection(
            "\nreinforce_budget:\n  max_live_agents: 1\n", grunt, nest),
        "reinforce_budget is below the largest wave"));

    // A prop has no AI state to read. Its door is taken off first, so this is
    // refused for the carrier and not for the entry.
    require(names(
        catalog->rejection(
            kBudgets,
            catalog->grunt(),
            replace_once(
                doorless(nest), "spawner:\n", "spawner:\n  trigger: on_alert\n")),
        "requires an agent with a sentry or chaser controller"));

    // The nest as shipped, door and all, switched to on_alert.
    require(names(
        catalog->rejection(
            kBudgets,
            catalog->grunt(),
            replace_once(nest, "spawner:\n", "spawner:\n  trigger: on_alert\n")),
        "on_alert cannot have an entry"));

    require(names(
        catalog->rejection(
            "\nagent_budget:\n  max_live_agents: 256\n  headroom: 2\n",
            catalog->grunt(),
            nest),
        "headroom"));
}

}  // namespace

int main() {
    CatalogCopy catalog;
    an_agent_carries_an_on_alert_spawner(&catalog);
    misauthored_on_alert_is_refused(&catalog);
    std::printf("alert_spawner_config_test: PASS\n");
    return 0;
}
