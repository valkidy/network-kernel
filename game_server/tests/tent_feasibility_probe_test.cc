// The questions the tent design rests on, measured against the shipped
// catalog rather than argued from the code:
//
// tent_kit throws tent_kit_prop, which wears the tent's hitbox, sweeps it in
// flight, and on landing spawns the tent (a pure prop, so it can carry a
// lifecycle) at event.position and destroys itself.
//
// 1. Landing. Where does the tent end up relative to where the kit came to
//    rest -- on flat ground, and thrown at an ice block?
//    Measured 2026-09-30: event.position is the contact point, not the kit's
//    rest position. On flat ground the tent appears ~1 m further along the
//    throw than the kit stopped; thrown at an ice block, the kit stops against
//    its side and the tent spawns on the side face, half inside the block
//    (overlap ~2.4 m^3, floating). With a terrain-only landing mask the kit
//    passes through the block and the tent lands fully inside it.
// 2. Reach. The interaction range is 3D: from the ground 1.8 m from a tent on
//    the ground, activation commits; 2.8 m from the one stuck on the ice
//    block, it is rejected out of range.
// 3. Pickup. The tent is not a world item: pickup is rejected.
// 4. Landing on units. The kit is a moving static obstacle, so it shoves the
//    bodies ahead of it along the throw; all end outside the tent's footprint.
// 5. Occupants. Four players moved to the tent centre under a terrain-only
//    movement mask stay inside and still; under the default mask all four are
//    pushed out.
// 6. Enemy fire at an occupant. A beam is stopped by the tent (the tent takes
//    every hit). A grenade's area effect still reaches the occupant, hence the
//    damage immunity. The spammer landed nothing even in the open control, so
//    it says nothing either way.
//
// Every case prints what it measured. The requires pin the controls and the
// answers the tent design relies on, so "nothing happened" cannot pass for an
// answer.

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

using network_example::game_server::GameServerGameplayConfig;

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
constexpr std::uint32_t kThrowerPeer = 7;
constexpr std::uint32_t kOtherPeer = 8;
// tent_hitbox: centred 1.2 up, half 1.2 on every axis.
constexpr float kTentHalf = 1.2f;
constexpr float kTentHeight = 2.4f;
// ice_block_hitbox: centred 1.5 up, half 1.5 x 1.5 x 0.8.
constexpr float kIceHalfX = 1.5f;
constexpr float kIceHalfZ = 0.8f;
constexpr float kIceHeight = 3.0f;

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

std::uint32_t entity_template_id_of(
    const GameServerGameplayConfig& config, const std::string& name) {
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == name) {
            return candidate.actor_template_id;
        }
    }
    return 0;
}

std::uint32_t item_template_id_of(
    const GameServerGameplayConfig& config, const std::string& name) {
    for (const auto& candidate : config.item_templates) {
        if (candidate.name == name) {
            return candidate.definition.item_template_id;
        }
    }
    return 0;
}

KernelHandle* make_world(
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene,
    std::uint16_t port) {
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
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartDedicatedServer(kernel, port));
    return kernel;
}

void step(KernelHandle* kernel, int ticks) {
    std::array<KernelEvent, 256> events{};
    for (int tick = 0; tick < ticks; ++tick) {
        Kernel_Update(kernel, kTickSeconds);
        Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
    }
}

// owner_peer decides a prop's side: non-zero is player_side, zero hostile_side.
// A thrown tent inherits its thrower's peer, so a test tent needs one too.
std::uint32_t create_entity(
    KernelHandle* kernel,
    std::uint32_t entity_template_id,
    const KernelVec3& position,
    std::uint32_t owner_peer = 0u) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_template_id = entity_template_id;
    create_info.owner_peer = owner_peer;
    create_info.position = position;
    create_info.rotation = kIdentityRotation;
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    require(net_id != 0u);
    return net_id;
}

std::uint32_t spawn_player(
    KernelHandle* kernel,
    const GameServerGameplayConfig& config,
    std::uint32_t peer,
    const KernelVec3& position) {
    const std::uint32_t template_id = config.player.actor_template_id;
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = network_example::game_server::kEntityTypeActor;
    create.actor_type = network_example::game_server::kActorTypePlayer;
    create.entity_template_id = template_id;
    create.actor_template_id = template_id;
    create.owner_peer = peer;
    create.position = position;
    create.rotation = kIdentityRotation;
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
    require(net_id != 0u);
    require(Kernel_ServerSetEntityActorTemplate(kernel, net_id, template_id));
    return net_id;
}

