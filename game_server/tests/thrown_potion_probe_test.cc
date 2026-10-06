// What a fungible_potion does when it is authored throwable, today.
//
// The design ask (2026-10-07): a thrown potion that lands should be a potion on
// the ground that can be picked back up; one that hits an actor should heal it.
// This pins what the current runtime actually does with the two obvious ways
// of authoring that, so the kernel change it needs can be scoped against
// measured behaviour instead of a reading of the code.
//
// The shipped catalog is copied and two files are overwritten: the item gets
// [consumable, pickupable, throwable] and an item-backed prop, as the shipped
// bottles have. The prop comes in two variants:
//
//   plain  -- no triggers at all.
//   heal   -- on_collision {actor | terrain | static_obstacle} heals
//             event.target by 30, the natural first try at "heal on hit".
//
// Each variant runs two throws over the shipped ground plane:
//
//   at_ally   -- level, at a wounded player 4 m away.
//   at_ground -- 40 degrees down, nobody near, then the thrower picks it up.
//
// The thrower is wounded too, so a heal landing on the wrong actor shows.
//
// Measured 2026-10-07 on main (78d9350); the requires below pin it:
//
//   Both variants load. The item side accepts the triple as authored.
//
//   plain -- the potion never lands. A thrown prop is swept against the world
//     only if it has an on_collision binding, so this one falls through the
//     ground plane and keeps falling (y < -40 after 2-3 s), InFlight forever.
//     It cannot be picked up (InvalidContext) and the item is lost.
//
//   heal  -- hitting the ally heals it (50 -> 80) and the potion flies on
//     through it, lands on the ground past it, and can be picked up again.
//     That is a reusable heal: throw, walk over, pick up, throw. A throw at
//     the ground lands, fires the same graph with target 0 (nothing to heal),
//     and picks back up, merging into the stack. The thrower is never healed.
//
// What the design wants -- heal and be used up on an actor, stay a potion on
// the ground -- needs the kernel to tell those two contacts apart; one
// on_collision binding serves both today.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace {

namespace fs = std::filesystem;
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
constexpr std::uint32_t kThrowerPeer = 7;
constexpr std::uint32_t kAllyPeer = 8;
constexpr std::uint16_t kWoundedHp = 50;

fs::path runfiles_catalog() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace / "game_server" /
        "gameplay_catalog";
}

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::trunc);
    require(file.good());
    file << text;
}

const char* const kThrowablePotionItem = R"(id: 3002
name: fungible_potion
mode: fungible
max_stack: 5
capabilities: [consumable, pickupable, throwable]
entity_template: potion
world_interaction:
  range: 3.0
  line_of_sight_required: false
throw:
  mode: identity_preserving
  trajectory_projectile: grenade_shell
use:
  quantity_cost: 1
  cooldown_ticks: 0
  destroy_when_empty: true
triggers:
  on_item_used:
    action_graph: action_apply_health_change_at_item_used
    parameters:
      target: event.target
      amount: 30
)";

const char* const kPlainPotionProp = R"(id: 218
name: potion
entity_type: prop
physics:
  collider_template: collision_damage_prop_hitbox
)";

const char* const kHealPotionProp = R"(id: 218
name: potion
entity_type: prop
physics:
  collider_template: collision_damage_prop_hitbox
triggers:
  on_collision:
    collision_mask: actor | terrain | static_obstacle
    action_graph: action_apply_health_change_at_item_used
    parameters:
      target: event.target
      amount: 30
)";

// The shipped catalog with the potion swapped for a throwable one. Returns the
// load error, or "" with *out filled.
std::string load_variant(
    const std::string& name,
    const char* prop_yaml,
    gs::GameServerGameplayConfig* out) {
    const char* tmp = std::getenv("TEST_TMPDIR");
    require(tmp != nullptr);
    const fs::path root = fs::path(tmp) / ("catalog_" + name);
    fs::remove_all(root);
    fs::copy(runfiles_catalog(), root, fs::copy_options::recursive);
    write_file(
        root / "item_templates" / "3002_fungible_potion.yaml",
        kThrowablePotionItem);
    write_file(root / "entity_templates" / "218_prop_potion.yaml", prop_yaml);
    try {
        *out = gs::load_gameplay_config_from_catalog_file(
            (root / "gameplay_catalog.yaml").string());
    } catch (const std::exception& error) {
        return error.what();
    }
    return "";
}

