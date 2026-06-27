#include "kernel/src/kernel_rpc.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "kernel/public/kernel_types.h"
#include "kernel/src/kernel.h"
#include "kernel/src/kernel_rpc_methods.generated.h"
#include "simulation/public/command.h"

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

bool has_exact_fields(
    const Json& object,
    std::initializer_list<std::string_view> required) {
    if (!object.is_object() || object.size() != required.size()) {
        return false;
    }
    return std::all_of(
        required.begin(),
        required.end(),
        [&object](std::string_view field) {
            return object.contains(std::string(field));
        });
}

template <typename T>
bool read_integer(const Json& object, const char* name, T* out_value) {
    if (out_value == nullptr || !object.contains(name) ||
        !(object[name].is_number_integer() ||
          object[name].is_number_unsigned())) {
        return false;
    }
    try {
        if constexpr (std::is_unsigned_v<T>) {
            std::uint64_t value = 0;
            if (object[name].is_number_unsigned()) {
                value = object[name].get<std::uint64_t>();
            } else {
                const auto signed_value = object[name].get<std::int64_t>();
                if (signed_value < 0) {
                    return false;
                }
                value = static_cast<std::uint64_t>(signed_value);
            }
            if (value > std::numeric_limits<T>::max()) {
                return false;
            }
            *out_value = static_cast<T>(value);
        } else {
            const auto value = object[name].get<std::int64_t>();
            if (value < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
                value > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
                return false;
            }
            *out_value = static_cast<T>(value);
        }
        return true;
    } catch (const Json::exception&) {
        return false;
    }
}

bool read_vec3(const Json& value, KernelVec3* out_value) {
    if (out_value == nullptr ||
        !has_exact_fields(value, {"x", "y", "z"})) {
        return false;
    }
    try {
        out_value->x = value.at("x").get<float>();
        out_value->y = value.at("y").get<float>();
        out_value->z = value.at("z").get<float>();
        return value.at("x").is_number() && value.at("y").is_number() &&
               value.at("z").is_number();
    } catch (const Json::exception&) {
        return false;
    }
}

bool read_quat(const Json& value, KernelQuat* out_value) {
    if (out_value == nullptr ||
        !has_exact_fields(value, {"x", "y", "z", "w"})) {
        return false;
    }
    try {
        out_value->x = value.at("x").get<float>();
        out_value->y = value.at("y").get<float>();
        out_value->z = value.at("z").get<float>();
        out_value->w = value.at("w").get<float>();
        return value.at("x").is_number() && value.at("y").is_number() &&
               value.at("z").is_number() && value.at("w").is_number();
    } catch (const Json::exception&) {
        return false;
    }
}

Json vec3_json(const KernelVec3& value) {
    return {{"x", value.x}, {"y", value.y}, {"z", value.z}};
}

Json quat_json(const KernelQuat& value) {
    return {
        {"x", value.x},
        {"y", value.y},
        {"z", value.z},
        {"w", value.w},
    };
}

