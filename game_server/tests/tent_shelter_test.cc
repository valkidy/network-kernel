// A building's two kernel halves, against the shipped catalog and the ground
// scene the tent probe uses:
//
// K1, open_ui. Activating the tent runs its on_activated graph, which names the
//     rest UI; the kernel reports who asked, for which building and which UI
//     (KernelEventType_UiOpened). It decides nothing else.
// K2, the shelter. Kernel_ServerEnqueueEntityShelter puts an actor inside: at
//     the building's origin, rooted, immune, able only to activate that
//     building. Leaving sets it down clear of the building on the side it came
//     in from and gives it its own mask back, so the building blocks it again.
//     Entering is refused for the dead and past KERNEL_SHELTER_CAPACITY.
//
// Each case keeps a control alongside its claim -- the same shot at an occupant
// placed by hand, the same walk outside -- so "nothing happened" cannot pass
// for an answer.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
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

using network_example::game_server::GameServer;
using network_example::game_server::GameServerGameplayConfig;

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
constexpr std::uint32_t kPeer = 7;
// tent_hitbox: half 1.2 across the ground. A player's capsule is 0.35 wide.
constexpr float kTentHalf = 1.2f;
constexpr float kCapsuleRadius = 0.35f;
// action_open_rest_ui.
constexpr std::uint32_t kRestUi = 1;
// gingerbread_mage's lobbed grenade: an area effect, which the tent's walls do
// not stop (tent_feasibility_probe_test, case 6).
constexpr std::uint8_t kMageGrenade = 12;

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

