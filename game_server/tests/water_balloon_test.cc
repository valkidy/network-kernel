// fungible_water_balloon end to end (docs/WATER_BUBBLE_PLAN.md, P6): a player
// throws the shipped balloon over the shipped ground plane, through the real
// throw request, and watches what the bubble does to each kind of target.
//
//   - a gingerbread (character controller): bubbled, held off the ground,
//     refused a velocity write -- what every AI controller ships through --
//     floating up for the status's 90 ticks, then dropped back to the ground,
//     after which it can be moved again;
//   - a beam drone (hover at 9 m): bubbled, floated, dropped all the way to
//     the ground, and back at its height a few seconds later;
//   - the hive airship (hover at 14 m, impulse_resistance 10): hit -- the
//     balloon bursts on it -- but never bubbled, because the balloon's
//     strength is 10 and the rule is strictly greater.
//
// No game_server AI runs here, so nothing moves a unit but the kernel.

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
constexpr std::uint32_t kBubbleTicks = 90;
constexpr float kThrowSpeed = 24.0f;  // grenade_shell
constexpr float kGravity = 9.81f;

fs::path runfiles_catalog() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace / "game_server" / "gameplay_catalog";
}

std::vector<std::uint8_t> read_ground_scene() {
    const fs::path path =
        runfiles_catalog() / "mesh_assets" / "jolt" / "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::uint32_t template_id(const gs::GameServerGameplayConfig& config, const char* name) {
    for (const auto& entity : config.entity_templates) {
        if (entity.name == name) return entity.actor_template_id;
    }
    require(false);
    return 0;
}

std::uint32_t item_id(const gs::GameServerGameplayConfig& config, const char* name) {
    for (const auto& item : config.item_templates) {
        if (item.name == name) return item.definition.item_template_id;
    }
    require(false);
    return 0;
}

KernelServerEntityState entity_state(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    if (!Kernel_ServerGetEntityState(kernel, net_id, &state)) {
        state.net_id = 0;
    }
    return state;
}

bool bubbled(KernelHandle* kernel, std::uint32_t net_id, std::uint32_t status_id) {
    std::vector<KernelStatusEffectView> effects(8);
    for (KernelStatusEffectView& effect : effects) effect.struct_size = sizeof(effect);
    const std::uint32_t count = Kernel_QueryStatusEffects(
        kernel, net_id, effects.data(), static_cast<std::uint32_t>(effects.size()));
    for (std::uint32_t index = 0; index < count && index < effects.size(); ++index) {
        if (effects[index].status_effect_id == status_id) return true;
    }
    return false;
}

bool suspended_flag(KernelHandle* kernel, std::uint32_t net_id) {
    return (entity_state(kernel, net_id).visual_flags & KERNEL_VISUAL_FLAG_SUSPENDED) != 0u;
}

// The direction that lobs a grenade_shell from `from` onto `to`: the flatter
// of the two solutions of the projectile equation.
KernelVec3 lob(const KernelVec3& from, const KernelVec3& to) {
    const float dx = to.x - from.x;
    const float dz = to.z - from.z;
    const float d = std::sqrt(dx * dx + dz * dz);
    const float h = to.y - from.y;
    const float v2 = kThrowSpeed * kThrowSpeed;
    const float root = v2 * v2 - kGravity * (kGravity * d * d + 2.0f * h * v2);
    require(root >= 0.0f);
    const float angle = std::atan((v2 - std::sqrt(root)) / (kGravity * d));
    return KernelVec3{
        std::cos(angle) * dx / d, std::sin(angle), std::cos(angle) * dz / d};
}

struct Arena {
    KernelHandle* kernel = nullptr;
    std::uint32_t thrower = 0;
    std::uint32_t target = 0;
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

