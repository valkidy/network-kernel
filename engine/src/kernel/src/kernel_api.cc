#include "kernel/src/kernel_api_internal.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>

#include <spdlog/spdlog.h>

#include "kernel/src/build_info.h"
#include "kernel/src/kernel.h"
#include "kernel/src/lan_discovery.h"

struct KernelHandle {
    std::unique_ptr<network_example::KernelEngine> engine;
};

struct KernelLANDiscoveryHandle {
    std::unique_ptr<network_example::LanDiscoveryService> service;
};

namespace {

template <typename Fn, typename Return>
Return abi_call(const char* name, Return fallback, Fn&& fn) {
    try {
        return fn();
    } catch (const std::exception& error) {
        spdlog::error("{} failed: {}", name, error.what());
        return fallback;
    } catch (...) {
        spdlog::error("{} failed with unknown exception", name);
        return fallback;
    }
}

template <typename Fn>
void abi_call_void(const char* name, Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& error) {
        spdlog::error("{} failed: {}", name, error.what());
    } catch (...) {
        spdlog::error("{} failed with unknown exception", name);
    }
}

}  // namespace

extern "C" {

KernelHandle* Kernel_Create(const KernelConfig* config) {
    return abi_call("Kernel_Create", static_cast<KernelHandle*>(nullptr), [&]() -> KernelHandle* {
        if (config == nullptr) {
            return nullptr;
        }
        auto* handle = new KernelHandle;
        handle->engine = std::make_unique<network_example::KernelEngine>(*config);
        return handle;
    });
}

void Kernel_Destroy(KernelHandle* kernel) {
    abi_call_void("Kernel_Destroy", [&]() {
        delete kernel;
    });
}

bool Kernel_SetPhysicsConfig(
    KernelHandle* kernel,
    const KernelPhysicsConfig* config) {
    return abi_call("Kernel_SetPhysicsConfig", false, [&]() {
        return kernel != nullptr && kernel->engine != nullptr &&
               config != nullptr &&
               config->struct_size >= sizeof(KernelPhysicsConfig) &&
               kernel->engine->set_physics_config(*config);
    });
}

bool Kernel_SetSessionRules(
    KernelHandle* kernel,
    const KernelSessionRulesConfig* config) {
    return abi_call("Kernel_SetSessionRules", false, [&]() {
        return kernel != nullptr && kernel->engine != nullptr &&
               config != nullptr &&
               config->struct_size >= sizeof(KernelSessionRulesConfig) &&
               kernel->engine->set_session_rules(*config);
    });
}

bool Kernel_SetStaticCollisionScene(
    KernelHandle* kernel,
    const KernelStaticCollisionSceneConfig* config) {
    return abi_call("Kernel_SetStaticCollisionScene", false, [&]() {
        if (kernel == nullptr || kernel->engine == nullptr) {
            spdlog::error(
                "Kernel_SetStaticCollisionScene rejected: invalid kernel handle");
            return false;
        }
        if (config == nullptr) {
            spdlog::error(
                "Kernel_SetStaticCollisionScene rejected: config is null");
            return false;
        }
        if (config->struct_size < sizeof(KernelStaticCollisionSceneConfig)) {
            spdlog::error(
                "Kernel_SetStaticCollisionScene rejected: struct_size={} "
                "required={}",
                config->struct_size,
                sizeof(KernelStaticCollisionSceneConfig));
            return false;
        }
        return kernel->engine->set_static_collision_scene(*config);
    });
}

bool Kernel_InvokeRpcCommand(
    KernelHandle* kernel,
    const char* request_json,
    uint32_t request_json_size,
    KernelRpcRequestId* out_request_id) {
    return abi_call("Kernel_InvokeRpcCommand", false, [&]() {
        return kernel != nullptr && kernel->engine != nullptr &&
               request_json != nullptr && out_request_id != nullptr &&
               kernel->engine->invoke_rpc(
                   std::string_view(request_json, request_json_size),
                   out_request_id);
    });
}

bool Kernel_PollRpcResponse(
    KernelHandle* kernel,
    KernelRpcRequestId request_id,
    char* out_response_json,
    uint32_t response_json_capacity,
    uint32_t* out_response_json_size) {
    return abi_call("Kernel_PollRpcResponse", false, [&]() {
        return kernel != nullptr && kernel->engine != nullptr &&
               kernel->engine->poll_rpc_response(
                   request_id,
                   out_response_json,
                   response_json_capacity,
                   out_response_json_size);
    });
}

bool Kernel_GetAbiInfo(KernelAbiInfo* out_info, uint32_t out_info_size) {
    return abi_call("Kernel_GetAbiInfo", false, [&]() {
        if (out_info == nullptr || out_info_size < sizeof(KernelAbiInfo)) {
            return false;
        }
        std::memset(out_info, 0, sizeof(KernelAbiInfo));
        out_info->struct_size = sizeof(KernelAbiInfo);
        out_info->abi_version = KERNEL_ABI_VERSION;
        out_info->kernel_config_size = sizeof(KernelConfig);
        out_info->player_input_size = sizeof(PlayerInput);
        out_info->render_entity_state_size = sizeof(RenderEntityState);
        out_info->kernel_event_size = sizeof(KernelEvent);
        out_info->local_player_info_size = sizeof(KernelLocalPlayerInfo);
        out_info->server_entity_create_info_size =
            sizeof(KernelServerEntityCreateInfo);
        out_info->server_entity_state_size = sizeof(KernelServerEntityState);
        out_info->weapon_mechanics_definition_size =
            sizeof(KernelWeaponMechanicsDefinition);
        out_info->projectile_mechanics_definition_size =
            sizeof(KernelProjectileMechanicsDefinition);
        out_info->area_effect_mechanics_definition_size =
            sizeof(KernelAreaEffectMechanicsDefinition);
        out_info->beam_mechanics_definition_size =
            sizeof(KernelBeamMechanicsDefinition);
        out_info->combat_state_definition_size =
            sizeof(KernelCombatStateDefinition);
        out_info->homing_mechanics_definition_size =
            sizeof(KernelHomingMechanicsDefinition);
        out_info->homing_state_size = sizeof(KernelHomingState);
        out_info->lan_discovery_server_config_size =
            sizeof(KernelLANDiscoveryServerConfig);
        out_info->lan_discovery_query_config_size =
            sizeof(KernelLANDiscoveryQueryConfig);
        out_info->lan_discovery_result_size = sizeof(KernelLANDiscoveryResult);
        out_info->gameplay_catalog_definition_size =
            sizeof(KernelGameplayCatalogDefinition);
        out_info->gameplay_catalog_load_result_size =
            sizeof(KernelGameplayCatalogLoadResult);
        out_info->gameplay_catalog_load_options_size =
            sizeof(KernelGameplayCatalogLoadOptions);
        out_info->actor_template_definition_size =
            sizeof(KernelActorTemplateDefinition);
        out_info->projectile_template_definition_size =
            sizeof(KernelProjectileTemplateDefinition);
        out_info->collider_template_definition_size =
            sizeof(KernelColliderTemplateDefinition);
        out_info->collider_binding_definition_size =
            sizeof(KernelColliderBindingDefinition);
        out_info->benchmark_stats_size = sizeof(KernelBenchmarkStats);
        out_info->network_stats_config_size = sizeof(KernelNetworkStatsConfig);
        out_info->network_stats_size = sizeof(KernelNetworkStats);
        out_info->debug_record_filter_size = sizeof(KernelDebugRecordFilter);
        out_info->debug_info_size = sizeof(KernelDebugInfo);
        out_info->collider_shape_query_size = sizeof(KernelColliderShapeQuery);
        out_info->collider_shape_view_size = sizeof(KernelColliderShapeView);
        out_info->agent_vision_config_size = sizeof(KernelAgentVisionConfig);
        out_info->vision_state_query_size = sizeof(KernelVisionStateQuery);
        out_info->vision_state_view_size = sizeof(KernelVisionStateView);
        out_info->gameplay_catalog_manifest_size =
            sizeof(KernelGameplayCatalogManifest);
        out_info->gameplay_catalog_sync_status_size =
            sizeof(KernelGameplayCatalogSyncStatus);
        out_info->entity_template_definition_size =
            sizeof(KernelEntityTemplateDefinition);
        out_info->entity_ai_definition_size =
            sizeof(KernelEntityAiDefinition);
        out_info->action_template_definition_size =
            sizeof(KernelActionTemplateDefinition);
        out_info->action_runtime_view_size = sizeof(KernelActionRuntimeView);
        out_info->local_action_result_size = sizeof(KernelLocalActionResult);
        out_info->remote_action_presentation_event_size =
            sizeof(KernelRemoteActionPresentationEvent);
        out_info->action_intent_size = sizeof(ActionIntent);
        out_info->action_input_size = sizeof(ActionInput);
        out_info->capability_flags =
            KERNEL_CAPABILITY_CLIENT_MODE |
            KERNEL_CAPABILITY_LISTEN_SERVER_MODE |
            KERNEL_CAPABILITY_DEDICATED_SERVER_MODE |
            KERNEL_CAPABILITY_INPUT_SUBMISSION |
            KERNEL_CAPABILITY_RENDER_STATES |
            KERNEL_CAPABILITY_EVENT_POLLING |
            KERNEL_CAPABILITY_CLIENT_PREDICTION |
            KERNEL_CAPABILITY_SNAPSHOT_INTERPOLATION |
            KERNEL_CAPABILITY_LAG_COMPENSATED_HITSCAN |
            KERNEL_CAPABILITY_LOCAL_PLAYER_INFO |
            KERNEL_CAPABILITY_SERVER_ENTITY_CREATE |
            KERNEL_CAPABILITY_SERVER_ENTITY_DESTROY |
            KERNEL_CAPABILITY_SERVER_ENTITY_TRANSFORM_WRITE |
            KERNEL_CAPABILITY_SERVER_ENTITY_VELOCITY_WRITE |
            KERNEL_CAPABILITY_SERVER_ENTITY_STATE_WRITE |
            KERNEL_CAPABILITY_SERVER_ENTITY_QUERY |
            KERNEL_CAPABILITY_SERVER_RELEVANCE_FILTER |
            KERNEL_CAPABILITY_LAG_COMPENSATED_PROJECTILE |
            KERNEL_CAPABILITY_EVENT_PRESENTATION_TIME |
            KERNEL_CAPABILITY_RENDER_STATES_AT_TIME |
            KERNEL_CAPABILITY_SERVER_MECHANICS_CONFIG |
            KERNEL_CAPABILITY_WEAPON_METADATA_QUERY |
            KERNEL_CAPABILITY_PROJECTILE_RESPONSE_MASKS |
            KERNEL_CAPABILITY_HOMING_PROJECTILES |
            KERNEL_CAPABILITY_LAN_DISCOVERY |
            KERNEL_CAPABILITY_GAMEPLAY_CATALOG |
            KERNEL_CAPABILITY_PROJECTILE_SPAWN_BATCH |
            KERNEL_CAPABILITY_DEBUG_RECORDS |
            KERNEL_CAPABILITY_COLLIDER_SHAPE_QUERY |
            KERNEL_CAPABILITY_BENCHMARK_STATS |
            KERNEL_CAPABILITY_NETWORK_STATS |
            KERNEL_CAPABILITY_ENTITY_LIFECYCLE_EVENTS |
            KERNEL_CAPABILITY_VISION_STATE_QUERY |
            KERNEL_CAPABILITY_GAMEPLAY_CATALOG_SYNC |
            KERNEL_CAPABILITY_CONTROL_PLANE_RPC |
            KERNEL_CAPABILITY_ACTION_TIMELINE |
            KERNEL_CAPABILITY_LOCAL_ACTION_RESULTS |
            KERNEL_CAPABILITY_REMOTE_ACTION_PRESENTATION |
            KERNEL_CAPABILITY_ACTION_INTENTS;
        return true;
    });
}

bool Kernel_GetBuildInfo(KernelBuildInfo* out_info, uint32_t out_info_size) {
    return abi_call("Kernel_GetBuildInfo", false, [&]() {
        if (out_info == nullptr || out_info_size < sizeof(KernelBuildInfo)) {
            return false;
        }
        *out_info = network_example::current_build_info();
        return true;
    });
}

bool Kernel_GetLocalPlayerInfo(
    KernelHandle* kernel,
    KernelLocalPlayerInfo* out_info) {
    return abi_call("Kernel_GetLocalPlayerInfo", false, [&]() {
        if (kernel == nullptr || out_info == nullptr) {
            return false;
        }
        *out_info = kernel->engine->local_player_info();
        return true;
    });
}

KernelLANDiscoveryHandle* Kernel_LANDiscovery_Create(void) {
    return abi_call(
        "Kernel_LANDiscovery_Create",
        static_cast<KernelLANDiscoveryHandle*>(nullptr),
        [&]() -> KernelLANDiscoveryHandle* {
            auto* handle = new KernelLANDiscoveryHandle;
            handle->service = std::make_unique<network_example::LanDiscoveryService>();
            return handle;
        });
}

void Kernel_LANDiscovery_Destroy(KernelLANDiscoveryHandle* discovery) {
    abi_call_void("Kernel_LANDiscovery_Destroy", [&]() {
        delete discovery;
    });
}

bool Kernel_LANDiscovery_StartServer(
    KernelLANDiscoveryHandle* discovery,
    const KernelLANDiscoveryServerConfig* config) {
    return abi_call("Kernel_LANDiscovery_StartServer", false, [&]() {
        return discovery != nullptr && discovery->service != nullptr &&
               config != nullptr && discovery->service->start_server(*config);
    });
}

void Kernel_LANDiscovery_StopServer(KernelLANDiscoveryHandle* discovery) {
    abi_call_void("Kernel_LANDiscovery_StopServer", [&]() {
        if (discovery != nullptr && discovery->service != nullptr) {
            discovery->service->stop_server();
        }
    });
}

bool Kernel_LANDiscovery_Query(
    KernelLANDiscoveryHandle* discovery,
    const KernelLANDiscoveryQueryConfig* config) {
    return abi_call("Kernel_LANDiscovery_Query", false, [&]() {
        return discovery != nullptr && discovery->service != nullptr &&
               config != nullptr && discovery->service->query(*config);
    });
}

uint32_t Kernel_LANDiscovery_PollResults(
    KernelLANDiscoveryHandle* discovery,
    KernelLANDiscoveryResult* out_results,
    uint32_t max_results) {
    return abi_call("Kernel_LANDiscovery_PollResults", 0u, [&]() -> std::uint32_t {
        if (discovery == nullptr || discovery->service == nullptr) {
            return 0u;
        }
        return discovery->service->poll_results(out_results, max_results);
    });
}

void Kernel_LANDiscovery_ClearResults(KernelLANDiscoveryHandle* discovery) {
    abi_call_void("Kernel_LANDiscovery_ClearResults", [&]() {
        if (discovery != nullptr && discovery->service != nullptr) {
            discovery->service->clear_results();
        }
    });
}

bool Kernel_StartClient(KernelHandle* kernel, const char* address) {
    return abi_call("Kernel_StartClient", false, [&]() {
        return kernel != nullptr && address != nullptr && address[0] != '\0' &&
               kernel->engine->start_client(address);
    });
}

bool Kernel_StartClientCatalogSync(
    KernelHandle* kernel,
    const char* address,
    const KernelGameplayCatalogSyncClientConfig* config) {
    return abi_call("Kernel_StartClientCatalogSync", false, [&]() {
        return kernel != nullptr && address != nullptr && address[0] != '\0' &&
               config != nullptr &&
               config->struct_size >= sizeof(KernelGameplayCatalogSyncClientConfig) &&
               kernel->engine->start_client_catalog_sync(address, *config);
    });
}

bool Kernel_StartListenServer(KernelHandle* kernel, uint16_t port) {
    return abi_call("Kernel_StartListenServer", false, [&]() {
        return kernel != nullptr && kernel->engine->start_listen_server(port);
    });
}

bool Kernel_StartDedicatedServer(KernelHandle* kernel, uint16_t port) {
    return abi_call("Kernel_StartDedicatedServer", false, [&]() {
        return kernel != nullptr && kernel->engine->start_dedicated_server(port);
    });
}

bool Kernel_SetGameplayCatalogSyncBundle(
    KernelHandle* kernel,
    const KernelGameplayCatalogSyncServerConfig* config,
    KernelGameplayCatalogManifest* out_manifest) {
    return abi_call("Kernel_SetGameplayCatalogSyncBundle", false, [&]() {
        return kernel != nullptr && config != nullptr &&
               config->struct_size >= sizeof(KernelGameplayCatalogSyncServerConfig) &&
               out_manifest != nullptr &&
               out_manifest->struct_size >= sizeof(KernelGameplayCatalogManifest) &&
               kernel->engine->set_gameplay_catalog_sync_bundle(
                   *config,
                   out_manifest);
    });
}

bool Kernel_GetGameplayCatalogSyncStatus(
    KernelHandle* kernel,
    KernelGameplayCatalogSyncStatus* out_status) {
    return abi_call("Kernel_GetGameplayCatalogSyncStatus", false, [&]() {
        return kernel != nullptr && out_status != nullptr &&
               out_status->struct_size >= sizeof(KernelGameplayCatalogSyncStatus) &&
               kernel->engine->get_gameplay_catalog_sync_status(out_status);
    });
}

bool Kernel_RequestGameplayCatalogBundle(KernelHandle* kernel) {
    return abi_call("Kernel_RequestGameplayCatalogBundle", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->request_gameplay_catalog_bundle();
    });
}

