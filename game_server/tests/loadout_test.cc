// The initial camp's loadout, end to end on a listen host (design D1-D6,
// P2, 2026-10-07).
//
// The shipped game rule puts the initial camp in the scene. Activating it sends
// the player the camp's offer (GAME_SERVER_MESSAGE_LOADOUT_OFFERS); a pick
// (LOADOUT_SELECT) is checked once, replaces the inventory at once, answers
// with LOADOUT_RESULT, and is what the next respawn gives. Every refusal leaves
// the inventory as it was.
//
// The listen host's own player has no wire to its server, so the messages run
// through the kernel's in-process queues; the network path is
// game_message_test's.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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

std::uint32_t read_u32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint16_t read_u16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

struct Entry {
    std::uint32_t item_template_id = 0;
    std::uint16_t quantity = 0;
};

struct WeaponOption {
    std::uint32_t item_template_id = 0;
    std::uint8_t category = 0;
};

struct Offers {
    std::uint32_t camp = 0;
    std::uint8_t capacity = 0;
    std::vector<Entry> options;
    std::vector<WeaponOption> weapon_options;
    std::vector<Entry> current;
    std::vector<std::uint32_t> current_weapons;
};

// Parses an OFFERS body exactly, or fails the test.
Offers parse_offers(const KernelGameMessage& message) {
    require(message.message_type == GAME_SERVER_MESSAGE_LOADOUT_OFFERS);
    const std::uint8_t* at = message.payload;
    const std::uint8_t* end = message.payload + message.payload_size;
    Offers offers;
    require(end - at >= 6);
    offers.camp = read_u32(at);
    offers.capacity = at[4];
    const std::uint8_t option_count = at[5];
    at += 6;
    for (std::uint8_t index = 0; index < option_count; ++index) {
        require(end - at >= 6);
        offers.options.push_back(Entry{read_u32(at), read_u16(at + 4)});
        at += 6;
    }
    require(end - at >= 1);
    const std::uint8_t weapon_option_count = *at++;
    for (std::uint8_t index = 0; index < weapon_option_count; ++index) {
        require(end - at >= 5);
        offers.weapon_options.push_back(WeaponOption{read_u32(at), at[4]});
        at += 5;
    }
    require(end - at >= 1);
    const std::uint8_t current_count = *at++;
    for (std::uint8_t index = 0; index < current_count; ++index) {
        require(end - at >= 6);
        offers.current.push_back(Entry{read_u32(at), read_u16(at + 4)});
        at += 6;
    }
    require(end - at >= 1);
    const std::uint8_t current_weapon_count = *at++;
    for (std::uint8_t index = 0; index < current_weapon_count; ++index) {
        require(end - at >= 4);
        offers.current_weapons.push_back(read_u32(at));
        at += 4;
    }
    require(at == end);
    return offers;
}

struct Harness {
    KernelHandle* kernel = nullptr;
    gs::GameServer* server = nullptr;
    std::uint32_t player = 0;
    std::uint32_t peer = 0;
    std::uint64_t next_request = 1;
    std::uint8_t last_weapon_picks = 0;
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

    void stand_near(std::uint32_t camp, float offset) {
        const KernelServerEntityState camp_state = state_of(camp);
        const KernelVec3 position{
            camp_state.position.x + offset, camp_state.position.y + 0.5f,
            camp_state.position.z};
        const KernelQuat rotation{0.0f, 0.0f, 0.0f, 1.0f};
        require(Kernel_ServerSetEntityTransform(kernel, player, &position, &rotation));
        step(5);
    }

    // Activates the camp and returns the offer it answers with.
    Offers activate(std::uint32_t camp) {
        KernelGameplayRequest request{};
        request.struct_size = sizeof(request);
        request.requester_peer = peer;
        request.request_id = next_request++;
        request.instigator_net_id = player;
        request.domain_action = KernelDomainAction_Activate;
        request.target_net_id = camp;
        require(Kernel_SubmitGameplayRequest(kernel, &request));
        KernelGameplayRequestOutcome outcome{};
        outcome.struct_size = sizeof(outcome);
        while (Kernel_PollGameplayRequestOutcomes(kernel, &outcome, 1) != 0u) {}
        step(2);
        KernelGameMessage message{};
        require(Kernel_PollGameMessages(kernel, &message, 1) == 1u);
        return parse_offers(message);
    }