// load_catalog false leaves the catalog to a GameServer, which loads its own.
KernelHandle* make_world(
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene,
    std::uint16_t port,
    bool load_catalog = true) {
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    if (load_catalog) {
        require(network_example::game_server::load_kernel_gameplay_catalog(
            kernel, config));
    }
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

// Every event the ticks raised.
std::vector<KernelEvent> step(KernelHandle* kernel, int ticks) {
    std::vector<KernelEvent> all;
    std::array<KernelEvent, 256> events{};
    for (int tick = 0; tick < ticks; ++tick) {
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        all.insert(all.end(), events.begin(), events.begin() + count);
    }
    return all;
}

std::vector<KernelEvent> of_type(
    const std::vector<KernelEvent>& events, KernelEventType type) {
    std::vector<KernelEvent> matching;
    for (const KernelEvent& event : events) {
        if (event.type == type) {
            matching.push_back(event);
        }
    }
    return matching;
}

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

KernelServerEntityState state_of(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    return state;
}

KernelVec3 position_of(KernelHandle* kernel, std::uint32_t net_id) {
    return state_of(kernel, net_id).position;
}

float horizontal_distance(const KernelVec3& a, const KernelVec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

struct Submitted {
    KernelGameplayRequestOutcome outcome{};
    std::vector<KernelEvent> events;
};

// A gameplay request, the ticks it took to answer, and what those ticks raised.
Submitted submit(
    KernelHandle* kernel,
    std::uint32_t peer,
    std::uint64_t request_id,
    std::uint32_t instigator,
    std::uint8_t domain_action,
    std::uint32_t target) {
    KernelGameplayRequest request{};
    request.struct_size = sizeof(request);
    request.requester_peer = peer;
    request.request_id = request_id;
    request.instigator_net_id = instigator;
    request.domain_action = domain_action;
    request.target_net_id = target;
    request.requested_quantity = 1;
    require(Kernel_ServerSubmitGameplayRequest(kernel, &request));
    Submitted submitted;
    KernelGameplayRequestOutcome outcomes[4]{};
    for (KernelGameplayRequestOutcome& outcome : outcomes) {
        outcome.struct_size = sizeof(outcome);
    }
    std::uint32_t count = Kernel_PollGameplayRequestOutcomes(kernel, outcomes, 4);
    for (int tick = 0; count == 0u && tick < 3; ++tick) {
        const std::vector<KernelEvent> events = step(kernel, 1);
        submitted.events.insert(submitted.events.end(), events.begin(), events.end());
        count = Kernel_PollGameplayRequestOutcomes(kernel, outcomes, 4);
    }
    require(count >= 1u);
    submitted.outcome = outcomes[0];
    // A request that commits on submit raised its events then, outside any
    // tick: they wait in the queue for the next poll.
    std::array<KernelEvent, 256> events{};
    const std::uint32_t pending = Kernel_PollEvents(
        kernel, events.data(), static_cast<std::uint32_t>(events.size()));
    submitted.events.insert(
        submitted.events.end(), events.begin(), events.begin() + pending);
    return submitted;
}

// Holds a move along `move` for `ticks`, from `first_seq` on.
std::uint32_t walk(
    KernelHandle* kernel,
    std::uint32_t net_id,
    KernelVec2 move,
    int ticks,
    std::uint32_t first_seq) {
    std::uint32_t seq = first_seq;
    for (int tick = 0; tick < ticks; ++tick) {
        KernelPlayerInput input{};
        input.input_seq = seq++;
        input.move = move;
        require(Kernel_ServerEnqueueEntityInput(
            kernel, KernelCommandSource_Test, net_id, &input));
        step(kernel, 1);
    }
    return seq;
}

std::vector<KernelEvent> enter(
    KernelHandle* kernel, std::uint32_t net_id, std::uint32_t shelter) {
    require(Kernel_ServerEnqueueEntityShelter(
        kernel, KernelCommandSource_Test, net_id, shelter));
    return step(kernel, 1);
}

// ---------------------------------------------------------------------------
// K1: activating the tent reports who asked, and nothing more.
// ---------------------------------------------------------------------------

void open_ui_reports_who_asked(
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene) {
    KernelHandle* kernel = make_world(config, scene, 7981);
    const std::uint32_t tent = create_entity(
        kernel, entity_template_id_of(config, "tent"), KernelVec3{}, kPeer);
    const std::uint32_t player =
        spawn_player(kernel, config, kPeer, KernelVec3{1.8f, 0.0f, 0.0f});
    step(kernel, 10);
    const KernelVec3 before = position_of(kernel, player);

    const Submitted activated =
        submit(kernel, kPeer, 1, player, KernelDomainAction_Activate, tent);
    require(activated.outcome.status == KernelGameplayRequestStatus_Committed);
    const std::vector<KernelEvent> opened =
        of_type(activated.events, KernelEventType_UiOpened);
    std::fprintf(
        stderr, "[k1]    activate: committed, UiOpened x%zu\n", opened.size());
    require(opened.size() == 1u);
    require(opened[0].net_id == tent);
    require(opened[0].related_net_id == player);
    require(opened[0].peer_id == kPeer);
    require(opened[0].code == kRestUi);
    // Only reported: nobody went inside.
    require(of_type(activated.events, KernelEventType_ShelterChanged).empty());
    require(horizontal_distance(position_of(kernel, player), before) < 0.05f);
    Kernel_Destroy(kernel);
}

// ---------------------------------------------------------------------------
// K2: in, held, out.
// ---------------------------------------------------------------------------

void enter_stay_and_leave(
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene) {
    KernelHandle* kernel = make_world(config, scene, 7982);
    const KernelVec3 tent_at{0.0f, 0.0f, 0.0f};
    const std::uint32_t tent = create_entity(
        kernel, entity_template_id_of(config, "tent"), tent_at, kPeer);
    // Another interactable, in reach of the tent's centre.
    const std::uint32_t terminal = create_entity(
        kernel,
        entity_template_id_of(config, "interaction_terminal"),
        KernelVec3{0.0f, 0.0f, -2.5f});
    const std::uint32_t player =
        spawn_player(kernel, config, kPeer, KernelVec3{4.0f, 0.0f, 0.0f});
    step(kernel, 10);

    // In: at the centre, announced.
    const std::vector<KernelEvent> entered =
        of_type(enter(kernel, player, tent), KernelEventType_ShelterChanged);
    require(entered.size() == 1u);
    require(entered[0].net_id == player);
    require(entered[0].code == tent);
    require(entered[0].related_net_id == 0u);
    const KernelVec3 inside = position_of(kernel, player);
    std::fprintf(
        stderr, "[k2]    entered at (%.2f, %.2f, %.2f)\n",
        inside.x, inside.y, inside.z);
    require(horizontal_distance(inside, tent_at) < 0.05f);

    // Held: a second of pushing gets nowhere, and the tent does not push it
    // out either.
    std::uint32_t seq = walk(kernel, player, KernelVec2{1.0f, 0.0f}, 30, 1u);
    const KernelVec3 held = position_of(kernel, player);
    std::fprintf(
        stderr, "[k2]    after 30 ticks of input (%.2f, %.2f, %.2f)\n",
        held.x, held.y, held.z);
    require(horizontal_distance(held, tent_at) < 0.05f);
    require(std::fabs(held.y - tent_at.y) < 0.3f);

    // From inside, only the tent itself answers.
    const Submitted elsewhere =
        submit(kernel, kPeer, 2, player, KernelDomainAction_Activate, terminal);
    require(elsewhere.outcome.status == KernelGameplayRequestStatus_Rejected);
    require(
        elsewhere.outcome.rejection_reason ==
        KernelGameplayRequestRejection_InstigatorSheltered);
    const Submitted again =
        submit(kernel, kPeer, 3, player, KernelDomainAction_Activate, tent);
    require(again.outcome.status == KernelGameplayRequestStatus_Committed);
    require(of_type(again.events, KernelEventType_UiOpened).size() == 1u);

    // Out: clear of the footprint, on the side it came in from, on the ground.
    const std::vector<KernelEvent> left =
        of_type(enter(kernel, player, 0u), KernelEventType_ShelterChanged);
    require(left.size() == 1u);
    require(left[0].net_id == player);
    require(left[0].code == 0u);
    require(left[0].related_net_id == tent);
    const KernelVec3 outside = position_of(kernel, player);
    std::fprintf(
        stderr, "[k2]    left to (%.2f, %.2f, %.2f), %.2f m from centre\n",
        outside.x, outside.y, outside.z,
        horizontal_distance(outside, tent_at));
    require(horizontal_distance(outside, tent_at) > kTentHalf + kCapsuleRadius);
    require(outside.x > kTentHalf);
    require(std::fabs(outside.y - tent_at.y) < 0.3f);

    // Its own mask is back: walking at the tent, it is stopped at the wall.
    seq = walk(kernel, player, KernelVec2{-1.0f, 0.0f}, 60, seq);
    const KernelVec3 blocked = position_of(kernel, player);
    std::fprintf(
        stderr, "[k2]    walked at the tent, stopped at (%.2f, %.2f, %.2f)\n",
        blocked.x, blocked.y, blocked.z);
    require(blocked.x >= kTentHalf);
    // Control: it did walk -- it closed on the wall rather than stand still.
    require(blocked.x < outside.x - 0.1f);

    // And activating from outside still reports, as before.
    const Submitted after =
        submit(kernel, kPeer, 4, player, KernelDomainAction_Activate, tent);
    require(after.outcome.status == KernelGameplayRequestStatus_Committed);
    Kernel_Destroy(kernel);
}

// ---------------------------------------------------------------------------
// K2: who may not go in.
// ---------------------------------------------------------------------------

void refusals(
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene) {
    KernelHandle* kernel = make_world(config, scene, 7983);
    const std::uint32_t tent_template = entity_template_id_of(config, "tent");
    const std::uint32_t tent =
        create_entity(kernel, tent_template, KernelVec3{}, kPeer);
    const std::uint32_t other_tent =
        create_entity(kernel, tent_template, KernelVec3{10.0f, 0.0f, 0.0f}, kPeer);
    std::array<std::uint32_t, KERNEL_SHELTER_CAPACITY + 1u> players{};
    for (std::size_t index = 0; index < players.size(); ++index) {
        players[index] = spawn_player(
            kernel, config, 20u + static_cast<std::uint32_t>(index),
            KernelVec3{4.0f, 0.0f, 2.0f * static_cast<float>(index)});
    }
    const std::uint32_t dead =
        spawn_player(kernel, config, 40u, KernelVec3{-4.0f, 0.0f, 0.0f});
    step(kernel, 10);

    // A squad fits; one more does not.
    for (const std::uint32_t player : players) {
        require(Kernel_ServerEnqueueEntityShelter(
            kernel, KernelCommandSource_Test, player, tent));
    }
    const std::vector<KernelEvent> entered =
        of_type(step(kernel, 1), KernelEventType_ShelterChanged);
    std::fprintf(
        stderr, "[k2]    %zu of %zu went in\n", entered.size(), players.size());
    require(entered.size() == KERNEL_SHELTER_CAPACITY);
    const std::uint32_t last = players.back();
    require(horizontal_distance(position_of(kernel, last), KernelVec3{}) > 3.0f);

    // In one building is not a way into another.
    require(of_type(enter(kernel, players[0], other_tent),
                    KernelEventType_ShelterChanged).empty());
    require(horizontal_distance(position_of(kernel, players[0]), KernelVec3{}) <
            0.05f);

    // The dead stay where they fell.
    require(Kernel_ServerSetEntityHealth(kernel, dead, 0));
    require(of_type(enter(kernel, dead, other_tent),
                    KernelEventType_ShelterChanged).empty());
    require(horizontal_distance(
                position_of(kernel, dead), KernelVec3{-4.0f, 0.0f, 0.0f}) < 0.5f);

    // Leaving when not inside is nothing to do.
    require(of_type(enter(kernel, last, 0u),
                    KernelEventType_ShelterChanged).empty());
    Kernel_Destroy(kernel);
}

// ---------------------------------------------------------------------------
// K2: an occupant takes no damage from what the walls do not stop.
// ---------------------------------------------------------------------------

struct FireOutcome {
    int occupant_hp_before = -1;
    int occupant_hp_after = -1;
    std::uint32_t hits_on_tent = 0;
    std::uint32_t hits_on_occupant = 0;
};

// The probe's case 6: a mage 8 m down +x lobs grenades at the occupant's
// chest for 120 ticks. `sheltered` puts the occupant in through the shelter;
// otherwise it is placed by hand under the same terrain-only mask, which is
// the probe's measured control (the area effect reaches it).
FireOutcome run_grenades(
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene,
    std::uint16_t port,
    bool sheltered) {
    KernelHandle* kernel = make_world(config, scene, port);
    const KernelVec3 origin{0.0f, 0.0f, 0.0f};
    const std::uint32_t tent = create_entity(
        kernel, entity_template_id_of(config, "tent"), origin, kPeer);
    const std::uint32_t occupant =
        spawn_player(kernel, config, kPeer, KernelVec3{6.0f, 0.0f, 6.0f});
    step(kernel, 5);
    if (sheltered) {
        require(of_type(enter(kernel, occupant, tent),
                        KernelEventType_ShelterChanged).size() == 1u);
    } else {
        require(Kernel_ServerSetEntityMovementCollisionMask(
            kernel, occupant, KERNEL_MOVEMENT_LAYER_TERRAIN));
        require(Kernel_ServerSetEntityTransform(
            kernel, occupant, &origin, &kIdentityRotation));
    }
    const std::uint32_t shooter = create_entity(
        kernel,
        entity_template_id_of(config, "gingerbread_mage"),
        KernelVec3{8.0f, 0.0f, 0.0f});
    require(Kernel_ServerSetEntityWeaponMechanics(
        kernel, shooter, &config.weapons.definitions[kMageGrenade]));
    step(kernel, 10);

    FireOutcome outcome;
    outcome.occupant_hp_before = static_cast<int>(state_of(kernel, occupant).hp);
    std::array<KernelEvent, 256> events{};
    std::uint32_t action_instance = 0;
    for (std::uint32_t tick = 0; tick < 120u; ++tick) {
        KernelVec3 launch = position_of(kernel, shooter);
        Kernel_ServerGetProjectileLaunchPosition(kernel, shooter, &launch);
        const KernelVec3 chest{0.0f, 0.9f, 0.0f};
        KernelVec3 aim{chest.x - launch.x, chest.y - launch.y, chest.z - launch.z};
        const float length =
            std::sqrt(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
        aim = KernelVec3{aim.x / length, aim.y / length, aim.z / length};
        KernelPlayerInput input{};
        input.input_seq = tick + 1u;
        input.selected_weapon = kMageGrenade;
        input.aim_dir = aim;
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
            if (events[index].type == KernelEventType_HitConfirmed) {
                outcome.hits_on_tent += events[index].net_id == tent ? 1u : 0u;
                outcome.hits_on_occupant +=
                    events[index].net_id == occupant ? 1u : 0u;
            }
        }
    }
    step(kernel, 30);
    outcome.occupant_hp_after = static_cast<int>(state_of(kernel, occupant).hp);
    Kernel_Destroy(kernel);
    return outcome;
}

void occupant_is_immune(
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene) {
    const FireOutcome by_hand = run_grenades(config, scene, 7984, false);
    const FireOutcome sheltered = run_grenades(config, scene, 7985, true);
    for (const auto& [label, outcome] :
         {std::pair{"placed by hand (control)", by_hand},
          std::pair{"sheltered", sheltered}}) {
        std::fprintf(
            stderr,
            "[k2]    grenades, %-24s tent hits %u, occupant hits %u, hp %d -> %d\n",
            label, outcome.hits_on_tent, outcome.hits_on_occupant,
            outcome.occupant_hp_before, outcome.occupant_hp_after);
    }
    require(by_hand.hits_on_occupant > 0u);
    require(by_hand.occupant_hp_after < by_hand.occupant_hp_before);
    require(sheltered.hits_on_tent > 0u);
    require(sheltered.hits_on_occupant == 0u);
    require(sheltered.occupant_hp_after == sheltered.occupant_hp_before);
}

// ---------------------------------------------------------------------------
// K3: a building evicted by its population cap lets its occupants out first.
// Eviction skips the on_destroy graph; it must not skip this.
// ---------------------------------------------------------------------------

void eviction_lets_occupants_out(
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene) {
    KernelHandle* kernel = make_world(config, scene, 7987);
    const std::uint32_t tent_template = entity_template_id_of(config, "tent");
    const KernelVec3 tent_at{0.0f, 0.0f, 0.0f};
    const std::uint32_t tent =
        create_entity(kernel, tent_template, tent_at, kPeer);
    const std::uint32_t player =
        spawn_player(kernel, config, kPeer, KernelVec3{4.0f, 0.0f, 0.0f});
    step(kernel, 10);
    require(of_type(enter(kernel, player, tent),
                    KernelEventType_ShelterChanged).size() == 1u);

    // Newer tents, far off, until the oldest -- the occupied one -- is evicted.
    std::vector<KernelEvent> events;
    for (int count = 1; count <= 16; ++count) {
        create_entity(
            kernel, tent_template,
            KernelVec3{20.0f + 6.0f * static_cast<float>(count), 0.0f, 20.0f},
            kPeer);
        const std::vector<KernelEvent> tick = step(kernel, 1);
        events.insert(events.end(), tick.begin(), tick.end());
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        if (!Kernel_ServerGetEntityState(kernel, tent, &state)) {
            break;
        }
    }
    std::size_t released_at = events.size();
    std::size_t evicted_at = events.size();
    for (std::size_t index = 0; index < events.size(); ++index) {
        if (events[index].type == KernelEventType_ShelterChanged &&
            events[index].net_id == player && events[index].code == 0u &&
            events[index].related_net_id == tent) {
            released_at = index;
        }
        if (events[index].type == KernelEventType_EntityDestroyed &&
            events[index].net_id == tent) {
            require(events[index].code == KernelDespawnReason_CapacityEvicted);
            evicted_at = index;
        }
    }
    require(evicted_at < events.size());
    require(released_at < evicted_at);
    const KernelVec3 released = position_of(kernel, player);
    std::fprintf(
        stderr, "[k3]    occupied tent evicted, player released at "
        "(%.2f, %.2f, %.2f)\n",
        released.x, released.y, released.z);
    require(horizontal_distance(released, tent_at) > kTentHalf + kCapsuleRadius);
    // Out for real: back to its own movement, it walks.
    walk(kernel, player, KernelVec2{1.0f, 0.0f}, 15, 1u);
    require(position_of(kernel, player).x > released.x + 0.5f);
    Kernel_Destroy(kernel);
}

// ---------------------------------------------------------------------------
// K4: a group that opts in runs on_destroy on cleanup too -- the tent's
// collapse blast throws its occupant clear however the tent goes.
// ---------------------------------------------------------------------------

enum class TentEnd { kDestroyed, kEvicted, kExpired };

const char* tent_end_name(TentEnd end) {
    switch (end) {
        case TentEnd::kDestroyed: return "destroyed";
        case TentEnd::kEvicted: return "evicted";
        case TentEnd::kExpired: return "expired";
    }
    return "?";
}

GameServerGameplayConfig with_tent_group(
    const GameServerGameplayConfig& base,
    bool cleanup_runs_on_destroy) {
    GameServerGameplayConfig config = base;
    std::uint32_t group = 0;
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == "tent") {
            group = candidate.prop.population_group_id;
        }
    }
    require(group != 0u);
    for (auto& rule : config.prop_population_rules) {
        if (rule.definition.population_group_id == group) {
            rule.definition.cleanup_runs_on_destroy =
                cleanup_runs_on_destroy ? 1u : 0u;
        }
    }
    return config;
}

