// Every agent the kernel holds must reach the controllers.
//
// AgentRuntimeManager discovers its agents through Kernel_ServerQueryEntities,
// which writes as many states as the buffer it is handed will take and then
// stops. It used to be handed a fixed 128-entry array, so on a server holding
// more than 128 actors the surplus agents were never discovered, never ticked,
// and stood still -- silently, with no error anywhere, and with world iteration
// order deciding which ones lost.
//
// This pins the population well past that old boundary.

#include "game_server/game_server.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "kernel/public/kernel_api.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}

#define require(condition) require_impl((condition), #condition, __LINE__)

// Comfortably past the old 128-entry buffer, and past it by more than the one
// player that also occupies a slot.
constexpr std::uint32_t kAgentCount = 200;
constexpr std::uint32_t kSentryGruntTemplateId = 2u;
constexpr float kTickSeconds = 1.0f / 30.0f;

}  // namespace

int main() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    config.max_events = 1024;
    config.max_render_states = 512;
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7842));

    network_example::game_server::GameServerGameplayConfig gameplay_config =
        network_example::game_server::default_game_server_gameplay_config();
    // The directors would spawn a population of their own on top of the one
    // this test places, which would make the expected count depend on whatever
    // the catalog currently populates itself with.
    gameplay_config.preload_director_template_ids.clear();
    network_example::game_server::GameServer game_server(kernel, gameplay_config);

    for (std::uint32_t index = 0; index < kAgentCount; ++index) {
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_type = network_example::game_server::kEntityTypeActor;
        create.actor_type = network_example::game_server::kActorTypeAgent;
        create.entity_template_id = kSentryGruntTemplateId;
        create.actor_template_id = kSentryGruntTemplateId;
        // Spread out, so no two agents start inside one another.
        create.position = KernelVec3{
            static_cast<float>(index % 20u) * 3.0f,
            0.0f,
            static_cast<float>(index / 20u) * 3.0f};
        create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t net_id = 0;
        require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
        require(net_id != 0);
        require(Kernel_ServerSetEntityActorTemplate(
            kernel, net_id, kSentryGruntTemplateId));
    }

    game_server.tick(kTickSeconds);
    Kernel_Update(kernel, kTickSeconds);
    require(
        game_server.agent_runtime_manager().agent_count() == kAgentCount);

    // And it stays complete once the buffer has already grown: the discovery
    // path reuses one buffer across ticks rather than sizing it per call.
    game_server.tick(kTickSeconds);
    Kernel_Update(kernel, kTickSeconds);
    require(
        game_server.agent_runtime_manager().agent_count() == kAgentCount);

    Kernel_Destroy(kernel);
    return 0;
}
