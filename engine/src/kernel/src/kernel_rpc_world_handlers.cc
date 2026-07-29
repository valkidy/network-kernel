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

bool enqueue_create_inventory_container(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    simulation::Command command{};
    command.id = simulation::CommandId::kCreateInventoryContainer;
    command.source = simulation::CommandSource::kControlPlane;
    command.completion_token = request_id;
    if (!rpc_json::has_exact_fields(params, {"owner_entity_id", "slot_capacity"}) ||
        !rpc_json::read_param(params, "owner_entity_id",
            &command.create_inventory_container.owner_entity_id) ||
        !rpc_json::read_param(params, "slot_capacity",
            &command.create_inventory_container.slot_capacity)) {
        response_store.complete_error(
            request_id, KernelRpcErrorCode::InvalidParams, "Invalid params");
        return true;
    }
    if (!KernelRpcWorldHandlers::enqueue_simulation_command(engine, command)) {
        response_store.complete_error(
            request_id, KernelRpcErrorCode::ExecutionFailed, "Execution failed");
    }
    return true;
}

bool enqueue_create_inventory_item(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    simulation::Command command{};
    command.id = simulation::CommandId::kCreateInventoryItem;
    command.source = simulation::CommandSource::kControlPlane;
    command.completion_token = request_id;
    if (!rpc_json::has_exact_fields(
            params, {"item_template_id", "quantity", "container_id"}) ||
        !rpc_json::read_param(params, "item_template_id",
            &command.create_inventory_item.item_template_id) ||
        !rpc_json::read_param(params, "quantity",
            &command.create_inventory_item.quantity) ||
        !rpc_json::read_param(params, "container_id",
            &command.create_inventory_item.container_id)) {
        response_store.complete_error(
            request_id, KernelRpcErrorCode::InvalidParams, "Invalid params");
        return true;
    }
    if (!KernelRpcWorldHandlers::enqueue_simulation_command(engine, command)) {
        response_store.complete_error(
            request_id, KernelRpcErrorCode::ExecutionFailed, "Execution failed");
    }
    return true;
}

bool enqueue_create_world_item(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    simulation::Command command{};
    command.id = simulation::CommandId::kCreateWorldItem;
    command.source = simulation::CommandSource::kControlPlane;
    command.completion_token = request_id;
    if (!rpc_json::has_exact_fields(
            params, {"item_template_id", "quantity", "position"}) ||
        !rpc_json::read_param(params, "item_template_id",
            &command.create_world_item.item_template_id) ||
        !rpc_json::read_param(params, "quantity",
            &command.create_world_item.quantity) ||
        !rpc_json::read_param(params, "position",
            &command.create_world_item.position)) {
        response_store.complete_error(
            request_id, KernelRpcErrorCode::InvalidParams, "Invalid params");
        return true;
    }
    if (!KernelRpcWorldHandlers::enqueue_simulation_command(engine, command)) {
        response_store.complete_error(
            request_id, KernelRpcErrorCode::ExecutionFailed, "Execution failed");
    }
    return true;
}

bool enqueue_gameplay_request(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    simulation::Command command{};
    command.id = simulation::CommandId::kSubmitGameplayRequest;
    command.source = simulation::CommandSource::kControlPlane;
    command.completion_token = request_id;
    if (!rpc_json::has_exact_fields(params, {"request"}) ||
        !rpc_json::read_param(
            params, "request", &command.submit_gameplay_request.request)) {
        response_store.complete_error(
            request_id, KernelRpcErrorCode::InvalidParams, "Invalid params");
        return true;
    }
    if (!KernelRpcWorldHandlers::enqueue_simulation_command(engine, command)) {
        response_store.complete_error(
            request_id, KernelRpcErrorCode::ExecutionFailed, "Execution failed");
    }
    return true;
}

Json item_view_json(const KernelItemInstanceView& item) {
    Json portable = Json::array();
    for (std::uint32_t index = 0; index < item.portable_state_field_count; ++index) {
        const KernelPortableStateFieldDefinition& field =
            item.portable_state_fields[index];
        portable.push_back(Json{
            {"field_id", field.field_id},
            {"field_type", field.type},
            {"uint32_value", field.uint32_default},
            {"float_value", field.float_default},
            {"bool_value", field.bool_default != 0u},
        });
    }
    return Json{
        {"item_instance_id", item.item_instance_id},
        {"item_template_id", item.item_template_id},
        {"quantity", item.quantity},
        {"next_use_tick", item.next_use_tick},
        {"terminal", item.terminal != 0u},
        {"residency_kind", item.residency},
        {"world_mode", item.world_mode},
        {"inventory_container_id", item.inventory_container_id},
        {"inventory_slot", item.slot},
        {"prop_entity_id", item.prop_entity_id},
        {"carrier_entity_id", item.carrier_entity_id},
        {"portable_values", std::move(portable)},
    };
}

bool get_item(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    KernelItemInstanceId item_id = 0;
    KernelItemInstanceView item{};
    item.struct_size = sizeof(item);
    if (!rpc_json::has_exact_fields(params, {"item_instance_id"}) ||
        !rpc_json::read_param(params, "item_instance_id", &item_id)) {
        response_store.complete_error(
            request_id, KernelRpcErrorCode::InvalidParams, "Invalid params");
    } else if (!engine.get_item_instance(item_id, &item)) {
        response_store.complete_error(
            request_id, KernelRpcErrorCode::ResourceNotFound, "Resource not found");
    } else {
        response_store.complete_result(
            request_id, Json{{"item", item_view_json(item)}}.dump());
    }
    return true;
}

