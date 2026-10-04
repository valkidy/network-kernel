// `controller: hover`, on a real kernel with the shipping catalog, a ground
// plane and an ice block for a static obstacle.
//
// The hovering actors are test-only copies of chaser_grunt with their movement
// switched to hover, so the only thing that differs from a unit already known
// to walk is the controller. No AI runs: each case submits the movement input
// itself, every tick, so what moves is the controller and nothing else.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

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
// sentry_grunt_movement_capsule: centre 0.8 up, half height 0.4, radius 0.4,
// so its bottom sits exactly at the entity's y.
constexpr float kCapsuleRadius = 0.4f;
// ice_block_hitbox: 3 x 3 x 1.6 m, standing on its base.
constexpr float kBlockTop = 3.0f;
constexpr float kBlockHalfX = 1.5f;

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

std::string replace_once(
    std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos) {
        std::fprintf(stderr, "replace_once: no \"%s\"\n", from.c_str());
    }
    require(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

// chaser_grunt, renamed and hovering.
std::string hover_template(
    std::uint32_t id,
    const std::string& name,
    float height_meters,
    float vertical_speed) {
    const std::string grunt = read_file(
        "game_server/gameplay_catalog/entity_templates/26_chaser_grunt.yaml");
    std::string hover = replace_once(
        grunt, "id: 26\nname: chaser_grunt\n",
        "id: " + std::to_string(id) + "\nname: " + name + "\n");
    return replace_once(
        hover,
        "  controller: character\n",
        "  controller: hover\n"
        "  hover:\n"
        "    height_meters: " + std::to_string(height_meters) + "\n"
        "    vertical_speed_meters_per_second: " +
            std::to_string(vertical_speed) + "\n");
}

GameServerGameplayConfig load_catalog_with(
    const std::vector<std::pair<std::string, std::string>>& extra) {
    const char* tmp = std::getenv("TEST_TMPDIR");
    const fs::path root =
        fs::path(tmp != nullptr ? tmp : "/tmp") / "hover_catalog";
    fs::remove_all(root);
    fs::copy("game_server/gameplay_catalog", root, fs::copy_options::recursive);
    for (const auto& [path, text] : extra) {
        write_file(root / path, text);
    }
    return network_example::game_server::load_gameplay_config_from_catalog_file(
        (root / "gameplay_catalog.yaml").string());
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

class World {
public:
    World(const GameServerGameplayConfig& config, std::uint16_t port) {
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
    }
    ~World() { Kernel_Destroy(kernel_); }

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

    // One tick. Every listed actor is told to move this way -- zero included,
    // since an actor told nothing keeps the velocity it had.
    void step(const std::vector<std::uint32_t>& movers, KernelVec2 move) {
        for (const std::uint32_t net_id : movers) {
            KernelPlayerInput input{};
            input.input_seq = ++input_seq_;
            input.move = move;
            input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
            Kernel_ServerSubmitEntityInput(kernel_, net_id, &input);
        }
        Kernel_Update(kernel_, kTickSeconds);
    }

    KernelVec3 position(std::uint32_t net_id) const {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(kernel_, net_id, &state));
        return state.position;
    }

private:
    KernelHandle* kernel_ = nullptr;
    std::uint32_t input_seq_ = 0;
};

constexpr const char* kDroneFile = "entity_templates/900_hover_drone.yaml";
constexpr const char* kSkimmerFile = "entity_templates/901_hover_skimmer.yaml";

// A drone at 4 m of clearance that climbs and sinks at 3 m/s, and a skimmer at
// half a metre that climbs at a crawl -- too slow to get over the block before
// it reaches it.
GameServerGameplayConfig hover_catalog() {
    return load_catalog_with({
        {kDroneFile, hover_template(900, "hover_drone", 4.0f, 3.0f)},
        {kSkimmerFile, hover_template(901, "hover_skimmer", 0.5f, 0.1f)},
    });
}

bool near(float value, float expected, float tolerance) {
    return std::fabs(value - expected) <= tolerance;
}

// It holds its clearance whatever height it starts at, and never falls: from
// the floor it climbs to 4 m, from 9 m it sinks to 4 m no faster than its
// vertical speed allows, and once there it stays. The control is chaser_grunt,
// the same unit on its own controller, which stands on the floor.
void it_holds_its_clearance_without_falling() {
    const GameServerGameplayConfig config = hover_catalog();
    World world(config, 7956);
    const std::uint32_t drone_template = template_id_of(config, "hover_drone");
    require(drone_template != 0u);
    const std::uint32_t from_floor = world.create(drone_template, {0.0f, 0.0f, 0.0f});
    const std::uint32_t from_high = world.create(drone_template, {0.0f, 9.0f, 10.0f});
    const std::uint32_t walker = world.create(
        template_id_of(config, "chaser_grunt"), {10.0f, 2.0f, 0.0f});
    const std::vector<std::uint32_t> all{from_floor, from_high, walker};

    float previous_high = 9.0f;
    for (int tick = 0; tick < 90; ++tick) {
        world.step(all, KernelVec2{0.0f, 0.0f});
        const float high = world.position(from_high).y;
        // Sinking at no more than 3 m/s, with a tick of slack for the first
        // step's finalisation.
        require(previous_high - high <= 3.0f * kTickSeconds + 0.01f);
        previous_high = high;
    }
    require(near(world.position(from_floor).y, 4.0f, 0.05f));
    require(near(world.position(from_high).y, 4.0f, 0.05f));
    require(near(world.position(walker).y, 0.0f, 0.1f));

    for (int tick = 0; tick < 60; ++tick) {
        world.step(all, KernelVec2{0.0f, 0.0f});
        require(near(world.position(from_floor).y, 4.0f, 0.05f));
    }
}

// Flying over a static obstacle, it rises to keep its clearance above the
// block's top, and settles back once past it.
void it_rises_over_a_static_obstacle_and_settles_back() {
    const GameServerGameplayConfig config = hover_catalog();
    World world(config, 7957);
    world.create(template_id_of(config, "ice_block"), {10.0f, 0.0f, 0.0f});
    const std::uint32_t drone =
        world.create(template_id_of(config, "hover_drone"), {0.0f, 4.0f, 0.0f});
    const std::vector<std::uint32_t> movers{drone};
    for (int tick = 0; tick < 10; ++tick) {
        world.step(movers, KernelVec2{0.0f, 0.0f});
    }
    require(near(world.position(drone).y, 4.0f, 0.05f));

    float highest_over_block = 0.0f;
    for (int tick = 0; tick < 400 && world.position(drone).x < 25.0f; ++tick) {
        world.step(movers, KernelVec2{1.0f, 0.0f});
        const KernelVec3 at = world.position(drone);
        if (std::fabs(at.x - 10.0f) < kBlockHalfX) {
            highest_over_block = std::max(highest_over_block, at.y);
        }
    }
    require(world.position(drone).x >= 25.0f);
    // 3 m of block and 4 m of clearance, give or take how far the climb had
    // got by the time it was over the far edge.
    require(highest_over_block > kBlockTop + 4.0f - 0.5f);
    for (int tick = 0; tick < 90; ++tick) {
        world.step(movers, KernelVec2{0.0f, 0.0f});
    }
    require(near(world.position(drone).y, 4.0f, 0.05f));
}

// A wall it cannot climb over in time stops it. The skimmer flies at half a
// metre and climbs at 0.1 m/s, and the block's face is 3 m high: pushed into
// it, it stops at the face. The control is the same push with no block, which
// carries it well past where the face would be.
void a_wall_it_cannot_climb_in_time_stops_it() {
    const GameServerGameplayConfig config = hover_catalog();
    const float face = 10.0f - kBlockHalfX - kCapsuleRadius;

    // One server at a time: two transports alive in one process crash on
    // teardown.
    {
        World walled(config, 7958);
        walled.create(template_id_of(config, "ice_block"), {10.0f, 0.0f, 0.0f});
        const std::uint32_t blocked = walled.create(
            template_id_of(config, "hover_skimmer"), {0.0f, 0.5f, 0.0f});
        for (int tick = 0; tick < 200; ++tick) {
            walled.step({blocked}, KernelVec2{1.0f, 0.0f});
            require(walled.position(blocked).x <= face + 0.05f);
        }
        require(walled.position(blocked).x > face - 0.5f);
    }
    {
        World open(config, 7959);
        const std::uint32_t free_flier = open.create(
            template_id_of(config, "hover_skimmer"), {0.0f, 0.5f, 0.0f});
        for (int tick = 0; tick < 200; ++tick) {
            open.step({free_flier}, KernelVec2{1.0f, 0.0f});
        }
        require(open.position(free_flier).x > face + 3.0f);
    }
}

// What the catalog refuses: a hover with no clearance, and a hover block on a
// controller that does not hover.
void misauthored_hover_is_refused() {
    const auto rejection = [](const std::string& text) -> std::string {
        try {
            (void)load_catalog_with({{kDroneFile, text}});
        } catch (const std::exception& error) {
            return error.what();
        }
        return {};
    };
    const std::string drone = hover_template(900, "hover_drone", 4.0f, 3.0f);
    require(rejection(drone).empty());
    require(!rejection(replace_once(
                  drone, "  hover:\n    height_meters: 4.000000\n", "  hover:\n"))
                 .empty());
    require(!rejection(replace_once(
                  drone, "height_meters: 4.000000", "height_meters: 0.0"))
                 .empty());
    require(rejection(replace_once(drone, "controller: hover", "controller: character"))
                .find("movement hover requires controller: hover") !=
            std::string::npos);
}

}  // namespace

int main() {
    it_holds_its_clearance_without_falling();
    it_rises_over_a_static_obstacle_and_settles_back();
    a_wall_it_cannot_climb_in_time_stops_it();
    misauthored_hover_is_refused();
    std::printf("hover_controller_test: PASS\n");
    return 0;
}