std::vector<std::uint8_t> read_ground_scene() {
    const fs::path path =
        runfiles_catalog() / "mesh_assets" / "jolt" / "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::uint32_t spawn_player(
    KernelHandle* kernel,
    std::uint32_t template_id,
    std::uint32_t peer,
    const KernelVec3& position) {
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = gs::kActorTypePlayer;
    create.entity_template_id = template_id;
    create.actor_template_id = template_id;
    create.owner_peer = peer;
    create.position = position;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
    require(net_id != 0);
    require(Kernel_ServerSetEntityActorTemplate(kernel, net_id, template_id));
    return net_id;
}

KernelServerEntityState entity_state(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    if (!Kernel_ServerGetEntityState(kernel, net_id, &state)) {
        state.net_id = 0;
    }
    return state;
}

KernelItemInstanceView item_view(KernelHandle* kernel, KernelItemInstanceId id) {
    KernelItemInstanceView view{};
    view.struct_size = sizeof(view);
    require(Kernel_GetItemInstance(kernel, id, &view));
    return view;
}

struct Arena {
    KernelHandle* kernel = nullptr;
    std::uint32_t thrower = 0;
    std::uint32_t ally = 0;
    KernelInventoryContainerId container = 0;
    KernelItemInstanceId stack = 0;
    std::uint64_t next_request = 1;

    ~Arena() {
        if (kernel != nullptr) Kernel_Destroy(kernel);
    }

    void tick(int count = 1) {
        for (int index = 0; index < count; ++index) {
            Kernel_Update(kernel, kTickSeconds);
        }
    }

    KernelGameplayRequestOutcome submit(KernelGameplayRequest request) {
        request.struct_size = sizeof(request);
        request.requester_peer = kThrowerPeer;
        request.request_id = next_request++;
        request.instigator_net_id = thrower;
        require(Kernel_ServerSubmitGameplayRequest(kernel, &request));
        KernelGameplayRequestOutcome outcome{};
        outcome.struct_size = sizeof(outcome);
        require(Kernel_PollGameplayRequestOutcomes(kernel, &outcome, 1) == 1u);
        return outcome;
    }
};

void open_arena(
    Arena* arena,
    const gs::GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene,
    bool with_ally,
    std::uint16_t port) {
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 1024;
    kernel_config.max_render_states = 64;
    arena->kernel = Kernel_Create(&kernel_config);
    require(arena->kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(arena->kernel, config));
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(arena->kernel, &scene_config));
    require(Kernel_StartDedicatedServer(arena->kernel, port));

    const std::uint32_t player_template = config.player.actor_template_id;
    arena->thrower = spawn_player(
        arena->kernel, player_template, kThrowerPeer, KernelVec3{0.0f, 1.0f, 0.0f});
    if (with_ally) {
        arena->ally = spawn_player(
            arena->kernel, player_template, kAllyPeer, KernelVec3{4.0f, 1.0f, 0.0f});
    }
    require(Kernel_ServerCreateInventoryContainer(
        arena->kernel, arena->thrower, 8, &arena->container));
    require(Kernel_ServerCreateInventoryItem(
        arena->kernel, 3002u, 2, arena->container, &arena->stack));
    // Settle everyone onto the ground before anything is measured.
    arena->tick(30);
    require(Kernel_ServerSetEntityHealth(arena->kernel, arena->thrower, kWoundedHp));
    if (with_ally) {
        require(Kernel_ServerSetEntityHealth(arena->kernel, arena->ally, kWoundedHp));
    }
}

struct Flight {
    KernelGameplayRequestOutcome throw_outcome{};
    std::uint32_t prop = 0;
    KernelItemInstanceId item = 0;
    // First tick the ally's hp changed, and the prop's state on that tick.
    int ally_changed_tick = -1;
    bool prop_alive_after_ally_hit = false;
    // First tick the prop read Placed, and where.
    int placed_tick = -1;
    KernelVec3 placed_at{};
    bool prop_alive_at_end = false;
    std::uint8_t mode_at_end = 0;
    KernelVec3 end_position{};
    std::uint16_t thrower_hp = 0;
    std::uint16_t ally_hp = 0;
};

