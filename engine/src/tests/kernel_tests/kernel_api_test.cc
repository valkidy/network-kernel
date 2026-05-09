#include <array>
#include <cassert>

#include "kernel/public/kernel_api.h"

int main() {
    KernelAbiInfo abi_info{};
    assert(Kernel_GetAbiInfo(&abi_info, sizeof(abi_info)));
    assert(abi_info.struct_size == sizeof(KernelAbiInfo));
    assert(abi_info.abi_version == KERNEL_ABI_VERSION);
    assert(abi_info.kernel_config_size == sizeof(KernelConfig));
    assert(abi_info.player_input_size == sizeof(PlayerInput));
    assert(abi_info.render_entity_state_size == sizeof(RenderEntityState));
    assert(abi_info.kernel_event_size == sizeof(KernelEvent));
    assert(abi_info.server_entity_create_info_size ==
           sizeof(KernelServerEntityCreateInfo));
    assert(abi_info.server_entity_state_size == sizeof(KernelServerEntityState));
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_CLIENT_MODE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LISTEN_SERVER_MODE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_DEDICATED_SERVER_MODE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_INPUT_SUBMISSION) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_RENDER_STATES) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_EVENT_POLLING) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_CLIENT_PREDICTION) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SNAPSHOT_INTERPOLATION) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LAG_COMPENSATED_HITSCAN) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LOCAL_PLAYER_INFO) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_CREATE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_DESTROY) != 0);
    assert(
        (abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_TRANSFORM_WRITE) !=
        0);
    assert(
        (abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_VELOCITY_WRITE) !=
        0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_STATE_WRITE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_QUERY) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_RELEVANCE_FILTER) != 0);
    assert(abi_info.local_player_info_size == sizeof(KernelLocalPlayerInfo));
    assert(!Kernel_GetAbiInfo(nullptr, sizeof(abi_info)));
    assert(!Kernel_GetAbiInfo(&abi_info, sizeof(abi_info) - 1));

    assert(Kernel_Create(nullptr) == nullptr);
    assert(!Kernel_StartClient(nullptr, "127.0.0.1:9"));
    assert(!Kernel_StartListenServer(nullptr, 7777));
    assert(!Kernel_StartDedicatedServer(nullptr, 7777));
    Kernel_Update(nullptr, 1.0f / 30.0f);
    Kernel_SubmitInput(nullptr, 1, nullptr);
    assert(Kernel_GetRenderStates(nullptr, nullptr, 0) == 0);
    assert(Kernel_PollEvents(nullptr, nullptr, 0) == 0);
    KernelLocalPlayerInfo local_info{};
    assert(!Kernel_GetLocalPlayerInfo(nullptr, &local_info));
    assert(!Kernel_GetLocalPlayerInfo(nullptr, nullptr));
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = 2;
    create_info.position = KernelVec3{1.0f, 0.0f, 0.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t created_net_id = 0;
    assert(!Kernel_ServerCreateEntity(nullptr, &create_info, &created_net_id));
    assert(!Kernel_ServerDestroyEntity(nullptr, 1, KernelDespawnReason_Destroyed));
    assert(!Kernel_ServerSetEntityTransform(
        nullptr,
        1,
        &create_info.position,
        &create_info.rotation));
    assert(!Kernel_ServerSetEntityVelocity(nullptr, 1, &create_info.position));
    assert(!Kernel_ServerSetEntityState(nullptr, 1, 2, 3));
    KernelServerEntityState server_state{};
    server_state.struct_size = sizeof(server_state);
    assert(!Kernel_ServerGetEntityState(nullptr, 1, &server_state));
    assert(Kernel_ServerQueryEntities(nullptr, 0, &server_state, 1) == 0);

    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_GetLocalPlayerInfo(kernel, &local_info));
    assert(local_info.peer_id == 0);
    assert(local_info.player_net_id == 0);
    assert(!local_info.has_welcome);
    assert(!local_info.connected);
    assert(!Kernel_GetLocalPlayerInfo(kernel, nullptr));
    assert(!Kernel_StartClient(kernel, nullptr));
    assert(!Kernel_StartClient(kernel, ""));
    assert(!Kernel_ServerCreateEntity(kernel, &create_info, &created_net_id));
    assert(Kernel_StartDedicatedServer(kernel, 7777));

    assert(Kernel_ServerCreateEntity(kernel, &create_info, &created_net_id));
    assert(created_net_id != 0);
    KernelVec3 enemy_position{5.0f, 0.0f, 0.0f};
    KernelQuat enemy_rotation{0.0f, 0.0f, 0.0f, 1.0f};
    assert(Kernel_ServerSetEntityTransform(
        kernel,
        created_net_id,
        &enemy_position,
        &enemy_rotation));
    KernelVec3 enemy_velocity{1.0f, 0.0f, 0.0f};
    assert(Kernel_ServerSetEntityVelocity(kernel, created_net_id, &enemy_velocity));
    assert(Kernel_ServerSetEntityState(kernel, created_net_id, 7, 0x12345678u));
    server_state = KernelServerEntityState{};
    server_state.struct_size = sizeof(server_state);
    assert(Kernel_ServerGetEntityState(kernel, created_net_id, &server_state));
    assert(server_state.valid);
    assert(server_state.net_id == created_net_id);
    assert(server_state.entity_type == 2);
    assert(server_state.animation_state == 7);
    assert(server_state.visual_flags == 0x12345678u);
    assert(server_state.position.x == 5.0f);
    std::array<KernelServerEntityState, 4> queried_states{};
    for (KernelServerEntityState& queried_state : queried_states) {
        queried_state.struct_size = sizeof(KernelServerEntityState);
    }
    assert(Kernel_ServerQueryEntities(
               kernel,
               2,
               queried_states.data(),
               static_cast<std::uint32_t>(queried_states.size())) == 1);

    PlayerInput input{};
    input.input_seq = 1;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    Kernel_SubmitInput(kernel, 1, &input);
    Kernel_Update(kernel, 1.0f / 30.0f);

    std::array<RenderEntityState, 8> states{};
    assert(Kernel_GetRenderStates(kernel, nullptr, states.size()) == 0);
    assert(Kernel_GetRenderStates(kernel, states.data(), 0) == 0);
    const std::uint32_t render_count =
        Kernel_GetRenderStates(kernel, states.data(), states.size());
    assert(render_count == 1);
    assert(states[0].net_id == created_net_id);
    assert(states[0].animation_state == 7);
    assert(states[0].visual_flags == 0x12345678u);

    std::array<KernelEvent, 16> events{};
    assert(Kernel_PollEvents(kernel, nullptr, events.size()) == 0);
    assert(Kernel_PollEvents(kernel, events.data(), 0) == 0);
    assert(Kernel_PollEvents(kernel, events.data(), events.size()) >= 1);
    assert(Kernel_ServerDestroyEntity(
        kernel,
        created_net_id,
        KernelDespawnReason_Destroyed));

    Kernel_Destroy(kernel);
    return 0;
}