bool Kernel_CopyGameplayCatalogBundle(
    KernelHandle* kernel,
    uint8_t* out_bundle,
    uint32_t out_capacity,
    uint32_t* out_bundle_size) {
    return abi_call("Kernel_CopyGameplayCatalogBundle", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->copy_gameplay_catalog_bundle(
                   out_bundle,
                   out_capacity,
                   out_bundle_size);
    });
}

bool Kernel_ContinueClientHandshake(KernelHandle* kernel) {
    return abi_call("Kernel_ContinueClientHandshake", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->continue_client_handshake();
    });
}

void Kernel_Update(KernelHandle* kernel, float delta_seconds) {
    abi_call_void("Kernel_Update", [&]() {
        if (kernel != nullptr) {
            kernel->engine->update(delta_seconds);
        }
    });
}

void Kernel_SubmitInput(
    KernelHandle* kernel,
    uint32_t local_player_id,
    const PlayerInput* input) {
    abi_call_void("Kernel_SubmitInput", [&]() {
        if (kernel != nullptr && input != nullptr) {
            kernel->engine->submit_input(local_player_id, *input);
        }
    });
}

bool Kernel_LoadGameplayCatalog(
    KernelHandle* kernel,
    const KernelGameplayCatalogDefinition* catalog,
    const KernelGameplayCatalogLoadOptions* options) {
    return abi_call("Kernel_LoadGameplayCatalog", false, [&]() {
        if (kernel == nullptr || kernel->engine == nullptr ||
            catalog == nullptr) {
            return false;
        }
        if (options == nullptr) {
            return kernel->engine->load_gameplay_catalog(*catalog);
        }
        if (options->struct_size < sizeof(KernelGameplayCatalogLoadOptions)) {
            return false;
        }
        if (options->static_collision_scene == nullptr) {
            if (options->out_static_scene_rejected != nullptr) {
                *options->out_static_scene_rejected = 0u;
            }
            return kernel->engine->load_gameplay_catalog(*catalog);
        }
        if (options->static_collision_scene->struct_size <
            sizeof(KernelStaticCollisionSceneConfig)) {
            if (options->out_static_scene_rejected != nullptr) {
                *options->out_static_scene_rejected = 1u;
            }
            return false;
        }
        bool static_scene_rejected = false;
        const bool loaded = kernel->engine
            ->load_gameplay_catalog_with_static_collision_scene(
                *catalog,
                *options->static_collision_scene,
                &static_scene_rejected);
        if (options->out_static_scene_rejected != nullptr) {
            *options->out_static_scene_rejected =
                static_scene_rejected ? 1u : 0u;
        }
        return loaded;
    });
}