bool exists(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    return Kernel_ServerGetEntityState(kernel, net_id, &state);
}

KernelVec3 position_of(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    return state.position;
}

KernelGameplayRequestOutcome submit(
    KernelHandle* kernel,
    std::uint32_t peer,
    std::uint64_t request_id,
    std::uint32_t instigator,
    std::uint8_t domain_action,
    KernelItemInstanceId item,
    std::uint32_t target,
    const KernelVec3& direction) {
    KernelGameplayRequest request{};
    request.struct_size = sizeof(request);
    request.requester_peer = peer;
    request.request_id = request_id;
    request.instigator_net_id = instigator;
    request.domain_action = domain_action;
    request.selected_item_instance_id = item;
    request.target_net_id = target;
    request.requested_quantity = 1;
    request.throw_direction = direction;
    require(Kernel_ServerSubmitGameplayRequest(kernel, &request));
    // Some requests commit on submit, some on the next tick.
    KernelGameplayRequestOutcome outcomes[4]{};
    for (KernelGameplayRequestOutcome& outcome : outcomes) {
        outcome.struct_size = sizeof(outcome);
    }
    std::uint32_t count = Kernel_PollGameplayRequestOutcomes(kernel, outcomes, 4);
    for (int tick = 0; count == 0u && tick < 3; ++tick) {
        step(kernel, 1);
        count = Kernel_PollGameplayRequestOutcomes(kernel, outcomes, 4);
    }
    require(count >= 1u);
    return outcomes[0];
}

const char* status_name(std::uint8_t status) {
    switch (status) {
        case KernelGameplayRequestStatus_Committed: return "committed";
        case KernelGameplayRequestStatus_Rejected: return "rejected";
        default: return "no_action";
    }
}

constexpr std::uint16_t kTentMaxHp = 600;

// The live tent, or 0.
std::uint32_t find_tent(KernelHandle* kernel) {
    std::vector<KernelServerEntityState> states(64);
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    const std::uint32_t count = Kernel_ServerQueryEntities(
        kernel, 0u, states.data(), static_cast<std::uint32_t>(states.size()));
    for (std::uint32_t index = 0; index < count && index < states.size(); ++index) {
        // Prop states carry no template id (it reads 0), so the tent is known
        // by its hp: 600, and no other prop in these worlds has that.
        if (states[index].entity_type == KernelEntityType_Prop &&
            states[index].max_hp == kTentMaxHp) {
            return states[index].net_id;
        }
    }
    return 0u;
}

// Throws one tent_kit from `thrower` along `direction`; returns the kit.
std::uint32_t throw_tent(
    KernelHandle* kernel,
    const GameServerGameplayConfig& config,
    std::uint32_t thrower,
    const KernelVec3& direction) {
    KernelInventoryContainerId container = 0;
    require(Kernel_ServerCreateInventoryContainer(kernel, thrower, 8, &container));
    KernelItemInstanceId item = 0;
    require(Kernel_ServerCreateInventoryItem(
        kernel, item_template_id_of(config, "tent_kit"), 1, container, &item));
    step(kernel, 1);
    const KernelGameplayRequestOutcome outcome = submit(
        kernel, kThrowerPeer, 1, thrower, KernelDomainAction_Throw, item, 0u,
        direction);
    std::fprintf(
        stderr, "  throw: %s reason=%u prop=%u\n",
        status_name(outcome.status), outcome.rejection_reason,
        outcome.prop_entity_id);
    require(outcome.status == KernelGameplayRequestStatus_Committed);
    require(outcome.prop_entity_id != 0u);
    return outcome.prop_entity_id;
}

struct Landing {
    // Where the kit was on the last tick it existed: its rest position.
    KernelVec3 kit{};
    std::uint32_t tent = 0;
    KernelVec3 tent_position{};
    KernelQuat tent_rotation{};
    int ticks = 0;
};

