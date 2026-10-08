// Weapons as items (design D10-D13, P3, 2026-10-07), on the shipped catalog.
//
// A player's weapon container is its loadout: one slot per numeric category,
// weapon items only. Creating it makes the player unarmed until something is
// put in; a weapon picked up goes to its category's slot, and one already
// there goes to the picker's feet; Place drops one. A weapon's magazine and
// reserve live on its item whenever it is out of hand, so a weapon swapped out
// and picked back up has the rounds it left with. Only players pick weapons up.
//
// The control for every "did not fire" below is the same rifle firing earlier.

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

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTick = 1.0f / 30.0f;
constexpr std::uint32_t kPeer = 7;
constexpr std::uint8_t kRifle = 0;
constexpr std::uint8_t kShotgun = 1;
constexpr std::uint8_t kBeamRifle = 5;

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

std::uint32_t item_id(const gs::GameServerGameplayConfig& config, const std::string& name) {
    for (const gs::ItemTemplateConfig& item : config.item_templates) {
        if (item.name == name) return item.definition.item_template_id;
    }
    require(false);
    return 0;
}

std::uint32_t portable(const KernelItemInstanceView& view, std::uint32_t field_id) {
    for (std::uint32_t index = 0; index < view.portable_state_field_count; ++index) {
        if (view.portable_state_fields[index].field_id == field_id) {
            return view.portable_state_fields[index].uint32_default;
        }
    }
    require(false);
    return 0;
}

struct Arena {
    KernelHandle* kernel = nullptr;
    std::uint32_t player = 0;
    std::uint32_t grunt = 0;
    std::uint64_t next_request = 1;
    std::uint32_t next_seq = 1;
    std::uint32_t next_action = 1;

    ~Arena() {
        if (kernel != nullptr) Kernel_Destroy(kernel);
    }

    void tick(int count = 1) {
        for (int index = 0; index < count; ++index) Kernel_Update(kernel, kTick);
    }

    KernelServerEntityState state(std::uint32_t net_id) {
        KernelServerEntityState out{};
        out.struct_size = sizeof(out);
        require(Kernel_ServerGetEntityState(kernel, net_id, &out));
        return out;
    }

    KernelItemInstanceView item(KernelItemInstanceId id) {
        KernelItemInstanceView view{};
        view.struct_size = sizeof(view);
        require(Kernel_GetItemInstance(kernel, id, &view));
        return view;
    }

    KernelGameplayRequestOutcome request(
        std::uint32_t instigator,
        std::uint32_t peer,
        std::uint8_t action,
        KernelItemInstanceId item_id,
        std::uint32_t target,
        KernelVec3 placement = {}) {
        KernelGameplayRequest request{};
        request.struct_size = sizeof(request);
        request.requester_peer = peer;
        request.request_id = next_request++;
        request.instigator_net_id = instigator;
        request.domain_action = action;
        request.selected_item_instance_id = item_id;
        request.target_net_id = target;
        request.requested_quantity = 1;
        request.placement_position = placement;
        require(Kernel_ServerSubmitGameplayRequest(kernel, &request));
        KernelGameplayRequestOutcome outcome{};
        outcome.struct_size = sizeof(outcome);
        require(Kernel_PollGameplayRequestOutcomes(kernel, &outcome, 1) == 1u);
        if (outcome.status != KernelGameplayRequestStatus_Committed) {
            std::fprintf(stderr, "request action=%u rejected reason=%u\n",
                         action, outcome.rejection_reason);
        }
        return outcome;
    }

    // A weapon item lying 1 m in front of the player.
    std::pair<KernelItemInstanceId, std::uint32_t> drop_in_world(std::uint32_t item_template) {
        const KernelServerEntityState me = state(player);
        const KernelVec3 at{me.position.x + 1.0f, me.position.y, me.position.z};
        KernelItemInstanceId item_id = 0;
        std::uint32_t prop = 0;
        require(Kernel_ServerCreateWorldItem(kernel, item_template, 1, &at, &item_id, &prop));
        tick();
        return {item_id, prop};
    }

