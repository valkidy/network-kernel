// A gingerbread at the foot of a 6 m riser, with a player on the ledge above
// it, backs off for a run-up and then leaps up onto the ledge.
//
// issue_20261118_221520: from the foot, the arc to the ledge runs straight into
// the riser. The kernel used to fly it anyway -- the wall took the horizontal
// speed and it went 12 m straight up -- and now refuses it (start_leap sweeps
// the arc). The chaser reads that refusal on the next tick, backs straight off
// from the target far enough for the arc to clear the edge, and leaps again.
//
// The shipped game, headless, on a test-only mesh: a lower floor (y = 0) on
// +x, the riser at x = 40 facing +x, the ledge (y = 6) on -x. Measured: the
// leap lifts off well back from the riser, never rises higher than a leap to
// the ledge does, and comes down on the ledge.
//
// issue_20261125_135253: the same unit with its leap's max_rise_meters under
// the riser's 6 m never leaps at all -- no lift-off, no run-up. And with the
// mesh's navmesh loaded, where the two floors do not connect, a capped unit
// that started out on the lower floor does not walk up to the riser and stand
// at its foot: there is no way up on foot, so it holds where it was alerted.
// Without the navmesh the same unit walks to the riser, which is what the
// navmesh changed.

#include <algorithm>
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

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}
#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr float kRiserX = 40.0f;
constexpr float kLedgeY = 6.0f;

std::vector<std::uint8_t> read_ledge_file(const char* directory, const char* name) {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    const std::filesystem::path path = std::filesystem::path(test_srcdir) /
        test_workspace / "game_server" / directory / name;
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

network_example::game_server::ActorTemplateConfig* template_named(
    network_example::game_server::GameServerGameplayConfig& config,
    const std::string& name) {
    for (auto& candidate : config.actor_templates) {
        if (candidate.name == name) return &candidate;
    }
    return nullptr;
}

struct Sample {
    KernelVec3 position;
    std::uint32_t flags;
};

const KernelVec3 kPlayerAt{kRiserX - 12.0f, kLedgeY, -70.0f};
const KernelVec3 kUnitAt{kRiserX + 0.6f, 0.0f, -70.0f};

// Fifteen seconds of the unit at the riser's foot with the player on the
// ledge, sampled every tick.
std::vector<Sample> run(
    const network_example::game_server::GameServerGameplayConfig& config,
    const network_example::game_server::ActorTemplateConfig& gingerbread,
    const std::vector<std::uint8_t>& scene,
    std::uint16_t port,
    const KernelVec3& unit_at = kUnitAt) {

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_ListenServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 4096;
    kernel_config.max_render_states = 512;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    KernelSessionRulesConfig session_rules{};
    session_rules.struct_size = sizeof(session_rules);
    session_rules.actor_blocking_mode = KernelActorBlockingMode_Predicted;
    require(Kernel_SetSessionRules(kernel, &session_rules));
    require(network_example::game_server::load_kernel_gameplay_catalog(kernel, config));
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartListenServer(kernel, port));

    network_example::game_server::GameServer game_server(kernel, config);
    require(game_server.preload_directors());
    std::array<KernelEvent, 4096> events{};
    const auto step = [&]() {
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t n = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t i = 0; i < n; ++i) game_server.handle_event(events[i]);
        game_server.tick(kTickSeconds);
    };
    for (int tick = 0; tick < 30 * 5; ++tick) step();
    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(kernel, &local));
    const std::uint32_t player = local.player_net_id;

    // The player up on the ledge, 12.6 m out from the unit -- inside the band
    // -- and 6 m up: a target up a ledge.
    const KernelQuat identity{0.0f, 0.0f, 0.0f, 1.0f};
    const KernelVec3 player_at = kPlayerAt;
    require(Kernel_ServerSetEntityTransform(kernel, player, &player_at, &identity));

    // The unit at the riser's foot, its capsule a few centimetres off the
    // face, facing -x (identity), where the player is.
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = KernelEntityType_Actor;
    create.actor_type = KernelActorType_Agent;
    create.entity_template_id = gingerbread.actor_template_id;
    create.actor_template_id = gingerbread.actor_template_id;
    create.position = unit_at;
    create.rotation = identity;
    std::uint32_t unit = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &unit));
    require(Kernel_ServerSetEntityActorTemplate(kernel, unit, gingerbread.actor_template_id));

    std::vector<Sample> samples;
    for (int tick = 0; tick < 30 * 15; ++tick) {
        step();
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        KernelServerEntityState target{};
        target.struct_size = sizeof(target);
        if (!Kernel_ServerGetEntityState(kernel, unit, &state) ||
            !Kernel_ServerGetEntityState(kernel, player, &target)) {
            break;
        }
        // The player stands still where it was put.
        if (std::hypot(target.position.x - player_at.x, target.position.z - player_at.z) >
                0.01f ||
            std::fabs(target.position.y - player_at.y) > 0.5f) {
            require(Kernel_ServerSetEntityTransform(kernel, player, &player_at, &identity));
        }
        samples.push_back(Sample{state.position, state.visual_flags});
    }
    Kernel_Destroy(kernel);
    return samples;
}

}  // namespace

