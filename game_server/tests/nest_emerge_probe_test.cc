// Two questions the nest "walk out of the building" design rests on, measured
// against the shipped catalog rather than argued from the code:
//
// 1. Does a player's rifle hit the nest at all, and does the nest shield a unit
//    standing inside it? Player hitscan is lag-compensated, and the rewound
//    raycast only tests history hit volumes -- so the answer depends on what the
//    nest records into history, not on its physics collider.
// 2. Can a gingerbread spawned inside the nest walk out, or does the nest's
//    collider hold it?
//
// Every measurement has a control beside it (a unit in the open, a walk with
// no nest), so "nothing happened" cannot pass for an answer.

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
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
constexpr std::uint8_t kRifleWeaponId = 0;
constexpr std::uint8_t kGingerbreadClawWeaponId = 10;

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
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

std::uint32_t template_id_of(
    const network_example::game_server::GameServerGameplayConfig& config,
    const std::string& name) {
    for (const network_example::game_server::EntityTemplateConfig& candidate :
         config.entity_templates) {
        if (candidate.name == name) {
            return candidate.actor_template_id;
        }
    }
    return 0;
}

// The catalog's own scene ids, as the apps pass them.
void install_ground(
    KernelHandle* kernel,
    const network_example::game_server::GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene) {
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
}

std::uint32_t create_from_template(
    KernelHandle* kernel,
    std::uint32_t entity_template_id,
    const KernelVec3& position) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_template_id = entity_template_id;
    create_info.position = position;
    create_info.rotation = kIdentityRotation;
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    require(net_id != 0u);
    return net_id;
}

// hp, or -1 when the entity is gone.
int hp_of(KernelHandle* kernel, std::uint32_t net_id) {
    if (net_id == 0u) {
        return -1;
    }
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    if (!Kernel_ServerGetEntityState(kernel, net_id, &state)) {
        return -1;
    }
    return static_cast<int>(state.hp);
}

KernelVec3 position_of(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    return state.position;
}

// ---------------------------------------------------------------------------
// 1. Rifle
// ---------------------------------------------------------------------------

struct RifleOutcome {
    KernelVec3 shooter{};
    std::uint32_t fire_confirmed = 0;
    int nest_hp_before = -1;
    int nest_hp_after = -1;
    int unit_hp_before = -1;
    int unit_hp_after = -1;
    std::uint32_t hits_on_nest = 0;
    std::uint32_t hits_on_unit = 0;
};