uint32_t Kernel_GetRenderStates(
    KernelHandle* kernel,
    RenderEntityState* out_states,
    uint32_t max_states) {
    return abi_call("Kernel_GetRenderStates", 0u, [&]() -> std::uint32_t {
        if (kernel == nullptr) {
            return 0u;
        }
        return kernel->engine->get_render_states(out_states, max_states);
    });
}

uint32_t Kernel_GetRenderStatesAtTime(
    KernelHandle* kernel,
    uint64_t client_render_time_us,
    RenderEntityState* out_states,
    uint32_t max_states) {
    return abi_call("Kernel_GetRenderStatesAtTime", 0u, [&]() -> std::uint32_t {
        if (kernel == nullptr) {
            return 0u;
        }
        return kernel->engine->get_render_states_at_time(
            client_render_time_us,
            out_states,
            max_states);
    });
}

uint32_t Kernel_PollEvents(
    KernelHandle* kernel,
    KernelEvent* out_events,
    uint32_t max_events) {
    return abi_call("Kernel_PollEvents", 0u, [&]() -> std::uint32_t {
        if (kernel == nullptr) {
            return 0u;
        }
        return kernel->engine->poll_events(out_events, max_events);
    });
}

uint32_t Kernel_PollEntityLifecycleEvents(
    KernelHandle* kernel,
    KernelEntityLifecycleEvent* out_events,
    uint32_t max_events) {
    return abi_call("Kernel_PollEntityLifecycleEvents", 0u, [&]() -> std::uint32_t {
        if (kernel == nullptr) {
            return 0u;
        }
        return kernel->engine->poll_entity_lifecycle_events(out_events, max_events);
    });
}

