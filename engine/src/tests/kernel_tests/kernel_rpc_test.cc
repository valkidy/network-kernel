#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "kernel/public/kernel_api.h"
#include "kernel/src/kernel_rpc.h"

namespace {

using Json = nlohmann::json;

static_assert(
    static_cast<int>(network_example::KernelRpcErrorCode::ParseError) ==
    -32700);
static_assert(
    static_cast<int>(
        network_example::KernelRpcErrorCode::WrongExecutionPhase) == -32002);
static_assert(
    static_cast<int>(network_example::KernelRpcErrorCode::ExecutionFailed) ==
    -32004);
static_assert(
    static_cast<int>(network_example::KernelRpcErrorCode::ResourceNotFound) ==
    -32005);

KernelConfig server_config() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    return config;
}

KernelRpcRequestId invoke(KernelHandle* kernel, const std::string& request) {
    KernelRpcRequestId request_id = 0;
    assert(Kernel_InvokeRpcCommand(
        kernel,
        request.data(),
        static_cast<std::uint32_t>(request.size()),
        &request_id));
    assert(request_id != 0);
    return request_id;
}

bool response_ready(KernelHandle* kernel, KernelRpcRequestId request_id) {
    std::uint32_t required_size = 0;
    return !Kernel_PollRpcResponse(
               kernel,
               request_id,
               nullptr,
               0,
               &required_size) &&
           required_size != 0;
}

Json poll(KernelHandle* kernel, KernelRpcRequestId request_id) {
    std::uint32_t required_size = 0;
    assert(!Kernel_PollRpcResponse(
        kernel,
        request_id,
        nullptr,
        0,
        &required_size));
    assert(required_size != 0);

    std::vector<char> response(required_size);
    std::uint32_t response_size = 0;
    assert(Kernel_PollRpcResponse(
        kernel,
        request_id,
        response.data(),
        static_cast<std::uint32_t>(response.size()),
        &response_size));
    assert(response_size == required_size);
    return Json::parse(response.begin(), response.end());
}

void dev_methods_and_protocol_errors() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);

    Json ping = poll(
        kernel,
        invoke(
            kernel,
            R"({"jsonrpc":"2.0","id":"ping-1","method":"dev.ping","params":{}})"));
    assert(ping["jsonrpc"] == "2.0");
    assert(ping["id"] == "ping-1");
    assert(ping["result"]["ok"] == true);

    Json ping_without_params = poll(
        kernel,
        invoke(
            kernel,
            R"({"jsonrpc":"2.0","id":"ping-2","method":"dev.ping"})"));
    assert(ping_without_params["result"]["ok"] == true);

    Json parse_error = poll(kernel, invoke(kernel, "{broken"));
    assert(parse_error["id"].is_null());
    assert(parse_error["error"]["code"] == -32700);

    Json invalid_request = poll(
        kernel,
        invoke(
            kernel,
            R"({"jsonrpc":"2.0","method":"dev.ping","params":{}})"));
    assert(invalid_request["error"]["code"] == -32600);

    Json invalid_params = poll(
        kernel,
        invoke(
            kernel,
            R"({"jsonrpc":"2.0","id":2,"method":"dev.ping","params":[]})"));
    assert(invalid_params["error"]["code"] == -32602);

    Json batch = poll(
        kernel,
        invoke(
            kernel,
            R"([{"jsonrpc":"2.0","id":2,"method":"dev.ping","params":{}}])"));
    assert(batch["error"]["code"] == -32600);

    Json unknown = poll(
        kernel,
        invoke(
            kernel,
            R"({"jsonrpc":"2.0","id":2,"method":"dev.unknown","params":{}})"));
    assert(unknown["error"]["code"] == -32601);

    Json methods = poll(
        kernel,
        invoke(
            kernel,
            R"({"jsonrpc":"2.0","id":3,"method":"dev.list_methods","params":{}})"));
    bool found_ping = false;
    bool found_transform = false;
    for (const Json& method : methods["result"]["methods"]) {
        found_ping = found_ping || method["method"] == "dev.ping";
        found_transform =
            found_transform || method["method"] == "world.set_transform";
    }
    assert(found_ping);
    assert(found_transform);

    Json description = poll(
        kernel,
        invoke(
            kernel,
            R"({"jsonrpc":"2.0","id":4,"method":"dev.describe_method","params":{"method":"world.set_transform"}})"));
    assert(description["result"]["method"] == "world.set_transform");
    assert(description["result"]["phase"] == "simulation_tick");

    Json schema = poll(
        kernel,
        invoke(
            kernel,
            R"({"jsonrpc":"2.0","id":5,"method":"dev.get_schema","params":{}})"));
    assert(schema["result"]["schema"]["reserved_scopes"].size() == 3);

    Json non_server = poll(
        kernel,
        invoke(
            kernel,
            R"({"jsonrpc":"2.0","id":6,"method":"world.get_entity_state","params":{"net_id":1}})"));
    assert(non_server["error"]["code"] == -32002);
    assert(non_server["error"]["message"] == "Wrong execution phase");

    assert(!Kernel_PollRpcResponse(kernel, 999999, nullptr, 0, nullptr));
    Kernel_Destroy(kernel);
}

void response_store_capacity_is_bounded() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);

    std::vector<KernelRpcRequestId> requests;
    requests.reserve(1024);
    for (std::uint32_t index = 0; index < 1024; ++index) {
        requests.push_back(invoke(
            kernel,
            std::string(
                R"({"jsonrpc":"2.0","id":)" + std::to_string(index) +
                R"(,"method":"dev.ping","params":{}})")));
    }
    KernelRpcRequestId rejected_id = 0;
    const std::string request =
        R"({"jsonrpc":"2.0","id":1025,"method":"dev.ping","params":{}})";
    assert(!Kernel_InvokeRpcCommand(
        kernel,
        request.data(),
        static_cast<std::uint32_t>(request.size()),
        &rejected_id));

    for (KernelRpcRequestId request_id : requests) {
        poll(kernel, request_id);
    }
    Kernel_Destroy(kernel);
}