struct Thrown {
    float released_distance = 0.0f;  // from the tent's centre, as let out
    float farthest = 0.0f;           // over the next second
    float highest = 0.0f;
};

// An occupant in the tent at the origin, and the tent ended by `end`. Measures
// where the occupant is let out and how far the next second carries it.
Thrown occupant_when_tent_ends(
    GameServerGameplayConfig config,
    const std::vector<std::uint8_t>& scene,
    std::uint16_t port,
    TentEnd end) {
    if (end == TentEnd::kExpired) {
        for (auto& candidate : config.entity_templates) {
            if (candidate.name == "tent") {
                candidate.prop.lifetime_ticks = 20u;
            }
        }
    }
    KernelHandle* kernel = make_world(config, scene, port);
    const std::uint32_t tent_template = entity_template_id_of(config, "tent");
    const KernelVec3 origin{0.0f, 0.0f, 0.0f};
    const std::uint32_t tent = create_entity(kernel, tent_template, origin, kPeer);
    const std::uint32_t player =
        spawn_player(kernel, config, kPeer, KernelVec3{1.8f, 0.0f, 0.0f});
    step(kernel, 2);
    require(of_type(enter(kernel, player, tent),
                    KernelEventType_ShelterChanged).size() == 1u);

    bool released = false;
    const auto watch = [&](const std::vector<KernelEvent>& events) {
        for (const KernelEvent& event : events) {
            released = released ||
                (event.type == KernelEventType_ShelterChanged &&
                 event.net_id == player && event.code == 0u);
        }
    };
    if (end == TentEnd::kDestroyed) {
        require(Kernel_ServerDestroyEntity(
            kernel, tent, KernelDespawnReason_Destroyed));
        watch(step(kernel, 1));
    } else if (end == TentEnd::kEvicted) {
        for (int count = 1; count <= 16 && !released; ++count) {
            create_entity(
                kernel, tent_template,
                KernelVec3{30.0f + 8.0f * static_cast<float>(count), 0.0f, 30.0f},
                kPeer);
            watch(step(kernel, 1));
        }
    } else {
        for (int tick = 0; tick < 40 && !released; ++tick) {
            watch(step(kernel, 1));
        }
    }
    require(released);
    Thrown thrown;
    thrown.released_distance =
        horizontal_distance(position_of(kernel, player), origin);
    for (int tick = 0; tick < 30; ++tick) {
        step(kernel, 1);
        const KernelVec3 at = position_of(kernel, player);
        thrown.farthest = std::max(thrown.farthest, horizontal_distance(at, origin));
        thrown.highest = std::max(thrown.highest, at.y - origin.y);
    }
    Kernel_Destroy(kernel);
    return thrown;
}

