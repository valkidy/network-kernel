// fungible_mp_potion refills the user's active weapon's reserve magazines
// (design D15/D16, 2026-10-07).
//
// The shipped potion refills 50% of the weapon template's reserve_magazines,
// rounded half up and at least one, never past the template's own count. A
// use that would refill nothing -- the reserve already full, or no weapon in
// hand -- is refused before the potion is spent.
//
// The rounding table is checked against the shared helper the kernel uses for
// both the admission check and the refill itself.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "world/public/components.h"

namespace {

namespace gs = network_example::game_server;
namespace ne = network_example;

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr std::uint32_t kPeer = 7;
constexpr std::uint8_t kMeteorStaff = 13;       // reserve_magazines 6
constexpr std::uint8_t kMeteorStormStaff = 14;  // reserve_magazines 4

std::uint16_t refill(std::uint32_t max, std::uint16_t current,
                     std::uint16_t count, std::uint16_t percent) {
    ne::WeaponState weapon{};
    weapon.weapon_slot_count = 1;
    weapon.weapon_ids[0] = 9;
    weapon.reserve_magazines[0] = current;
    ne::WeaponTuning tuning{};
    tuning.configured[9] = true;
    tuning.definitions[9].reserve_magazines = static_cast<std::uint16_t>(max);
    return ne::weapon_reserve_refill_amount(weapon, tuning, count, percent);
}

void rounding_table() {
    // percent: half up, at least one.
    require(refill(1, 0, 0, 50) == 1u);
    require(refill(1, 0, 0, 30) == 1u);
    require(refill(3, 0, 0, 50) == 2u);
    require(refill(3, 0, 0, 30) == 1u);
    require(refill(6, 0, 0, 50) == 3u);
    require(refill(6, 0, 0, 30) == 2u);
    // Capped at what the template holds.
    require(refill(6, 5, 0, 50) == 1u);
    require(refill(6, 2, 4, 0) == 4u);
    require(refill(6, 4, 4, 0) == 2u);
    // Full: nothing.
    require(refill(6, 6, 0, 50) == 0u);
    require(refill(6, 6, 1, 0) == 0u);
    // No weapon: nothing.
    ne::WeaponState unarmed{};
    ne::WeaponTuning tuning{};
    require(ne::weapon_reserve_refill_amount(unarmed, tuning, 1, 0) == 0u);
}

KernelServerEntityState entity_state(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    return state;
}

std::uint32_t item_template_id_of(
    const gs::GameServerGameplayConfig& config,
    const std::string& name) {
    for (const gs::ItemTemplateConfig& candidate : config.item_templates) {
        if (candidate.name == name) return candidate.definition.item_template_id;
    }
    return 0;
}

struct Arena {
    KernelHandle* kernel = nullptr;
    std::uint32_t player = 0;
    KernelItemInstanceId potions = 0;
    std::uint64_t next_request = 1;
    KernelCombatStateDefinition combat{};

    ~Arena() {
        if (kernel != nullptr) Kernel_Destroy(kernel);
    }

    void set_loadout(std::uint8_t slot_count, std::uint8_t active,
                     std::uint16_t reserve0, std::uint16_t reserve1) {
        combat.weapon_slot_count = slot_count;
        combat.active_weapon_slot = active;
        combat.weapon_ids[0] = kMeteorStaff;
        combat.weapon_ids[1] = kMeteorStormStaff;
        combat.ammo[0] = 3;
        combat.ammo[1] = 1;
        combat.reserve_magazines[0] = reserve0;
        combat.reserve_magazines[1] = reserve1;
        require(Kernel_ServerSetEntityCombatState(kernel, player, &combat));
        Kernel_Update(kernel, kTickSeconds);
    }