Flight throw_and_watch(Arena* arena, const KernelVec3& direction, int ticks) {
    KernelGameplayRequest request{};
    request.domain_action = KernelDomainAction_Throw;
    request.selected_item_instance_id = arena->stack;
    request.requested_quantity = 1;
    request.throw_direction = direction;
    Flight flight;
    flight.throw_outcome = arena->submit(request);
    flight.prop = flight.throw_outcome.prop_entity_id;
    flight.item = flight.throw_outcome.item_instance_id;
    if (flight.throw_outcome.status != KernelGameplayRequestStatus_Committed) {
        return flight;
    }
    std::uint16_t ally_hp = arena->ally == 0
        ? 0
        : entity_state(arena->kernel, arena->ally).hp;
    for (int tick = 1; tick <= ticks; ++tick) {
        arena->tick();
        const KernelServerEntityState prop = entity_state(arena->kernel, flight.prop);
        if (arena->ally != 0 && flight.ally_changed_tick < 0) {
            const std::uint16_t now = entity_state(arena->kernel, arena->ally).hp;
            if (now != ally_hp) {
                flight.ally_changed_tick = tick;
                flight.prop_alive_after_ally_hit = prop.net_id != 0;
            }
            ally_hp = now;
        }
        if (prop.net_id != 0 && flight.placed_tick < 0 &&
            prop.world_item_mode == KernelWorldItemMode_Placed) {
            flight.placed_tick = tick;
            flight.placed_at = prop.position;
        }
    }
    const KernelServerEntityState prop = entity_state(arena->kernel, flight.prop);
    flight.prop_alive_at_end = prop.net_id != 0;
    flight.mode_at_end = prop.world_item_mode;
    flight.end_position = prop.position;
    flight.thrower_hp = entity_state(arena->kernel, arena->thrower).hp;
    flight.ally_hp = arena->ally == 0 ? 0 : entity_state(arena->kernel, arena->ally).hp;
    return flight;
}

void print_flight(const char* label, const Flight& flight) {
    std::fprintf(
        stderr,
        "%s: throw status=%u reject=%u prop=%u | ally hp change at tick %d "
        "(prop alive then=%d) | placed at tick %d (%.2f, %.2f, %.2f) | "
        "end alive=%d mode=%u pos=(%.2f, %.2f, %.2f) | thrower hp=%u ally hp=%u\n",
        label,
        flight.throw_outcome.status,
        flight.throw_outcome.rejection_reason,
        flight.prop,
        flight.ally_changed_tick,
        flight.prop_alive_after_ally_hit ? 1 : 0,
        flight.placed_tick,
        flight.placed_at.x,
        flight.placed_at.y,
        flight.placed_at.z,
        flight.prop_alive_at_end ? 1 : 0,
        flight.mode_at_end,
        flight.end_position.x,
        flight.end_position.y,
        flight.end_position.z,
        flight.thrower_hp,
        flight.ally_hp);
}

struct VariantResult {
    Flight at_ally;
    Flight at_ground;
    KernelGameplayRequestOutcome pickup{};
    std::uint32_t stack_quantity_after_throw = 0;
    std::uint32_t stack_quantity_after_pickup = 0;
};