Json entity_state_json(const KernelServerEntityState& state) {
    return {
        {"net_id", state.net_id},
        {"entity_type", state.entity_type},
        {"actor_type", state.actor_type},
        {"owner_peer", state.owner_peer},
        {"position", vec3_json(state.position)},
        {"rotation", quat_json(state.rotation)},
        {"velocity", vec3_json(state.velocity)},
        {"hp", state.hp},
        {"max_hp", state.max_hp},
        {"animation_state", state.animation_state},
        {"visual_flags", state.visual_flags},
        {"valid", state.valid},
        {"actor_template_id", state.actor_template_id},
    };
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

    if (method == "dev.ping") {
        if (!params_value.empty()) {
            response_store_->complete_error(
                request_id,
                KernelRpcErrorCode::InvalidParams,
                "Invalid params");
        } else {
            response_store_->complete_result(request_id, R"({"ok":true})");
        }
        return true;
    }
    if (method == "dev.list_methods") {
        if (!params_value.empty()) {
            response_store_->complete_error(
                request_id,
                KernelRpcErrorCode::InvalidParams,
                "Invalid params");
            return true;
        }
        Json methods = Json::array();
        for (const KernelRpcMethodDescriptor& value : registry_->methods()) {
            methods.push_back({
                {"method", value.method},
                {"authority", authority_name(value.authority)},
                {"phase", phase_name(value.phase)},
                {"implementation", value.implementation},
            });
        }
        response_store_->complete_result(
            request_id,
            Json({{"methods", methods}}).dump());
        return true;
    }
    if (method == "dev.describe_method") {
        if (!has_exact_fields(params_value, {"method"}) ||
            !params_value["method"].is_string()) {
            response_store_->complete_error(
                request_id,
                KernelRpcErrorCode::InvalidParams,
                "Invalid params");
            return true;
        }
        const std::string requested =
            params_value["method"].get<std::string>();
        const Json schema = Json::parse(registry_->schema_json());
        const auto found = std::find_if(
            schema.at("methods").begin(),
            schema.at("methods").end(),
            [&requested](const Json& value) {
                return value.at("method") == requested;
            });
        if (found == schema.at("methods").end()) {
            response_store_->complete_error(
                request_id,
                KernelRpcErrorCode::MethodNotFound,
                "Method not found");
        } else {
            response_store_->complete_result(request_id, found->dump());
        }
        return true;
    }
    if (method == "dev.get_schema") {
        if (!params_value.empty()) {
            response_store_->complete_error(
                request_id,
                KernelRpcErrorCode::InvalidParams,
                "Invalid params");
        } else {
            const Json result = {
                {"schema", Json::parse(registry_->schema_json())},
            };
            response_store_->complete_result(request_id, result.dump());
        }
        return true;
    }

    if (!engine.running_ ||
        (engine.config_.mode != KernelMode_ListenServer &&
         engine.config_.mode != KernelMode_DedicatedServer)) {
        response_store_->complete_error(
            request_id,
            KernelRpcErrorCode::WrongExecutionPhase,
            "Wrong execution phase");
        return true;
    }
    if (method == "world.get_entity_state") {
        std::uint32_t net_id = 0;
        if (!has_exact_fields(params_value, {"net_id"}) ||
            !read_integer(params_value, "net_id", &net_id)) {
            response_store_->complete_error(
                request_id,
                KernelRpcErrorCode::InvalidParams,
                "Invalid params");
            return true;
        }
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        if (!engine.server_get_entity_state(net_id, &state)) {
            response_store_->complete_error(
                request_id,
                KernelRpcErrorCode::ResourceNotFound,
                "Resource not found");
        } else {
            response_store_->complete_result(
                request_id,
                Json({{"state", entity_state_json(state)}}).dump());
        }
        return true;
    }

    simulation::Command command{};
    command.source = simulation::CommandSource::kControlPlane;
    command.completion_token = request_id;
    bool valid_params = false;
    if (method == "world.create_entity") {
        valid_params = has_exact_fields(params_value, {"create_info"}) &&
                       has_exact_fields(
                           params_value["create_info"],
                           {
                               "entity_type",
                               "actor_type",
                               "owner_peer",
                               "position",
                               "rotation",
                               "animation_state",
                               "visual_flags",
                               "actor_template_id",
                           });
        if (valid_params) {
            const Json& value = params_value["create_info"];
            KernelServerEntityCreateInfo& info =
                command.create_entity.create_info;
            info.struct_size = sizeof(info);
            valid_params =
                read_integer(value, "entity_type", &info.entity_type) &&
                read_integer(value, "actor_type", &info.actor_type) &&
                read_integer(value, "owner_peer", &info.owner_peer) &&
                read_vec3(value["position"], &info.position) &&
                read_quat(value["rotation"], &info.rotation) &&
                read_integer(
                    value,
                    "animation_state",
                    &info.animation_state) &&
                read_integer(value, "visual_flags", &info.visual_flags) &&
                read_integer(
                    value,
                    "actor_template_id",
                    &info.actor_template_id);
            command.id = simulation::CommandId::kCreateEntity;
        }
    } else if (method == "world.destroy_entity") {
        valid_params =
            has_exact_fields(params_value, {"net_id", "reason"}) &&
            read_integer(
                params_value,
                "net_id",
                &command.destroy_entity.net_id) &&
            read_integer(
                params_value,
                "reason",
                &command.destroy_entity.reason);
        command.id = simulation::CommandId::kDestroyEntity;
    } else if (method == "world.set_transform") {
        valid_params =
            has_exact_fields(
                params_value,
                {"net_id", "position", "rotation"}) &&
            read_integer(
                params_value,
                "net_id",
                &command.set_entity_transform.net_id) &&
            read_vec3(
                params_value["position"],
                &command.set_entity_transform.position) &&
            read_quat(
                params_value["rotation"],
                &command.set_entity_transform.rotation);
        command.id = simulation::CommandId::kSetEntityTransform;
    } else if (method == "world.set_velocity") {
        valid_params =
            has_exact_fields(params_value, {"net_id", "velocity"}) &&
            read_integer(
                params_value,
                "net_id",
                &command.set_entity_velocity.net_id) &&
            read_vec3(
                params_value["velocity"],
                &command.set_entity_velocity.velocity);
        command.id = simulation::CommandId::kSetEntityVelocity;
    } else if (method == "world.set_entity_state") {
        valid_params =
            has_exact_fields(
                params_value,
                {"net_id", "animation_state", "visual_flags"}) &&
            read_integer(
                params_value,
                "net_id",
                &command.set_entity_state.net_id) &&
            read_integer(
                params_value,
                "animation_state",
                &command.set_entity_state.animation_state) &&
            read_integer(
                params_value,
                "visual_flags",
                &command.set_entity_state.visual_flags);
        command.id = simulation::CommandId::kSetEntityState;
    } else if (method == "world.set_entity_health") {
        valid_params =
            has_exact_fields(params_value, {"net_id", "hp"}) &&
            read_integer(
                params_value,
                "net_id",
                &command.set_entity_state.net_id) &&
            read_integer(
                params_value,
                "hp",
                &command.set_entity_state.animation_state);
        command.id = simulation::CommandId::kSetEntityHealth;
    }
    if (!valid_params) {
        response_store_->complete_error(
            request_id,
            KernelRpcErrorCode::InvalidParams,
            "Invalid params");
        return true;
    }
    if (!engine.enqueue_simulation_command(command)) {
        response_store_->complete_error(
            request_id,
            KernelRpcErrorCode::ExecutionFailed,
            "Execution failed");
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
    if (!result.ok) {
        response_store_->complete_error(
            completion_token,
            KernelRpcErrorCode::ExecutionFailed,
            "Execution failed");
        return;
    }
    if (command_id == simulation::CommandId::kCreateEntity) {
        response_store_->complete_result(
            completion_token,
            Json({{"net_id", result.net_id}}).dump());
    } else {
        response_store_->complete_result(
            completion_token,
            R"({"ok":true})");
    }
}

}  // namespace network_example