// Ticks until the kit has spawned the tent, or 240 ticks pass.
Landing land(KernelHandle* kernel, std::uint32_t kit) {
    Landing landing;
    for (int tick = 0; tick < 240 && landing.tent == 0u; ++tick) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        if (Kernel_ServerGetEntityState(kernel, kit, &state)) {
            landing.kit = state.position;
        }
        step(kernel, 1);
        landing.tent = find_tent(kernel);
        landing.ticks = tick + 1;
    }
    if (landing.tent != 0u) {
        step(kernel, 5);
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(kernel, landing.tent, &state));
        landing.tent_position = state.position;
        landing.tent_rotation = state.rotation;
    }
    if (landing.tent == 0u) {
        std::vector<KernelServerEntityState> states(64);
        for (KernelServerEntityState& state : states) {
            state.struct_size = sizeof(KernelServerEntityState);
        }
        const std::uint32_t count = Kernel_ServerQueryEntities(
            kernel, 0u, states.data(), static_cast<std::uint32_t>(states.size()));
        for (std::uint32_t index = 0; index < count && index < states.size(); ++index) {
            std::fprintf(
                stderr, "  entity %u type=%u template=%u hp=%u/%u at (%.2f, %.2f, %.2f)\n",
                states[index].net_id, states[index].entity_type,
                states[index].actor_template_id, states[index].hp,
                states[index].max_hp, states[index].position.x,
                states[index].position.y, states[index].position.z);
        }
    }
    std::fprintf(
        stderr,
        "  kit rest=(%.2f, %.2f, %.2f) -> tent=%u at (%.2f, %.2f, %.2f) "
        "rot=(%.2f, %.2f, %.2f, %.2f) after %d ticks; kit still exists: %s\n",
        landing.kit.x, landing.kit.y, landing.kit.z,
        landing.tent,
        landing.tent_position.x, landing.tent_position.y,
        landing.tent_position.z,
        landing.tent_rotation.x, landing.tent_rotation.y,
        landing.tent_rotation.z, landing.tent_rotation.w,
        landing.ticks,
        exists(kernel, kit) ? "yes" : "no");
    return landing;
}

float overlap_1d(float a_min, float a_max, float b_min, float b_max) {
    return std::fmax(0.0f, std::fmin(a_max, b_max) - std::fmax(a_min, b_min));
}

// Overlap volume of the tent's box and an ice block's box, both unrotated.
float tent_ice_overlap(const KernelVec3& tent, const KernelVec3& ice) {
    return overlap_1d(tent.x - kTentHalf, tent.x + kTentHalf,
                      ice.x - kIceHalfX, ice.x + kIceHalfX) *
        overlap_1d(tent.y, tent.y + kTentHeight, ice.y, ice.y + kIceHeight) *
        overlap_1d(tent.z - kTentHalf, tent.z + kTentHalf,
                   ice.z - kIceHalfZ, ice.z + kIceHalfZ);
}

bool inside_tent_footprint(const KernelVec3& tent, const KernelVec3& body) {
    return std::fabs(body.x - tent.x) < kTentHalf &&
        std::fabs(body.z - tent.z) < kTentHalf;
}

const KernelVec3 kThrowDirection{0.94f, 0.34f, 0.0f};

// ---------------------------------------------------------------------------
// 1-3. Landing, reach, pickup
// ---------------------------------------------------------------------------

struct LandingOutcome {
    KernelVec3 kit{};
    KernelVec3 tent{};
    float overlap = 0.0f;
    KernelGameplayRequestOutcome activate{};
    KernelGameplayRequestOutcome pickup{};
    float reach_distance = 0.0f;
};