int main() {
    auto config = network_example::game_server::default_game_server_gameplay_config();
    const auto* gingerbread = template_named(config, "gingerbread");
    require(gingerbread != nullptr);
    const network_example::game_server::AgentLeapTuning& leap = gingerbread->chaser.leap;
    require(leap.max_distance_meters > 0.0f);
    // The shipped cap clears this riser, or the leap below proves nothing.
    require(leap.max_rise_meters == 0.0f || leap.max_rise_meters > kLedgeY);
    const std::vector<std::uint8_t> scene = read_ledge_file("jolt", "ledge_6m.joltmesh");
    const std::vector<std::uint8_t> navmesh = read_ledge_file("recast", "ledge_6m.navmesh");
    const KernelVec3 player_at = kPlayerAt;

    const std::vector<Sample> samples = run(config, *gingerbread, scene, 8077);
    require(samples.size() == 30u * 15u);

    // The first leap, its lift-off and its landing.
    std::size_t lift = samples.size();
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if ((samples[i].flags & KERNEL_VISUAL_FLAG_LEAPING) != 0u) { lift = i; break; }
    }
    std::printf("unit started at x %.2f; ", kUnitAt.x);
    if (lift == samples.size()) {
        std::printf("never leapt; last at (%.2f, %.2f, %.2f)\n",
                    samples.back().position.x, samples.back().position.y,
                    samples.back().position.z);
    }
    require(lift < samples.size());
    require(lift > 0u);
    std::size_t landed = lift;
    float highest = samples[lift].position.y;
    while (landed < samples.size() &&
           (samples[landed].flags & KERNEL_VISUAL_FLAG_LEAPING) != 0u) {
        highest = std::max(highest, samples[landed].position.y);
        ++landed;
    }
    require(landed < samples.size());
    const KernelVec3 from = samples[lift - 1u].position;
    const KernelVec3 at = samples[landed].position;
    std::printf("lifted off at tick %zu from x %.2f (%.2f m back from the riser), "
                "peaked at y %.2f, landed at tick %zu on (%.2f, %.2f, %.2f)\n",
                lift, from.x, from.x - kRiserX, highest, landed, at.x, at.y, at.z);

    // Backed off before it went: the arc from the foot would hit the riser.
    require(from.x - kRiserX > 2.0f);
    // A leap onto the ledge peaks apex_meters over it, give or take the
    // landing a little past the player; the rocket off the riser went 12 m up.
    require(highest < kLedgeY + leap.apex_meters + 1.0f);
    // And it came down on the ledge, near the player.
    require((samples[landed].flags & KERNEL_VISUAL_FLAG_GROUNDED) != 0u);
    require(std::fabs(at.y - kLedgeY) < 0.2f);
    require(at.x < kRiserX - 1.0f);
    require(std::hypot(at.x - player_at.x, at.z - player_at.z) < 3.0f);

    // Capped under the riser: the same approach, and no leap at all. The run
    // above is the control -- this setup does leap when the cap allows it.
    auto capped_config = config;
    auto* capped = template_named(capped_config, "gingerbread");
    require(capped != nullptr);
    capped->chaser.leap.max_rise_meters = kLedgeY - 1.0f;
    const std::vector<Sample> capped_samples = run(capped_config, *capped, scene, 8078);
    require(capped_samples.size() == 30u * 15u);
    float farthest_back = 0.0f;
    for (const Sample& sample : capped_samples) {
        require((sample.flags & KERNEL_VISUAL_FLAG_LEAPING) == 0u);
        require(sample.position.y < 1.0f);
        farthest_back = std::max(farthest_back, sample.position.x - kRiserX);
    }
    std::printf("capped at %.1f m: never leapt, backed off at most %.2f m\n",
                capped->chaser.leap.max_rise_meters, farthest_back);
    // No run-up either: that only follows a leap the kernel refused.
    require(farthest_back < 2.0f);

    // Capped, starting 10 m out on the lower floor: inside vision of the
    // player up on the ledge, and no way up on foot.
    const KernelVec3 out_on_floor{kRiserX + 10.0f, 0.0f, -70.0f};
    auto nav_config = capped_config;
    nav_config.navigation_mesh.artifact = navmesh;
    const std::vector<Sample> held =
        run(nav_config, *template_named(nav_config, "gingerbread"), scene, 8079, out_on_floor);
    require(held.size() == 30u * 15u);
    float nearest_riser = 1000.0f;
    for (const Sample& sample : held) {
        require((sample.flags & KERNEL_VISUAL_FLAG_LEAPING) == 0u);
        nearest_riser = std::min(nearest_riser, sample.position.x - kRiserX);
    }
    const float held_at = held.back().position.x - kRiserX;
    std::printf("capped, navmesh: came within %.2f m of the riser, ended %.2f m out\n",
                nearest_riser, held_at);

    // The control: the same with no navmesh walks up to the riser's foot.
    const std::vector<Sample> blind = run(capped_config, *capped, scene, 8080, out_on_floor);
    require(blind.size() == 30u * 15u);
    const float blind_at = blind.back().position.x - kRiserX;
    std::printf("capped, no navmesh: ended %.2f m out\n", blind_at);
    require(blind_at < 2.0f);
    // It ends back where it was alerted, near where it started, not at the
    // riser.
    require(held_at > 8.0f);
    return 0;
}
