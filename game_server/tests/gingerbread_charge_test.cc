// A gingerbread akaoni charges a player who stands in its charge band: it
// stops and winds up, then runs at where the player stood, and the run itself
// is the hit.
//
// The shipped game, headless, as the dedicated server runs it: a unit created
// in an open corner of the plane, driven by its own chaser AI. The player
// backs off ahead of it through most of the alert -- a chaser closes the whole
// time it is alert -- then stands still inside the band. Measured: the unit
// stands still for its wind-up; the run shows as KERNEL_VISUAL_FLAG_CHARGING,
// at the authored speed; it strikes the player once, for the shot template's
// damage, and throws it; and it stops where it struck instead of running on.

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
    const auto* akaoni = template_named(config, "gingerbread_akaoni");
    require(akaoni != nullptr);
    const network_example::game_server::AgentChargeTuning& charge = akaoni->chaser.charge;
    require(charge.max_distance_meters > 0.0f);
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
    require(Kernel_StartListenServer(kernel, 8023));

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
    const KernelQuat identity{0.0f, 0.0f, 0.0f, 1.0f};
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = KernelEntityType_Actor;
    create.actor_type = KernelActorType_Agent;
    create.entity_template_id = akaoni->actor_template_id;
    create.actor_template_id = akaoni->actor_template_id;
    create.position = KernelVec3{-70.0f, 0.0f, -70.0f};
    create.rotation = identity;
    std::uint32_t unit = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &unit));
    require(Kernel_ServerSetEntityActorTemplate(kernel, unit, akaoni->actor_template_id));
    for (int tick = 0; tick < 10; ++tick) step();
    // Backing off 10 m ahead of it, out on -x where it looks, through most of
    // the alert: inside the band, but a charge only starts once it attacks.
    const std::uint32_t backing_ticks = akaoni->sentry.alert_ticks - 10u;
    KernelVec3 player_at{};
    for (std::uint32_t tick = 0; tick < backing_ticks; ++tick) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(kernel, unit, &state));
        player_at = KernelVec3{state.position.x - 10.0f, state.position.y, state.position.z};
        require(Kernel_ServerSetEntityTransform(kernel, player, &player_at, &identity));
        step();
    }
    // Then stands still, 8 m out.
    KernelServerEntityState before{};
    before.struct_size = sizeof(before);
    require(Kernel_ServerGetEntityState(kernel, unit, &before));
    const float distance = 8.0f;
    require(distance > charge.min_distance_meters && distance < charge.max_distance_meters);
    player_at = KernelVec3{before.position.x - distance, before.position.y, before.position.z};
    require(Kernel_ServerSetEntityTransform(kernel, player, &player_at, &identity));
    std::printf("unit #%u at (%.2f, %.2f), player stands %.2f m out\n",
                unit, before.position.x, before.position.z, distance);

    struct Sample {
        KernelVec3 position;
        std::uint32_t flags;
        std::uint16_t player_hp;
        std::uint32_t player_flags;
        KernelVec3 player_position;
    };
    std::vector<Sample> samples;
    bool struck = false;
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
        // The player holds still where it was put, until it is struck: then
        // the knockback is what moves it.
        struck = struck || (!samples.empty() && target.hp < samples.front().player_hp);
        if (!struck && horizontal(target.position, player_at) > 0.01f) {
            require(Kernel_ServerSetEntityTransform(kernel, player, &player_at, &identity));
        }
        samples.push_back(Sample{
            state.position, state.visual_flags, target.hp, target.visual_flags,
            target.position});
    }
    Kernel_Destroy(kernel);

    // The run: the first tick flagged, and the first after it that is not.
    std::size_t run = samples.size();
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if ((samples[i].flags & KERNEL_VISUAL_FLAG_CHARGING) != 0u) { run = i; break; }
    }
    require(run < samples.size() && run > 0u);
    std::size_t stopped = run;
    while (stopped < samples.size() &&
           (samples[stopped].flags & KERNEL_VISUAL_FLAG_CHARGING) != 0u) {
        ++stopped;
    }
    require(stopped < samples.size());

    // The wind-up before it: standing still for windup_ticks.
    std::size_t still = 0;
    for (std::size_t i = run - 1u; i > 0u; --i) {
        if (horizontal(samples[i].position, samples[i - 1u].position) > 0.01f) break;
        ++still;
    }
    std::printf("stood still %zu ticks before the run (wind-up %u)\n",
                still, charge.windup_ticks);
    require(still + 1u >= charge.windup_ticks);

    // At the authored speed, along the line to the player.
    const float first_step = horizontal(samples[run].position, samples[run - 1u].position);
    std::printf("run from %zu to %zu (%zu ticks), first step %.3f m (authored %.3f)\n",
                run, stopped, stopped - run, first_step,
                charge.speed_meters_per_second * kTickSeconds);
    require(std::fabs(first_step - charge.speed_meters_per_second * kTickSeconds) < 0.03f);

    // It struck the player, once, for the shot's 35.
    const std::uint16_t hp_before = samples[run - 1u].player_hp;
    std::size_t hurt = samples.size();
    for (std::size_t i = run; i < samples.size(); ++i) {
        if (samples[i].player_hp < hp_before) { hurt = i; break; }
    }
    require(hurt < samples.size());
    std::printf("player hp %u -> %u at %zu\n", hp_before, samples[hurt].player_hp, hurt);
    require(hp_before - samples[hurt].player_hp == 35);
    // Nothing more from the charge itself in the ticks that follow.
    for (std::size_t i = hurt; i < std::min(samples.size(), hurt + 4u); ++i) {
        require(samples[i].player_hp == samples[hurt].player_hp);
    }
    // And threw it: knocked back, away from where it stood.
    bool thrown = false;
    for (std::size_t i = hurt; i < std::min(samples.size(), hurt + 10u); ++i) {
        thrown = thrown || (samples[i].player_flags & KERNEL_VISUAL_FLAG_KNOCKED_BACK) != 0u;
    }
    require(thrown);
    // The hit ended the run. The contact and the end of the run are the same
    // tick; the damage pipeline lands the hp a few ticks later. And it ended
    // well before the ticks it was given to cover distance + overshoot.
    const std::uint32_t given = static_cast<std::uint32_t>(std::ceil(
        (distance + charge.overshoot_meters) / charge.speed_meters_per_second /
        kTickSeconds));
    std::printf("run ended %zu ticks before its hp landed; given %u ticks\n",
                hurt - stopped, given);
    require(stopped <= hurt && hurt - stopped <= 4u);
    require(stopped - run + 5u < given);
    const float ran = horizontal(samples[stopped].position, samples[run - 1u].position);
    std::printf("ran %.2f m of %.2f + %.2f overshoot\n",
                ran, distance, charge.overshoot_meters);
    require(ran < distance + charge.overshoot_meters - 0.5f);
    return 0;
}