    void fire(std::uint8_t weapon, std::uint16_t binding, int hold_ticks) {
        const KernelServerEntityState me = state(player);
        const KernelServerEntityState target = state(grunt);
        const std::uint32_t action = 9000u + next_action++;
        for (int tick_index = 0; tick_index <= hold_ticks; ++tick_index) {
            KernelPlayerInput input{};
            input.input_seq = next_seq++;
            input.aim_dir = KernelVec3{
                target.position.x - me.position.x, 0.0f,
                target.position.z - me.position.z};
            input.selected_weapon = weapon;
            input.action_intent = KernelActionIntent{action, binding, 0u, 0u};
            input.action_input = KernelActionInput{
                action, static_cast<std::uint8_t>(tick_index < hold_ticks ? 1u : 0u), 0u, 0u};
            require(Kernel_ServerSubmitEntityInput(kernel, player, &input));
            tick();
        }
        tick(10);
    }
};

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    // The id the kernel uses for the two fields is the loader's name hash.
    require(gs::item_portable_state_field_id("weapon_ammo") == KERNEL_PORTABLE_FIELD_WEAPON_AMMO);
    require(gs::item_portable_state_field_id("weapon_reserve") ==
            KERNEL_PORTABLE_FIELD_WEAPON_RESERVE);
    const std::uint32_t rifle_item = item_id(config, "stateful_weapon_rifle");
    const std::uint32_t shotgun_item = item_id(config, "stateful_weapon_shotgun");
    const std::uint32_t beam_item = item_id(config, "stateful_weapon_beam_rifle");
    const std::uint32_t potion_item = item_id(config, "fungible_potion");
    const gs::ActorTemplateConfig* grunt_template = nullptr;
    for (const auto& candidate : config.actor_templates) {
        if (candidate.name == "chaser_grunt") grunt_template = &candidate;
    }
    require(grunt_template != nullptr);
    require(config.weapons.categories[kRifle] == 0u);
    require(config.weapons.categories[kShotgun] == 0u);
    require(config.weapons.categories[kBeamRifle] == 2u);

    const std::vector<std::uint8_t> scene = read_ground_scene();
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 2048;
    kernel_config.max_render_states = 128;
    Arena arena;
    arena.kernel = Kernel_Create(&kernel_config);
    require(arena.kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(arena.kernel, config));
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(arena.kernel, &scene_config));
    require(Kernel_StartDedicatedServer(arena.kernel, 8061));

    const auto spawn = [&](std::uint32_t template_id, std::uint16_t actor_type,
                           std::uint32_t peer, KernelVec3 position) {
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_type = gs::kEntityTypeActor;
        create.actor_type = actor_type;
        create.entity_template_id = template_id;
        create.actor_template_id = template_id;
        create.owner_peer = peer;
        create.position = position;
        create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t net_id = 0;
        require(Kernel_ServerCreateEntity(arena.kernel, &create, &net_id));
        require(Kernel_ServerSetEntityActorTemplate(arena.kernel, net_id, template_id));
        return net_id;
    };
    arena.player = spawn(config.player.actor_template_id, gs::kActorTypePlayer, kPeer,
                         KernelVec3{0.0f, 1.0f, 0.0f});
    arena.grunt = spawn(grunt_template->actor_template_id, gs::kActorTypeAgent, 0u,
                        KernelVec3{8.0f, 1.0f, 0.0f});
    // What game_server does at spawn: a combat state, then every pickable
    // weapon's mechanics.
    KernelCombatStateDefinition combat = gs::make_player_combat_state(config);
    require(Kernel_ServerSetEntityCombatState(arena.kernel, arena.player, &combat));
    for (std::size_t id = 0; id < config.weapons.definitions.size(); ++id) {
        if (!config.weapons.configured[id] || !config.weapons.has_category[id]) continue;
        KernelWeaponMechanicsDefinition mechanics = config.weapons.definitions[id];
        require(Kernel_ServerSetEntityWeaponMechanics(arena.kernel, arena.player, &mechanics));
    }
    KernelInventoryContainerId items = 0;
    require(Kernel_ServerCreateInventoryContainer(arena.kernel, arena.player, 8, &items));
    arena.tick(30);
    require(arena.state(arena.player).weapon_slot_count == 4u);

    // The weapon container takes over the loadout: empty is unarmed.
    KernelInventoryContainerId weapons = 0;
    require(Kernel_ServerCreateWeaponContainer(arena.kernel, arena.player, &weapons));
    require(!Kernel_ServerCreateWeaponContainer(arena.kernel, arena.player, &weapons));
    require(arena.state(arena.player).weapon_slot_count == 0u);

    // Weapons and items never mix.
    KernelItemInstanceId refused = 0;
    require(!Kernel_ServerCreateInventoryItem(arena.kernel, potion_item, 1, weapons, &refused));
    require(!Kernel_ServerCreateInventoryItem(arena.kernel, rifle_item, 1, items, &refused));

    // Both containers are the owner's, and say which is which.
    KernelInventoryContainerView owned[2]{};
    for (auto& view : owned) view.struct_size = sizeof(view);
    require(Kernel_CopyOwnedInventoryContainers(arena.kernel, arena.player, owned, 2) == 2u);
    int weapon_kinds = 0;
    for (const auto& view : owned) {
        if (view.container_kind == KernelInventoryContainerKind_Weapons) {
            ++weapon_kinds;
            require(view.inventory_container_id == weapons);
            require(view.slot_capacity == KERNEL_WEAPON_CATEGORY_COUNT);
        }
    }
    require(weapon_kinds == 1);

    // A rifle in hand: a fresh magazine, and it fires.
    KernelItemInstanceId rifle = 0;
    require(Kernel_ServerCreateInventoryItem(arena.kernel, rifle_item, 1, weapons, &rifle));
    require(arena.item(rifle).slot == 0u);
    KernelServerEntityState me = arena.state(arena.player);
    require(me.weapon_slot_count == 1u);
    require(me.weapon_ids[0] == kRifle);
    const std::uint16_t full = config.weapons.definitions[kRifle].magazine_size;
    require(me.ammo[0] == full);
    require(me.reserve_magazines[0] == config.weapons.definitions[kRifle].reserve_magazines);
    const std::uint16_t grunt_full = arena.state(arena.grunt).hp;
    arena.fire(kRifle, KernelActionBinding_PrimaryFire, 9);
    me = arena.state(arena.player);
    const std::uint16_t rifle_left = me.ammo[0];
    require(rifle_left < full);
    require(arena.state(arena.grunt).hp < grunt_full);
    // In hand, the magazine is the snapshot's to report, not the item's.
    require(portable(arena.item(rifle), KERNEL_PORTABLE_FIELD_WEAPON_AMMO) == full);

    // A shotgun is the same category: it takes slot 0, the rifle goes to the
    // picker's feet with the rounds it had.
    auto [shotgun, shotgun_prop] = arena.drop_in_world(shotgun_item);
    KernelGameplayRequestOutcome outcome = arena.request(
        arena.player, kPeer, KernelDomainAction_Pickup, shotgun, shotgun_prop);
    require(outcome.status == KernelGameplayRequestStatus_Committed);
    me = arena.state(arena.player);
    require(me.weapon_slot_count == 1u);
    require(me.weapon_ids[0] == kShotgun);
    KernelItemInstanceView rifle_view = arena.item(rifle);
    require(rifle_view.residency == KernelItemResidency_World);
    require(rifle_view.prop_entity_id != 0u);
    require(portable(rifle_view, KERNEL_PORTABLE_FIELD_WEAPON_AMMO) == rifle_left);
    const std::uint32_t rifle_prop = rifle_view.prop_entity_id;
    const KernelServerEntityState rifle_prop_state = arena.state(rifle_prop);
    require(std::fabs(rifle_prop_state.position.x - me.position.x) < 0.5f);

    // The rifle goes on firing from where it stopped after being picked back
    // up, and the shotgun goes down in turn.
    arena.tick(3);
    outcome = arena.request(
        arena.player, kPeer, KernelDomainAction_Pickup, rifle, rifle_prop);
    require(outcome.status == KernelGameplayRequestStatus_Committed);
    me = arena.state(arena.player);
    require(me.weapon_ids[0] == kRifle);
    require(me.ammo[0] == rifle_left);
    require(arena.item(shotgun).residency == KernelItemResidency_World);

    // A different category packs in category order.
    auto [beam, beam_prop] = arena.drop_in_world(beam_item);
    require(arena.request(arena.player, kPeer, KernelDomainAction_Pickup, beam, beam_prop)
                .status == KernelGameplayRequestStatus_Committed);
    me = arena.state(arena.player);
    require(me.weapon_slot_count == 2u);
    require(me.weapon_ids[0] == kRifle);
    require(me.weapon_ids[1] == kBeamRifle);
    require(arena.item(beam).slot == 2u);

    // A reload spends a reserve magazine, and the item hears of it.
    arena.fire(kRifle, KernelActionBinding_Reload, 1);
    arena.tick(40);
    me = arena.state(arena.player);
    require(me.ammo[0] == full);
    require(me.reserve_magazines[0] ==
            config.weapons.definitions[kRifle].reserve_magazines - 1u);
    require(portable(arena.item(rifle), KERNEL_PORTABLE_FIELD_WEAPON_RESERVE) ==
            me.reserve_magazines[0]);

    // Place drops a weapon: out of the loadout, onto the ground, state kept.
    // Behind the player, clear of the shotgun lying at its feet, and just
    // clear of the ground: a placement may not overlap anything.
    const KernelVec3 drop_at{me.position.x - 2.5f, me.position.y + 0.1f, me.position.z};
    outcome = arena.request(
        arena.player, kPeer, KernelDomainAction_Place, rifle, 0u, drop_at);
    require(outcome.status == KernelGameplayRequestStatus_Committed);
    me = arena.state(arena.player);
    require(me.weapon_slot_count == 1u);
    require(me.weapon_ids[0] == kBeamRifle);
    rifle_view = arena.item(rifle);
    require(rifle_view.residency == KernelItemResidency_World);
    require(portable(rifle_view, KERNEL_PORTABLE_FIELD_WEAPON_RESERVE) ==
            config.weapons.definitions[kRifle].reserve_magazines - 1u);
    // The rifle is not in the loadout, so it does not fire.
    const std::uint16_t grunt_before = arena.state(arena.grunt).hp;
    arena.fire(kRifle, KernelActionBinding_PrimaryFire, 9);
    require(arena.state(arena.grunt).hp == grunt_before);

    // Only a player picks a weapon up -- refused for that reason, standing
    // right beside it, not for being out of reach.
    {
        const KernelServerEntityState rifle_lying = arena.state(rifle_view.prop_entity_id);
        const KernelVec3 beside{rifle_lying.position.x, rifle_lying.position.y + 0.5f,
                                rifle_lying.position.z + 1.0f};
        const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
        require(Kernel_ServerSetEntityTransform(arena.kernel, arena.grunt, &beside, &facing));
    }
    arena.tick(3);
    outcome = arena.request(
        arena.grunt, 0u, KernelDomainAction_Pickup, rifle, rifle_view.prop_entity_id);
    require(outcome.status == KernelGameplayRequestStatus_Rejected);
    require(outcome.rejection_reason == KernelGameplayRequestRejection_NotAuthorized);
    require(arena.item(rifle).residency == KernelItemResidency_World);

    // Clearing the weapon container leaves the player unarmed.
    require(Kernel_ServerClearInventoryContainer(arena.kernel, weapons));
    require(arena.state(arena.player).weapon_slot_count == 0u);

    std::puts("weapon_item_test passed");
    return 0;
}
