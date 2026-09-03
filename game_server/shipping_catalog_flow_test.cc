// The shipped catalog, driven end to end.
//
// Every other test in this area builds its own catalog, which is what makes
// them precise and also what lets the authored one drift out from under them.
// This one loads what actually ships and follows the mission through: a player
// arrives, the mission places its nests, and each nest starts putting
// gingerbread out around itself.
//
// It deliberately asserts the shape of the flow rather than the numbers in it.
// Counts, intervals and radii are tuning and will move; "a nest appears and
// then emits" is the design.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "game_server/agent_runtime.h"
#include "game_server/game_server.h"
#include "game_server/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;

std::uint32_t template_id_of(
    const network_example::game_server::GameServerGameplayConfig& config,
    const std::string& name) {
    for (const network_example::game_server::EntityTemplateConfig& candidate :
         config.entity_templates) {
        if (candidate.name == name) {
            return candidate.actor_template_id;
        }
    }
    return 0;
}

std::uint32_t count_of_template(
    KernelHandle* kernel,
    std::uint16_t entity_type,
    std::uint32_t entity_template_id) {
    std::vector<KernelServerEntityState> states(256);
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    const std::uint32_t count = Kernel_ServerQueryEntities(
        kernel,
        entity_type,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    std::uint32_t matches = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].entity_template_id == entity_template_id) {
            ++matches;
        }
    }
    return matches;
}

std::uint32_t create_player(KernelHandle* kernel) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = network_example::game_server::kEntityTypeActor;
    create_info.actor_type = network_example::game_server::kActorTypePlayer;
    create_info.position = KernelVec3{0.0f, 0.0f, 0.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    return net_id;
}

}  // namespace

int main() {
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();

    const std::uint32_t nest_template = template_id_of(config, "gingerbread_nest");
    const std::uint32_t gingerbread_template =
        template_id_of(config, "gingerbread");
    require(nest_template != 0);
    require(gingerbread_template != 0);

    // What the authoring says, read back off the loaded catalog: the nest is
    // the thing that carries the rule.
    const auto carrier = std::find_if(
        config.spawner_carriers.begin(),
        config.spawner_carriers.end(),
        [nest_template](
            const network_example::game_server::SpawnerCarrierConfig& candidate) {
            return candidate.entity_template_id == nest_template;
        });
    require(carrier != config.spawner_carriers.end());
    require(carrier->entity_type == KernelEntityType_Prop);
    require(!carrier->spawner.composition.empty());
    require(
        carrier->spawner.composition[0].entity_template_id ==
        gingerbread_template);

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 64;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7871));
    require(network_example::game_server::load_kernel_gameplay_catalog(
        kernel, config));

    network_example::game_server::GameServer game_server(kernel, config);
    require(game_server.preload_directors());

    // No player, so the mission has not opened and no nest exists.
    for (int tick = 0; tick < 30; ++tick) {
        game_server.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
    }
    require(count_of_template(kernel, KernelEntityType_Prop, nest_template) == 0u);

    // Somebody arrives, and the mission places what it wants destroyed.
    create_player(kernel);
    for (int tick = 0; tick < 30; ++tick) {
        game_server.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
    }
    const std::uint32_t nests =
        count_of_template(kernel, KernelEntityType_Prop, nest_template);
    require(nests > 0u);

    // Each nest gets a rule of its own -- that is what putting the rule on the
    // carrier means -- and they start putting gingerbread out.
    //
    // Asked of the spawner rather than of the world. The shipping catalog also
    // runs a patrol that spawns gingerbread, so "are there any gingerbread" is
    // answered yes by something else entirely: this assertion passed with the
    // spawner disabled until it was made attributable.
    const network_example::game_server::SpawnerDirector& spawner =
        game_server.agent_runtime_manager().spawner_director();
    require(spawner.instances().size() == nests);
    for (int tick = 0; tick < 900 && spawner.spawned_unit_count() == 0u; ++tick) {
        game_server.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
    }
    require(spawner.spawned_unit_count() > 0u);
    require(
        count_of_template(kernel, KernelEntityType_Actor, gingerbread_template) >
        0u);
    // Still standing: emitting is not what removes a nest.
    require(
        count_of_template(kernel, KernelEntityType_Prop, nest_template) == nests);

    Kernel_Destroy(kernel);
    return 0;
}