VariantResult run_variant(
    const char* name,
    const gs::GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene,
    std::uint16_t first_port) {
    VariantResult result;
    {
        Arena arena;
        open_arena(&arena, config, scene, true, first_port);
        result.at_ally = throw_and_watch(&arena, KernelVec3{1.0f, 0.0f, 0.0f}, 90);
        print_flight((std::string(name) + " at_ally").c_str(), result.at_ally);
    }
    {
        Arena arena;
        open_arena(&arena, config, scene, false, static_cast<std::uint16_t>(first_port + 1));
        const float down = 40.0f * 3.14159265f / 180.0f;
        result.at_ground = throw_and_watch(
            &arena, KernelVec3{std::cos(down), -std::sin(down), 0.0f}, 60);
        print_flight((std::string(name) + " at_ground").c_str(), result.at_ground);
        result.stack_quantity_after_throw = item_view(arena.kernel, arena.stack).quantity;
        if (result.at_ground.prop_alive_at_end) {
            KernelGameplayRequest pickup{};
            pickup.domain_action = KernelDomainAction_Pickup;
            pickup.selected_item_instance_id = result.at_ground.item;
            pickup.target_net_id = result.at_ground.prop;
            pickup.requested_quantity = 1;
            result.pickup = arena.submit(pickup);
            arena.tick();
        }
        result.stack_quantity_after_pickup = item_view(arena.kernel, arena.stack).quantity;
        std::fprintf(
            stderr,
            "%s pickup: status=%u reject=%u | stack %u after throw, %u after pickup\n",
            name,
            result.pickup.status,
            result.pickup.rejection_reason,
            result.stack_quantity_after_throw,
            result.stack_quantity_after_pickup);
    }
    return result;
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> scene = read_ground_scene();

    gs::GameServerGameplayConfig plain_config;
    const std::string plain_error = load_variant("plain", kPlainPotionProp, &plain_config);
    std::fprintf(stderr, "plain load: %s\n", plain_error.empty() ? "ok" : plain_error.c_str());
    gs::GameServerGameplayConfig heal_config;
    const std::string heal_error = load_variant("heal", kHealPotionProp, &heal_config);
    std::fprintf(stderr, "heal load: %s\n", heal_error.empty() ? "ok" : heal_error.c_str());

    require(plain_error.empty());
    require(heal_error.empty());

    const VariantResult plain = run_variant("plain", plain_config, scene, 8052);
    for (const Flight* flight : {&plain.at_ally, &plain.at_ground}) {
        require(flight->throw_outcome.status == KernelGameplayRequestStatus_Committed);
        // Never placed: it fell through the ground and is still falling.
        require(flight->placed_tick < 0);
        require(flight->prop_alive_at_end);
        require(flight->mode_at_end == KernelWorldItemMode_InFlight);
        require(flight->end_position.y < -10.0f);
        require(flight->thrower_hp == kWoundedHp);
    }
    require(plain.at_ally.ally_changed_tick < 0);
    require(plain.at_ally.ally_hp == kWoundedHp);
    require(plain.pickup.status == KernelGameplayRequestStatus_Rejected);
    require(plain.pickup.rejection_reason == KernelGameplayRequestRejection_InvalidContext);
    require(plain.stack_quantity_after_throw == 1u);
    require(plain.stack_quantity_after_pickup == 1u);

    const VariantResult heal = run_variant("heal", heal_config, scene, 8054);
    // At the ally: healed by the flight, and the potion survives it ...
    require(heal.at_ally.throw_outcome.status == KernelGameplayRequestStatus_Committed);
    require(heal.at_ally.ally_changed_tick > 0);
    require(heal.at_ally.ally_hp == kWoundedHp + 30u);
    require(heal.at_ally.prop_alive_after_ally_hit);
    // ... flies on through, and lands on the ground beyond the ally.
    require(heal.at_ally.placed_tick > heal.at_ally.ally_changed_tick);
    require(heal.at_ally.placed_at.x > 4.0f);
    require(std::fabs(heal.at_ally.placed_at.y) < 0.5f);
    require(heal.at_ally.prop_alive_at_end);
    require(heal.at_ally.mode_at_end == KernelWorldItemMode_Placed);
    require(heal.at_ally.thrower_hp == kWoundedHp);
    // At the ground: lands, heals nobody, and goes back into the stack.
    require(heal.at_ground.placed_tick > 0);
    require(std::fabs(heal.at_ground.placed_at.y) < 0.5f);
    require(heal.at_ground.thrower_hp == kWoundedHp);
    require(heal.stack_quantity_after_throw == 1u);
    require(heal.pickup.status == KernelGameplayRequestStatus_Committed);
    require(heal.stack_quantity_after_pickup == 2u);
    std::puts("thrown_potion_probe_test passed");
    return 0;
}
