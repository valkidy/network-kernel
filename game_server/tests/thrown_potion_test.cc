// A thrown fungible_potion (design #5, 2026-10-07): hitting an actor heals it
// and uses the potion up; landing leaves a potion on the ground that can be
// picked up again.
//
// The shipped catalog does this with no kernel change. Prop 218 `potion`
// binds on_collision {actor | terrain | static_obstacle} to
// action_heal_target_and_consume_self_at_collision, whose two actions are both
// `when: event.has_target` -- an actor contact has a target, a landing has
// none.
//
// Measured before that graph existed, on main 78d9350 (P0, 12795b3):
//
//   - With no on_collision at all the potion never landed: a thrown prop is
//     swept against the world only through that binding, so it fell through
//     the ground for good (y < -40), could not be picked up, and the item was
//     lost. The catalog now refuses that authoring; the first case below
//     checks the refusal.
//   - With a plain heal-the-target graph, a hit healed the ally but the potion
//     flew on through it, landed and could be picked up again -- a reusable
//     heal. The self-damage now ends it on the hit.
//
// Two throws over the shipped ground plane: level at a wounded player 4 m
// away, and 40 degrees down with nobody near, then a pickup. The thrower is
// wounded too, so a heal landing on the wrong actor shows.

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

const char* const kPlainPotionProp = R"(id: 218
name: potion
entity_type: prop
physics:
  collider_template: collision_damage_prop_hitbox
)";

// The shipped catalog with the potion's prop overwritten. Returns the load
// error, or "" with *out filled.
std::string load_with_potion_prop(
    const std::string& name,
    const char* prop_yaml,
    gs::GameServerGameplayConfig* out) {
    const char* tmp = std::getenv("TEST_TMPDIR");
    require(tmp != nullptr);
    const fs::path root = fs::path(tmp) / ("catalog_" + name);
    fs::remove_all(root);
    fs::copy(runfiles_catalog(), root, fs::copy_options::recursive);
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
    std::uint8_t item_residency_at_end = 0;
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
    flight.item_residency_at_end = item_view(arena->kernel, flight.item).residency;
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
    // Without terrain in on_collision a throw never lands: refused at load.
    gs::GameServerGameplayConfig refused;
    const std::string error = load_with_potion_prop("plain", kPlainPotionProp, &refused);
    std::fprintf(stderr, "plain prop load: %s\n", error.c_str());
    require(error.find("never lands") != std::string::npos);

    const gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    const std::vector<std::uint8_t> scene = read_ground_scene();
    const VariantResult shipped = run_variant("shipped", config, scene, 8052);

    // At the ally: healed once, and the potion is used up on the hit -- it
    // does not fly on and land.
    require(shipped.at_ally.throw_outcome.status == KernelGameplayRequestStatus_Committed);
    require(shipped.at_ally.ally_changed_tick > 0);
    require(shipped.at_ally.ally_hp == kWoundedHp + 30u);
    require(shipped.at_ally.placed_tick < 0);
    require(!shipped.at_ally.prop_alive_at_end);
    require(shipped.at_ally.item_residency_at_end == KernelItemResidency_Terminal);
    require(shipped.at_ally.thrower_hp == kWoundedHp);

    // At the ground: lands, heals nobody, stays a potion, and goes back into
    // the stack.
    require(shipped.at_ground.throw_outcome.status == KernelGameplayRequestStatus_Committed);
    require(shipped.at_ground.placed_tick > 0);
    require(std::fabs(shipped.at_ground.placed_at.y) < 0.5f);
    require(shipped.at_ground.prop_alive_at_end);
    require(shipped.at_ground.mode_at_end == KernelWorldItemMode_Placed);
    require(shipped.at_ground.thrower_hp == kWoundedHp);
    require(shipped.stack_quantity_after_throw == 1u);
    require(shipped.pickup.status == KernelGameplayRequestStatus_Committed);
    require(shipped.stack_quantity_after_pickup == 2u);

    std::puts("thrown_potion_test passed");
    return 0;
}
