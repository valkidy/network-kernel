// The authoring half of hit stagger: an actor's `stagger:` block and an
// apply_damage action's `stagger:` literal survive the whole trip from the real
// catalog bundle to the kernel ABI, and the kernel refuses a profile the loader
// would have refused.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"

namespace {

using network_example::game_server::GameServerGameplayConfig;
using network_example::game_server::KernelGameplayCatalogStorage;

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "stagger_config_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

std::filesystem::path runfiles_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace;
}

std::vector<std::uint8_t> read_bundle() {
    const std::filesystem::path path = runfiles_root() / "game_server" /
        "gameplay_catalog_bundle" / "bundle.zip";
    std::ifstream stream(path, std::ios::binary);
    require(stream.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::uint32_t template_id(const GameServerGameplayConfig& config, const char* name) {
    const auto found = std::find_if(
        config.entity_templates.begin(),
        config.entity_templates.end(),
        [name](const auto& entity) { return entity.name == name; });
    require(found != config.entity_templates.end());
    return found->actor_template_id;
}

KernelEntityTemplateDefinition* compiled_entity(
    KernelGameplayCatalogStorage& catalog,
    std::uint32_t id) {
    const auto found = std::find_if(
        catalog.entity_templates.begin(),
        catalog.entity_templates.end(),
        [id](const auto& entity) { return entity.entity_template_id == id; });
    require(found != catalog.entity_templates.end());
    return &*found;
}

const KernelActionTriggerDefinition& projectile_trigger(
    const GameServerGameplayConfig& config,
    const KernelGameplayCatalogStorage& catalog,
    const char* name) {
    const auto authored = std::find_if(
        config.projectile_templates.begin(),
        config.projectile_templates.end(),
        [name](const auto& projectile) { return projectile.name == name; });
    require(authored != config.projectile_templates.end());
    const std::uint32_t id = authored->definition.projectile_template_id;
    const auto found = std::find_if(
        catalog.projectile_templates.begin(),
        catalog.projectile_templates.end(),
        [id](const auto& projectile) { return projectile.projectile_template_id == id; });
    require(found != catalog.projectile_templates.end());
    return found->mechanics.projectile_impact_trigger;
}

const KernelActionDefinition* first_damage_action(
    const KernelActionTriggerDefinition& trigger) {
    for (std::uint32_t index = 0; index < trigger.action_count; ++index) {
        if (trigger.actions[index].action_type ==
            KernelEntityTriggerActionType_ApplyDamage) {
            return &trigger.actions[index];
        }
    }
    return nullptr;
}

bool kernel_accepts(const KernelGameplayCatalogStorage& catalog) {
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    const bool accepted = Kernel_LoadGameplayCatalog(kernel, &catalog.definition, nullptr);
    Kernel_Destroy(kernel);
    return accepted;
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> bundle = read_bundle();
    const GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_bundle_memory(
            bundle.data(),
            static_cast<std::uint32_t>(bundle.size()),
            "gameplay_catalog.yaml");
    KernelGameplayCatalogStorage catalog =
        network_example::game_server::build_kernel_gameplay_catalog(config);

    // Profiles reach the ABI field for field.
    const KernelEntityTemplateDefinition* player =
        compiled_entity(catalog, template_id(config, "player"));
    require(player->stagger_threshold == 120.0f);
    require(player->stagger_per_damage == 1.0f);
    require(player->stagger_decay_per_tick == 3.0f);
    require(player->stagger_decay_delay_ticks == 30u);
    require(player->stagger_ticks == 12u);
    require(player->stagger_immunity_ticks == 60u);
    require(compiled_entity(catalog, template_id(config, "chaser_grunt"))
                ->stagger_threshold == 150.0f);
    require(compiled_entity(catalog, template_id(config, "tripod_chaser"))
                ->stagger_ticks == 20u);
    // Not authored means not staggerable -- the control for the three above.
    require(compiled_entity(catalog, template_id(config, "sentry_grunt"))
                ->stagger_threshold == 0.0f);

    // The slam's authored stagger rides its projectile's impact trigger...
    const KernelActionDefinition* slam =
        first_damage_action(projectile_trigger(config, catalog, "grunt_slam_hit"));
    require(slam != nullptr);
    require(slam->damage_stagger_authored == 1u);
    require(slam->damage_stagger == 120.0f);
    // ...and a graph that authors none stays derived.
    const KernelActionDefinition* rocket =
        first_damage_action(projectile_trigger(config, catalog, "rocket_explosion"));
    require(rocket != nullptr);
    require(rocket->damage_stagger_authored == 0u);

    // The death policy rides the same template: authored on the player, left to
    // the kernel's by-kind default everywhere else.
    require(player->death_policy == KernelDeathPolicy_Dormant);
    require(compiled_entity(catalog, template_id(config, "chaser_grunt"))
                ->death_policy == KernelDeathPolicy_Default);

    // The kernel takes the real catalog, and turns away what the loader would.
    require(kernel_accepts(catalog));
    KernelEntityTemplateDefinition* mutable_player =
        compiled_entity(catalog, template_id(config, "player"));
    mutable_player->stagger_ticks = 0u;
    require(!kernel_accepts(catalog));
    mutable_player->stagger_ticks = KERNEL_MAX_STAGGER_TICKS + 1u;
    require(!kernel_accepts(catalog));
    mutable_player->stagger_ticks = 12u;
    mutable_player->stagger_per_damage = std::numeric_limits<float>::quiet_NaN();
    require(!kernel_accepts(catalog));
    mutable_player->stagger_per_damage = 1.0f;
    require(kernel_accepts(catalog));
    mutable_player->death_policy = KernelDeathPolicy_Dormant + 1u;
    require(!kernel_accepts(catalog));
    mutable_player->death_policy = KernelDeathPolicy_Dormant;
    require(kernel_accepts(catalog));

    std::puts("stagger_config_test: ok");
    return 0;
}