uint32_t Kernel_PollLocalActionResults(
    KernelHandle* kernel,
    KernelLocalActionResult* out_results,
    uint32_t max_results) {
    return abi_call("Kernel_PollLocalActionResults", 0u, [&]() -> std::uint32_t {
        if (kernel == nullptr) {
            return 0u;
        }
        return kernel->engine->poll_local_action_results(out_results, max_results);
    });
}

uint32_t Kernel_PollRemoteActionPresentationEvents(
    KernelHandle* kernel,
    KernelRemoteActionPresentationEvent* out_events,
    uint32_t max_events) {
    return abi_call(
        "Kernel_PollRemoteActionPresentationEvents",
        0u,
        [&]() -> std::uint32_t {
            if (kernel == nullptr) {
                return 0u;
            }
            return kernel->engine->poll_remote_action_presentation_events(
                out_events,
                max_events);
        });
}

bool Kernel_GetBenchmarkStats(
    KernelHandle* kernel,
    KernelBenchmarkStats* out_stats) {
    return abi_call("Kernel_GetBenchmarkStats", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->get_benchmark_stats(out_stats);
    });
}

bool Kernel_GetNetworkStats(
    KernelHandle* kernel,
    KernelNetworkStats* out_stats) {
    return abi_call("Kernel_GetNetworkStats", false, [&]() {
        return kernel != nullptr && kernel->engine->get_network_stats(out_stats);
    });
}