    // Uses one potion with no target, as a client using it on itself might.
    KernelGameplayRequestOutcome drink() {
        KernelGameplayRequest request{};
        request.struct_size = sizeof(request);
        request.requester_peer = kPeer;
        request.request_id = next_request++;
        request.instigator_net_id = player;
        request.domain_action = KernelDomainAction_Consume;
        request.selected_item_instance_id = potions;
        request.requested_quantity = 1;
        require(Kernel_ServerSubmitGameplayRequest(kernel, &request));
        KernelGameplayRequestOutcome outcome{};
        outcome.struct_size = sizeof(outcome);
        require(Kernel_PollGameplayRequestOutcomes(kernel, &outcome, 1) == 1u);
        Kernel_Update(kernel, kTickSeconds);
        std::fprintf(stderr, "drink: status=%u graph=%u reject=%u\n",
                     outcome.status, outcome.graph_outcome, outcome.rejection_reason);
        return outcome;
    }

    std::uint32_t potions_left() {
        KernelItemInstanceView view{};
        view.struct_size = sizeof(view);
        require(Kernel_GetItemInstance(kernel, potions, &view));
        return view.residency == KernelItemResidency_Terminal ? 0u : view.quantity;
    }

    std::uint16_t reserve(std::size_t slot) {
        return entity_state(kernel, player).reserve_magazines[slot];
    }
};

}  // namespace

int main() {
    rounding_table();

    const gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    const std::uint32_t mp_potion = item_template_id_of(config, "fungible_mp_potion");
    require(mp_potion != 0u);

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 64;
    Arena arena;
    arena.kernel = Kernel_Create(&kernel_config);
    require(arena.kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(arena.kernel, config));
    require(Kernel_StartDedicatedServer(arena.kernel, 8058));

    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = gs::kActorTypePlayer;
    create.entity_template_id = config.player.actor_template_id;
    create.actor_template_id = config.player.actor_template_id;
    create.owner_peer = kPeer;
    create.position = KernelVec3{0.0f, 1.0f, 0.0f};
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerCreateEntity(arena.kernel, &create, &arena.player));
    require(Kernel_ServerSetEntityActorTemplate(
        arena.kernel, arena.player, config.player.actor_template_id));
    for (const std::uint8_t weapon : {kMeteorStaff, kMeteorStormStaff}) {
        KernelWeaponMechanicsDefinition mechanics = config.weapons.definitions[weapon];
        require(Kernel_ServerSetEntityWeaponMechanics(arena.kernel, arena.player, &mechanics));
    }
    arena.combat = gs::make_player_combat_state(config);

    KernelInventoryContainerId container = 0;
    require(Kernel_ServerCreateInventoryContainer(arena.kernel, arena.player, 8, &container));
    require(Kernel_ServerCreateInventoryItem(
        arena.kernel, mp_potion, 5, container, &arena.potions));

    // Meteor staff in hand, 1 of 6 left: half of 6 is 3, so 1 -> 4. The other
    // slot is not touched.
    arena.set_loadout(2, 0, 1, 0);
    KernelGameplayRequestOutcome outcome = arena.drink();
    require(outcome.status == KernelGameplayRequestStatus_Committed);
    require(arena.reserve(0) == 4u);
    require(arena.reserve(1) == 0u);
    require(arena.potions_left() == 4u);

    // 4 of 6: capped at the template's 6.
    outcome = arena.drink();
    require(outcome.status == KernelGameplayRequestStatus_Committed);
    require(arena.reserve(0) == 6u);
    require(arena.potions_left() == 3u);

    // Full: refused, and the potion is not spent.
    outcome = arena.drink();
    require(outcome.status == KernelGameplayRequestStatus_Rejected);
    require(outcome.rejection_reason == KernelGameplayRequestRejection_GraphRejected);
    require(arena.reserve(0) == 6u);
    require(arena.potions_left() == 3u);

    // The storm staff in hand (4 at most, 0 left): half of 4 is 2.
    arena.set_loadout(2, 1, 6, 0);
    outcome = arena.drink();
    require(outcome.status == KernelGameplayRequestStatus_Committed);
    require(arena.reserve(1) == 2u);
    require(arena.reserve(0) == 6u);
    require(arena.potions_left() == 2u);

    // Unarmed: refused, not spent.
    arena.set_loadout(0, 0, 0, 0);
    outcome = arena.drink();
    require(outcome.status == KernelGameplayRequestStatus_Rejected);
    require(outcome.rejection_reason == KernelGameplayRequestRejection_GraphRejected);
    require(arena.potions_left() == 2u);

    std::puts("mp_potion_test passed");
    return 0;
}