    // Sends a raw SELECT body and returns {result, pick_count}.
    std::pair<std::uint8_t, std::uint8_t> select_raw(const std::vector<std::uint8_t>& body) {
        require(Kernel_SendGameMessage(
            kernel, GAME_SERVER_MESSAGE_LOADOUT_SELECT, body.data(),
            static_cast<std::uint32_t>(body.size())));
        step(2);
        KernelGameMessage reply{};
        require(Kernel_PollGameMessages(kernel, &reply, 1) == 1u);
        require(reply.message_type == GAME_SERVER_MESSAGE_LOADOUT_RESULT);
        require(reply.payload_size == 7u);
        last_weapon_picks = reply.payload[6];
        return {reply.payload[4], reply.payload[5]};
    }

    std::pair<std::uint8_t, std::uint8_t> select(
        std::uint32_t camp, const std::vector<std::uint8_t>& picks,
        const std::vector<std::uint8_t>& weapon_picks = {}) {
        std::vector<std::uint8_t> body;
        for (int shift = 0; shift < 32; shift += 8) {
            body.push_back(static_cast<std::uint8_t>((camp >> shift) & 0xffu));
        }
        body.push_back(static_cast<std::uint8_t>(picks.size()));
        body.insert(body.end(), picks.begin(), picks.end());
        body.push_back(static_cast<std::uint8_t>(weapon_picks.size()));
        body.insert(body.end(), weapon_picks.begin(), weapon_picks.end());
        return select_raw(body);
    }

    // The weapon ids in hand, in loadout (category) order.
    std::vector<std::uint32_t> weapons() {
        const KernelServerEntityState me = state_of(player);
        return std::vector<std::uint32_t>(
            me.weapon_ids, me.weapon_ids + me.weapon_slot_count);
    }

    // Item template id -> total quantity in the player's inventory.
    std::map<std::uint32_t, std::uint32_t> inventory() {
        KernelInventoryContainerView owned[2]{};
        for (KernelInventoryContainerView& view : owned) view.struct_size = sizeof(view);
        const std::uint32_t owned_count =
            Kernel_CopyOwnedInventoryContainers(kernel, player, owned, 2);
        KernelInventoryContainerView container{};
        for (std::uint32_t index = 0; index < owned_count; ++index) {
            if (owned[index].container_kind == KernelInventoryContainerKind_Items) {
                container = owned[index];
            }
        }
        require(container.inventory_container_id != 0u);
        std::vector<KernelItemInstanceView> items(64);
        for (KernelItemInstanceView& item : items) item.struct_size = sizeof(item);
        const std::uint32_t count = Kernel_CopyInventorySlots(
            kernel, container.inventory_container_id, items.data(),
            static_cast<std::uint32_t>(items.size()));
        std::map<std::uint32_t, std::uint32_t> totals;
        for (std::uint32_t index = 0; index < count; ++index) {
            totals[items[index].item_template_id] += items[index].quantity;
        }
        return totals;
    }
};

