// A gingerbread closes the last metres on a player with a leap, and its claw
// comes down with its feet.
//
// The shipped game, headless, as the dedicated server runs it: a unit its nest
// put out, driven by its own chaser AI. The player backs off ahead of it while
// it is alerted -- a chaser closes the whole time it is alert, and a player
// that stood still would be walked up to long before the unit may attack --
// then stops inside its leap band. Measured: the leap shows as KERNEL_VISUAL_FLAG_LEAPING
// and never as a knockback; it lands where 28_gingerbread.yaml says, short of
// the player; the swing starts in the air; its commit falls on the landing;
// and it walks again with no knockdown.

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

std::vector<std::uint8_t> read_ground_scene() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    const std::filesystem::path path = std::filesystem::path(test_srcdir) /
        test_workspace / "game_server" / "gameplay_catalog" / "mesh_assets" /
        "jolt" / "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

const network_example::game_server::ActorTemplateConfig* template_named(
    const network_example::game_server::GameServerGameplayConfig& config,
    const std::string& name) {
    for (const auto& candidate : config.actor_templates) {
        if (candidate.name == name) return &candidate;
    }
    return nullptr;
}

float horizontal(const KernelVec3& a, const KernelVec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

}  // namespace

int main() {
    const auto config = network_example::game_server::default_game_server_gameplay_config();
    const auto* gingerbread = template_named(config, "gingerbread");
    require(gingerbread != nullptr);
    const network_example::game_server::AgentLeapTuning& leap = gingerbread->chaser.leap;
    require(leap.max_distance_meters > 0.0f);
    const std::vector<std::uint8_t> scene = read_ground_scene();

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
    require(Kernel_StartListenServer(kernel, 8021));

    network_example::game_server::GameServer game_server(kernel, config);
    require(game_server.preload_directors());
    std::array<KernelEvent, 4096> events{};
    const auto step = [&]() {
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t n = Kernel_PollEvents(kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t i = 0; i < n; ++i) game_server.handle_event(events[i]);
        game_server.tick(kTickSeconds);
    };
    for (int tick = 0; tick < 30 * 5; ++tick) step();
    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(kernel, &local));
    const std::uint32_t player = local.player_net_id;
    // A unit of our own in an open corner of the plane, away from the nests:
    // one still standing in its nest's doorway leaps into the nest's roof.
    const KernelQuat identity{0.0f, 0.0f, 0.0f, 1.0f};
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = KernelEntityType_Actor;
    create.actor_type = KernelActorType_Agent;
    create.entity_template_id = gingerbread->actor_template_id;
    create.actor_template_id = gingerbread->actor_template_id;
    // Far enough in from the -x edge that it can chase out that way for the
    // whole alert with the player 25 m ahead of it and still on the plane.
    create.position = KernelVec3{40.0f, 0.0f, -70.0f};
    create.rotation = identity;
    std::uint32_t unit = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &unit));
    require(Kernel_ServerSetEntityActorTemplate(kernel, unit, gingerbread->actor_template_id));
    const KernelVec3 unit_at = create.position;
    for (int tick = 0; tick < 10; ++tick) step();
    // Out on -x, where a unit created facing identity looks.
    const float distance = 0.5f * (leap.min_distance_meters + leap.max_distance_meters);
    // Backing off: kept 25 m ahead of it -- inside its 26 m vision cone, past
    // the far end of the band -- through the alert.
    const std::uint32_t backing_ticks = gingerbread->sentry.alert_ticks + 20u;
    KernelVec3 player_at{unit_at.x - 25.0f, unit_at.y, unit_at.z};
    for (std::uint32_t tick = 0; tick < backing_ticks; ++tick) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(kernel, unit, &state));
        player_at = KernelVec3{state.position.x - 25.0f, state.position.y, state.position.z};
        require(Kernel_ServerSetEntityTransform(kernel, player, &player_at, &identity));
        step();
    }
    // Then stops, inside the band.
    KernelServerEntityState before{};
    before.struct_size = sizeof(before);
    require(Kernel_ServerGetEntityState(kernel, unit, &before));
    player_at = KernelVec3{before.position.x - distance, before.position.y, before.position.z};
    require(Kernel_ServerSetEntityTransform(kernel, player, &player_at, &identity));
    std::printf("unit #%u at (%.2f, %.2f), player stops %.2f m out\n",
                unit, before.position.x, before.position.z, distance);

    struct Sample {
        KernelVec3 position;
        std::uint32_t flags;
        std::uint32_t action_instance;
        std::uint32_t commit_count;
        std::uint16_t player_hp;
        KernelVec3 player_position;
    };
    std::vector<Sample> samples;
    for (int tick = 0; tick < 30 * 8; ++tick) {
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
        if (horizontal(target.position, player_at) > 0.01f) {
            require(Kernel_ServerSetEntityTransform(kernel, player, &player_at, &identity));
        }
        samples.push_back(Sample{
            state.position, state.visual_flags, state.action.action_instance_id,
            state.action.commit_count, target.hp, target.position});
    }
    Kernel_Destroy(kernel);

    // The leap: the first tick flagged, and the first after it that is not.
    std::size_t lift = samples.size();
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if ((samples[i].flags & KERNEL_VISUAL_FLAG_LEAPING) != 0u) { lift = i; break; }
    }
    require(lift < samples.size());
    std::size_t landed = lift;
    while (landed < samples.size() &&
           (samples[landed].flags & KERNEL_VISUAL_FLAG_LEAPING) != 0u) {
        // A leap, never a knockback.
        require((samples[landed].flags & KERNEL_VISUAL_FLAG_KNOCKED_BACK) == 0u);
        ++landed;
    }
    require(landed + 30u < samples.size());
    require((samples[landed].flags & KERNEL_VISUAL_FLAG_GROUNDED) != 0u);
    const float flown = horizontal(samples[landed].position, samples[lift - 1u].position);
    const float short_of = horizontal(samples[landed].position, player_at);
    std::printf("lifted at %zu, landed at %zu (%zu ticks flagged), flew %.2f m, "
                "landed %.3f m from the player (authored %.2f)\n",
                lift, landed, landed - lift, flown, short_of,
                leap.landing_distance_meters);
    require(flown > leap.min_distance_meters - leap.landing_distance_meters - 0.5f);
    require(std::fabs(short_of - leap.landing_distance_meters) < 0.05f);

    // The swing: started in the air, committed with the landing.
    std::size_t swing = samples.size();
    for (std::size_t i = lift; i < samples.size(); ++i) {
        if (samples[i].action_instance != 0u &&
            samples[i].action_instance != samples[lift - 1u].action_instance) {
            swing = i;
            break;
        }
    }
    require(swing < landed);
    std::size_t commit = samples.size();
    for (std::size_t i = swing; i < samples.size(); ++i) {
        if (samples[i].commit_count > 0u) { commit = i; break; }
    }
    require(commit < samples.size());
    const long commit_after_landing =
        static_cast<long>(commit) - static_cast<long>(landed);
    std::printf("swing started at %zu (%zu before landing), committed %ld ticks "
                "after landing\n",
                swing, landed - swing, commit_after_landing);
    require(commit_after_landing == 0);
    // The claw's damage reaches the player a few ticks after the commit.
    std::size_t hurt = samples.size();
    for (std::size_t i = commit; i < std::min(samples.size(), commit + 6u); ++i) {
        if (samples[i].player_hp < samples[lift].player_hp) { hurt = i; break; }
    }
    require(hurt < samples.size());
    std::printf("player hp %u -> %u, %zu ticks after the commit\n",
                samples[lift].player_hp, samples[hurt].player_hp, hurt - commit);

    // No knockdown: it is not rooted after it lands. It holds inside its stop
    // distance, so what shows it free is that nothing refuses its next swing.
    bool swung_again = false;
    for (std::size_t i = commit + 1u; i < samples.size(); ++i) {
        if (samples[i].action_instance != samples[commit].action_instance &&
            samples[i].action_instance != 0u) {
            swung_again = true;
            std::printf("swung again at %zu, %zu ticks after landing\n", i, i - landed);
            break;
        }
    }
    require(swung_again);
    return 0;
}
