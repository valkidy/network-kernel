#include "kernel/src/kernel_rpc.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <optional>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "kernel/src/kernel.h"
#include "kernel/src/kernel_rpc_dev_handlers.h"
#include "kernel/src/kernel_rpc_methods.generated.h"
#include "kernel/src/kernel_rpc_world_handlers.h"

namespace network_example {
namespace {

using Json = nlohmann::json;

KernelRpcAuthority parse_authority(std::string_view value) {
    if (value == "developer_write") {
        return KernelRpcAuthority::kDeveloperWrite;
    }
    if (value == "admin") {
        return KernelRpcAuthority::kAdmin;
    }
    if (value == "director_ai") {
        return KernelRpcAuthority::kDirectorAi;
    }
    return KernelRpcAuthority::kDeveloperReadOnly;
}

KernelRpcExecutionPhase parse_phase(std::string_view value) {
    if (value == "simulation_tick") {
        return KernelRpcExecutionPhase::kSimulationTick;
    }
    if (value == "post_simulation") {
        return KernelRpcExecutionPhase::kPostSimulation;
    }
    return KernelRpcExecutionPhase::kImmediateReadOnly;
}

bool authority_allows(
    KernelRpcAuthority caller,
    KernelRpcAuthority required) {
    switch (caller) {
        case KernelRpcAuthority::kDeveloperReadOnly:
            return required == KernelRpcAuthority::kDeveloperReadOnly;
        case KernelRpcAuthority::kDeveloperWrite:
            return required == KernelRpcAuthority::kDeveloperReadOnly ||
                   required == KernelRpcAuthority::kDeveloperWrite;
        case KernelRpcAuthority::kAdmin:
            return required == KernelRpcAuthority::kDeveloperReadOnly ||
                   required == KernelRpcAuthority::kDeveloperWrite ||
                   required == KernelRpcAuthority::kAdmin;
        case KernelRpcAuthority::kDirectorAi:
            return required == KernelRpcAuthority::kDeveloperReadOnly ||
                   required == KernelRpcAuthority::kDirectorAi;
    }
    return false;
}

KernelRpcMethodHandler handler_for_generated_method(
    const KernelRpcGeneratedMethodDescriptor& generated) {
    if (KernelRpcMethodHandler handler =
            kernel_rpc_dev_handler_for_symbol(generated.symbol);
        handler != nullptr) {
        return handler;
    }
    return KernelRpcWorldHandlers::handler_for_symbol(generated.symbol);
}

bool requires_running_server(const KernelRpcMethodDescriptor& descriptor) {
    return descriptor.method.rfind("dev.", 0) != 0;
}

const char* passing_name(KernelRpcGeneratedPassing passing) {
    switch (passing) {
        case KernelRpcGeneratedPassing::kValue:
            return "value";
        case KernelRpcGeneratedPassing::kConstPtr:
            return "const_ptr";
        case KernelRpcGeneratedPassing::kMutablePtr:
            return "mutable_ptr";
    }
    return "value";
}

const char* direction_name(KernelRpcGeneratedDirection direction) {
    switch (direction) {
        case KernelRpcGeneratedDirection::kInput:
            return "input";
        case KernelRpcGeneratedDirection::kOutput:
            return "output";
    }
    return "input";
}

std::vector<KernelRpcParameterDescriptor> parameters_for_generated_method(
    const KernelRpcGeneratedMethodDescriptor& generated) {
    std::vector<KernelRpcParameterDescriptor> parameters;
    parameters.reserve(generated.params.size());
    for (const KernelRpcGeneratedParamDescriptor& param : generated.params) {
        parameters.push_back(KernelRpcParameterDescriptor{
            param.name,
            param.type,
            passing_name(param.passing),
            direction_name(param.direction),
        });
    }
    return parameters;
}

}  // namespace

struct KernelRpcMethodRegistry::Impl {
    std::vector<KernelRpcMethodDescriptor> methods;
    std::string schema;
};

KernelRpcMethodRegistry::KernelRpcMethodRegistry()
    : impl_(std::make_unique<Impl>()) {
    impl_->schema = std::string(generated_kernel_rpc_schema());
    const Json schema = Json::parse(impl_->schema);
    for (const auto& generated : generated_kernel_rpc_methods()) {
        const auto schema_method = std::find_if(
            schema.at("methods").begin(),
            schema.at("methods").end(),
            [&generated](const Json& method) {
                return method.at("method") == generated.method;
            });
        impl_->methods.push_back(KernelRpcMethodDescriptor{
            generated.method,
            parse_authority(generated.authority),
            parse_phase(generated.phase),
            schema_method == schema.at("methods").end()
                ? "implemented"
                : schema_method->value("implementation", "implemented"),
            generated.internal,
            parameters_for_generated_method(generated),
            handler_for_generated_method(generated),
        });
    }
}

KernelRpcMethodRegistry::~KernelRpcMethodRegistry() = default;

const KernelRpcMethodDescriptor* KernelRpcMethodRegistry::find(
    std::string_view method) const {
    const auto found = std::find_if(
        impl_->methods.begin(),
        impl_->methods.end(),
        [method](const KernelRpcMethodDescriptor& descriptor) {
            return descriptor.method == method;
        });
    return found == impl_->methods.end() ? nullptr : &(*found);
}

const std::vector<KernelRpcMethodDescriptor>&
KernelRpcMethodRegistry::methods() const {
    return impl_->methods;
}

std::string_view KernelRpcMethodRegistry::schema_json() const {
    return impl_->schema;
}

struct KernelRpcResponseStore::Impl {
    struct Entry {
        std::string external_id_json = "null";
        std::string method;
        std::optional<std::string> response;
    };

