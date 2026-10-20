// P5 of docs/PLAYER_HOVER_PLAN.md: the hover potion in the shipped catalog,
// end to end on a dedicated server -- the loader takes the status, its graph
// and the item; a player who drinks one rises 3 m, says HOVERING, and comes
// back down when the 30 s are up.

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
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}
#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr std::uint32_t kHoverStatus = 1006u;
constexpr std::uint32_t kHoverTicks = 900u;

std::filesystem::path catalog_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr && test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace / "game_server" /
        "gameplay_catalog";
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
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

std::uint32_t item_id(const gs::GameServerGameplayConfig& config, const std::string& name) {
    for (const gs::ItemTemplateConfig& item : config.item_templates) {
        if (item.name == name) return item.definition.item_template_id;
    }
    return 0u;
}

// The catalog: the status, its on_apply and the item, as authored.
void the_catalog_has_the_potion(const gs::GameServerGameplayConfig& config) {
    const gs::KernelGameplayCatalogStorage storage = gs::build_kernel_gameplay_catalog(config);
    const KernelStatusEffectDefinition* hover = nullptr;
    for (const KernelStatusEffectDefinition& status : storage.status_effects) {
        if (status.status_effect_id == kHoverStatus) hover = &status;
    }
    require(hover != nullptr);
    require(hover->duration_ticks == kHoverTicks);
    const KernelActionDefinition& action = hover->on_apply_trigger.actions[0];
    require(action.action_type == KernelEntityTriggerActionType_ApplyHoverMovement);
    require(action.target_source == KernelEntityRefSource_EventSubject);
    require(action.hover_height_meters == 3.0f);
    require(action.hover_vertical_speed == 4.0f);
    require(item_id(config, "fungible_hover_potion") == 3036u);
}

bool kernel_takes(const gs::KernelGameplayCatalogStorage& storage) {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    const bool loaded = Kernel_LoadGameplayCatalog(kernel, &storage.definition, nullptr);
    Kernel_Destroy(kernel);
    return loaded;
}

// An item's on_item_used may apply a status now (P5), on the terms an entity
// trigger's apply_status has: a status that exists, onto someone the event
// names. Edited where it lies, past the loader, so only the kernel decides.
void the_kernel_refuses_an_item_status_it_cannot_apply(
    const gs::GameServerGameplayConfig& config) {
    require(kernel_takes(gs::build_kernel_gameplay_catalog(config)));
    const std::uint32_t potion = item_id(config, "fungible_hover_potion");
    for (int bad = 0; bad < 3; ++bad) {
        gs::KernelGameplayCatalogStorage storage = gs::build_kernel_gameplay_catalog(config);
        bool found = false;
        for (KernelItemTemplateDefinition& item : storage.item_templates) {
            if (item.item_template_id != potion) continue;
            KernelActionDefinition& action = item.item_used_trigger.actions[0];
            require(action.action_type == KernelEntityTriggerActionType_ApplyStatus);
            require(action.status_effect_id == kHoverStatus);
            require(action.target_source == KernelEntityRefSource_EventInstigator);
            if (bad == 0) action.status_effect_id = 9999u;       // no such status
            if (bad == 1) action.status_effect_id = 0u;          // none at all
            if (bad == 2) action.target_source = 200u;           // nobody the event names
            found = true;
        }
        require(found);
        require(!kernel_takes(storage));
    }
}

void drinking_it_lifts_the_player_for_thirty_seconds(
    const gs::GameServerGameplayConfig& config) {
    const std::vector<std::uint8_t> scene =
        read_file(catalog_root() / config.static_collision_scene.entry_path);
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
    require(Kernel_StartDedicatedServer(kernel, 8461));

    // On the camp pad, where the catalog spawns players: flat ground.
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = gs::kActorTypePlayer;
    create.entity_template_id = config.player.actor_template_id;
    create.actor_template_id = config.player.actor_template_id;
    create.position = config.player.spawn_position;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t player = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &player));
    require(Kernel_ServerSetEntityActorTemplate(kernel, player, config.player.actor_template_id));
    KernelCombatStateDefinition combat = gs::make_player_combat_state(config);
    require(Kernel_ServerSetEntityCombatState(kernel, player, &combat));

    KernelInventoryContainerId bag = 0;
    require(Kernel_ServerCreateInventoryContainer(kernel, player, 4u, &bag));
    KernelItemInstanceId potions = 0;
    require(Kernel_ServerCreateInventoryItem(
        kernel, item_id(config, "fungible_hover_potion"), 2u, bag, &potions));

    for (int index = 0; index < 60; ++index) Kernel_Update(kernel, kTickSeconds);
    const KernelServerEntityState standing = entity_state(kernel, player);
    const float floor = standing.position.y;
    require((standing.visual_flags & KERNEL_VISUAL_FLAG_GROUNDED) != 0u);
    require((standing.visual_flags & KERNEL_VISUAL_FLAG_HOVERING) == 0u);

    KernelGameplayRequest request{};
    request.struct_size = sizeof(request);
    request.request_id = 1;
    request.instigator_net_id = player;
    request.requested_quantity = 1;
    request.domain_action = KernelDomainAction_Consume;
    request.selected_item_instance_id = potions;
    require(Kernel_SubmitGameplayRequest(kernel, &request));
    KernelGameplayRequestOutcome result{};
    result.struct_size = sizeof(result);
    require(Kernel_PollGameplayRequestOutcomes(kernel, &result, 1) == 1u);
    require(result.status == KernelGameplayRequestStatus_Committed);

    int ticks = 0;
    for (; ticks < 60; ++ticks) Kernel_Update(kernel, kTickSeconds);
    const KernelServerEntityState up = entity_state(kernel, player);
    std::printf("[potion] floor %.3f, after 2 s %.3f, flags 0x%x\n",
                floor, up.position.y, up.visual_flags);
    require(std::fabs(up.position.y - (floor + 3.0f)) < 0.05f);
    require((up.visual_flags & KERNEL_VISUAL_FLAG_HOVERING) != 0u);
    require((up.visual_flags & (KERNEL_VISUAL_FLAG_FALLING | KERNEL_VISUAL_FLAG_GROUNDED)) == 0u);

    // Still up a tick before the 30 s are out, down within a second after.
    for (; ticks < static_cast<int>(kHoverTicks) - 2; ++ticks) Kernel_Update(kernel, kTickSeconds);
    require(std::fabs(entity_state(kernel, player).position.y - (floor + 3.0f)) < 0.05f);
    for (; ticks < static_cast<int>(kHoverTicks) + 40; ++ticks) Kernel_Update(kernel, kTickSeconds);
    const KernelServerEntityState down = entity_state(kernel, player);
    std::printf("[potion] 40 ticks past 30 s: %.3f, flags 0x%x\n",
                down.position.y, down.visual_flags);
    require(std::fabs(down.position.y - floor) < 0.05f);
    require((down.visual_flags & KERNEL_VISUAL_FLAG_HOVERING) == 0u);
    require((down.visual_flags & KERNEL_VISUAL_FLAG_GROUNDED) != 0u);
    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_catalog_file(
        (catalog_root() / "gameplay_catalog.yaml").string());
    the_catalog_has_the_potion(config);
    the_kernel_refuses_an_item_status_it_cannot_apply(config);
    drinking_it_lifts_the_player_for_thirty_seconds(config);
    std::printf("hover_potion_test passed\n");
    return 0;
}
