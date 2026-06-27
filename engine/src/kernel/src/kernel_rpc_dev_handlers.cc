#include "kernel/src/kernel_rpc_dev_handlers.h"

#include <algorithm>

#include "kernel/src/kernel_rpc_json_binding.h"

namespace network_example {
namespace {

using Json = nlohmann::json;

const char* authority_name(KernelRpcAuthority authority) {
    switch (authority) {
        case KernelRpcAuthority::kDeveloperReadOnly:
            return "developer_read_only";
        case KernelRpcAuthority::kDeveloperWrite:
            return "developer_write";
        case KernelRpcAuthority::kAdmin:
            return "admin";
        case KernelRpcAuthority::kDirectorAi:
            return "director_ai";
    }
    return "developer_read_only";
}

const char* phase_name(KernelRpcExecutionPhase phase) {
    switch (phase) {
        case KernelRpcExecutionPhase::kImmediateReadOnly:
            return "immediate_read_only";
        case KernelRpcExecutionPhase::kSimulationTick:
            return "simulation_tick";
        case KernelRpcExecutionPhase::kPostSimulation:
            return "post_simulation";
    }
    return "immediate_read_only";
}

bool dev_ping(
    KernelEngine&,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry&) {
    if (!params.empty()) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
    } else {
        response_store.complete_result(request_id, R"({"ok":true})");
    }
    return true;
}

bool dev_list_methods(
    KernelEngine&,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry& registry) {
    if (!params.empty()) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
        return true;
    }
    Json methods = Json::array();
    for (const KernelRpcMethodDescriptor& value : registry.methods()) {
        methods.push_back({
            {"method", value.method},
            {"authority", authority_name(value.authority)},
            {"phase", phase_name(value.phase)},
            {"implementation", value.implementation},
        });
    }
    response_store.complete_result(
        request_id,
        Json({{"methods", methods}}).dump());
    return true;
}

bool dev_describe_method(
    KernelEngine&,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry& registry) {
    if (!rpc_json::has_exact_fields(params, {"method"}) ||
        !params["method"].is_string()) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
        return true;
    }
    const std::string requested = params["method"].get<std::string>();
    const Json schema = Json::parse(registry.schema_json());
    const auto found = std::find_if(
        schema.at("methods").begin(),
        schema.at("methods").end(),
        [&requested](const Json& value) {
            return value.at("method") == requested;
        });
    if (found == schema.at("methods").end()) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::MethodNotFound,
            "Method not found");
    } else {
        response_store.complete_result(request_id, found->dump());
    }
    return true;
}

bool dev_get_schema(
    KernelEngine&,
    const Json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry& registry) {
    if (!params.empty()) {
        response_store.complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
    } else {
        const Json result = {
            {"schema", Json::parse(registry.schema_json())},
        };
        response_store.complete_result(request_id, result.dump());
    }
    return true;
}

}  // namespace

KernelRpcMethodHandler kernel_rpc_dev_handler_for_symbol(
    std::string_view symbol) {
    if (symbol == "KernelRpc_DevPing") {
        return dev_ping;
    }
    if (symbol == "KernelRpc_DevListMethods") {
        return dev_list_methods;
    }
    if (symbol == "KernelRpc_DevDescribeMethod") {
        return dev_describe_method;
    }
    if (symbol == "KernelRpc_DevGetSchema") {
        return dev_get_schema;
    }
    return nullptr;
}

}  // namespace network_example
