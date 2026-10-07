// An item request that takes the hands -- Throw, Consume, Place, Carry --
// ends a weapon action under way (the user's call, 2026-10-08): a throw
// mid-charge cancels the charge with nothing spent, and a throw mid-beam
// stops the beam. Picking up does not take the hands, and is the control:
// the same moment with a pickup leaves the charge to cast and the beam to
// fire. The client keeps holding after the request, as one that ignored the
// interrupt would, so the stop is the server's.
//
// A dedicated server with the shipped catalog: a player with the meteor staff
// (charge) and the beam rifle (hold), and potions to throw and pick up.

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

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr std::uint8_t kMeteorStaff = 13;  // charges 20 ticks
constexpr std::uint8_t kBeamRifle = 5;     // hold: 1 MP a tick after 5 ticks
constexpr int kChargeTicks = 20;

std::filesystem::path catalog_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace / "game_server" /
        "gameplay_catalog";
}

std::vector<std::uint8_t> read_ground_scene() {
    const std::filesystem::path path =
        catalog_root() / "mesh_assets" / "jolt" / "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

KernelServerEntityState entity_state(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    return state;
}


enum class Hands { kThrow, kPickup };
enum class Weapon { kCharge, kBeam };

struct Outcome {
    std::uint16_t ammo_before = 0;  // when the item request went in
    std::uint16_t ammo_after = 0;   // after holding on, and letting go
    KernelGameplayRequestStatus status = KernelGameplayRequestStatus_NoAction;
};

// Starts `weapon` and holds it 10 ticks, makes the `hands` request, then
// keeps holding (as a client that ignores the interrupt would) and lets go.
Outcome run(
    const gs::GameServerGameplayConfig& config, Weapon weapon, Hands hands,
    std::uint16_t port) {
    const std::vector<std::uint8_t> scene = read_ground_scene();
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 1024;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(kernel, config));
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartDedicatedServer(kernel, port));

    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = gs::kActorTypePlayer;
    create.entity_template_id = config.player.actor_template_id;
    create.actor_template_id = config.player.actor_template_id;
    create.position = KernelVec3{0.0f, 1.0f, 0.0f};
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t player = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &player));
    require(Kernel_ServerSetEntityActorTemplate(kernel, player, config.player.actor_template_id));
    KernelCombatStateDefinition combat = gs::make_player_combat_state(config);
    for (std::size_t slot = 0; slot < KERNEL_MAX_WEAPON_SLOTS; ++slot) {
        combat.weapon_ids[slot] = 0;
        combat.ammo[slot] = 0;
        combat.reserve_magazines[slot] = 0;
    }
    combat.weapon_slot_count = 2;
    combat.active_weapon_slot = 0;
    combat.weapon_ids[0] = kMeteorStaff;
    combat.ammo[0] = 3;
    combat.reserve_magazines[0] = 6;
    combat.weapon_ids[1] = kBeamRifle;
    combat.ammo[1] = 12;
    combat.reserve_magazines[1] = 6;
    require(Kernel_ServerSetEntityCombatState(kernel, player, &combat));
    for (const std::uint8_t id : {kMeteorStaff, kBeamRifle}) {
        KernelWeaponMechanicsDefinition mechanics = config.weapons.definitions[id];
        require(Kernel_ServerSetEntityWeaponMechanics(kernel, player, &mechanics));
    }
    std::uint32_t potion_template = 0;
    for (const gs::ItemTemplateConfig& item : config.item_templates) {
        if (item.name == "fungible_potion") potion_template = item.definition.item_template_id;
    }
    KernelInventoryContainerId bag = 0;
    require(Kernel_ServerCreateInventoryContainer(kernel, player, 4u, &bag));
    KernelItemInstanceId potions = 0;
    require(Kernel_ServerCreateInventoryItem(kernel, potion_template, 3u, bag, &potions));
    // For the pickup: one more potion on the ground beside the player.
    KernelItemInstanceId loose = 0;
    require(Kernel_ServerCreateInventoryItem(kernel, potion_template, 1u, bag, &loose));
    std::uint32_t loose_prop = 0;
    {
        const KernelVec3 beside{1.0f, 0.2f, 0.0f};
        require(Kernel_ServerDropInventoryItem(kernel, loose, &beside, &loose_prop));
    }
    for (int index = 0; index < 20; ++index) Kernel_Update(kernel, kTickSeconds);

    const std::uint8_t weapon_id = weapon == Weapon::kCharge ? kMeteorStaff : kBeamRifle;
    const std::size_t slot = weapon == Weapon::kCharge ? 0u : 1u;
    std::uint32_t seq = 1;
    const std::uint32_t action = 7001u;
    const auto send = [&](bool start, bool held) {
        KernelPlayerInput input{};
        input.input_seq = seq++;
        // Down at the ground ahead: the staff's strike needs somewhere to land.
        input.aim_dir = KernelVec3{1.0f, -0.4f, 0.0f};
        input.selected_weapon = weapon_id;
        if (start) {
            input.action_intent =
                KernelActionIntent{action, KernelActionBinding_PrimaryFire, 0u, 0u};
        }
        input.action_input =
            KernelActionInput{action, static_cast<std::uint8_t>(held ? 1u : 0u), 0u, 0u};
        require(Kernel_ServerSubmitEntityInput(kernel, player, &input));
        Kernel_Update(kernel, kTickSeconds);
    };
    const auto ammo = [&]() { return entity_state(kernel, player).ammo[slot]; };

    for (int index = 0; index < 10; ++index) send(index == 0, true);
    Outcome outcome;
    outcome.ammo_before = ammo();
    KernelGameplayRequest request{};
    request.struct_size = sizeof(request);
    request.requester_peer = 0;
    request.request_id = 1;
    request.instigator_net_id = player;
    request.requested_quantity = 1;
    if (hands == Hands::kThrow) {
        request.domain_action = KernelDomainAction_Throw;
        request.selected_item_instance_id = potions;
        request.throw_direction = KernelVec3{0.0f, -0.3f, 1.0f};
    } else {
        request.domain_action = KernelDomainAction_Pickup;
        request.selected_item_instance_id = loose;
        request.target_net_id = loose_prop;
    }
    require(Kernel_SubmitGameplayRequest(kernel, &request));
    KernelGameplayRequestOutcome result{};
    result.struct_size = sizeof(result);
    require(Kernel_PollGameplayRequestOutcomes(kernel, &result, 1) == 1u);
    outcome.status = static_cast<KernelGameplayRequestStatus>(result.status);
    for (int index = 0; index < kChargeTicks; ++index) send(false, true);
    send(false, false);
    for (int index = 0; index < 5; ++index) Kernel_Update(kernel, kTickSeconds);
    outcome.ammo_after = ammo();
    Kernel_Destroy(kernel);
    return outcome;
}

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_catalog_file(
        (catalog_root() / "gameplay_catalog.yaml").string());

    // Mid-charge. A pickup leaves the charge alone: it casts on release.
    const Outcome charge_pickup = run(config, Weapon::kCharge, Hands::kPickup, 8068);
    std::printf("charge + pickup: %u -> %u\n", charge_pickup.ammo_before, charge_pickup.ammo_after);
    require(charge_pickup.status == KernelGameplayRequestStatus_Committed);
    require(charge_pickup.ammo_before == 3u);
    require(charge_pickup.ammo_after == 2u);
    // A throw goes, and ends the charge: no cast, nothing spent.
    const Outcome charge_throw = run(config, Weapon::kCharge, Hands::kThrow, 8069);
    std::printf("charge + throw: %u -> %u\n", charge_throw.ammo_before, charge_throw.ammo_after);
    require(charge_throw.status == KernelGameplayRequestStatus_Committed);
    require(charge_throw.ammo_before == 3u);
    require(charge_throw.ammo_after == 3u);

    // Mid-beam. A pickup leaves the beam firing.
    const Outcome beam_pickup = run(config, Weapon::kBeam, Hands::kPickup, 8070);
    std::printf("beam + pickup: %u -> %u\n", beam_pickup.ammo_before, beam_pickup.ammo_after);
    require(beam_pickup.status == KernelGameplayRequestStatus_Committed);
    require(beam_pickup.ammo_before < 12u);  // it was firing
    require(beam_pickup.ammo_after < beam_pickup.ammo_before);
    // A throw goes, and the beam stops where it was.
    const Outcome beam_throw = run(config, Weapon::kBeam, Hands::kThrow, 8071);
    std::printf("beam + throw: %u -> %u\n", beam_throw.ammo_before, beam_throw.ammo_after);
    require(beam_throw.status == KernelGameplayRequestStatus_Committed);
    require(beam_throw.ammo_before < 12u);
    require(beam_throw.ammo_after == beam_throw.ammo_before);

    std::puts("hands_interrupt_test passed");
    return 0;
}