// `ice_at_x` < 0 means no ice block. The reacher stands `stand_off` metres
// short of the landing spot along -x, on the ground.
LandingOutcome run_landing_case(
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene,
    std::uint16_t port,
    float ice_at_x,
    float stand_off) {
    KernelHandle* kernel = make_world(config, scene, port);
    const std::uint32_t thrower =
        spawn_player(kernel, config, kThrowerPeer, KernelVec3{0.0f, 0.0f, 0.0f});
    KernelVec3 ice{};
    if (ice_at_x >= 0.0f) {
        ice = KernelVec3{ice_at_x, 0.0f, 0.0f};
        create_entity(kernel, entity_template_id_of(config, "ice_block"), ice);
    }
    step(kernel, 5);
    const std::uint32_t kit = throw_tent(kernel, config, thrower, kThrowDirection);
    const Landing landing = land(kernel, kit);
    require(landing.tent != 0u);
    const std::uint32_t tent = landing.tent;

    LandingOutcome outcome;
    outcome.kit = landing.kit;
    outcome.tent = landing.tent_position;
    if (ice_at_x >= 0.0f) {
        outcome.overlap = tent_ice_overlap(outcome.tent, ice);
    }

    const KernelVec3 stand{outcome.tent.x - stand_off, 0.0f, outcome.tent.z};
    const std::uint32_t reacher = spawn_player(kernel, config, kOtherPeer, stand);
    step(kernel, 10);
    const KernelVec3 at = position_of(kernel, reacher);
    const float dx = at.x - outcome.tent.x;
    const float dy = at.y - outcome.tent.y;
    const float dz = at.z - outcome.tent.z;
    outcome.reach_distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    outcome.activate = submit(
        kernel, kOtherPeer, 10, reacher, KernelDomainAction_Activate, 0u, tent,
        KernelVec3{1.0f, 0.0f, 0.0f});
    outcome.pickup = submit(
        kernel, kOtherPeer, 11, reacher, KernelDomainAction_Pickup, 0u, tent,
        KernelVec3{1.0f, 0.0f, 0.0f});
    step(kernel, 2);
    std::fprintf(
        stderr, "  tent still in world after pickup: %s\n",
        exists(kernel, tent) ? "yes" : "no");
    Kernel_Destroy(kernel);
    return outcome;
}

void print_landing(const char* label, const LandingOutcome& outcome) {
    std::fprintf(
        stderr,
        "[land]  %-30s kit=(%.2f, %.2f, %.2f) tent=(%.2f, %.2f, %.2f) "
        "ice_overlap=%.2f m^3 reach=%.2f m activate=%s(%u) pickup=%s(%u)\n",
        label,
        outcome.kit.x, outcome.kit.y, outcome.kit.z,
        outcome.tent.x, outcome.tent.y, outcome.tent.z,
        outcome.overlap,
        outcome.reach_distance,
        status_name(outcome.activate.status), outcome.activate.rejection_reason,
        status_name(outcome.pickup.status), outcome.pickup.rejection_reason);
}

// ---------------------------------------------------------------------------
// 4. Landing on units
// ---------------------------------------------------------------------------

void run_landing_on_units(
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene,
    std::uint16_t port,
    const KernelVec3& landing) {
    KernelHandle* kernel = make_world(config, scene, port);
    const std::uint32_t thrower =
        spawn_player(kernel, config, kThrowerPeer, KernelVec3{0.0f, 0.0f, 0.0f});
    // Offsets from the measured landing spot: dead centre, half-way to a face,
    // near a face, near a corner. Three enemies and one friendly player.
    const std::array<KernelVec3, 4> offsets{
        KernelVec3{0.0f, 0.0f, 0.0f},
        KernelVec3{0.0f, 0.0f, 0.6f},
        KernelVec3{1.0f, 0.0f, 0.0f},
        KernelVec3{0.9f, 0.0f, -0.9f},
    };
    std::array<std::uint32_t, 4> bodies{};
    const std::uint32_t gingerbread =
        entity_template_id_of(config, "gingerbread");
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        const KernelVec3 at{
            landing.x + offsets[index].x, 0.0f, landing.z + offsets[index].z};
        bodies[index] = index == 3
            ? spawn_player(kernel, config, kOtherPeer, at)
            : create_entity(kernel, gingerbread, at);
    }
    step(kernel, 10);
    std::array<KernelVec3, 4> before{};
    for (std::size_t index = 0; index < bodies.size(); ++index) {
        before[index] = position_of(kernel, bodies[index]);
    }
    const std::uint32_t kit = throw_tent(kernel, config, thrower, kThrowDirection);
    const Landing landed = land(kernel, kit);
    require(landed.tent != 0u);
    const KernelVec3 tent_at = landed.tent_position;
    step(kernel, 30);
    std::fprintf(
        stderr, "[units] tent landed at (%.2f, %.2f, %.2f)\n",
        tent_at.x, tent_at.y, tent_at.z);
    for (std::size_t index = 0; index < bodies.size(); ++index) {
        const KernelVec3 after = position_of(kernel, bodies[index]);
        std::fprintf(
            stderr,
            "[units] %-11s offset=(%.1f, %.1f) before=(%.2f, %.2f, %.2f) "
            "after=(%.2f, %.2f, %.2f) inside_footprint=%s\n",
            index == 3 ? "player" : "gingerbread",
            offsets[index].x, offsets[index].z,
            before[index].x, before[index].y, before[index].z,
            after.x, after.y, after.z,
            inside_tent_footprint(tent_at, after) ? "YES" : "no");
    }
    Kernel_Destroy(kernel);
}