bool list_owned_inventory(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    std::uint32_t owner = 0;
    if (!rpc_json::has_exact_fields(params, {"owner_entity_id"}) ||
        !rpc_json::read_param(params, "owner_entity_id", &owner)) {
        response_store.complete_error(
            request_id, KernelRpcErrorCode::InvalidParams, "Invalid params");
        return true;
    }
    const std::uint32_t count =
        engine.copy_owned_inventory_containers(owner, nullptr, 0u);
    std::vector<KernelInventoryContainerView> containers(count);
    engine.copy_owned_inventory_containers(owner, containers.data(), count);
    Json values = Json::array();
    for (const KernelInventoryContainerView& container : containers) {
        values.push_back(Json{
            {"container_id", container.inventory_container_id},
            {"owner_entity_id", container.owner_entity_id},
            {"revision", container.revision},
            {"slot_capacity", container.slot_capacity},
            {"occupied_slot_count", container.occupied_slot_count},
            {"sync_state", container.sync_state},
        });
    }
    response_store.complete_result(
        request_id, Json{{"containers", std::move(values)}}.dump());
    return true;
}

bool get_inventory_snapshot(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    KernelInventoryContainerId container_id = 0;
    KernelInventoryContainerView container{};
    container.struct_size = sizeof(container);
    if (!rpc_json::has_exact_fields(params, {"container_id"}) ||
        !rpc_json::read_param(params, "container_id", &container_id)) {
        response_store.complete_error(
            request_id, KernelRpcErrorCode::InvalidParams, "Invalid params");
        return true;
    }
    if (!engine.get_inventory_container(container_id, &container)) {
        response_store.complete_error(
            request_id, KernelRpcErrorCode::ResourceNotFound, "Resource not found");
        return true;
    }
    const std::uint32_t count = engine.copy_inventory_slots(
        container_id, nullptr, 0u);
    std::vector<KernelItemInstanceView> items(count);
    engine.copy_inventory_slots(container_id, items.data(), count);
    Json values = Json::array();
    for (const KernelItemInstanceView& item : items) {
        values.push_back(item_view_json(item));
    }
    response_store.complete_result(request_id, Json{
        {"container", Json{
            {"container_id", container.inventory_container_id},
            {"owner_entity_id", container.owner_entity_id},
            {"revision", container.revision},
            {"slot_capacity", container.slot_capacity},
            {"sync_state", container.sync_state},
        }},
        {"items", std::move(values)},
    }.dump());
    return true;
}

bool get_gameplay_request_outcome(
    KernelEngine& engine,
    const Json& params,
    std::uint64_t rpc_request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    std::uint32_t requester_peer = 0;
    std::uint64_t request_id = 0;
    KernelGameplayRequestOutcome outcome{};
    if (!rpc_json::has_exact_fields(
            params, {"requester_peer", "request_id"}) ||
        !rpc_json::read_param(params, "requester_peer", &requester_peer) ||
        !rpc_json::read_param(params, "request_id", &request_id)) {
        response_store.complete_error(
            rpc_request_id, KernelRpcErrorCode::InvalidParams, "Invalid params");
    } else if (!engine.get_gameplay_request_outcome(
            requester_peer, request_id, &outcome)) {
        response_store.complete_error(
            rpc_request_id,
            KernelRpcErrorCode::ResourceNotFound,
            "Resource not found");
    } else {
        response_store.complete_result(rpc_request_id, Json{{"outcome", Json{
            {"requester_peer", outcome.requester_peer},
            {"request_id", outcome.request_id},
            {"status", outcome.status},
            {"graph_outcome", outcome.graph_outcome},
            {"domain_action", outcome.domain_action},
            {"rejection_reason", outcome.rejection_reason},
            {"item_instance_id", outcome.item_instance_id},
            {"prop_entity_id", outcome.prop_entity_id},
            {"committed_quantity", outcome.committed_quantity},
        }}}.dump());
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
    if (symbol == "KernelRpc_CreateInventoryContainer") {
        return enqueue_create_inventory_container;
    }
    if (symbol == "KernelRpc_CreateInventoryItem") {
        return enqueue_create_inventory_item;
    }
    if (symbol == "KernelRpc_CreateWorldItem") {
        return enqueue_create_world_item;
    }
    if (symbol == "KernelRpc_SubmitGameplayRequest") {
        return enqueue_gameplay_request;
    }
    if (symbol == "KernelRpc_GetItem") return get_item;
    if (symbol == "KernelRpc_ListOwnedInventory") return list_owned_inventory;
    if (symbol == "KernelRpc_GetInventorySnapshot") return get_inventory_snapshot;
    if (symbol == "KernelRpc_GetGameplayRequestOutcome") {
        return get_gameplay_request_outcome;
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
    } else if (command_id == simulation::CommandId::kCreateInventoryContainer) {
        response_store.complete_result(completion_token, Json{
            {"container_id", result.inventory_container_id}}.dump());
    } else if (command_id == simulation::CommandId::kCreateInventoryItem) {
        response_store.complete_result(completion_token, Json{
            {"item_instance_id", result.item_instance_id}}.dump());
    } else if (command_id == simulation::CommandId::kCreateWorldItem) {
        response_store.complete_result(completion_token, Json{
            {"item_instance_id", result.item_instance_id},
            {"prop_entity_id", result.net_id}}.dump());
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
