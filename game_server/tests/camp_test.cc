// A temporary camp (P4: K9, design D7-D9, D22, D25), end to end on a listen
// host with the shipped catalog.
//
// field_camp is stocked by game_server the moment it appears: a container the
// camp owns, one slot per camp.stock entry. Only someone inside may take from
// it (Transfer), only onto themselves, never back. A weapon taken swaps out the
// one held in its category, which goes down where the taker went in. What is
// taken is untagged (D22). When the camp goes, the people inside are let out
// first and whatever stock is left goes with it.

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

#include "game_server/public/game_server_types.h"
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

std::uint32_t item_id(const gs::GameServerGameplayConfig& config, const std::string& name) {
    for (const gs::ItemTemplateConfig& item : config.item_templates) {
        if (item.name == name) return item.definition.item_template_id;
    }
    require(false);
    return 0;
}

struct Harness {
    KernelHandle* kernel = nullptr;
    gs::GameServer* server = nullptr;
    std::uint32_t player = 0;
    std::uint32_t peer = 0;
    std::uint64_t next_request = 1;
    std::array<KernelEvent, 4096> events{};

    void step(int count = 1) {
        for (int index = 0; index < count; ++index) {
            Kernel_Update(kernel, kTick);
            const std::uint32_t n = Kernel_PollEvents(
                kernel, events.data(), static_cast<std::uint32_t>(events.size()));
            for (std::uint32_t i = 0; i < n; ++i) server->handle_event(events[i]);
            server->tick(kTick);
        }
    }

    KernelServerEntityState state_of(std::uint32_t net_id) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(kernel, net_id, &state));
        return state;
    }

    KernelItemInstanceView item(KernelItemInstanceId id) {
        KernelItemInstanceView view{};
        view.struct_size = sizeof(view);
        require(Kernel_GetItemInstance(kernel, id, &view));
        return view;
    }

    void stand_at(const KernelVec3& at) {
        const KernelVec3 position{at.x, at.y + 0.5f, at.z};
        const KernelQuat rotation{0.0f, 0.0f, 0.0f, 1.0f};
        require(Kernel_ServerSetEntityTransform(kernel, player, &position, &rotation));
        step(5);
    }

    KernelGameplayRequestOutcome request(
        std::uint8_t action, KernelItemInstanceId item_id, std::uint32_t target,
        std::uint32_t quantity = 1, KernelVec3 placement = {},
        KernelVec3 throw_direction = {}) {
        KernelGameplayRequest request{};
        request.struct_size = sizeof(request);
        request.requester_peer = peer;
        request.request_id = next_request++;
        request.instigator_net_id = player;
        request.domain_action = action;
        request.selected_item_instance_id = item_id;
        request.target_net_id = target;
        request.requested_quantity = quantity;
        request.placement_position = placement;
        request.throw_direction = throw_direction;
        require(Kernel_SubmitGameplayRequest(kernel, &request));
        KernelGameplayRequestOutcome outcome{};
        outcome.struct_size = sizeof(outcome);
        require(Kernel_PollGameplayRequestOutcomes(kernel, &outcome, 1) == 1u);
        if (outcome.status != KernelGameplayRequestStatus_Committed) {
            std::fprintf(stderr, "request action=%u rejected reason=%u\n",
                         action, outcome.rejection_reason);
        }
        step(2);
        return outcome;
    }

    // The player's containers by kind.
    KernelInventoryContainerId container(std::uint8_t kind) {
        KernelInventoryContainerView owned[2]{};
        for (KernelInventoryContainerView& view : owned) view.struct_size = sizeof(view);
        const std::uint32_t count =
            Kernel_CopyOwnedInventoryContainers(kernel, player, owned, 2);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (owned[index].container_kind == kind) return owned[index].inventory_container_id;
        }
        require(false);
        return 0;
    }

    std::vector<KernelItemInstanceView> slots(KernelInventoryContainerId id) {
        std::vector<KernelItemInstanceView> items(32);
        for (KernelItemInstanceView& item : items) item.struct_size = sizeof(item);
        items.resize(Kernel_CopyInventorySlots(
            kernel, id, items.data(), static_cast<std::uint32_t>(items.size())));
        return items;
    }

    bool holds(KernelInventoryContainerId id, KernelItemInstanceId item_id) {
        for (const KernelItemInstanceView& view : slots(id)) {
            if (view.item_instance_id == item_id) return true;
        }
        return false;
    }

    std::vector<std::uint32_t> weapons() {
        const KernelServerEntityState me = state_of(player);
        return std::vector<std::uint32_t>(me.weapon_ids, me.weapon_ids + me.weapon_slot_count);
    }

    std::pair<std::uint8_t, std::uint8_t> select(
        std::uint32_t camp, const std::vector<std::uint8_t>& picks,
        const std::vector<std::uint8_t>& weapon_picks) {
        std::vector<std::uint8_t> body;
        for (int shift = 0; shift < 32; shift += 8) {
            body.push_back(static_cast<std::uint8_t>((camp >> shift) & 0xffu));
        }
        body.push_back(static_cast<std::uint8_t>(picks.size()));
        body.insert(body.end(), picks.begin(), picks.end());
        body.push_back(static_cast<std::uint8_t>(weapon_picks.size()));
        body.insert(body.end(), weapon_picks.begin(), weapon_picks.end());
        require(Kernel_SendGameMessage(
            kernel, GAME_SERVER_MESSAGE_LOADOUT_SELECT, body.data(),
            static_cast<std::uint32_t>(body.size())));
        step(2);
        KernelGameMessage reply{};
        while (Kernel_PollGameMessages(kernel, &reply, 1) == 1u) {
            if (reply.message_type == GAME_SERVER_MESSAGE_LOADOUT_RESULT) {
                return {reply.payload[4], reply.payload[6]};
            }
        }
        require(false);
        return {};
    }
};

