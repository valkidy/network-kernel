// Spawners that call for help: `trigger: on_alert` on an agent, the two
// catalog ceilings that bound them, and the engagement signal they run on.
//
// Each case loads the shipping catalog with one or two files rewritten, so what
// is exercised is the real loader on real templates rather than a hand-built
// config -- and a rejected case is refused for the reason it names, not for
// whichever other check happened to run first.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/spawner_director.h"
#include "kernel/public/kernel_api.h"

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
using network_example::game_server::AlertSignal;
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

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};

std::vector<std::uint8_t> read_ground_scene() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    const fs::path path = fs::path(test_srcdir) / test_workspace /
        "game_server" / "gameplay_catalog" / "mesh_assets" / "jolt" /
        "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::uint32_t template_id_of(
    const GameServerGameplayConfig& config, const std::string& name) {
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == name) {
            return candidate.actor_template_id;
        }
    }
    return 0;
}

// A dedicated server on a ground plane, run the way the real one is: the game
// server's tick, then the kernel's, then the kernel's events back.
class Server {
public:
    Server(const GameServerGameplayConfig& config, std::uint16_t port) {
        KernelConfig kernel_config{};
        kernel_config.mode = KernelMode_DedicatedServer;
        kernel_config.tick.server_tick_rate = 30;
        kernel_config.tick.snapshot_rate = 30;
        kernel_config.max_events = 256;
        kernel_config.max_render_states = 64;
        kernel_ = Kernel_Create(&kernel_config);
        require(kernel_ != nullptr);
        require(network_example::game_server::load_kernel_gameplay_catalog(
            kernel_, config));
        const std::vector<std::uint8_t> scene = read_ground_scene();
        KernelStaticCollisionSceneConfig scene_config{};
        scene_config.struct_size = sizeof(scene_config);
        scene_config.artifact_bytes = scene.data();
        scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
        scene_config.scene_id = config.static_collision_scene.scene_id;
        scene_config.collider_id = config.static_collision_scene.collider_id;
        scene_config.collision_layer =
            config.static_collision_scene.collision_layer;
        require(Kernel_SetStaticCollisionScene(kernel_, &scene_config));
        require(Kernel_StartDedicatedServer(kernel_, port));
        // No preload: patrols and world rules would put agents of their own
        // into a test about two.
        game_server_ =
            new network_example::game_server::GameServer(kernel_, config);
    }
    ~Server() {
        delete game_server_;
        Kernel_Destroy(kernel_);
    }

    KernelHandle* kernel() const { return kernel_; }