void query_and_mutation_phase_behavior() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartDedicatedServer(kernel, 7811));

    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = 1;
    create_info.actor_type = KernelActorType_Player;
    create_info.position = KernelVec3{1.0f, 0.0f, 2.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    assert(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));

    Json state = poll(
        kernel,
        invoke(
            kernel,
            std::string(
                R"({"jsonrpc":"2.0","id":10,"method":"world.get_entity_state","params":{"net_id":)") +
                std::to_string(net_id) + "}}"));
    assert(state["result"]["state"]["net_id"] == net_id);
    assert(state["result"]["state"]["position"]["x"] == 1.0f);

    Json missing_state = poll(
        kernel,
        invoke(
            kernel,
            R"({"jsonrpc":"2.0","id":16,"method":"world.get_entity_state","params":{"net_id":4294967295}})"));
    assert(missing_state["error"]["code"] == -32005);
    assert(missing_state["error"]["message"] == "Resource not found");

    const KernelRpcRequestId transform_request = invoke(
        kernel,
        std::string(
            R"({"jsonrpc":"2.0","id":11,"method":"world.set_transform","params":{"net_id":)") +
            std::to_string(net_id) +
            R"(,"position":{"x":5.0,"y":0.0,"z":6.0},"rotation":{"x":0.0,"y":0.0,"z":0.0,"w":1.0}}})");
    assert(!response_ready(kernel, transform_request));

    KernelServerEntityState before{};
    before.struct_size = sizeof(before);
    assert(Kernel_ServerGetEntityState(kernel, net_id, &before));
    assert(before.position.x == 1.0f);

    Kernel_Update(kernel, 1.0f / 30.0f);
    Json transform = poll(kernel, transform_request);
    assert(transform["result"]["ok"] == true);

    KernelServerEntityState after{};
    after.struct_size = sizeof(after);
    assert(Kernel_ServerGetEntityState(kernel, net_id, &after));
    assert(after.position.x == 5.0f);
    assert(after.position.z == 6.0f);

    const KernelRpcRequestId velocity_request = invoke(
        kernel,
        std::string(
            R"({"jsonrpc":"2.0","id":13,"method":"world.set_velocity","params":{"net_id":)") +
            std::to_string(net_id) +
            R"(,"velocity":{"x":2.0,"y":0.0,"z":3.0}}})");
    const KernelRpcRequestId state_request = invoke(
        kernel,
        std::string(
            R"({"jsonrpc":"2.0","id":14,"method":"world.set_entity_state","params":{"net_id":)") +
            std::to_string(net_id) +
            R"(,"animation_state":7,"visual_flags":9}})");
    assert(!response_ready(kernel, velocity_request));
    assert(!response_ready(kernel, state_request));
    Kernel_Update(kernel, 1.0f / 30.0f);
    assert(poll(kernel, velocity_request)["result"]["ok"] == true);
    assert(poll(kernel, state_request)["result"]["ok"] == true);

    KernelServerEntityState updated{};
    updated.struct_size = sizeof(updated);
    assert(Kernel_ServerGetEntityState(kernel, net_id, &updated));
    assert(updated.velocity.x == 2.0f);
    assert(updated.velocity.z == 3.0f);
    assert(updated.animation_state == 7);
    assert((updated.visual_flags & 9u) == 9u);

    const KernelRpcRequestId create_request = invoke(
        kernel,
        R"({"jsonrpc":"2.0","id":12,"method":"world.create_entity","params":{"create_info":{"entity_type":1,"actor_type":1,"owner_peer":0,"position":{"x":9.0,"y":0.0,"z":0.0},"rotation":{"x":0.0,"y":0.0,"z":0.0,"w":1.0},"animation_state":0,"visual_flags":0,"actor_template_id":0}}})");
    assert(!response_ready(kernel, create_request));
    Kernel_Update(kernel, 1.0f / 30.0f);
    Json created = poll(kernel, create_request);
    assert(created["result"]["net_id"].get<std::uint32_t>() != 0);

    const KernelRpcRequestId destroy_request = invoke(
        kernel,
        std::string(
            R"({"jsonrpc":"2.0","id":15,"method":"world.destroy_entity","params":{"net_id":)") +
            std::to_string(net_id) +
            R"(,"reason":1}})");
    assert(!response_ready(kernel, destroy_request));
    Kernel_Update(kernel, 1.0f / 30.0f);
    assert(poll(kernel, destroy_request)["result"]["ok"] == true);
    KernelServerEntityState destroyed{};
    destroyed.struct_size = sizeof(destroyed);
    assert(!Kernel_ServerGetEntityState(kernel, net_id, &destroyed));

    const KernelRpcRequestId destroy_missing_request = invoke(
        kernel,
        std::string(
            R"({"jsonrpc":"2.0","id":17,"method":"world.destroy_entity","params":{"net_id":)") +
            std::to_string(net_id) +
            R"(,"reason":1}})");
    Kernel_Update(kernel, 1.0f / 30.0f);
    Json destroy_missing = poll(kernel, destroy_missing_request);
    assert(destroy_missing["error"]["code"] == -32004);
    assert(destroy_missing["error"]["message"] == "Execution failed");

    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    dev_methods_and_protocol_errors();
    response_store_capacity_is_bounded();
    query_and_mutation_phase_behavior();
    return 0;
}