// ---------------------------------------------------------------------------
// 5. Occupants
// ---------------------------------------------------------------------------

void run_occupants(
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene,
    std::uint16_t port,
    std::uint32_t mask,
    const char* label,
    int* out_inside) {
    KernelHandle* kernel = make_world(config, scene, port);
    const KernelVec3 tent_at{0.0f, 0.0f, 0.0f};
    create_entity(
        kernel, entity_template_id_of(config, "tent"), tent_at, kThrowerPeer);
    std::array<std::uint32_t, 4> players{};
    for (std::size_t index = 0; index < players.size(); ++index) {
        players[index] = spawn_player(
            kernel, config, 20u + static_cast<std::uint32_t>(index),
            KernelVec3{6.0f, 0.0f, 2.0f * static_cast<float>(index)});
    }
    step(kernel, 10);
    // The entry sequence: mask first, then the move, both before the next
    // tick, so no physics tick ever runs with a body inside the tent under
    // the default mask.
    for (const std::uint32_t player : players) {
        if (mask != 0u) {
            require(Kernel_ServerSetEntityMovementCollisionMask(
                kernel, player, mask));
        }
        require(Kernel_ServerSetEntityTransform(
            kernel, player, &tent_at, &kIdentityRotation));
    }
    step(kernel, 1);
    std::array<KernelVec3, 4> first{};
    for (std::size_t index = 0; index < players.size(); ++index) {
        first[index] = position_of(kernel, players[index]);
    }
    step(kernel, 60);
    int inside = 0;
    for (std::size_t index = 0; index < players.size(); ++index) {
        const KernelVec3 after = position_of(kernel, players[index]);
        const bool in = inside_tent_footprint(tent_at, after) &&
            std::fabs(after.y) < 0.5f;
        inside += in ? 1 : 0;
        std::fprintf(
            stderr,
            "[occ]   %-22s player %zu tick1=(%.2f, %.2f, %.2f) "
            "tick61=(%.2f, %.2f, %.2f) inside=%s\n",
            label, index,
            first[index].x, first[index].y, first[index].z,
            after.x, after.y, after.z,
            in ? "YES" : "no");
    }
    *out_inside = inside;
    Kernel_Destroy(kernel);
}

// ---------------------------------------------------------------------------
// 6. Enemy fire at an occupant
// ---------------------------------------------------------------------------

struct FireOutcome {
    int tent_hp_before = -1;
    int tent_hp_after = -1;
    int occupant_hp_before = -1;
    int occupant_hp_after = -1;
    std::uint32_t fire_confirmed = 0;
    std::uint32_t hits_on_tent = 0;
    std::uint32_t hits_on_occupant = 0;
};

int hp_of(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    if (net_id == 0u || !Kernel_ServerGetEntityState(kernel, net_id, &state)) {
        return -1;
    }
    return static_cast<int>(state.hp);
}