uint32_t Kernel_PollDebugRecords(
    KernelHandle* kernel,
    const KernelDebugRecordFilter* filter,
    KernelDebugInfo* out_records,
    uint32_t max_records) {
    return abi_call("Kernel_PollDebugRecords", 0u, [&]() -> std::uint32_t {
        if (kernel == nullptr) {
            return 0u;
        }
        return kernel->engine->poll_debug_records(filter, out_records, max_records);
    });
}

uint32_t Kernel_QueryColliderShapes(
    KernelHandle* kernel,
    const KernelColliderShapeQuery* query,
    KernelColliderShapeView* out_shapes,
    uint32_t max_shapes) {
    return abi_call("Kernel_QueryColliderShapes", 0u, [&]() -> std::uint32_t {
        if (kernel == nullptr) {
            return 0u;
        }
        return kernel->engine->query_collider_shapes(query, out_shapes, max_shapes);
    });
}

uint32_t Kernel_QueryVisionState(
    KernelHandle* kernel,
    const KernelVisionStateQuery* query,
    KernelVisionStateView* out_states,
    uint32_t max_states) {
    return abi_call("Kernel_QueryVisionState", 0u, [&]() -> std::uint32_t {
        if (kernel == nullptr) {
            return 0u;
        }
        return kernel->engine->query_vision_state(query, out_states, max_states);
    });
}

