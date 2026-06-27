#ifdef NDEBUG
#undef NDEBUG
#endif

#include <array>
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

void load_health_catalog(KernelHandle* kernel) {
    KernelColliderTemplateDefinition collider_template{};
    collider_template.struct_size = sizeof(collider_template);
    collider_template.template_id = 10;
    collider_template.shape_type = KernelColliderShapeType_Aabb;
    collider_template.center = KernelVec3{0.0f, 0.8f, 0.0f};
    collider_template.shape_params = KernelVec4{0.25f, 0.25f, 0.25f, 0.0f};
    collider_template.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;
    collider_template.purpose_flags = KernelColliderPurpose_Damage;
    std::array<KernelColliderTemplateDefinition, 1> collider_templates = {
        collider_template,
    };
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.catalog_hash = 0x1234ull;
    catalog.collider_templates = collider_templates.data();
    catalog.collider_template_count =
        static_cast<std::uint32_t>(collider_templates.size());
    assert(Kernel_LoadGameplayCatalog(kernel, &catalog));
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
    network_example::KernelRpcMethodRegistry registry;
    const network_example::KernelRpcMethodDescriptor* velocity_descriptor =
        registry.find("world.set_velocity");
    assert(velocity_descriptor != nullptr);
    assert(velocity_descriptor->parameters.size() == 2);
    assert(velocity_descriptor->parameters[0].name == "net_id");
    assert(velocity_descriptor->parameters[0].type == "uint32_t");
    assert(velocity_descriptor->parameters[0].passing == "value");
    assert(velocity_descriptor->parameters[0].direction == "input");
    assert(velocity_descriptor->parameters[1].name == "velocity");
    assert(velocity_descriptor->parameters[1].type == "KernelVec3");
    assert(velocity_descriptor->parameters[1].passing == "const_ptr");
    assert(velocity_descriptor->parameters[1].direction == "input");

    const network_example::KernelRpcMethodDescriptor* create_descriptor =
        registry.find("world.create_entity");
    assert(create_descriptor != nullptr);
    assert(create_descriptor->parameters.size() == 2);
    assert(create_descriptor->parameters[0].name == "create_info");
    assert(create_descriptor->parameters[0].type ==
           "KernelServerEntityCreateInfo");
    assert(create_descriptor->parameters[0].passing == "const_ptr");
    assert(create_descriptor->parameters[0].direction == "input");
    assert(create_descriptor->parameters[1].name == "net_id");
    assert(create_descriptor->parameters[1].type == "uint32_t");
    assert(create_descriptor->parameters[1].passing == "mutable_ptr");
    assert(create_descriptor->parameters[1].direction == "output");

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
    load_health_catalog(kernel);
    assert(Kernel_StartDedicatedServer(kernel, 7811));

    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = 1;
    create_info.actor_type = KernelActorType_Player;
    create_info.position = KernelVec3{1.0f, 0.0f, 2.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    assert(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    KernelCombatStateDefinition combat_state{};
    combat_state.struct_size = sizeof(combat_state);
    combat_state.hp = 240;
    combat_state.max_hp = 240;
    combat_state.active_weapon_id = 0;
    combat_state.collider_template_id = 10;
    combat_state.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    combat_state.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    assert(Kernel_ServerSetEntityCombatState(kernel, net_id, &combat_state));

    Json missing_rotation = poll(
        kernel,
        invoke(
            kernel,
            std::string(
                R"({"jsonrpc":"2.0","id":21,"method":"world.set_transform","params":{"net_id":)") +
                std::to_string(net_id) +
                R"(,"position":{"x":5.0,"y":0.0,"z":6.0}}})"));
    assert(missing_rotation["error"]["code"] == -32602);

    Json extra_velocity_field = poll(
        kernel,
        invoke(
            kernel,
            std::string(
                R"({"jsonrpc":"2.0","id":22,"method":"world.set_velocity","params":{"net_id":)") +
                std::to_string(net_id) +
                R"(,"velocity":{"x":2.0,"y":0.0,"z":3.0},"unexpected":true}})"));
    assert(extra_velocity_field["error"]["code"] == -32602);

    Json wrong_net_id_type = poll(
        kernel,
        invoke(
            kernel,
            R"({"jsonrpc":"2.0","id":23,"method":"world.destroy_entity","params":{"net_id":"not-a-number","reason":1}})"));
    assert(wrong_net_id_type["error"]["code"] == -32602);

    Json extra_create_info_field = poll(
        kernel,
        invoke(
            kernel,
            R"({"jsonrpc":"2.0","id":24,"method":"world.create_entity","params":{"create_info":{"entity_type":1,"actor_type":1,"owner_peer":0,"position":{"x":9.0,"y":0.0,"z":0.0},"rotation":{"x":0.0,"y":0.0,"z":0.0,"w":1.0},"animation_state":0,"visual_flags":0,"actor_template_id":0,"unexpected":1}}})"));
    assert(extra_create_info_field["error"]["code"] == -32602);

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
    assert(updated.hp == 240);
    assert(updated.max_hp == 240);

    const KernelRpcRequestId invalid_health_request = invoke(
        kernel,
        std::string(
            R"({"jsonrpc":"2.0","id":18,"method":"world.set_entity_health","params":{"net_id":)") +
            std::to_string(net_id) +
            R"(,"hp":70000}})");
    Json invalid_health = poll(kernel, invalid_health_request);
    assert(invalid_health["error"]["code"] == -32602);

    const KernelRpcRequestId health_request = invoke(
        kernel,
        std::string(
            R"({"jsonrpc":"2.0","id":19,"method":"world.set_entity_health","params":{"net_id":)") +
            std::to_string(net_id) + R"(,"hp":123}})");
    assert(!response_ready(kernel, health_request));
    Kernel_Update(kernel, 1.0f / 30.0f);
    assert(poll(kernel, health_request)["result"]["ok"] == true);

    KernelServerEntityState health_updated{};
    health_updated.struct_size = sizeof(health_updated);
    assert(Kernel_ServerGetEntityState(kernel, net_id, &health_updated));
    assert(health_updated.hp == 123);
    assert(health_updated.max_hp == 240);

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

    const KernelRpcRequestId health_destroyed_request = invoke(
        kernel,
        std::string(
            R"({"jsonrpc":"2.0","id":20,"method":"world.set_entity_health","params":{"net_id":)") +
            std::to_string(net_id) + R"(,"hp":1}})");
    Kernel_Update(kernel, 1.0f / 30.0f);
    Json health_destroyed = poll(kernel, health_destroyed_request);
    assert(health_destroyed["error"]["code"] == -32004);
    assert(health_destroyed["error"]["message"] == "Execution failed");

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
