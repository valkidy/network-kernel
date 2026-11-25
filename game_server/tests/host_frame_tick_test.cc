// The AI runs once per kernel tick, however often the game server is ticked.
//
// A Unity host calls Kernel_Update and GameServer_Tick once per rendered frame.
// Kernel_Update only simulates when a fixed tick's worth of time has built up,
// and only a simulated tick drains the command queue, but the game server used
// to run its AI on every call. At a few hundred frames a second that was ten
// AI passes per kernel tick, each enqueuing a state and a move for every
// agent: the queue overflowed (hundreds of thousands of rejected commands in
// one session, the agents that enqueued last losing theirs), and every timer
// the AI counts in ticks ran at the frame rate.

#include "game_server/src/game_server.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "kernel/public/kernel_api.h"
#include "kernel/src/kernel_api_internal.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::uint32_t kAgentCount = 20;
constexpr std::uint32_t kSentryGruntTemplateId = 2u;
// A 240 Hz editor against a 30 Hz kernel: eight frames per kernel tick.
constexpr float kFrameSeconds = 1.0f / 240.0f;
constexpr int kFrames = 240;

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
    require(Kernel_StartDedicatedServer(kernel, 8481));

    network_example::game_server::GameServerGameplayConfig gameplay_config =
        network_example::game_server::default_game_server_gameplay_config();
    gameplay_config.preload_director_template_ids.clear();
    network_example::game_server::GameServer game_server(kernel, gameplay_config);

    for (std::uint32_t index = 0; index < kAgentCount; ++index) {
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_type = network_example::game_server::kEntityTypeActor;
        create.actor_type = network_example::game_server::kActorTypeAgent;
        create.entity_template_id = kSentryGruntTemplateId;
        create.actor_template_id = kSentryGruntTemplateId;
        create.position = KernelVec3{static_cast<float>(index) * 3.0f, 0.0f, 0.0f};
        create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t net_id = 0;
        require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
        require(Kernel_ServerSetEntityActorTemplate(
            kernel, net_id, kSentryGruntTemplateId));
    }

    const network_example::game_server::AgentRuntimeManager& agents =
        game_server.agent_runtime_manager();

    // The first call runs before the kernel has simulated anything.
    game_server.tick(kFrameSeconds);
    require(agents.ticks() == 1u);
    // More calls with no kernel tick in between run nothing.
    for (int frame = 0; frame < 7; ++frame) {
        game_server.tick(kFrameSeconds);
    }
    require(agents.ticks() == 1u);
    require(agents.agent_count() == kAgentCount);

    // A second of frames, the way NetworkHost.Update drives them.
    const std::uint32_t first_tick = Kernel_GetCurrentTick(kernel);
    for (int frame = 0; frame < kFrames; ++frame) {
        Kernel_Update(kernel, kFrameSeconds);
        game_server.tick(kFrameSeconds);
    }
    const std::uint32_t kernel_ticks = Kernel_GetCurrentTick(kernel) - first_tick;
    std::fprintf(
        stderr,
        "frames=%d kernel_ticks=%u agent_ticks=%llu\n",
        kFrames,
        kernel_ticks,
        static_cast<unsigned long long>(agents.ticks() - 1u));
    require(kernel_ticks >= 28u && kernel_ticks <= 31u);
    require(agents.ticks() - 1u == kernel_ticks);
    require(agents.agent_count() == kAgentCount);

    Kernel_Destroy(kernel);
    return 0;
}