// A listen-server host firing is the same queued input a remote player's shot
// becomes (controlled_net_id == 0), so it takes the lag-compensated path.
RifleOutcome run_rifle_case(
    const network_example::game_server::GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene,
    std::uint16_t port,
    bool with_nest,
    bool with_unit) {
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_ListenServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    // The order the host app uses: catalog, then scene, then start.
    require(network_example::game_server::load_kernel_gameplay_catalog(
        kernel, config));
    install_ground(kernel, config, scene);
    require(Kernel_StartListenServer(kernel, port));
    // Constructed but never ticked: no mission, no patrols, nothing else in the
    // line of fire. It is here only so PlayerJoined arms the host with the
    // shipped loadout.
    network_example::game_server::GameServer game_server(kernel, config);

    RifleOutcome outcome;
    std::uint32_t nest = 0;
    std::uint32_t unit = 0;
    bool counting = false;
    auto drain_events = [&]() {
        std::array<KernelEvent, 256> events{};
        const std::uint32_t count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            const KernelEvent& event = events[index];
            game_server.handle_event(event);
            if (!counting) {
                continue;
            }
            if (event.type == KernelEventType_FireConfirmed) {
                ++outcome.fire_confirmed;
            }
            if (event.type == KernelEventType_HitConfirmed) {
                if (nest != 0u && event.net_id == nest) {
                    ++outcome.hits_on_nest;
                }
                if (unit != 0u && event.net_id == unit) {
                    ++outcome.hits_on_unit;
                }
            }
        }
    };

    drain_events();
    KernelLocalPlayerInfo local_info{};
    require(Kernel_GetLocalPlayerInfo(kernel, &local_info));
    require(local_info.player_net_id != 0u);
    const std::uint32_t player = local_info.player_net_id;
    const std::uint32_t local_player_id =
        local_info.peer_id != 0u ? local_info.peer_id : 1u;

    for (int tick = 0; tick < 15; ++tick) {
        Kernel_Update(kernel, kTickSeconds);
        drain_events();
    }

    const KernelVec3 origin = position_of(kernel, player);
    // Ten metres down +x, on the ground the player stands on.
    const KernelVec3 target{origin.x + 10.0f, origin.y, origin.z};
    if (with_nest) {
        nest = create_from_template(
            kernel, template_id_of(config, "gingerbread_nest"), target);
    }
    if (with_unit) {
        unit = create_from_template(
            kernel, template_id_of(config, "gingerbread"), target);
    }

    // Long enough for both to settle and to be in several history frames.
    for (int tick = 0; tick < 20; ++tick) {
        Kernel_Update(kernel, kTickSeconds);
        drain_events();
    }

    outcome.shooter = position_of(kernel, player);
    outcome.nest_hp_before = hp_of(kernel, nest);
    outcome.unit_hp_before = hp_of(kernel, unit);
    if (unit != 0u) {
        const KernelVec3 at = position_of(kernel, unit);
        std::fprintf(
            stderr, "  unit settled at (%.2f, %.2f, %.2f)\n", at.x, at.y, at.z);
    }

    // rifle_fire commits every 3 ticks while held; 7 ticks is 3 shots -- enough
    // for a hit to show, few enough that 45-damage shots do not simply kill a
    // 50 hp unit before the comparison can say who took them.
    counting = true;
    constexpr std::uint32_t kFireTicks = 7u;
    for (std::uint32_t tick = 1; tick <= kFireTicks; ++tick) {
        KernelPlayerInput input{};
        input.input_seq = tick;
        input.selected_weapon = kRifleWeaponId;
        input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
        if (tick == 1u) {
            input.action_intent = KernelActionIntent{
                5001u, KernelActionBinding_PrimaryFire, 0u, 0u};
        }
        input.action_input = KernelActionInput{5001u, 1u, 0u, 0u};
        Kernel_SubmitPlayerInput(kernel, local_player_id, &input);
        Kernel_Update(kernel, kTickSeconds);
        drain_events();
    }
    for (int tick = 0; tick < 5; ++tick) {
        Kernel_Update(kernel, kTickSeconds);
        drain_events();
    }

    outcome.nest_hp_after = hp_of(kernel, nest);
    outcome.unit_hp_after = hp_of(kernel, unit);
    Kernel_Destroy(kernel);
    return outcome;
}

void print_rifle(const char* label, const RifleOutcome& outcome) {
    std::fprintf(
        stderr,
        "[rifle] %-26s shooter_y=%.2f fire_confirmed=%u "
        "nest_hp %d -> %d (hits %u) unit_hp %d -> %d (hits %u)\n",
        label,
        outcome.shooter.y,
        outcome.fire_confirmed,
        outcome.nest_hp_before,
        outcome.nest_hp_after,
        outcome.hits_on_nest,
        outcome.unit_hp_before,
        outcome.unit_hp_after,
        outcome.hits_on_unit);
}

// ---------------------------------------------------------------------------
// 2. Walking out
// ---------------------------------------------------------------------------

struct WalkOutcome {
    KernelVec3 spawned{};
    KernelVec3 after_first_tick{};
    KernelVec3 start{};
    KernelVec3 end{};
    float horizontal_distance = 0.0f;
};