std::uint32_t item_id(const gs::GameServerGameplayConfig& config, const std::string& name) {
    for (const gs::ItemTemplateConfig& item : config.item_templates) {
        if (item.name == name) return item.definition.item_template_id;
    }
    require(false);
    return 0;
}

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    const gs::ActorTemplateConfig* camp_template = nullptr;
    for (const gs::EntityTemplateConfig& candidate : config.entity_templates) {
        if (candidate.name == "initial_camp") camp_template = &candidate;
    }
    require(camp_template != nullptr);
    require(!camp_template->loadout_options.empty());
    const gs::ActorTemplateConfig* player_template =
        gs::find_actor_template(config, config.player.actor_template_id);
    require(player_template != nullptr);
    const std::uint32_t potion = item_id(config, "fungible_potion");
    const std::uint32_t mp_potion = item_id(config, "fungible_mp_potion");
    const std::uint32_t tornado = item_id(config, "fungible_tornado_bottle");

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
    require(Kernel_StartListenServer(kernel, 8060));

    gs::GameServer server(kernel, config);
    require(server.preload_directors());
    Harness harness{kernel, &server};
    harness.step(30);
    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(kernel, &local));
    harness.player = local.player_net_id;
    harness.peer = local.peer_id;
    require(harness.player != 0u);

    // The game rule put exactly one camp down.
    std::vector<KernelServerEntityState> props(512);
    for (KernelServerEntityState& state : props) state.struct_size = sizeof(state);
    const std::uint32_t prop_count = Kernel_ServerQueryEntities(
        kernel, KernelEntityType_Prop, props.data(), static_cast<std::uint32_t>(props.size()));
    std::uint32_t camp = 0;
    for (std::uint32_t index = 0; index < prop_count; ++index) {
        if (props[index].entity_template_id == camp_template->actor_template_id) {
            require(camp == 0u);
            camp = props[index].net_id;
        }
    }
    require(camp != 0u);

    // The default loadout before any pick: the template's items, and its
    // weapons as weapon items, packed in category order (rifle 0, meteor
    // staff 1, sky laser 2, meteor storm staff 3).
    const std::map<std::uint32_t, std::uint32_t> defaults = harness.inventory();
    require(defaults.count(tornado) == 1u);
    require(defaults.count(potion) == 0u);
    const std::vector<std::uint32_t> default_weapons = harness.weapons();
    require((default_weapons == std::vector<std::uint32_t>{0u, 13u, 15u, 14u}));

    // Activating it sends the offer: every option, the slot cap, no pick yet.
    harness.stand_near(camp, 2.0f);
    Offers offers = harness.activate(camp);
    require(offers.camp == camp);
    require(offers.capacity == player_template->inventory_slot_capacity);
    require(offers.options.size() == camp_template->loadout_options.size());
    for (std::size_t index = 0; index < offers.options.size(); ++index) {
        require(offers.options[index].item_template_id ==
                camp_template->loadout_options[index].item_template_id);
        require(offers.options[index].quantity ==
                camp_template->loadout_options[index].quantity);
    }
    require(offers.current.empty());
    require(offers.current_weapons.empty());
    require(offers.options[0].item_template_id == potion);
    require(offers.options[1].item_template_id == mp_potion);
    require(offers.weapon_options.size() == camp_template->loadout_weapon_options.size());
    const auto weapon_option = [&](const std::string& name) -> std::uint8_t {
        const std::uint32_t id = item_id(config, name);
        for (std::size_t index = 0; index < offers.weapon_options.size(); ++index) {
            if (offers.weapon_options[index].item_template_id == id) {
                return static_cast<std::uint8_t>(index);
            }
        }
        require(false);
        return 0;
    };
    const std::uint8_t pick_rifle = weapon_option("stateful_weapon_rifle");
    const std::uint8_t pick_shotgun = weapon_option("stateful_weapon_shotgun");
    const std::uint8_t pick_beam = weapon_option("stateful_weapon_beam_rifle");
    require(offers.weapon_options[pick_rifle].category == 0u);
    require(offers.weapon_options[pick_shotgun].category == 0u);
    require(offers.weapon_options[pick_beam].category == 2u);

    // Items with no weapons: the items, and unarmed (D18).
    {
        auto [items_only, items_only_count] = harness.select(camp, {0});
        require(items_only == GAME_SERVER_LOADOUT_RESULT_APPLIED);
        require(items_only_count == 1u);
        require(harness.last_weapon_picks == 0u);
        require(harness.weapons().empty());
    }

    // A pick replaces the inventory at once. Option 0 twice: two picks, one
    // slot each, 3 + 3 potions.
    auto [result, picked] = harness.select(camp, {0, 0, 1}, {pick_beam, pick_rifle});
    require(result == GAME_SERVER_LOADOUT_RESULT_APPLIED);
    require(picked == 3u);
    require(harness.last_weapon_picks == 2u);
    const std::vector<std::uint32_t> picked_weapons{0u, 5u};
    require(harness.weapons() == picked_weapons);
    std::map<std::uint32_t, std::uint32_t> held = harness.inventory();
    require(held[potion] == 6u);
    require(held[mp_potion] == 2u);
    require(held.size() == 2u);

    // The camp remembers it.
    offers = harness.activate(camp);
    require(offers.current.size() == 3u);
    require(offers.current[0].item_template_id == potion);
    require(offers.current[2].item_template_id == mp_potion);
    require(offers.current_weapons.size() == 2u);

    // Refusals change nothing.
    const auto unchanged = [&]() {
        const std::map<std::uint32_t, std::uint32_t> now = harness.inventory();
        require(now == held);
        require(harness.weapons() == picked_weapons);
    };
    // Two weapons of one category: one slot, so refused.
    require(harness.select(camp, {0}, {pick_rifle, pick_shotgun}).first ==
            GAME_SERVER_LOADOUT_RESULT_CATEGORY_TAKEN);
    unchanged();
    require(harness.select(camp, {0}, {99}).first == GAME_SERVER_LOADOUT_RESULT_BAD_OPTION);
    unchanged();
    require(harness.select(camp, {0, 99}).first == GAME_SERVER_LOADOUT_RESULT_BAD_OPTION);
    unchanged();
    std::vector<std::uint8_t> too_many(player_template->inventory_slot_capacity + 1u, 1u);
    require(harness.select(camp, too_many).first ==
            GAME_SERVER_LOADOUT_RESULT_TOO_MANY_PICKS);
    unchanged();
    require(harness.select(harness.player, {0}).first ==
            GAME_SERVER_LOADOUT_RESULT_NOT_A_CAMP);
    unchanged();
    require(harness.select_raw({1, 2, 3}).first == GAME_SERVER_LOADOUT_RESULT_MALFORMED);
    require(harness.select_raw({0, 0, 0, 0, 2, 0}).first ==
            GAME_SERVER_LOADOUT_RESULT_MALFORMED);
    unchanged();
    harness.stand_near(camp, 20.0f);
    require(harness.select(camp, {1}).first == GAME_SERVER_LOADOUT_RESULT_OUT_OF_RANGE);
    unchanged();

    // A message game_server does not know is left alone: no answer.
    const std::uint8_t noise[1] = {0};
    require(Kernel_SendGameMessage(kernel, 999u, noise, 1u));
    harness.step(2);
    KernelGameMessage nothing{};
    require(Kernel_PollGameMessages(kernel, &nothing, 1) == 0u);

    // A respawn gives the picked loadout, not the default -- even after the
    // player used some of it.
    {
        KernelGameplayRequest drink{};
        drink.struct_size = sizeof(drink);
        drink.requester_peer = harness.peer;
        drink.request_id = harness.next_request++;
        drink.instigator_net_id = harness.player;
        drink.target_net_id = harness.player;
        drink.domain_action = KernelDomainAction_Consume;
        KernelInventoryContainerView container{};
        container.struct_size = sizeof(container);
        require(Kernel_CopyOwnedInventoryContainers(kernel, harness.player, &container, 1) == 1u);
        std::vector<KernelItemInstanceView> items(16);
        for (KernelItemInstanceView& item : items) item.struct_size = sizeof(item);
        const std::uint32_t count = Kernel_CopyInventorySlots(
            kernel, container.inventory_container_id, items.data(), 16u);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (items[index].item_template_id == potion) {
                drink.selected_item_instance_id = items[index].item_instance_id;
            }
        }
        require(drink.selected_item_instance_id != 0u);
        require(Kernel_SubmitGameplayRequest(kernel, &drink));
        harness.step(2);
        require(harness.inventory()[potion] == 5u);
    }
    require(Kernel_ServerSetEntityHealth(kernel, harness.player, 0u));
    KernelEvent died{};
    died.type = KernelEventType_EntityDied;
    died.net_id = harness.player;
    server.handle_event(died);
    const int respawn_ticks =
        static_cast<int>(std::lround(config.player.respawn.delay_seconds * 30.0f)) + 5;
    harness.step(respawn_ticks);
    require(harness.state_of(harness.player).hp != 0u);
    require(harness.inventory() == held);
    // The picked weapons, fresh.
    require(harness.weapons() == picked_weapons);
    {
        const KernelServerEntityState me = harness.state_of(harness.player);
        require(me.ammo[0] == config.weapons.definitions[0].magazine_size);
        require(me.reserve_magazines[1] == config.weapons.definitions[5].reserve_magazines);
    }

    // A dead player cannot pick.
    harness.stand_near(camp, 2.0f);
    require(Kernel_ServerSetEntityHealth(kernel, harness.player, 0u));
    require(harness.select(camp, {1}).first == GAME_SERVER_LOADOUT_RESULT_DEAD);
    require(Kernel_ServerSetEntityHealth(kernel, harness.player, 100u));

    // No picks: back to the default.
    auto [reset, reset_count] = harness.select(camp, {});
    require(reset == GAME_SERVER_LOADOUT_RESULT_APPLIED);
    require(reset_count == 0u);
    require(harness.inventory() == defaults);
    require(harness.weapons() == default_weapons);

    Kernel_Destroy(kernel);
    std::puts("loadout_test passed");
    return 0;
}