    KernelGameplayRequestOutcome throw_at(const KernelVec3& direction) {
        KernelGameplayRequest request{};
        request.struct_size = sizeof(request);
        request.requester_peer = kThrowerPeer;
        request.request_id = next_request++;
        request.instigator_net_id = thrower;
        request.domain_action = KernelDomainAction_Throw;
        request.selected_item_instance_id = stack;
        request.requested_quantity = 1;
        request.throw_direction = direction;
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
    const char* target_template,
    const KernelVec3& target_at,
    std::uint16_t port) {
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 2048;
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
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = gs::kActorTypePlayer;
    create.entity_template_id = player_template;
    create.actor_template_id = player_template;
    create.owner_peer = kThrowerPeer;
    create.position = KernelVec3{0.0f, 1.0f, 0.0f};
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerCreateEntity(arena->kernel, &create, &arena->thrower));
    require(Kernel_ServerSetEntityActorTemplate(arena->kernel, arena->thrower, player_template));

    const std::uint32_t target_template_id = template_id(config, target_template);
    KernelServerEntityCreateInfo agent{};
    agent.struct_size = sizeof(agent);
    agent.entity_type = KernelEntityType_Actor;
    agent.actor_type = KernelActorType_Agent;
    agent.entity_template_id = target_template_id;
    agent.actor_template_id = target_template_id;
    agent.position = target_at;
    agent.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerCreateEntity(arena->kernel, &agent, &arena->target));
    require(Kernel_ServerSetEntityActorTemplate(
        arena->kernel, arena->target, target_template_id));

    KernelInventoryContainerId container = 0;
    require(Kernel_ServerCreateInventoryContainer(arena->kernel, arena->thrower, 8, &container));
    require(Kernel_ServerCreateInventoryItem(
        arena->kernel, item_id(config, "fungible_water_balloon"), 3, container,
        &arena->stack));
    // Settle everyone: onto the ground, or up to hover height.
    arena->tick(150);
}

// Throws at the target's body and waits for the balloon to burst. Returns
// the tick it burst on, counting from the throw, or -1; *last_seen is where
// the balloon was on the tick before.
int throw_and_wait_for_burst(
    Arena* arena, float aim_height, KernelVec3* last_seen = nullptr) {
    const KernelServerEntityState thrower = entity_state(arena->kernel, arena->thrower);
    KernelVec3 at = entity_state(arena->kernel, arena->target).position;
    at.y += aim_height;
    const KernelVec3 from{thrower.position.x, thrower.position.y + 1.4f, thrower.position.z};
    const KernelGameplayRequestOutcome outcome = arena->throw_at(lob(from, at));
    require(outcome.status == KernelGameplayRequestStatus_Committed);
    for (int tick = 1; tick <= 60; ++tick) {
        arena->tick();
        const KernelServerEntityState prop =
            entity_state(arena->kernel, outcome.prop_entity_id);
        if (prop.net_id == 0) {
            return tick;
        }
        if (last_seen != nullptr) *last_seen = prop.position;
    }
    return -1;
}

void a_gingerbread_is_bubbled_floats_and_drops(
    const gs::GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene,
    std::uint32_t status_id) {
    Arena arena;
    open_arena(&arena, config, scene, "gingerbread", KernelVec3{5.0f, 0.0f, 0.0f}, 8090);
    const float ground = entity_state(arena.kernel, arena.target).position.y;
    // The control for the velocity refusal: before the bubble, a write lands.
    const KernelVec3 still{0.0f, 0.0f, 0.0f};
    require(Kernel_ServerSetEntityVelocity(arena.kernel, arena.target, &still));

    const int burst = throw_and_wait_for_burst(&arena, 1.0f);
    std::fprintf(stderr, "gingerbread: burst on tick %d\n", burst);
    require(burst > 0);
    require(bubbled(arena.kernel, arena.target, status_id));
    arena.tick();
    require(suspended_flag(arena.kernel, arena.target));
    // What every AI controller ships its movement through is refused.
    const KernelVec3 away{-5.0f, 0.0f, 0.0f};
    require(!Kernel_ServerSetEntityVelocity(arena.kernel, arena.target, &away));

    float top = ground;
    int bubble_left = 0;
    for (int tick = 0; tick < static_cast<int>(kBubbleTicks) + 5; ++tick) {
        arena.tick();
        top = std::max(top, entity_state(arena.kernel, arena.target).position.y);
        if (!bubbled(arena.kernel, arena.target, status_id) && bubble_left == 0) {
            bubble_left = tick;
        }
    }
    std::fprintf(stderr, "gingerbread: rose %.2f m, bubble left after %d ticks\n",
                 top - ground, bubble_left);
    // Ninety ticks at 1 m/s, less the ticks already spent: well over two metres.
    require(top - ground > 2.5f);
    require(bubble_left > 0);
    // Down again, and its own again.
    arena.tick(60);
    const KernelServerEntityState after = entity_state(arena.kernel, arena.target);
    std::fprintf(stderr, "gingerbread: back at %.2f\n", after.position.y);
    require(std::fabs(after.position.y - ground) < 0.2f);
    require((after.visual_flags & KERNEL_VISUAL_FLAG_SUSPENDED) == 0u);
    require(Kernel_ServerSetEntityVelocity(arena.kernel, arena.target, &still));
}