// A player stands at the origin under the terrain-only mask, inside a tent or
// in the open; `shooter` stands 8 m down +x and holds primary fire at the
// player's chest the way ActorIntentExecutor does, for 120 ticks.
FireOutcome run_fire_case(
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene,
    std::uint16_t port,
    const std::string& shooter_name,
    std::uint8_t weapon_id,
    bool with_tent) {
    KernelHandle* kernel = make_world(config, scene, port);
    const KernelVec3 origin{0.0f, 0.0f, 0.0f};
    std::uint32_t tent = 0;
    if (with_tent) {
        tent = create_entity(
            kernel, entity_template_id_of(config, "tent"), origin, kThrowerPeer);
    }
    const std::uint32_t occupant =
        spawn_player(kernel, config, kThrowerPeer, KernelVec3{6.0f, 0.0f, 6.0f});
    require(Kernel_ServerSetEntityMovementCollisionMask(
        kernel, occupant, KERNEL_MOVEMENT_LAYER_TERRAIN));
    require(Kernel_ServerSetEntityTransform(
        kernel, occupant, &origin, &kIdentityRotation));
    const std::uint32_t shooter_template =
        entity_template_id_of(config, shooter_name);
    require(shooter_template != 0u);
    const std::uint32_t shooter =
        create_entity(kernel, shooter_template, KernelVec3{8.0f, 0.0f, 0.0f});
    // What AgentRuntimeManager does when it first sees an agent; no game_server
    // runs here, so nothing else would arm it.
    require(Kernel_ServerSetEntityWeaponMechanics(
        kernel, shooter, &config.weapons.definitions[weapon_id]));
    step(kernel, 10);

    FireOutcome outcome;
    outcome.tent_hp_before = hp_of(kernel, tent);
    outcome.occupant_hp_before = hp_of(kernel, occupant);
    std::array<KernelEvent, 256> events{};
    std::uint32_t action_instance = 0;
    for (std::uint32_t tick = 0; tick < 120u; ++tick) {
        KernelVec3 launch = position_of(kernel, shooter);
        Kernel_ServerGetProjectileLaunchPosition(kernel, shooter, &launch);
        const KernelVec3 chest{0.0f, 0.9f, 0.0f};
        KernelVec3 aim{chest.x - launch.x, chest.y - launch.y, chest.z - launch.z};
        const float length = std::sqrt(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
        aim = KernelVec3{aim.x / length, aim.y / length, aim.z / length};
        KernelPlayerInput input{};
        input.input_seq = tick + 1u;
        input.selected_weapon = weapon_id;
        input.aim_dir = aim;
        // A fresh press every 30 ticks, so an action that ended (a burst, a
        // reload) does not leave the rest of the window silent.
        if (tick % 30u == 0u) {
            ++action_instance;
            input.action_intent = KernelActionIntent{
                action_instance, KernelActionBinding_PrimaryFire, 0u, 0u};
        }
        input.action_input = KernelActionInput{action_instance, 1u, 0u, 0u};
        Kernel_ServerEnqueueEntityInput(
            kernel, KernelCommandSource_AI, shooter, &input);
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            const KernelEvent& event = events[index];
            if (event.type == KernelEventType_FireConfirmed) {
                ++outcome.fire_confirmed;
            }
            if (event.type == KernelEventType_HitConfirmed) {
                outcome.hits_on_tent += tent != 0u && event.net_id == tent ? 1u : 0u;
                outcome.hits_on_occupant += event.net_id == occupant ? 1u : 0u;
            }
        }
    }
    step(kernel, 30);
    outcome.tent_hp_after = hp_of(kernel, tent);
    outcome.occupant_hp_after = hp_of(kernel, occupant);
    Kernel_Destroy(kernel);
    return outcome;
}

void print_fire(const char* label, const FireOutcome& outcome) {
    std::fprintf(
        stderr,
        "[fire]  %-32s fires=%u tent_hp %d -> %d (hits %u) "
        "occupant_hp %d -> %d (hits %u)\n",
        label,
        outcome.fire_confirmed,
        outcome.tent_hp_before, outcome.tent_hp_after, outcome.hits_on_tent,
        outcome.occupant_hp_before, outcome.occupant_hp_after,
        outcome.hits_on_occupant);
}

}  // namespace