uint32_t Kernel_GetProjectileTemplates(
    KernelHandle* kernel,
    KernelProjectileTemplateDefinition* out_templates,
    uint32_t max_templates) {
    return abi_call("Kernel_GetProjectileTemplates", 0u, [&]() -> std::uint32_t {
        if (kernel == nullptr) {
            return 0u;
        }
        return kernel->engine->get_projectile_templates(
            out_templates,
            max_templates);
    });
}

bool Kernel_GetActionTemplate(
    KernelHandle* kernel,
    uint32_t action_template_id,
    KernelActionTemplateDefinition* out_definition) {
    return abi_call("Kernel_GetActionTemplate", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->get_action_template(
                   action_template_id,
                   out_definition);
    });
}

uint32_t Kernel_GetActorTemplates(
    KernelHandle* kernel,
    KernelActorTemplateDefinition* out_templates,
    uint32_t max_templates) {
    return abi_call("Kernel_GetActorTemplates", 0u, [&]() -> std::uint32_t {
        if (kernel == nullptr) {
            return 0u;
        }
        return kernel->engine->get_actor_templates(out_templates, max_templates);
    });
}

uint32_t Kernel_GetColliderTemplates(
    KernelHandle* kernel,
    KernelColliderTemplateDefinition* out_templates,
    uint32_t max_templates) {
    return abi_call("Kernel_GetColliderTemplates", 0u, [&]() -> std::uint32_t {
        if (kernel == nullptr) {
            return 0u;
        }
        return kernel->engine->get_collider_templates(
            out_templates,
            max_templates);
    });
}

uint32_t Kernel_GetColliderBindings(
    KernelHandle* kernel,
    KernelColliderBindingDefinition* out_bindings,
    uint32_t max_bindings) {
    return abi_call("Kernel_GetColliderBindings", 0u, [&]() -> std::uint32_t {
        if (kernel == nullptr) {
            return 0u;
        }
        return kernel->engine->get_collider_bindings(out_bindings, max_bindings);
    });
}

