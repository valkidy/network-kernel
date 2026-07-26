#include "kernel/src/kernel_rpc_world_handlers.h"

#include "kernel/src/kernel.h"
#include "kernel/src/kernel_rpc_json_binding.h"
#include "simulation/public/command.h"

namespace network_example {
namespace {

using Json = nlohmann::json;

bool get_entity_state(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    std::uint32_t net_id = 0;
    if (!rpc_json::has_exact_fields(params, {"net_id"}) ||
        !rpc_json::read_param(params, "net_id", &net_id)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
        return true;
    }
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    if (!KernelRpcWorldHandlers::server_get_entity_state(
            engine,
            net_id,
            &state)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::ResourceNotFound,
            "Resource not found");
    } else {
        response_store.complete_result(
            request_id,
            Json({{"state", rpc_json::entity_state_json(state)}}).dump());
    }
    return true;
}

bool enqueue_create_entity(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    if (!rpc_json::has_exact_fields(params, {"create_info"})) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
        return true;
    }
    simulation::Command command{};
    command.id = simulation::CommandId::kCreateEntity;
    command.source = simulation::CommandSource::kControlPlane;
    command.completion_token = request_id;
    if (!rpc_json::read_param(
            params,
            "create_info",
            &command.create_entity.create_info)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
        return true;
    }
    if (!KernelRpcWorldHandlers::enqueue_simulation_command(engine, command)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::ExecutionFailed,
            "Execution failed");
    }
    return true;
}

bool enqueue_activate_entity(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    if (!rpc_json::has_exact_fields(params, {"activate_info"})) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
        return true;
    }
    simulation::Command command{};
    command.id = simulation::CommandId::kActivateEntity;
    command.source = simulation::CommandSource::kControlPlane;
    command.completion_token = request_id;
    if (!rpc_json::read_param(
            params,
            "activate_info",
            &command.activate_entity.activate_info)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
        return true;
    }
    if (!KernelRpcWorldHandlers::enqueue_simulation_command(engine, command)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::ExecutionFailed,
            "Execution failed");
    }
    return true;
}

bool enqueue_destroy_entity(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    simulation::Command command{};
    command.id = simulation::CommandId::kDestroyEntity;
    command.source = simulation::CommandSource::kControlPlane;
    command.completion_token = request_id;
    if (!rpc_json::has_exact_fields(params, {"net_id", "reason"}) ||
        !rpc_json::read_param(
            params,
            "net_id",
            &command.destroy_entity.net_id) ||
        !rpc_json::read_param(
            params,
            "reason",
            &command.destroy_entity.reason)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
        return true;
    }
    if (!KernelRpcWorldHandlers::enqueue_simulation_command(engine, command)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::ExecutionFailed,
            "Execution failed");
    }
    return true;
}

bool enqueue_set_transform(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    simulation::Command command{};
    command.id = simulation::CommandId::kSetEntityTransform;
    command.source = simulation::CommandSource::kControlPlane;
    command.completion_token = request_id;
    if (!rpc_json::has_exact_fields(params, {"net_id", "position", "rotation"}) ||
        !rpc_json::read_param(
            params,
            "net_id",
            &command.set_entity_transform.net_id) ||
        !rpc_json::read_param(
            params,
            "position",
            &command.set_entity_transform.position) ||
        !rpc_json::read_param(
            params,
            "rotation",
            &command.set_entity_transform.rotation)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
        return true;
    }
    if (!KernelRpcWorldHandlers::enqueue_simulation_command(engine, command)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::ExecutionFailed,
            "Execution failed");
    }
    return true;
}

bool enqueue_set_velocity(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    simulation::Command command{};
    command.id = simulation::CommandId::kSetEntityVelocity;
    command.source = simulation::CommandSource::kControlPlane;
    command.completion_token = request_id;
    if (!rpc_json::has_exact_fields(params, {"net_id", "velocity"}) ||
        !rpc_json::read_param(
            params,
            "net_id",
            &command.set_entity_velocity.net_id) ||
        !rpc_json::read_param(
            params,
            "velocity",
            &command.set_entity_velocity.velocity)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
        return true;
    }
    if (!KernelRpcWorldHandlers::enqueue_simulation_command(engine, command)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::ExecutionFailed,
            "Execution failed");
    }
    return true;
}

