// Drop tags (K12, design D19/D22/D23, 2026-10-07), end to end on a listen host
// with the shipped catalog.
//
// The scene puts a beam rifle (a map weapon) and a quest relic near the spawn:
// the shipped catalog with `scene_items:` added, in a temporary copy -- the
// shipped one places none, since every entity placed moves the net ids that
// order-sensitive tests key on.
// Their tags are set where they are made -- the relic's by its template, the
// rifle's because the map placed it -- and travel with them. Reapplying a
// loadout (a camp pick, a respawn) clears only untagged things: the relic
// stays, and the map weapon stays unless the pick wants its category, in which
// case it goes to the player's feet, as a same-category pickup would put it.
// Fungible stacks merge only on an equal tag, and a split keeps its source's.
// A death (K11) puts the tagged on the ground round the body and leaves the
// untagged for the respawn to replace.

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

std::filesystem::path catalog_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace / "game_server" /
        "gameplay_catalog";
}

// The shipped catalog with a beam rifle and a quest relic laid near the spawn.
gs::GameServerGameplayConfig catalog_with_scene_items() {
    const char* tmp = std::getenv("TEST_TMPDIR");
    require(tmp != nullptr);
    const std::filesystem::path root = std::filesystem::path(tmp) / "catalog_scene_items";
    std::filesystem::remove_all(root);
    std::filesystem::copy(catalog_root(), root, std::filesystem::copy_options::recursive);
    const std::filesystem::path catalog = root / "gameplay_catalog.yaml";
    std::ifstream in(catalog);
    require(in.good());
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const std::size_t player = text.find("\nplayer:");
    require(player != std::string::npos);
    text.insert(player + 1,
                "scene_items:\n"
                "  - item_template: stateful_weapon_beam_rifle\n"
                "    position: {x: -6.0, y: 0.1, z: 0.0}\n"
                "  - item_template: stateful_quest_relic\n"
                "    position: {x: -6.0, y: 0.1, z: 3.0}\n");
    std::ofstream(catalog, std::ios::trunc) << text;
    return gs::load_gameplay_config_from_catalog_file(catalog.string());
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
    const gs::GameServerGameplayConfig config = catalog_with_scene_items();
    require(config.scene_items.size() == 2u);
    const std::uint32_t relic_item = item_id(config, "stateful_quest_relic");
    const std::uint32_t beam_item = item_id(config, "stateful_weapon_beam_rifle");
    const std::uint32_t sky_item = item_id(config, "stateful_weapon_sky_laser");
    const std::uint32_t potion_item = item_id(config, "fungible_potion");
    const gs::ActorTemplateConfig* camp_template = nullptr;
    for (const gs::EntityTemplateConfig& candidate : config.entity_templates) {
        if (candidate.name == "initial_camp") camp_template = &candidate;
    }
    require(camp_template != nullptr);
    const auto weapon_option = [&](std::uint32_t id) {
        for (std::size_t index = 0; index < camp_template->loadout_weapon_options.size(); ++index) {
            if (camp_template->loadout_weapon_options[index].item_template_id == id) {
                return static_cast<std::uint8_t>(index);
            }
        }
        require(false);
        return std::uint8_t{0};
    };
    const std::uint8_t pick_rifle = weapon_option(item_id(config, "stateful_weapon_rifle"));
    const std::uint8_t pick_sky = weapon_option(sky_item);

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
    require(Kernel_StartListenServer(kernel, 8062));

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

    // The scene: a map weapon, tagged by where it was put; a quest relic,
    // tagged by what it is.
    auto [beam, beam_prop] = world_item(harness, beam_item);
    auto [relic, relic_prop] = world_item(harness, relic_item);
    require(harness.item(beam).drop_tag == KERNEL_DROP_TAG_MAP_WEAPON);
    require(harness.item(relic).drop_tag == KERNEL_DROP_TAG_QUEST);

    // Picked up, both keep their tags. The beam rifle shares the sky laser's
    // category, so the (untagged) sky laser goes to the feet.
    harness.stand_at(harness.state_of(beam_prop).position);
    require(harness.request(KernelDomainAction_Pickup, beam, beam_prop).status ==
            KernelGameplayRequestStatus_Committed);
    harness.stand_at(harness.state_of(relic_prop).position);
    require(harness.request(KernelDomainAction_Pickup, relic, relic_prop).status ==
            KernelGameplayRequestStatus_Committed);
    require(harness.holds(weapons, beam));
    require(harness.holds(items, relic));
    require(harness.item(beam).drop_tag == KERNEL_DROP_TAG_MAP_WEAPON);
    require(harness.item(relic).drop_tag == KERNEL_DROP_TAG_QUEST);
    require((harness.weapons() == std::vector<std::uint32_t>{0u, 13u, 5u, 14u}));

    // Death (K11): the tagged go to the ground round the body, as
    // themselves; the untagged stay for the respawn to replace. The player
    // dies in mid-air -- lifted 6 m and killed before it falls -- and the
    // drops still land on the ground, not where it hung.
    const std::size_t items_before_death = harness.slots(items).size();
    KernelVec3 death_point = harness.state_of(harness.player).position;
    death_point.y += 6.0f;
    {
        const KernelQuat rotation{0.0f, 0.0f, 0.0f, 1.0f};
        require(Kernel_ServerSetEntityTransform(kernel, harness.player, &death_point, &rotation));
    }
    require(Kernel_ServerSetEntityHealth(kernel, harness.player, 0u));
    KernelEvent died{};
    died.type = KernelEventType_EntityDied;
    died.net_id = harness.player;
    server.handle_event(died);
    KernelItemInstanceView beam_view = harness.item(beam);
    KernelItemInstanceView relic_view = harness.item(relic);
    require(beam_view.residency == KernelItemResidency_World);
    require(relic_view.residency == KernelItemResidency_World);
    require(beam_view.drop_tag == KERNEL_DROP_TAG_MAP_WEAPON);
    require(relic_view.drop_tag == KERNEL_DROP_TAG_QUEST);
    require(!harness.holds(weapons, beam));
    require(!harness.holds(items, relic));
    // The control: everything untagged is still on the body -- one item fewer
    // (the relic), and the default weapons other than the map one.
    require(harness.slots(items).size() == items_before_death - 1u);
    require((harness.weapons() == std::vector<std::uint32_t>{0u, 13u, 14u}));
    {
        const KernelServerEntityState lying_beam = harness.state_of(beam_view.prop_entity_id);
        const KernelServerEntityState lying_relic = harness.state_of(relic_view.prop_entity_id);
        for (const KernelServerEntityState* lying : {&lying_beam, &lying_relic}) {
            require(std::hypot(lying->position.x - death_point.x,
                               lying->position.z - death_point.z) < 1.5f);
            require(lying->position.y < 1.0f);
        }
        // Spread, not stacked.
        require(std::hypot(lying_beam.position.x - lying_relic.position.x,
                           lying_beam.position.z - lying_relic.position.z) > 1.0f);
    }

    // The respawn reapplies the default loadout over what is left.
    harness.step(static_cast<int>(std::lround(config.player.respawn.delay_seconds * 30.0f)) + 5);
    require(harness.state_of(harness.player).hp != 0u);
    require((harness.weapons() == std::vector<std::uint32_t>{0u, 13u, 15u, 14u}));
    require(harness.item(beam).residency == KernelItemResidency_World);
    require(harness.item(relic).residency == KernelItemResidency_World);

    // Both can be picked back up, still tagged; the beam rifle swaps the sky
    // laser out again.
    harness.stand_at(harness.state_of(relic_view.prop_entity_id).position);
    require(harness.request(KernelDomainAction_Pickup, relic, relic_view.prop_entity_id).status ==
            KernelGameplayRequestStatus_Committed);
    require(harness.holds(items, relic));
    require(harness.item(relic).drop_tag == KERNEL_DROP_TAG_QUEST);

    // A camp pick that leaves category 2 free keeps the map weapon.
    harness.stand_at(harness.state_of(beam_view.prop_entity_id).position);
    require(harness.request(KernelDomainAction_Pickup, beam, beam_view.prop_entity_id).status ==
            KernelGameplayRequestStatus_Committed);
    std::uint32_t camp = 0;
    {
        std::vector<KernelServerEntityState> props(512);
        for (KernelServerEntityState& state : props) state.struct_size = sizeof(state);
        const std::uint32_t count = Kernel_ServerQueryEntities(
            kernel, KernelEntityType_Prop, props.data(), static_cast<std::uint32_t>(props.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            if (props[index].entity_template_id == camp_template->actor_template_id) {
                camp = props[index].net_id;
            }
        }
    }
    require(camp != 0u);
    KernelServerEntityState camp_state = harness.state_of(camp);
    camp_state.position.x += 2.0f;
    harness.stand_at(camp_state.position);
    // Picks are made from inside the camp, like any building's UI.
    const KernelVec3 entry = harness.state_of(harness.player).position;
    require(harness.request(KernelDomainAction_Activate, 0u, camp, 0u).status ==
            KernelGameplayRequestStatus_Committed);
    harness.step(4);
    auto [applied, weapon_count] = harness.select(camp, {0}, {pick_rifle});
    require(applied == GAME_SERVER_LOADOUT_RESULT_APPLIED);
    require(weapon_count == 1u);
    require((harness.weapons() == std::vector<std::uint32_t>{0u, 5u}));
    require(harness.holds(weapons, beam));
    require(harness.holds(items, relic));

    // A pick that wants category 2 puts the map weapon at the feet, tag and all.
    auto [applied_sky, sky_count] = harness.select(camp, {0}, {pick_rifle, pick_sky});
    require(applied_sky == GAME_SERVER_LOADOUT_RESULT_APPLIED);
    require(sky_count == 2u);
    require((harness.weapons() == std::vector<std::uint32_t>{0u, 15u}));
    beam_view = harness.item(beam);
    require(beam_view.residency == KernelItemResidency_World);
    require(beam_view.drop_tag == KERNEL_DROP_TAG_MAP_WEAPON);
    {
        const KernelServerEntityState camp_now = harness.state_of(camp);
        const KernelServerEntityState lying = harness.state_of(beam_view.prop_entity_id);
        // At the feet: inside the camp the player stands at its centre, so
        // the feet are where it went in, outside -- not in the camp.
        require(std::hypot(lying.position.x - entry.x, lying.position.z - entry.z) < 1.5f);
        require(std::hypot(lying.position.x - camp_now.position.x,
                           lying.position.z - camp_now.position.z) > 1.0f);
    }
    require(harness.holds(items, relic));
    // Back out of the camp: inside, nothing else may be done.
    require(harness.request(KernelDomainAction_Activate, 0u, camp, 0u).status ==
            KernelGameplayRequestStatus_Committed);
    harness.step(4);

    // Fungible stacks merge only on an equal tag. Merging happens on pickup,
    // so each potion goes down and comes back up. The loadout gave an untagged
    // stack of 3 (option 0).
    const auto potion_stacks = [&]() {
        std::vector<KernelItemInstanceView> found;
        for (const KernelItemInstanceView& view : harness.slots(items)) {
            if (view.item_template_id == potion_item) found.push_back(view);
        }
        return found;
    };
    require(potion_stacks().size() == 1u);
    const KernelItemInstanceId loadout_stack = potion_stacks()[0].item_instance_id;
    require(potion_stacks()[0].quantity == 3u);
    const auto down_and_up = [&](KernelItemInstanceId id) {
        const KernelServerEntityState me = harness.state_of(harness.player);
        const KernelVec3 at{me.position.x - 2.5f, me.position.y + 0.1f, me.position.z};
        std::uint32_t prop = 0;
        require(Kernel_ServerDropInventoryItem(kernel, id, &at, &prop));
        require(harness.item(id).residency == KernelItemResidency_World);
        harness.step(2);
        harness.stand_at(harness.state_of(prop).position);
        require(harness.request(KernelDomainAction_Pickup, id, prop).status ==
                KernelGameplayRequestStatus_Committed);
    };
    // The control: an untagged potion merges into the untagged stack.
    KernelItemInstanceId plain = 0;
    require(Kernel_ServerCreateInventoryItem(kernel, potion_item, 1, items, &plain));
    down_and_up(plain);
    require(potion_stacks().size() == 1u);
    require(harness.item(loadout_stack).quantity == 4u);
    // A tagged one does not: it comes back as its own stack, tag kept.
    KernelItemInstanceId tagged = 0;
    require(Kernel_ServerCreateInventoryItem(kernel, potion_item, 1, items, &tagged));
    require(Kernel_ServerSetItemDropTag(kernel, tagged, KERNEL_DROP_TAG_QUEST));
    down_and_up(tagged);
    require(potion_stacks().size() == 2u);
    require(harness.item(loadout_stack).quantity == 4u);
    require(harness.item(tagged).quantity == 1u);
    require(harness.item(tagged).drop_tag == KERNEL_DROP_TAG_QUEST);

    // A split keeps its source's tag: throw one potion off a tagged stack of 2.
    KernelItemInstanceId pair = 0;
    require(Kernel_ServerCreateInventoryItem(kernel, potion_item, 2, items, &pair));
    require(Kernel_ServerSetItemDropTag(kernel, pair, KERNEL_DROP_TAG_QUEST));
    {
        const KernelGameplayRequestOutcome thrown = harness.request(
            KernelDomainAction_Throw, pair, 0u, 1u, KernelVec3{},
            KernelVec3{0.0f, -0.5f, 1.0f});
        require(thrown.status == KernelGameplayRequestStatus_Committed);
        require(thrown.item_instance_id != pair);
        require(harness.item(thrown.item_instance_id).residency == KernelItemResidency_World);
        require(harness.item(thrown.item_instance_id).drop_tag == KERNEL_DROP_TAG_QUEST);
        require(harness.item(pair).quantity == 1u);
    }

    // The relic is still there, through all of it.
    require(harness.holds(items, relic));

    Kernel_Destroy(kernel);
    std::puts("drop_tag_test passed");
    return 0;
}