void a_drone_is_bubbled_dropped_and_climbs_back(
    const gs::GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene,
    std::uint32_t status_id) {
    Arena arena;
    open_arena(&arena, config, scene, "beam_drone", KernelVec3{5.0f, 9.0f, 0.0f}, 8091);
    const float hover = entity_state(arena.kernel, arena.target).position.y;
    std::fprintf(stderr, "drone: hovering at %.2f\n", hover);
    require(hover > 8.0f);

    const int burst = throw_and_wait_for_burst(&arena, 0.8f);
    std::fprintf(stderr, "drone: burst on tick %d\n", burst);
    require(burst > 0);
    require(bubbled(arena.kernel, arena.target, status_id));

    float top = hover;
    float bottom = hover;
    for (int tick = 0; tick < static_cast<int>(kBubbleTicks) + 60; ++tick) {
        arena.tick();
        const float y = entity_state(arena.kernel, arena.target).position.y;
        top = std::max(top, y);
        bottom = std::min(bottom, y);
    }
    std::fprintf(stderr, "drone: up to %.2f, down to %.2f\n", top, bottom);
    require(top > hover + 2.0f);
    // All the way to the ground, not to its hover height.
    require(bottom < 0.5f);
    // Then a hover again: back at its height at 3 m/s.
    arena.tick(150);
    const float back = entity_state(arena.kernel, arena.target).position.y;
    std::fprintf(stderr, "drone: back at %.2f\n", back);
    require(std::fabs(back - hover) < 0.2f);
}

void the_airship_is_too_heavy_for_it(
    const gs::GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene,
    std::uint32_t status_id) {
    Arena arena;
    open_arena(&arena, config, scene, "hive_airship", KernelVec3{8.0f, 14.0f, 0.0f}, 8092);
    const float hover = entity_state(arena.kernel, arena.target).position.y;
    std::fprintf(stderr, "airship: hovering at %.2f\n", hover);

    KernelVec3 last_seen{};
    const int burst = throw_and_wait_for_burst(&arena, 1.0f, &last_seen);
    std::fprintf(stderr, "airship: burst on tick %d, last seen at (%.2f, %.2f, %.2f)\n",
                 burst, last_seen.x, last_seen.y, last_seen.z);
    // It was hit: the balloon burst up at the airship, not on the ground 14 m
    // below -- and nothing came of it.
    require(burst > 0);
    require(last_seen.y > hover - 2.0f);
    for (int tick = 0; tick < 30; ++tick) {
        arena.tick();
        require(!bubbled(arena.kernel, arena.target, status_id));
        require(!suspended_flag(arena.kernel, arena.target));
    }
    require(std::fabs(entity_state(arena.kernel, arena.target).position.y - hover) < 0.2f);
}

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    const std::vector<std::uint8_t> scene = read_ground_scene();
    std::uint32_t status_id = 0;
    for (const auto& status : config.status_effect_templates) {
        if (status.name == "water_bubble") status_id = status.status_effect_id;
    }
    require(status_id != 0u);

    a_gingerbread_is_bubbled_floats_and_drops(config, scene, status_id);
    a_drone_is_bubbled_dropped_and_climbs_back(config, scene, status_id);
    the_airship_is_too_heavy_for_it(config, scene, status_id);
    std::puts("water_balloon_test passed");
    return 0;
}