int main() {
    const GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    require(entity_template_id_of(config, "tent") != 0u);
    require(item_template_id_of(config, "tent_kit") != 0u);
    const std::vector<std::uint8_t> scene = read_ground_scene();

    // --- 1-3. Landing, reach, pickup ---------------------------------------
    const LandingOutcome ground = run_landing_case(config, scene, 7961, -1.0f, 1.8f);
    print_landing("flat ground (control)", ground);

    // An ice block where the control landed.
    const LandingOutcome on_ice =
        run_landing_case(config, scene, 7962, ground.tent.x, 2.2f);
    print_landing("onto ice, terrain|static", on_ice);

    // The same throw with static_obstacle struck from the landing mask.
    GameServerGameplayConfig terrain_only = config;
    for (auto& candidate : terrain_only.entity_templates) {
        if (candidate.name == "tent_kit_prop") {
            candidate.collision_trigger_mask = KERNEL_COLLISION_LAYER_TERRAIN;
        }
    }
    const LandingOutcome through_ice =
        run_landing_case(terrain_only, scene, 7963, ground.tent.x, 2.2f);
    print_landing("onto ice, terrain only", through_ice);

    // --- 4. Landing on units -----------------------------------------------
    run_landing_on_units(config, scene, 7964, ground.tent);

    // --- 5. Occupants ------------------------------------------------------
    int inside_terrain_only = 0;
    run_occupants(
        config, scene, 7965, KERNEL_MOVEMENT_LAYER_TERRAIN, "mask terrain",
        &inside_terrain_only);
    int inside_terrain_actor = 0;
    run_occupants(
        config, scene, 7966,
        KERNEL_MOVEMENT_LAYER_TERRAIN | KERNEL_MOVEMENT_LAYER_ACTOR,
        "mask terrain|actor", &inside_terrain_actor);
    int inside_default = 0;
    run_occupants(
        config, scene, 7967, 0u, "default mask (control)", &inside_default);
    std::fprintf(
        stderr, "[occ]   inside after 60 ticks: terrain=%d terrain|actor=%d "
        "default=%d (of 4)\n",
        inside_terrain_only, inside_terrain_actor, inside_default);

    // --- 6. Enemy fire -----------------------------------------------------
    struct Shooter {
        const char* name;
        std::uint8_t weapon;
    };
    const std::array<Shooter, 3> shooters{
        Shooter{"sentry_grunt", 2},      // spammer: a projectile naming no side
        Shooter{"beam_sentry", 8},       // beam
        Shooter{"gingerbread_mage", 12}, // lobbed grenade, area effect
    };
    std::uint16_t port = 7970;
    std::array<FireOutcome, 3> open{};
    std::array<FireOutcome, 3> tented{};
    for (std::size_t index = 0; index < shooters.size(); ++index) {
        const Shooter& shooter = shooters[index];
        open[index] = run_fire_case(
            config, scene, port++, shooter.name, shooter.weapon, false);
        print_fire(
            (std::string(shooter.name) + " / open (control)").c_str(),
            open[index]);
        tented[index] = run_fire_case(
            config, scene, port++, shooter.name, shooter.weapon, true);
        print_fire(
            (std::string(shooter.name) + " / in tent").c_str(), tented[index]);
    }

    // Controls: the tent was thrown clear of the thrower and came to rest on
    // the ground, so every other landing number is about the tent and the
    // ground, not a throw that never left the hand.
    require(ground.tent.x > 3.0f);
    require(std::fabs(ground.tent.y) < 0.5f);

    // Reach and pickup: from the ground beside a tent on the ground, the tent
    // is interactable; it is never a world item, so it cannot be picked up.
    require(ground.activate.status == KernelGameplayRequestStatus_Committed);
    require(ground.pickup.status == KernelGameplayRequestStatus_Rejected);

    // The entry mask holds: four occupants under terrain-only stay inside and
    // on the ground. The default-mask control shows the tent does push bodies
    // out, so "inside" is the mask's doing.
    require(inside_terrain_only == 4);
    require(inside_default == 0);

    // A beam aimed at an occupant is stopped by the tent. The control shows the
    // same beam lands on a player in the open.
    require(open[1].hits_on_occupant > 0u);
    require(tented[1].hits_on_occupant == 0u);
    require(tented[1].hits_on_tent > 0u);
    // An area effect is not stopped: a grenade that lands on the tent still
    // reaches the occupant. This is why occupants need damage immunity; pinned
    // so the day it changes is noticed.
    require(open[2].hits_on_occupant > 0u);
    require(tented[2].hits_on_tent > 0u);
    require(tented[2].hits_on_occupant > 0u);
    return 0;
}