    std::unordered_map<std::uint64_t, Entry> entries;
    std::uint64_t next_request_id = 1;
};

KernelRpcResponseStore::KernelRpcResponseStore()
    : impl_(std::make_unique<Impl>()) {}

KernelRpcResponseStore::~KernelRpcResponseStore() = default;

bool KernelRpcResponseStore::reserve(std::uint64_t* out_request_id) {
    if (out_request_id == nullptr || impl_->entries.size() >= kCapacity) {
        return false;
    }
    if (impl_->next_request_id == 0) {
        impl_->next_request_id = 1;
    }
    const std::uint64_t request_id = impl_->next_request_id++;
    impl_->entries.emplace(request_id, Impl::Entry{});
    *out_request_id = request_id;
    return true;
}

bool KernelRpcResponseStore::set_context(
    std::uint64_t request_id,
    std::string external_id_json,
    std::string method) {
    const auto found = impl_->entries.find(request_id);
    if (found == impl_->entries.end()) {
        return false;
    }
    found->second.external_id_json = std::move(external_id_json);
    found->second.method = std::move(method);
    return true;
}

bool KernelRpcResponseStore::complete_result(
    std::uint64_t request_id,
    std::string result_json) {
    const auto found = impl_->entries.find(request_id);
    if (found == impl_->entries.end() || found->second.response.has_value()) {
        return false;
    }
    found->second.response =
        std::string("{\"id\":") + found->second.external_id_json +
        ",\"jsonrpc\":\"2.0\",\"result\":" + result_json + "}";
    return true;
}

bool KernelRpcResponseStore::complete_error(
    std::uint64_t request_id,
    KernelRpcErrorCode code,
    std::string_view message) {
    const auto found = impl_->entries.find(request_id);
    if (found == impl_->entries.end() || found->second.response.has_value()) {
        return false;
    }
    const Json response = {
        {"jsonrpc", "2.0"},
        {"id", Json::parse(found->second.external_id_json)},
        {"error",
         {
             {"code", static_cast<int>(code)},
             {"message", message},
         }},
    };
    found->second.response = response.dump();
    return true;
}

bool KernelRpcResponseStore::poll(
    std::uint64_t request_id,
    char* out_response_json,
    std::uint32_t response_json_capacity,
    std::uint32_t* out_response_json_size) {
    if (out_response_json_size == nullptr) {
        return false;
    }
    *out_response_json_size = 0;
    const auto found = impl_->entries.find(request_id);
    if (found == impl_->entries.end() || !found->second.response.has_value()) {
        return false;
    }
    const std::string& response = *found->second.response;
    if (response.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    *out_response_json_size = static_cast<std::uint32_t>(response.size());
    if (out_response_json == nullptr ||
        response_json_capacity < response.size()) {
        return false;
    }
    std::memcpy(out_response_json, response.data(), response.size());
    impl_->entries.erase(found);
    return true;
}

std::string_view KernelRpcResponseStore::method(
    std::uint64_t request_id) const {
    const auto found = impl_->entries.find(request_id);
    return found == impl_->entries.end() ? std::string_view{} :
                                          std::string_view(found->second.method);
}

void KernelRpcResponseStore::clear() {
    impl_->entries.clear();
}

KernelRpcDispatcher::KernelRpcDispatcher(
    KernelRpcMethodRegistry* registry,
    KernelRpcResponseStore* response_store)
    : registry_(registry), response_store_(response_store) {}

bool KernelRpcDispatcher::invoke(
    KernelEngine& engine,
    std::string_view request_json,
    KernelRpcAuthority caller_authority,
    std::uint64_t* out_request_id) {
    if (registry_ == nullptr || response_store_ == nullptr ||
        out_request_id == nullptr ||
        !response_store_->reserve(out_request_id)) {
        return false;
    }
    const std::uint64_t request_id = *out_request_id;

    try {
        return invoke_request(
            engine,
            request_json,
            caller_authority,
            request_id);
    } catch (const std::exception&) {
        response_store_->complete_error(
            request_id,
            KernelRpcErrorCode::InternalError,
            "Internal error");
        return true;
    } catch (...) {
        response_store_->complete_error(
            request_id,
            KernelRpcErrorCode::InternalError,
            "Internal error");
        return true;
    }
}

bool KernelRpcDispatcher::invoke_request(
    KernelEngine& engine,
    std::string_view request_json,
    KernelRpcAuthority caller_authority,
    std::uint64_t request_id) {
    Json request;
    try {
        request = Json::parse(request_json.begin(), request_json.end());
    } catch (const Json::parse_error&) {
        response_store_->complete_error(
            request_id,
            KernelRpcErrorCode::ParseError,
            "Parse error");
        return true;
    }
    if (!request.is_object()) {
        response_store_->complete_error(
            request_id,
            KernelRpcErrorCode::InvalidRequest,
            "Invalid request");
        return true;
    }
    if (request.contains("id") &&
        (request["id"].is_number() || request["id"].is_string())) {
        response_store_->set_context(request_id, request["id"].dump(), "");
    }
    if (!request.contains("jsonrpc") || request["jsonrpc"] != "2.0" ||
        !request.contains("id") ||
        !(request["id"].is_number() || request["id"].is_string()) ||
        !request.contains("method") || !request["method"].is_string()) {
        response_store_->complete_error(
            request_id,
            KernelRpcErrorCode::InvalidRequest,
            "Invalid request");
        return true;
    }
    const std::string method = request["method"].get<std::string>();
    response_store_->set_context(request_id, request["id"].dump(), method);
    Json empty_params = Json::object();
    const Json* params = &empty_params;
    if (request.contains("params")) {
        if (!request["params"].is_object()) {
            response_store_->complete_error(
                request_id,
                KernelRpcErrorCode::InvalidParams,
                "Invalid params");
            return true;
        }
        params = &request["params"];
    }
    const Json& params_value = *params;
    const KernelRpcMethodDescriptor* descriptor = registry_->find(method);
    if (descriptor == nullptr) {
        response_store_->complete_error(
            request_id,
            KernelRpcErrorCode::MethodNotFound,
            "Method not found");
        return true;
    }
    if (!authority_allows(caller_authority, descriptor->authority)) {
        response_store_->complete_error(
            request_id,
            KernelRpcErrorCode::PermissionDenied,
            "Permission denied");
        return true;
    }
    if (descriptor->implementation != "implemented") {
        response_store_->complete_error(
            request_id,
            KernelRpcErrorCode::NotImplemented,
            "Not implemented");
        return true;
    }

    if (requires_running_server(*descriptor) && (!engine.running_ ||
        (engine.config_.mode != KernelMode_ListenServer &&
         engine.config_.mode != KernelMode_DedicatedServer))) {
        response_store_->complete_error(
            request_id,
            KernelRpcErrorCode::WrongExecutionPhase,
            "Wrong execution phase");
        return true;
    }
    if (descriptor->handler == nullptr ||
        !descriptor->handler(
            engine,
            params_value,
            request_id,
            *response_store_,
            *registry_)) {
        response_store_->complete_error(
            request_id,
            KernelRpcErrorCode::NotImplemented,
            "Not implemented");
    }
    return true;
}

bool KernelRpcDispatcher::poll(
    std::uint64_t request_id,
    char* out_response_json,
    std::uint32_t response_json_capacity,
    std::uint32_t* out_response_json_size) {
    return response_store_ != nullptr &&
           response_store_->poll(
               request_id,
               out_response_json,
               response_json_capacity,
               out_response_json_size);
}

void KernelRpcDispatcher::complete_simulation_command(
    std::uint64_t completion_token,
    simulation::CommandId command_id,
    const simulation::CommandResult& result) {
    if (completion_token == 0 || response_store_ == nullptr) {
        return;
    }
    KernelRpcWorldHandlers::complete_simulation_command(
        *response_store_,
        completion_token,
        command_id,
        result);
}

}  // namespace network_example
