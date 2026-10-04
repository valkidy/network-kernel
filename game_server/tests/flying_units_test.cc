// The three shipped units built on hover and on_alert, as the shipping catalog
// authors them: beam_drone, hive_airship and gingerbread_courier.
//
// Behaviour of the mechanisms themselves is pinned elsewhere --
// hover_controller_test and alert_spawner_test. This checks that the units
// put them together the way their templates say: a drone that flies at 9 m
// and burns a player from up there, an airship at 14 m whose drop falls to the
// ground, and a courier on foot that calls gingerbread.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/src/kernel_api_internal.h"

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

// The shipping catalog on a dedicated server and a ground plane, run the way
// the real one is. Mission, world-rule and patrol population are cleared, so
// a case holds only what it creates.
class Server {
public:
    explicit Server(std::uint16_t port)
        : config_(network_example::game_server::default_game_server_gameplay_config()) {
        GameServerGameplayConfig quiet = config_;
        quiet.game_rules.clear();
        quiet.world_rule_spawns.clear();
        quiet.patrols.clear();
        KernelConfig kernel_config{};
        kernel_config.mode = KernelMode_DedicatedServer;
        kernel_config.tick.server_tick_rate = 30;
        kernel_config.tick.snapshot_rate = 30;
        kernel_config.max_events = 256;
        kernel_config.max_render_states = 64;
        kernel_ = Kernel_Create(&kernel_config);
        require(kernel_ != nullptr);
        require(network_example::game_server::load_kernel_gameplay_catalog(
            kernel_, quiet));
        const std::vector<std::uint8_t> scene = read_ground_scene();
        KernelStaticCollisionSceneConfig scene_config{};
        scene_config.struct_size = sizeof(scene_config);
        scene_config.artifact_bytes = scene.data();
        scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
        scene_config.scene_id = quiet.static_collision_scene.scene_id;
        scene_config.collider_id = quiet.static_collision_scene.collider_id;
        scene_config.collision_layer = quiet.static_collision_scene.collision_layer;
        require(Kernel_SetStaticCollisionScene(kernel_, &scene_config));
        require(Kernel_StartDedicatedServer(kernel_, port));
        game_server_ = new network_example::game_server::GameServer(kernel_, quiet);
    }
    ~Server() {
        delete game_server_;
        Kernel_Destroy(kernel_);
    }

    const GameServerGameplayConfig& config() const { return config_; }