bool Kernel_ServerCreateEntity(
    KernelHandle* kernel,
    const KernelServerEntityCreateInfo* create_info,
    uint32_t* out_net_id) {
    return abi_call("Kernel_ServerCreateEntity", false, [&]() {
        return kernel != nullptr && create_info != nullptr &&
               kernel->engine->server_create_entity(*create_info, out_net_id);
    });
}

bool Kernel_ServerActivateEntity(
    KernelHandle* kernel,
    const KernelServerEntityActivateInfo* activate_info) {
    return abi_call("Kernel_ServerActivateEntity", false, [&]() {
        return kernel != nullptr && activate_info != nullptr &&
               kernel->engine->server_activate_entity(*activate_info);
    });
}

bool Kernel_ServerDestroyEntity(
    KernelHandle* kernel,
    uint32_t net_id,
    uint32_t reason) {
    return abi_call("Kernel_ServerDestroyEntity", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->server_destroy_entity(net_id, reason);
    });
}

bool Kernel_ServerEnqueueEntityLifecycle(
    KernelHandle* kernel,
    uint32_t command_source,
    const KernelEntityLifecycleCommand* command) {
    return abi_call("Kernel_ServerEnqueueEntityLifecycle", false, [&]() {
        return kernel != nullptr && command != nullptr &&
               kernel->engine->server_enqueue_entity_lifecycle(
                   command_source,
                   *command);
    });
}

bool Kernel_ServerSetEntityTransform(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelVec3* position,
    const KernelQuat* rotation) {
    return abi_call("Kernel_ServerSetEntityTransform", false, [&]() {
        return kernel != nullptr && position != nullptr && rotation != nullptr &&
               kernel->engine->server_set_entity_transform(
                   net_id,
                   *position,
                   *rotation);
    });
}

bool Kernel_ServerEnqueueEntityTransform(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    const KernelVec3* position,
    const KernelQuat* rotation) {
    return abi_call("Kernel_ServerEnqueueEntityTransform", false, [&]() {
        return kernel != nullptr && position != nullptr && rotation != nullptr &&
               kernel->engine->server_enqueue_entity_transform(
                   command_source,
                   net_id,
                   *position,
                   *rotation);
    });
}

bool Kernel_ServerSetEntityVelocity(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelVec3* velocity) {
    return abi_call("Kernel_ServerSetEntityVelocity", false, [&]() {
        return kernel != nullptr && velocity != nullptr &&
               kernel->engine->server_set_entity_velocity(net_id, *velocity);
    });
}

bool Kernel_ServerEnqueueEntityVelocity(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    const KernelVec3* velocity) {
    return abi_call("Kernel_ServerEnqueueEntityVelocity", false, [&]() {
        return kernel != nullptr && velocity != nullptr &&
               kernel->engine->server_enqueue_entity_velocity(
                   command_source,
                   net_id,
                   *velocity);
    });
}

bool Kernel_ServerSetEntityState(
    KernelHandle* kernel,
    uint32_t net_id,
    uint16_t animation_state,
    uint32_t visual_flags) {
    return abi_call("Kernel_ServerSetEntityState", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->server_set_entity_state(
                   net_id,
                   animation_state,
                   visual_flags);
    });
}

bool Kernel_ServerSetEntityHealth(
    KernelHandle* kernel,
    uint32_t net_id,
    uint16_t hp) {
    return abi_call("Kernel_ServerSetEntityHealth", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->server_set_entity_health(net_id, hp);
    });
}

bool Kernel_ServerEnqueueEntityState(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    uint16_t animation_state,
    uint32_t visual_flags) {
    return abi_call("Kernel_ServerEnqueueEntityState", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->server_enqueue_entity_state(
                   command_source,
                   net_id,
                   animation_state,
                   visual_flags);
    });
}

bool Kernel_ServerSubmitEntityInput(
    KernelHandle* kernel,
    uint32_t net_id,
    const PlayerInput* input) {
    return abi_call("Kernel_ServerSubmitEntityInput", false, [&]() {
        return kernel != nullptr && input != nullptr &&
               kernel->engine->server_submit_entity_input(net_id, *input);
    });
}

bool Kernel_ServerEnqueueEntityInput(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    const PlayerInput* input) {
    return abi_call("Kernel_ServerEnqueueEntityInput", false, [&]() {
        return kernel != nullptr && input != nullptr &&
               kernel->engine->server_enqueue_entity_input(
                   command_source,
                   net_id,
                   *input);
    });
}