WalkOutcome run_walk_case(
    const network_example::game_server::GameServerGameplayConfig& base_config,
    const std::vector<std::uint8_t>& scene,
    std::uint16_t port,
    bool with_nest,
    const KernelVec3& unit_spawn,
    int settle_ticks,
    std::uint32_t movement_mask_override = 0u,
    std::uint32_t runtime_mask = 0u) {
    // A copy, so an override is scoped to this one world.
    network_example::game_server::GameServerGameplayConfig config = base_config;
    if (movement_mask_override != 0u) {
        for (auto& candidate : config.entity_templates) {
            if (candidate.name == "gingerbread") {
                candidate.movement_collision_mask = movement_mask_override;
            }
        }
    }
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(network_example::game_server::load_kernel_gameplay_catalog(
        kernel, config));
    install_ground(kernel, config, scene);
    require(Kernel_StartDedicatedServer(kernel, port));

    if (with_nest) {
        create_from_template(
            kernel,
            template_id_of(config, "gingerbread_nest"),
            KernelVec3{0.0f, 0.0f, 0.0f});
    }
    const std::uint32_t unit = create_from_template(
        kernel, template_id_of(config, "gingerbread"), unit_spawn);
    // The runtime switch, which is what the entry sequence will actually use:
    // set between creation and the first Kernel_Update, so the unit's first
    // physics tick already runs under the entry mask and it is never shoved out
    // of the box it was spawned in.
    if (runtime_mask != 0u) {
        require(Kernel_ServerSetEntityMovementCollisionMask(
            kernel, unit, runtime_mask));
    }

    WalkOutcome outcome;
    outcome.spawned = unit_spawn;
    std::array<KernelEvent, 256> events{};
    auto step = [&](bool move, std::uint32_t seq) {
        if (move) {
            KernelPlayerInput input{};
            input.input_seq = seq;
            input.move = KernelVec2{1.0f, 0.0f};
            input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
            input.selected_weapon = kGingerbreadClawWeaponId;
            require(Kernel_ServerSubmitEntityInput(kernel, unit, &input));
        }
        Kernel_Update(kernel, kTickSeconds);
        Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
    };

    std::uint32_t seq = 0;
    for (int tick = 0; tick < settle_ticks; ++tick) {
        step(false, 0u);
    }
    outcome.start = position_of(kernel, unit);
    // 45 ticks: the 1.5 s upper bound of the emerge animation.
    for (std::uint32_t tick = 1; tick <= 45u; ++tick) {
        step(true, ++seq);
        if (tick == 1u) {
            outcome.after_first_tick = position_of(kernel, unit);
        }
    }
    outcome.end = position_of(kernel, unit);
    const float dx = outcome.end.x - outcome.start.x;
    const float dz = outcome.end.z - outcome.start.z;
    outcome.horizontal_distance = std::sqrt(dx * dx + dz * dz);
    Kernel_Destroy(kernel);
    return outcome;
}

void print_walk(const char* label, const WalkOutcome& outcome) {
    std::fprintf(
        stderr,
        "[walk]  %-26s spawned=(%.2f, %.2f, %.2f) start=(%.2f, %.2f, %.2f) "
        "tick1=(%.2f, %.2f, %.2f) end=(%.2f, %.2f, %.2f) horizontal=%.2f m\n",
        label,
        outcome.spawned.x, outcome.spawned.y, outcome.spawned.z,
        outcome.start.x, outcome.start.y, outcome.start.z,
        outcome.after_first_tick.x, outcome.after_first_tick.y,
        outcome.after_first_tick.z,
        outcome.end.x, outcome.end.y, outcome.end.z,
        outcome.horizontal_distance);
}

}  // namespace