    void step() {
        game_server_->tick(kTickSeconds);
        Kernel_Update(kernel_, kTickSeconds);
        const std::uint32_t count = Kernel_PollEvents(
            kernel_, events_.data(), static_cast<std::uint32_t>(events_.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            game_server_->handle_event(events_[index]);
        }
    }

    void steps(int count) {
        for (int tick = 0; tick < count; ++tick) {
            step();
        }
    }

    std::uint32_t create(const std::string& name, const KernelVec3& position) {
        const std::uint32_t template_id = template_id_of(config_, name);
        require(template_id != 0u);
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

    // A player from the shipping template, so it has health to lose, and
    // seen as the other side by every agent's vision.
    std::uint32_t create_player(const KernelVec3& position) {
        const std::uint32_t net_id = create("player", position);
        KernelAgentVisionConfig vision{};
        vision.struct_size = sizeof(vision);
        vision.camp = KernelAgentCamp_PlayerSide;
        require(Kernel_ServerSetEntityVisionConfig(kernel_, net_id, &vision));
        return net_id;
    }

    KernelServerEntityState state(std::uint32_t net_id) const {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(kernel_, net_id, &state));
        return state;
    }

    void place(std::uint32_t net_id, const KernelVec3& position) {
        require(Kernel_ServerSetEntityTransform(
            kernel_, net_id, &position, &kIdentityRotation));
    }

    std::vector<std::uint32_t> spawned_by(std::uint32_t carrier) const {
        for (const auto& instance :
             game_server_->agent_runtime_manager().spawner_director().instances()) {
            if (instance.carrier_net_id == carrier) {
                return instance.spawned_net_ids;
            }
        }
        return {};
    }

private:
    GameServerGameplayConfig config_;
    KernelHandle* kernel_ = nullptr;
    network_example::game_server::GameServer* game_server_ = nullptr;
    std::array<KernelEvent, 256> events_{};
};

bool near(float value, float expected, float tolerance) {
    return std::fabs(value - expected) <= tolerance;
}

// The drone climbs to 9 m and holds it, and a player it can see takes damage
// from up there. The player starts out of sight, so the control -- no damage
// before it is seen -- comes first.
void the_drone_flies_and_burns_from_above() {
    Server server(7912);
    const std::uint32_t drone = server.create("beam_drone", {0.0f, 0.0f, 0.0f});
    const std::uint32_t player = server.create_player({60.0f, 0.0f, 60.0f});
    server.steps(120);
    require(near(server.state(drone).position.y, 9.0f, 0.1f));
    const std::uint16_t full = server.state(player).hp;
    require(full > 0u);
    require(server.state(player).hp == full);

    // Ahead of it (agents face -x until they turn) and 12 m across the ground:
    // inside its 20 m cone and outside its 8 m stop, so it closes, then fires
    // down from 9 m.
    server.place(drone, server.state(drone).position);
    server.place(player, {-12.0f, 0.0f, 0.0f});
    bool burned = false;
    for (int tick = 0; tick < 300 && !burned; ++tick) {
        server.step();
        burned = server.state(player).hp < full;
    }
    require(burned);
    // Still flying while it fights.
    require(near(server.state(drone).position.y, 9.0f, 0.2f));
}

// The airship climbs to 14 m. Once it sees a player it drops gingerbread from
// under its hull, and the drop falls to the ground.
void the_airship_drops_gingerbread_that_falls() {
    Server server(7913);
    const std::uint32_t airship =
        server.create("hive_airship", {0.0f, 0.0f, 0.0f});
    const std::uint32_t player = server.create_player({80.0f, 0.0f, 80.0f});
    server.steps(240);
    require(near(server.state(airship).position.y, 14.0f, 0.1f));
    require(server.spawned_by(airship).empty());

    server.place(airship, server.state(airship).position);
    server.place(player, {-10.0f, 0.0f, 0.0f});
    std::vector<std::uint32_t> drop;
    for (int tick = 0; tick < 30 && drop.empty(); ++tick) {
        server.step();
        drop = server.spawned_by(airship);
    }
    require(drop.size() >= 2u);
    const std::uint32_t gingerbread = template_id_of(server.config(), "gingerbread");
    for (const std::uint32_t unit : drop) {
        const KernelServerEntityState state = server.state(unit);
        require(state.entity_template_id == gingerbread);
        // Out from under the hull, 3 m below the clearance it holds.
        require(state.position.y > 9.0f);
    }
    server.steps(120);
    for (const std::uint32_t unit : drop) {
        require(server.state(unit).position.y < 0.5f);
    }
}

// A gingerbread_courier on foot calls two gingerbread when it spots a player,
// and nothing before.
void the_courier_calls_gingerbread() {
    Server server(7914);
    const std::uint32_t courier =
        server.create("gingerbread_courier", {0.0f, 0.0f, 0.0f});
    const std::uint32_t player = server.create_player({60.0f, 0.0f, 60.0f});
    server.steps(10);
    require(server.spawned_by(courier).empty());

    server.place(courier, server.state(courier).position);
    server.place(player, {-8.0f, 0.0f, 0.0f});
    std::vector<std::uint32_t> called;
    for (int tick = 0; tick < 10 && called.empty(); ++tick) {
        server.step();
        called = server.spawned_by(courier);
    }
    require(called.size() == 2u);
    const std::uint32_t gingerbread = template_id_of(server.config(), "gingerbread");
    for (const std::uint32_t unit : called) {
        require(server.state(unit).entity_template_id == gingerbread);
    }
}

}  // namespace

int main() {
    the_drone_flies_and_burns_from_above();
    the_airship_drops_gingerbread_that_falls();
    the_courier_calls_gingerbread();
    std::printf("flying_units_test: PASS\n");
    return 0;
}