bool Kernel_ServerSetEntityCombatState(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelCombatStateDefinition* combat_state) {
    return abi_call("Kernel_ServerSetEntityCombatState", false, [&]() {
        return kernel != nullptr && combat_state != nullptr &&
               kernel->engine->server_set_entity_combat_state(
                   net_id,
                   *combat_state);
    });
}

bool Kernel_ServerSetEntityActorTemplate(
    KernelHandle* kernel,
    uint32_t net_id,
    uint32_t actor_template_id) {
    return abi_call("Kernel_ServerSetEntityActorTemplate", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->server_set_entity_actor_template(
                   net_id,
                   actor_template_id);
    });
}

bool Kernel_ServerSetEntityVisionConfig(
    KernelHandle* kernel,
    std::uint32_t net_id,
    const KernelAgentVisionConfig* vision_config) {
    return abi_call("Kernel_ServerSetEntityVisionConfig", false, [&]() {
        return kernel != nullptr && vision_config != nullptr &&
               kernel->engine->server_set_entity_vision_config(
                   net_id,
                   *vision_config);
    });
}

bool Kernel_ServerClearEntityVisionConfig(
    KernelHandle* kernel,
    std::uint32_t net_id) {
    return abi_call("Kernel_ServerClearEntityVisionConfig", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->server_clear_entity_vision_config(net_id);
    });
}

bool Kernel_ServerSetEntityWeaponMechanics(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelWeaponMechanicsDefinition* weapon_mechanics) {
    return abi_call("Kernel_ServerSetEntityWeaponMechanics", false, [&]() {
        return kernel != nullptr && weapon_mechanics != nullptr &&
               kernel->engine->server_set_entity_weapon_mechanics(
                   net_id,
                   *weapon_mechanics);
    });
}

bool Kernel_ServerClearEntityWeaponMechanics(
    KernelHandle* kernel,
    uint32_t net_id,
    uint8_t weapon_id) {
    return abi_call("Kernel_ServerClearEntityWeaponMechanics", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->server_clear_entity_weapon_mechanics(
                   net_id,
                   weapon_id);
    });
}

bool Kernel_ServerGetEntityWeaponMechanics(
    KernelHandle* kernel,
    uint32_t net_id,
    uint8_t weapon_id,
    KernelWeaponMechanicsDefinition* out_weapon_mechanics) {
    return abi_call("Kernel_ServerGetEntityWeaponMechanics", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->server_get_entity_weapon_mechanics(
                   net_id,
                   weapon_id,
                   out_weapon_mechanics);
    });
}

bool Kernel_ServerGetHomingState(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelHomingState* out_state) {
    return abi_call("Kernel_ServerGetHomingState", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->server_get_homing_state(net_id, out_state);
    });
}

bool Kernel_ServerValidateMechanicsConfig(
    const KernelWeaponMechanicsDefinition* weapon_mechanics) {
    return abi_call("Kernel_ServerValidateMechanicsConfig", false, [&]() {
        if (weapon_mechanics == nullptr ||
            weapon_mechanics->struct_size < sizeof(KernelWeaponMechanicsDefinition) ||
            weapon_mechanics->magazine_size == 0 ||
            (weapon_mechanics->fire_mode != KernelWeaponFireMode_Projectile &&
             weapon_mechanics->damage == 0) ||
            weapon_mechanics->fire_action_template_id == 0u ||
            weapon_mechanics->reload_action_template_id == 0u ||
            weapon_mechanics->fire_mode > KernelWeaponFireMode_Projectile) {
            return false;
        }
        if (weapon_mechanics->fire_mode == KernelWeaponFireMode_Projectile) {
            return weapon_mechanics->projectile_template_id != 0;
        }
        if (weapon_mechanics->max_range <= 0.0f) {
            return false;
        }
        return weapon_mechanics->fire_mode != KernelWeaponFireMode_Shotgun ||
               weapon_mechanics->pellet_count != 0;
    });
}

bool Kernel_ServerGetEntityState(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelServerEntityState* out_state) {
    return abi_call("Kernel_ServerGetEntityState", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->server_get_entity_state(net_id, out_state);
    });
}

uint32_t Kernel_ServerQueryEntities(
    KernelHandle* kernel,
    uint16_t entity_type_filter,
    KernelServerEntityState* out_states,
    uint32_t max_states) {
    return abi_call("Kernel_ServerQueryEntities", 0u, [&]() -> std::uint32_t {
        if (kernel == nullptr) {
            return 0u;
        }
        return kernel->engine->server_query_entities(
            static_cast<network_example::EntityType>(entity_type_filter),
            out_states,
            max_states);
    });
}

}  // extern "C"
