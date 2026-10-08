// The Water Bubble Staff end to end: a player armed with the shipped staff
// (weapon 17, item 3027) through a weapon container, as a loadout pick arms
// one, fires its bolt (projectile 30) through the real input path.
//
//   - into the ground first: the charge is spent, and nobody is bubbled --
//     an impact with no actor names no target, and the graph's
//     `when: event.has_target` lets it pass;
//   - then level at a gingerbread 6 m away: bubbled by the bolt's on_impact,
//     carried up for the status's 90 ticks, and dropped back down.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
constexpr std::uint8_t kStaff = 17;

fs::path runfiles_catalog() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace / "game_server" / "gameplay_catalog";
}

std::vector<std::uint8_t> read_ground_scene() {
    std::ifstream file(
        runfiles_catalog() / "mesh_assets" / "jolt" / "plane_200x200.joltmesh",
        std::ios::binary);
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

bool has_status(KernelHandle* kernel, std::uint32_t net_id, std::uint32_t status_id) {
    std::vector<KernelStatusEffectView> effects(8);
    for (KernelStatusEffectView& effect : effects) effect.struct_size = sizeof(effect);
    const std::uint32_t count = Kernel_QueryStatusEffects(
        kernel, net_id, effects.data(), static_cast<std::uint32_t>(effects.size()));
    for (std::uint32_t index = 0; index < count && index < effects.size(); ++index) {
        if (effects[index].status_effect_id == status_id) return true;
    }
    return false;
}

std::uint16_t staff_ammo(const KernelServerEntityState& state) {
    for (std::uint32_t slot = 0; slot < state.weapon_slot_count; ++slot) {
        if (state.weapon_ids[slot] == kStaff) return state.ammo[slot];
    }
    require(false);
    return 0;
}

struct Arena {
    KernelHandle* kernel = nullptr;
    std::uint32_t player = 0;
    std::uint32_t target = 0;
    std::uint32_t next_seq = 1;
    std::uint32_t next_action = 1;

    ~Arena() {
        if (kernel != nullptr) Kernel_Destroy(kernel);
    }

    void tick(int count = 1) {
        for (int index = 0; index < count; ++index) Kernel_Update(kernel, kTickSeconds);
    }

    void send(const KernelVec3& aim, std::uint32_t action, bool start, bool held) {
        KernelPlayerInput input{};
        input.input_seq = next_seq++;
        input.aim_dir = aim;
        input.selected_weapon = kStaff;
        if (action != 0u) {
            if (start) {
                input.action_intent =
                    KernelActionIntent{action, KernelActionBinding_PrimaryFire, 0u, 0u};
            }
            input.action_input =
                KernelActionInput{action, static_cast<std::uint8_t>(held ? 1u : 0u), 0u, 0u};
        }
        require(Kernel_ServerSubmitEntityInput(kernel, player, &input));
        tick();
    }

    void idle(const KernelVec3& aim, int ticks) {
        for (int index = 0; index < ticks; ++index) send(aim, 0u, false, false);
    }

    // One press of the trigger, then a hold until the cast commits.
    void cast(const KernelVec3& aim) {
        const std::uint32_t action = 9100u + next_action++;
        send(aim, action, true, true);
        for (int index = 0; index < 10; ++index) send(aim, action, false, true);
        send(aim, action, false, false);
    }
};

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    std::uint32_t status_id = 0;
    for (const auto& status : config.status_effect_templates) {
        if (status.name == "water_bubble") status_id = status.status_effect_id;
    }
    std::uint32_t staff_item = 0;
    for (const auto& item : config.item_templates) {
        if (item.name == "stateful_weapon_water_bubble_staff") {
            staff_item = item.definition.item_template_id;
        }
    }
    std::uint32_t gingerbread = 0;
    for (const auto& entity : config.entity_templates) {
        if (entity.name == "gingerbread") gingerbread = entity.actor_template_id;
    }
    require(status_id != 0u && staff_item != 0u && gingerbread != 0u);
    require(config.weapons.configured[kStaff]);

    const std::vector<std::uint8_t> scene = read_ground_scene();
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 2048;
    kernel_config.max_render_states = 64;
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
    require(Kernel_StartDedicatedServer(arena.kernel, 8093));

    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = gs::kActorTypePlayer;
    create.entity_template_id = config.player.actor_template_id;
    create.actor_template_id = config.player.actor_template_id;
    create.position = KernelVec3{0.0f, 1.0f, 0.0f};
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerCreateEntity(arena.kernel, &create, &arena.player));
    require(Kernel_ServerSetEntityActorTemplate(
        arena.kernel, arena.player, config.player.actor_template_id));
    KernelCombatStateDefinition combat = gs::make_player_combat_state(config);
    require(Kernel_ServerSetEntityCombatState(arena.kernel, arena.player, &combat));
    KernelWeaponMechanicsDefinition mechanics = config.weapons.definitions[kStaff];
    require(Kernel_ServerSetEntityWeaponMechanics(arena.kernel, arena.player, &mechanics));
    KernelInventoryContainerId weapons = 0;
    require(Kernel_ServerCreateWeaponContainer(arena.kernel, arena.player, &weapons));
    KernelItemInstanceId staff = 0;
    require(Kernel_ServerCreateInventoryItem(arena.kernel, staff_item, 1, weapons, &staff));

    KernelServerEntityCreateInfo agent{};
    agent.struct_size = sizeof(agent);
    agent.entity_type = KernelEntityType_Actor;
    agent.actor_type = KernelActorType_Agent;
    agent.entity_template_id = gingerbread;
    agent.actor_template_id = gingerbread;
    agent.position = KernelVec3{6.0f, 0.0f, 0.0f};
    agent.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerCreateEntity(arena.kernel, &agent, &arena.target));
    require(Kernel_ServerSetEntityActorTemplate(arena.kernel, arena.target, gingerbread));

    const KernelVec3 at_target{1.0f, 0.0f, 0.0f};
    arena.idle(at_target, 60);
    const KernelServerEntityState me = entity_state(arena.kernel, arena.player);
    require(me.weapon_ids[me.active_weapon_slot] == kStaff);
    const std::uint16_t full = staff_ammo(me);
    require(full == 3u);
    const float ground = entity_state(arena.kernel, arena.target).position.y;

    // Into the ground: spent, and nobody bubbled.
    arena.cast(KernelVec3{1.0f, -1.5f, 0.0f});
    arena.idle(at_target, 30);
    require(staff_ammo(entity_state(arena.kernel, arena.player)) == full - 1u);
    require(!has_status(arena.kernel, arena.target, status_id));
    require(!has_status(arena.kernel, arena.player, status_id));

    // At the gingerbread.
    arena.cast(at_target);
    int bubbled_at = -1;
    for (int tick = 0; tick < 30 && bubbled_at < 0; ++tick) {
        arena.idle(at_target, 1);
        if (has_status(arena.kernel, arena.target, status_id)) bubbled_at = tick;
    }
    std::fprintf(stderr, "staff: bubbled %d ticks after the cast\n", bubbled_at);
    require(bubbled_at >= 0);
    require(staff_ammo(entity_state(arena.kernel, arena.player)) == full - 2u);
    require(!has_status(arena.kernel, arena.player, status_id));
    float top = ground;
    for (int tick = 0; tick < 95; ++tick) {
        arena.idle(at_target, 1);
        top = std::max(top, entity_state(arena.kernel, arena.target).position.y);
    }
    std::fprintf(stderr, "staff: rose %.2f m\n", top - ground);
    require(top - ground > 2.5f);
    arena.idle(at_target, 60);
    const KernelServerEntityState after = entity_state(arena.kernel, arena.target);
    require(!has_status(arena.kernel, arena.target, status_id));
    require(std::fabs(after.position.y - ground) < 0.2f);

    std::puts("water_bubble_staff_test passed");
    return 0;
}
