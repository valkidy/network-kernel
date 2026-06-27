#ifndef KERNEL_SRC_KERNEL_RPC_H_
#define KERNEL_SRC_KERNEL_RPC_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace network_example {

class KernelEngine;
class KernelRpcMethodRegistry;
class KernelRpcResponseStore;

namespace simulation {
enum class CommandId : std::uint8_t;
struct Command;
struct CommandResult;
}  // namespace simulation

enum class KernelRpcAuthority {
    kDeveloperReadOnly,
    kDeveloperWrite,
    kAdmin,
    kDirectorAi,
};

enum class KernelRpcExecutionPhase {
    kImmediateReadOnly,
    kSimulationTick,
    kPostSimulation,
};

enum class KernelRpcErrorCode : int {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,

    PermissionDenied = -32001,
    WrongExecutionPhase = -32002,
    NotImplemented = -32003,
    ExecutionFailed = -32004,
    ResourceNotFound = -32005,
};

using KernelRpcMethodHandler = bool (*)(
    KernelEngine& engine,
    const nlohmann::json& params,
    std::uint64_t request_id,
    KernelRpcResponseStore& response_store,
    const KernelRpcMethodRegistry& registry);

struct KernelRpcParameterDescriptor {
    std::string name;
    std::string type;
    std::string passing;
    std::string direction;
};

struct KernelRpcMethodDescriptor {
    std::string method;
    KernelRpcAuthority authority = KernelRpcAuthority::kDeveloperReadOnly;
    KernelRpcExecutionPhase phase =
        KernelRpcExecutionPhase::kImmediateReadOnly;
    std::string implementation;
    bool internal = false;
    std::vector<KernelRpcParameterDescriptor> parameters;
    KernelRpcMethodHandler handler = nullptr;
};

class KernelRpcMethodRegistry {
public:
    KernelRpcMethodRegistry();
    ~KernelRpcMethodRegistry();

    KernelRpcMethodRegistry(const KernelRpcMethodRegistry&) = delete;
    KernelRpcMethodRegistry& operator=(const KernelRpcMethodRegistry&) = delete;

    const KernelRpcMethodDescriptor* find(std::string_view method) const;
    const std::vector<KernelRpcMethodDescriptor>& methods() const;
    std::string_view schema_json() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class KernelRpcResponseStore {
public:
    static constexpr std::size_t kCapacity = 1024;

    KernelRpcResponseStore();
    ~KernelRpcResponseStore();

    KernelRpcResponseStore(const KernelRpcResponseStore&) = delete;
    KernelRpcResponseStore& operator=(const KernelRpcResponseStore&) = delete;

    bool reserve(std::uint64_t* out_request_id);
    bool set_context(
        std::uint64_t request_id,
        std::string external_id_json,
        std::string method);
    bool complete_result(std::uint64_t request_id, std::string result_json);
    bool complete_error(
        std::uint64_t request_id,
        KernelRpcErrorCode code,
        std::string_view message);
    bool poll(
        std::uint64_t request_id,
        char* out_response_json,
        std::uint32_t response_json_capacity,
        std::uint32_t* out_response_json_size);
    std::string_view method(std::uint64_t request_id) const;
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class KernelRpcDispatcher {
public:
    KernelRpcDispatcher(
        KernelRpcMethodRegistry* registry,
        KernelRpcResponseStore* response_store);

    bool invoke(
        KernelEngine& engine,
        std::string_view request_json,
        KernelRpcAuthority caller_authority,
        std::uint64_t* out_request_id);
    bool poll(
        std::uint64_t request_id,
        char* out_response_json,
        std::uint32_t response_json_capacity,
        std::uint32_t* out_response_json_size);
    void complete_simulation_command(
        std::uint64_t completion_token,
        simulation::CommandId command_id,
        const simulation::CommandResult& result);

private:
    bool invoke_request(
        KernelEngine& engine,
        std::string_view request_json,
        KernelRpcAuthority caller_authority,
        std::uint64_t request_id);

    KernelRpcMethodRegistry* registry_ = nullptr;
    KernelRpcResponseStore* response_store_ = nullptr;
};

}  // namespace network_example

#endif  // KERNEL_SRC_KERNEL_RPC_H_