bool enqueue_set_entity_state(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    simulation::Command command{};
    command.id = simulation::CommandId::kSetEntityState;
    command.source = simulation::CommandSource::kControlPlane;
    command.completion_token = request_id;
    if (!rpc_json::has_exact_fields(
            params,
            {"net_id", "animation_state", "visual_flags"}) ||
        !rpc_json::read_param(
            params,
            "net_id",
            &command.set_entity_state.net_id) ||
        !rpc_json::read_param(
            params,
            "animation_state",
            &command.set_entity_state.animation_state) ||
        !rpc_json::read_param(
            params,
            "visual_flags",
            &command.set_entity_state.visual_flags)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
        return true;
    }
    if (!KernelRpcWorldHandlers::enqueue_simulation_command(engine, command)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::ExecutionFailed,
            "Execution failed");
    }
    return true;
}

bool enqueue_set_entity_health(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    simulation::Command command{};
    command.id = simulation::CommandId::kSetEntityHealth;
    command.source = simulation::CommandSource::kControlPlane;
    command.completion_token = request_id;
    if (!rpc_json::has_exact_fields(params, {"net_id", "hp"}) ||
        !rpc_json::read_param(
            params,
            "net_id",
            &command.set_entity_state.net_id) ||
        !rpc_json::read_param(
            params,
            "hp",
            &command.set_entity_state.animation_state)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
        return true;
    }
    if (!KernelRpcWorldHandlers::enqueue_simulation_command(engine, command)) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::ExecutionFailed,
            "Execution failed");
    }
    return true;
}

}  // namespace

KernelRpcMethodHandler KernelRpcWorldHandlers::handler_for_symbol(
    std::string_view symbol) {
    if (symbol == "KernelRpc_GetEntityState") {
        return get_entity_state;
    }
    if (symbol == "Kernel_ServerCreateEntity") {
        return enqueue_create_entity;
    }
    if (symbol == "Kernel_ServerActivateEntity") {
        return enqueue_activate_entity;
    }
    if (symbol == "Kernel_ServerDestroyEntity") {
        return enqueue_destroy_entity;
    }
    if (symbol == "Kernel_ServerSetEntityTransform") {
        return enqueue_set_transform;
    }
    if (symbol == "Kernel_ServerSetEntityVelocity") {
        return enqueue_set_velocity;
    }
    if (symbol == "Kernel_ServerSetEntityState") {
        return enqueue_set_entity_state;
    }
    if (symbol == "Kernel_ServerSetEntityHealth") {
        return enqueue_set_entity_health;
    }
    return nullptr;
}

void KernelRpcWorldHandlers::complete_simulation_command(
    KernelRpcResponseStore& response_store,
    std::uint64_t completion_token,
    simulation::CommandId command_id,
    const simulation::CommandResult& result) {
    if (completion_token == 0) {
        return;
    }
    if (!result.ok) {
        response_store.complete_error(
            completion_token,
            KernelRpcErrorCode::ExecutionFailed,
            "Execution failed");
        return;
    }
    if (command_id == simulation::CommandId::kCreateEntity) {
        response_store.complete_result(
            completion_token,
            Json({{"net_id", result.net_id}}).dump());
    } else {
        response_store.complete_result(completion_token, R"({"ok":true})");
    }
}

bool KernelRpcWorldHandlers::server_get_entity_state(
    const KernelEngine& engine,
    std::uint32_t net_id,
    KernelServerEntityState* out_state) {
    return engine.server_get_entity_state(net_id, out_state);
}

bool KernelRpcWorldHandlers::enqueue_simulation_command(
    KernelEngine& engine,
    const simulation::Command& command) {
    return engine.enqueue_simulation_command(command);
}

}  // namespace network_example