void collapse_throws_occupants_clear(
    const GameServerGameplayConfig& base,
    const std::vector<std::uint8_t>& scene) {
    // The shipped catalog opts the tent group in.
    const GameServerGameplayConfig opted_in = base;
    const GameServerGameplayConfig opted_out = with_tent_group(base, false);
    std::uint16_t port = 7988;
    for (const TentEnd end :
         {TentEnd::kDestroyed, TentEnd::kEvicted, TentEnd::kExpired}) {
        const Thrown thrown = occupant_when_tent_ends(opted_in, scene, port++, end);
        std::fprintf(
            stderr,
            "[k4]    %-9s let out at %.2f m, then farthest %.2f m, highest %.2f m\n",
            tent_end_name(end), thrown.released_distance, thrown.farthest,
            thrown.highest);
        require(thrown.released_distance > kTentHalf + kCapsuleRadius);
        require(thrown.farthest > thrown.released_distance + 1.0f);
        require(thrown.highest > 0.3f);
    }
    // The control: with the group not opted in, cleanup skips the graph as it
    // always has -- let out, and left standing there.
    for (const TentEnd end : {TentEnd::kEvicted, TentEnd::kExpired}) {
        const Thrown thrown =
            occupant_when_tent_ends(opted_out, scene, port++, end);
        std::fprintf(
            stderr,
            "[k4]    %-9s (control, not opted in) let out at %.2f m, then "
            "farthest %.2f m, highest %.2f m\n",
            tent_end_name(end), thrown.released_distance, thrown.farthest,
            thrown.highest);
        require(thrown.farthest < thrown.released_distance + 0.2f);
        require(thrown.highest < 0.2f);
    }

    // A group that opts in may not spawn into any group from on_destroy: an
    // eviction spawning the next tent could evict the next without end. The
    // same graph is fine for a group that does not opt in.
    const auto spawning_a_tent = [&](bool opt_in) {
        GameServerGameplayConfig config = with_tent_group(base, opt_in);
        for (auto& candidate : config.entity_templates) {
            if (candidate.name == "tent") {
                candidate.destroy_entity_trigger.action_graph_ref =
                    "action_spawn_entity_at_destroy_entity";
                candidate.destroy_entity_trigger.parameters = {
                    {"template", "tent"},
                    {"position", "event.position"},
                    {"owner", "event.instigator"},
                };
            }
        }
        return config;
    };
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 64;
    const auto loads = [&](const GameServerGameplayConfig& config) {
        KernelHandle* kernel = Kernel_Create(&kernel_config);
        require(kernel != nullptr);
        bool loaded = false;
        try {
            loaded = network_example::game_server::load_kernel_gameplay_catalog(
                kernel, config);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[k4]    refused: %s\n", error.what());
        }
        Kernel_Destroy(kernel);
        return loaded;
    };
    require(!loads(spawning_a_tent(true)));
    require(loads(spawning_a_tent(false)));
}