    void step() {
        game_server_->tick(kTickSeconds);
        Kernel_Update(kernel_, kTickSeconds);
        const std::uint32_t count = Kernel_PollEvents(
            kernel_, events_.data(), static_cast<std::uint32_t>(events_.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            game_server_->handle_event(events_[index]);
        }
    }

    const AlertSignal* signal_for(std::uint32_t net_id) const {
        for (const AlertSignal& signal :
             game_server_->agent_runtime_manager().alert_signals()) {
            if (signal.net_id == net_id) {
                return &signal;
            }
        }
        return nullptr;
    }

    bool engaged(std::uint32_t net_id) const {
        for (const auto& agent : game_server_->agent_runtime_manager().agents()) {
            if (agent.net_id == net_id) {
                return network_example::game_server::is_engaged(agent.sentry.state);
            }
        }
        return false;
    }

    std::uint32_t create(std::uint32_t template_id, const KernelVec3& position) {
        KernelServerEntityCreateInfo create_info{};
        create_info.struct_size = sizeof(create_info);
        create_info.entity_template_id = template_id;
        create_info.position = position;
        create_info.rotation = kIdentityRotation;
        std::uint32_t net_id = 0;
        require(Kernel_ServerCreateEntity(kernel_, &create_info, &net_id));
        require(net_id != 0u);
        return net_id;
    }

    std::uint32_t create_player(const KernelVec3& position) {
        KernelServerEntityCreateInfo create_info{};
        create_info.struct_size = sizeof(create_info);
        create_info.entity_type = KernelEntityType_Actor;
        create_info.actor_type = KernelActorType_Player;
        create_info.position = position;
        create_info.rotation = kIdentityRotation;
        std::uint32_t net_id = 0;
        require(Kernel_ServerCreateEntity(kernel_, &create_info, &net_id));
        KernelAgentVisionConfig vision{};
        vision.struct_size = sizeof(vision);
        vision.camp = KernelAgentCamp_PlayerSide;
        require(Kernel_ServerSetEntityVisionConfig(kernel_, net_id, &vision));
        return net_id;
    }

    void place(std::uint32_t net_id, const KernelVec3& position) {
        require(Kernel_ServerSetEntityTransform(
            kernel_, net_id, &position, &kIdentityRotation));
    }

private:
    KernelHandle* kernel_ = nullptr;
    network_example::game_server::GameServer* game_server_ = nullptr;
    std::array<KernelEvent, 256> events_{};
};

// The engagement signal, over one whole engagement and the start of the next.
//
// Both agents below see the player and both engage; only the one that carries
// an on_alert spawner is reported, which is the control for the filter. The
// player is placed where an agent at the origin, facing the template default
// of -x, has it inside a 12 m, 90 degree cone.
void an_engagement_is_signalled_from_start_to_end(CatalogCopy* catalog) {
    const GameServerGameplayConfig config = catalog->load(
        kBudgets, catalog->grunt() + kGruntCallsForHelp, catalog->nest());
    const std::uint32_t grunt_template = template_id_of(config, "chaser_grunt");
    const std::uint32_t sentry_template = template_id_of(config, "beam_sentry");
    require(grunt_template != 0u && sentry_template != 0u);

    Server server(config, 7971);
    const KernelVec3 grunt_home{0.0f, 0.0f, 0.0f};
    const KernelVec3 in_sight{-8.0f, 0.0f, 0.0f};
    const KernelVec3 out_of_sight{60.0f, 0.0f, 60.0f};
    const std::uint32_t grunt = server.create(grunt_template, grunt_home);
    const std::uint32_t sentry = server.create(sentry_template, {0.0f, 0.0f, 4.0f});
    const std::uint32_t player = server.create_player(out_of_sight);

    // Nobody in sight: nobody engaged, nothing reported.
    for (int tick = 0; tick < 10; ++tick) {
        server.step();
        require(server.signal_for(grunt) == nullptr);
    }

    // In sight. Vision is computed inside the kernel's update, so the manager
    // sees it a step later; the first signal is the engagement starting, and
    // it sees its target.
    server.place(player, in_sight);
    const AlertSignal* first = nullptr;
    for (int tick = 0; tick < 5 && first == nullptr; ++tick) {
        server.step();
        first = server.signal_for(grunt);
    }
    require(first != nullptr);
    require(first->engagement_started);
    require(first->sees_target);
    // The sentry engaged too, and is not a carrier, so it is not reported.
    // Waited for rather than assumed, or the filter would be proven by an
    // agent that never engaged.
    for (int tick = 0; tick < 5 && !server.engaged(sentry); ++tick) {
        server.step();
    }
    require(server.engaged(sentry));
    require(server.signal_for(sentry) == nullptr);

    // Carrying on, through alert into attack, is not starting again.
    for (int tick = 0; tick < 120; ++tick) {
        server.step();
        const AlertSignal* signal = server.signal_for(grunt);
        require(signal != nullptr);
        require(!signal->engagement_started);
    }

    // Out of sight. Engagement outlives sight, and every tick of that tail
    // says it cannot see -- until the engagement ends and the signal goes.
    server.place(player, out_of_sight);
    server.step();
    server.step();
    int tail = 0;
    while (server.signal_for(grunt) != nullptr) {
        require(!server.signal_for(grunt)->sees_target);
        require(!server.signal_for(grunt)->engagement_started);
        server.step();
        require(++tail < 400);
    }

    // Back in sight: a new engagement. The grunt is put back where it started,
    // facing where it started, because it chased and then wandered.
    server.place(grunt, grunt_home);
    server.place(player, in_sight);
    const AlertSignal* again = nullptr;
    for (int tick = 0; tick < 5 && again == nullptr; ++tick) {
        server.step();
        again = server.signal_for(grunt);
    }
    require(again != nullptr);
    require(again->engagement_started);
}

}  // namespace

int main() {
    CatalogCopy catalog;
    an_agent_carries_an_on_alert_spawner(&catalog);
    misauthored_on_alert_is_refused(&catalog);
    an_engagement_is_signalled_from_start_to_end(&catalog);
    std::printf("alert_spawner_test: PASS\n");
    return 0;
}