// Finds the world item of `item_template` and returns {item, prop}.
std::pair<KernelItemInstanceId, std::uint32_t> world_item(
    Harness& harness, std::uint32_t item_template) {
    std::vector<KernelServerEntityState> props(512);
    for (KernelServerEntityState& state : props) state.struct_size = sizeof(state);
    const std::uint32_t count = Kernel_ServerQueryEntities(
        harness.kernel, KernelEntityType_Prop, props.data(),
        static_cast<std::uint32_t>(props.size()));
    for (std::uint32_t index = 0; index < count; ++index) {
        if (props[index].item_template_id == item_template) {
            return {props[index].item_instance_id, props[index].net_id};
        }
    }
    require(false);
    return {};
}

}  // namespace

int main() {
    const std::filesystem::path catalog = catalog_root() / "gameplay_catalog.yaml";
    const gs::GameServerGameplayConfig config =
        gs::load_gameplay_config_from_catalog_file(catalog.string());
    const gs::EntityTemplateConfig* camp_template = nullptr;
    const gs::EntityTemplateConfig* tent_template = nullptr;
    for (const gs::EntityTemplateConfig& candidate : config.entity_templates) {
        if (candidate.name == "field_camp") camp_template = &candidate;
        if (candidate.name == "tent") tent_template = &candidate;
    }
    require(camp_template != nullptr && tent_template != nullptr);
    require(camp_template->camp_stock.size() == 4u);
    // D25: tents and camps share one population group.
    require(camp_template->prop.population_group_id != 0u);
    require(camp_template->prop.population_group_id ==
            tent_template->prop.population_group_id);
    const std::uint32_t potion_item = item_id(config, "fungible_potion");
    const std::uint32_t shotgun_item = item_id(config, "stateful_weapon_shotgun");

    const std::vector<std::uint8_t> scene = read_ground_scene();
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_ListenServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 4096;
    kernel_config.max_render_states = 512;
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
    require(Kernel_StartListenServer(kernel, 8063));

    gs::GameServer server(kernel, config);
    require(server.preload_directors());
    Harness harness{kernel, &server};
    harness.step(30);
    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(kernel, &local));
    harness.player = local.player_net_id;
    harness.peer = local.peer_id;
    const KernelInventoryContainerId items = harness.container(KernelInventoryContainerKind_Items);
    const KernelInventoryContainerId weapons =
        harness.container(KernelInventoryContainerKind_Weapons);

    // A camp appears (as a thrown kit's landing makes one) and is stocked.
    const KernelVec3 camp_at{-12.0f, 0.0f, 6.0f};
    std::uint32_t camp = 0;
    {
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_type = KernelEntityType_Prop;
        create.entity_template_id = camp_template->actor_template_id;
        create.position = camp_at;
        create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        require(Kernel_ServerCreateEntity(kernel, &create, &camp));
    }
    harness.step(2);
    KernelInventoryContainerId stock = 0;
    {
        KernelInventoryContainerView owned[2]{};
        for (KernelInventoryContainerView& view : owned) view.struct_size = sizeof(view);
        require(Kernel_CopyOwnedInventoryContainers(kernel, camp, owned, 2) == 1u);
        require(owned[0].slot_capacity == 4u);
        stock = owned[0].inventory_container_id;
    }
    std::vector<KernelItemInstanceView> stocked = harness.slots(stock);
    require(stocked.size() == 4u);
    const auto stock_of = [&](std::uint32_t item_template) {
        for (const KernelItemInstanceView& view : harness.slots(stock)) {
            if (view.item_template_id == item_template) return view;
        }
        return KernelItemInstanceView{};
    };
    const KernelItemInstanceId stock_potions = stock_of(potion_item).item_instance_id;
    const KernelItemInstanceId stock_shotgun = stock_of(shotgun_item).item_instance_id;
    require(stock_potions != 0u && stock_shotgun != 0u);
    require(stock_of(potion_item).quantity == 3u);
    const auto potions_held = [&]() {
        std::uint32_t total = 0;
        for (const KernelItemInstanceView& view : harness.slots(items)) {
            if (view.item_template_id == potion_item) total += view.quantity;
        }
        return total;
    };

    // From outside: refused, and nothing moves.
    KernelServerEntityState camp_state = harness.state_of(camp);
    harness.stand_at(KernelVec3{camp_state.position.x + 2.0f, camp_state.position.y,
                                camp_state.position.z});
    const std::uint32_t potions_before = potions_held();
    {
        const KernelGameplayRequestOutcome outside =
            harness.request(KernelDomainAction_Transfer, stock_potions, camp, 2u);
        require(outside.status == KernelGameplayRequestStatus_Rejected);
        require(outside.rejection_reason == KernelGameplayRequestRejection_NotAuthorized);
        require(stock_of(potion_item).quantity == 3u);
        require(potions_held() == potions_before);
    }

    // Activating the camp takes the player inside.
    require(harness.request(KernelDomainAction_Activate, 0u, camp).status ==
            KernelGameplayRequestStatus_Committed);
    harness.step(3);
    {
        KernelLocalShelterState shelter{};
        shelter.struct_size = sizeof(shelter);
        require(Kernel_GetLocalShelterState(kernel, &shelter));
        require(shelter.shelter_net_id == camp);
    }

    // Inside: two potions taken, one left; more than is left is refused.
    {
        const KernelGameplayRequestOutcome taken =
            harness.request(KernelDomainAction_Transfer, stock_potions, camp, 2u);
        require(taken.status == KernelGameplayRequestStatus_Committed);
        require(taken.committed_quantity == 2u);
        require(stock_of(potion_item).quantity == 1u);
        require(potions_held() == potions_before + 2u);
        const KernelGameplayRequestOutcome greedy =
            harness.request(KernelDomainAction_Transfer, stock_potions, camp, 2u);
        require(greedy.status == KernelGameplayRequestStatus_Rejected);
        require(greedy.rejection_reason == KernelGameplayRequestRejection_InvalidQuantity);
        require(stock_of(potion_item).quantity == 1u);
    }
    // Take-only: the player's own items cannot go the other way.
    {
        const KernelItemInstanceId own_item = harness.slots(items).front().item_instance_id;
        const KernelGameplayRequestOutcome back =
            harness.request(KernelDomainAction_Transfer, own_item, camp, 1u);
        require(back.status == KernelGameplayRequestStatus_Rejected);
        require(back.rejection_reason == KernelGameplayRequestRejection_NotAuthorized);
    }

    // A weapon: into its category's slot; the one held there goes down where
    // the player went in, untagged like everything from a camp (D22).
    {
        KernelItemInstanceView shotgun_view = harness.item(stock_shotgun);
        std::uint32_t category = 0;
        for (const gs::ItemTemplateConfig& candidate : config.item_templates) {
            if (candidate.definition.item_template_id == shotgun_item) {
                category = candidate.definition.weapon_category;
            }
        }
        KernelItemInstanceId held_before = 0;
        for (const KernelItemInstanceView& view : harness.slots(weapons)) {
            if (view.slot == category) held_before = view.item_instance_id;
        }
        require(held_before != 0u);
        const KernelGameplayRequestOutcome taken =
            harness.request(KernelDomainAction_Transfer, stock_shotgun, camp, 0u);
        require(taken.status == KernelGameplayRequestStatus_Committed);
        require(harness.holds(weapons, stock_shotgun));
        shotgun_view = harness.item(stock_shotgun);
        require(shotgun_view.drop_tag == KERNEL_DROP_TAG_NONE);
        const KernelItemInstanceView swapped = harness.item(held_before);
        require(swapped.residency == KernelItemResidency_World);
        const KernelServerEntityState lying = harness.state_of(swapped.prop_entity_id);
        require(std::hypot(lying.position.x - camp_state.position.x,
                           lying.position.z - camp_state.position.z) < 4.0f);
        require(stock_of(shotgun_item).item_instance_id == 0u);
    }

    // The camp goes: the player is let out, and the stock left goes with it.
    const std::vector<KernelItemInstanceView> left = harness.slots(stock);
    require(left.size() == 3u);
    require(Kernel_ServerDestroyEntity(kernel, camp, KernelDespawnReason_Destroyed));
    harness.step(2);
    {
        KernelLocalShelterState shelter{};
        shelter.struct_size = sizeof(shelter);
        require(Kernel_GetLocalShelterState(kernel, &shelter));
        require(shelter.shelter_net_id == 0u);
        KernelInventoryContainerView owned[2]{};
        for (KernelInventoryContainerView& view : owned) view.struct_size = sizeof(view);
        require(Kernel_CopyOwnedInventoryContainers(kernel, camp, owned, 2) == 0u);
        for (const KernelItemInstanceView& view : left) {
            KernelItemInstanceView after{};
            after.struct_size = sizeof(after);
            require(!Kernel_GetItemInstance(kernel, view.item_instance_id, &after) ||
                    after.residency == KernelItemResidency_Terminal);
        }
    }
    // What the player took stays theirs.
    require(harness.holds(weapons, stock_shotgun));
    require(potions_held() == potions_before + 2u);

    Kernel_Destroy(kernel);
    std::puts("camp_test passed");
    return 0;
}