// ---------------------------------------------------------------------------
// game_server: activating a building is the door, both ways.
// ---------------------------------------------------------------------------

void game_server_runs_the_door(
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene) {
    KernelHandle* kernel = make_world(config, scene, 7986, false);
    // Events only: its tick would start the directors, and nothing here needs
    // them -- the shelter flow runs entirely off events.
    GameServer server(kernel, config);
    const auto pump = [&](const std::vector<KernelEvent>& events) {
        for (const KernelEvent& event : events) {
            server.handle_event(event);
        }
    };
    const auto frame = [&]() { pump(step(kernel, 1)); };
    const auto activate = [&](std::uint64_t request_id,
                              std::uint32_t player,
                              std::uint32_t building) {
        const Submitted submitted = submit(
            kernel, kPeer, request_id, player, KernelDomainAction_Activate,
            building);
        pump(submitted.events);
        return submitted.outcome;
    };

    const KernelVec3 tent_at{0.0f, 0.0f, 0.0f};
    const std::uint32_t tent = create_entity(
        kernel, entity_template_id_of(config, "tent"), tent_at, kPeer);
    const KernelVec3 start{1.8f, 0.0f, 0.0f};
    const std::uint32_t player = spawn_player(kernel, config, kPeer, start);
    pump(step(kernel, 10));

    // In: the activation asks, the next tick carries it out.
    require(activate(1, player, tent).status ==
            KernelGameplayRequestStatus_Committed);
    require(server.shelter_director().shelter_of(player) == 0u);
    frame();
    require(server.shelter_director().shelter_of(player) == tent);
    require(horizontal_distance(position_of(kernel, player), tent_at) < 0.05f);

    // Out: the same activation, from inside.
    require(activate(2, player, tent).status ==
            KernelGameplayRequestStatus_Committed);
    frame();
    require(server.shelter_director().shelter_of(player) == 0u);
    const KernelVec3 outside = position_of(kernel, player);
    std::fprintf(
        stderr, "[gs]    in and out by activation, out at (%.2f, %.2f, %.2f)\n",
        outside.x, outside.y, outside.z);
    require(horizontal_distance(outside, tent_at) > kTentHalf + kCapsuleRadius);

    // Back in from where leaving set it down: that spot is within reach.
    require(activate(3, player, tent).status ==
            KernelGameplayRequestStatus_Committed);
    frame();
    require(server.shelter_director().occupants_of(tent) ==
            std::vector<std::uint32_t>{player});

    // The tent goes with the player inside. The kernel lets it out as part of
    // the destroy -- before the tent's own EntityDestroyed -- around the tent,
    // on the side it came in from.
    require(Kernel_ServerDestroyEntity(
        kernel, tent, KernelDespawnReason_Destroyed));
    const std::vector<KernelEvent> destroyed = step(kernel, 1);
    pump(destroyed);
    std::size_t released_at = destroyed.size();
    std::size_t gone_at = destroyed.size();
    for (std::size_t index = 0; index < destroyed.size(); ++index) {
        if (destroyed[index].type == KernelEventType_ShelterChanged &&
            destroyed[index].net_id == player && destroyed[index].code == 0u) {
            released_at = index;
        }
        if (destroyed[index].type == KernelEventType_EntityDestroyed &&
            destroyed[index].net_id == tent) {
            gone_at = index;
        }
    }
    require(released_at < gone_at && gone_at < destroyed.size());
    require(server.shelter_director().shelter_of(player) == 0u);
    const KernelVec3 released = position_of(kernel, player);
    std::fprintf(
        stderr, "[gs]    tent destroyed with the player inside, released at "
        "(%.2f, %.2f, %.2f)\n",
        released.x, released.y, released.z);
    require(horizontal_distance(released, tent_at) > kTentHalf + kCapsuleRadius);
    require(released.x > kTentHalf);
    // The tent's collapse blast then throws it clear; once it has landed it is
    // free again, and a second tent can be walked into.
    pump(step(kernel, 60));
    const KernelVec3 landed = position_of(kernel, player);
    require(horizontal_distance(landed, tent_at) >
            horizontal_distance(released, tent_at) + 1.0f);
    const std::uint32_t second = create_entity(
        kernel, entity_template_id_of(config, "tent"),
        KernelVec3{landed.x + 1.8f, 0.0f, landed.z}, kPeer);
    pump(step(kernel, 2));
    require(activate(4, player, second).status ==
            KernelGameplayRequestStatus_Committed);
    frame();
    require(server.shelter_director().shelter_of(player) == second);
    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    const GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    require(entity_template_id_of(config, "tent") != 0u);
    const std::vector<std::uint8_t> scene = read_ground_scene();

    open_ui_reports_who_asked(config, scene);
    enter_stay_and_leave(config, scene);
    refusals(config, scene);
    occupant_is_immune(config, scene);
    eviction_lets_occupants_out(config, scene);
    collapse_throws_occupants_clear(config, scene);
    game_server_runs_the_door(config, scene);
    return 0;
}