int main() {
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    require(template_id_of(config, "gingerbread_nest") != 0u);
    require(template_id_of(config, "gingerbread") != 0u);
    const std::vector<std::uint8_t> scene = read_ground_scene();

    // --- 1. Rifle -----------------------------------------------------------
    const RifleOutcome unit_in_open =
        run_rifle_case(config, scene, 7931, false, true);
    print_rifle("unit in the open (control)", unit_in_open);
    const RifleOutcome nest_alone =
        run_rifle_case(config, scene, 7932, true, false);
    print_rifle("nest alone", nest_alone);
    const RifleOutcome unit_in_nest =
        run_rifle_case(config, scene, 7933, true, true);
    print_rifle("unit inside nest", unit_in_nest);

    // --- 2. Walking ---------------------------------------------------------
    const WalkOutcome walk_open = run_walk_case(
        config, scene, 7934, false, KernelVec3{0.0f, 0.0f, 0.0f}, 15);
    print_walk("no nest (control)", walk_open);
    // Spawned dead centre of the nest's box and walked on the first tick, so
    // any push-out the nest applies shows in tick1 rather than hiding in a
    // settle period.
    const WalkOutcome walk_out = run_walk_case(
        config, scene, 7935, true, KernelVec3{0.0f, 0.0f, 0.0f}, 0);
    print_walk("out from nest centre", walk_out);
    // From outside, straight at the nest: if its collider blocks movement the
    // unit stops at the box face (x ~ -1.4) instead of reaching x ~ 3.5.
    const WalkOutcome walk_in = run_walk_case(
        config, scene, 7936, true, KernelVec3{-4.0f, 0.0f, 0.0f}, 15);
    print_walk("into nest from outside", walk_in);
    // The same two walks with static_obstacle struck from the unit's mask.
    constexpr std::uint32_t kNoStaticObstacle =
        KERNEL_MOVEMENT_LAYER_TERRAIN | KERNEL_MOVEMENT_LAYER_ACTOR;
    const WalkOutcome walk_out_masked = run_walk_case(
        config, scene, 7938, true, KernelVec3{0.0f, 0.0f, 0.0f}, 0,
        kNoStaticObstacle);
    print_walk("out, mask terrain|actor", walk_out_masked);
    const WalkOutcome walk_in_masked = run_walk_case(
        config, scene, 7939, true, KernelVec3{-4.0f, 0.0f, 0.0f}, 15,
        kNoStaticObstacle);
    print_walk("into, mask terrain|actor", walk_in_masked);

    // The same two walks again, but through the runtime API rather than a
    // doctored template -- this is the switch the entry sequence will make.
    const WalkOutcome walk_out_runtime = run_walk_case(
        config, scene, 7940, true, KernelVec3{0.0f, 0.0f, 0.0f}, 0, 0u,
        kNoStaticObstacle);
    print_walk("out, runtime mask", walk_out_runtime);
    const WalkOutcome walk_in_runtime = run_walk_case(
        config, scene, 7941, true, KernelVec3{-4.0f, 0.0f, 0.0f}, 15, 0u,
        kNoStaticObstacle);
    print_walk("into, runtime mask", walk_in_runtime);

    // Set before the first tick, the unit is never shoved out of the box it
    // spawned in, and it walks the same line as the template-level override.
    require(std::fabs(walk_out_runtime.after_first_tick.z) < 0.01f);
    require(walk_out_runtime.horizontal_distance > 3.0f);
    // And the nest stops blocking while the mask is off: it walks through.
    require(walk_in_runtime.end.x > 1.0f);
    // Default mask, same walk: the nest is a wall again.
    require(walk_in.end.x < -1.0f);

    // The controls have to show a grounded shooter, the rifle firing and
    // landing, and the walker covering real ground on the ground, or every
    // other number above means nothing.
    require(std::fabs(unit_in_open.shooter.y) < 0.5f);
    require(unit_in_open.fire_confirmed > 0u);
    require(nest_alone.fire_confirmed > 0u);
    require(unit_in_nest.fire_confirmed > 0u);
    require(unit_in_open.unit_hp_before > 0);
    require(unit_in_open.unit_hp_after < unit_in_open.unit_hp_before);
    // A prop sized from its collider is in the rewound world: the rifle lands.
    require(nest_alone.nest_hp_before > 0);
    require(nest_alone.nest_hp_after < nest_alone.nest_hp_before);
    require(nest_alone.hits_on_nest > 0u);
    require(std::fabs(walk_open.end.y) < 0.5f);
    require(walk_open.horizontal_distance > 3.0f);

    return 0;
}
