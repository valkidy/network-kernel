#include "kernel/public/kernel_api.h"

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
        out_info->combat_state_definition_size =
            sizeof(KernelCombatStateDefinition);
        out_info->area_effect_mechanics_definition_size =
            sizeof(KernelAreaEffectMechanicsDefinition);
        out_info->area_effect_state_size = sizeof(KernelAreaEffectState);
        out_info->beam_mechanics_definition_size =
            sizeof(KernelBeamMechanicsDefinition);
        out_info->beam_state_size = sizeof(KernelBeamState);
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
        out_info->projectile_template_definition_size =
            sizeof(KernelProjectileTemplateDefinition);
        out_info->collider_template_definition_size =
            sizeof(KernelColliderTemplateDefinition);
        out_info->collider_binding_definition_size =
            sizeof(KernelColliderBindingDefinition);
        out_info->benchmark_stats_size = sizeof(KernelBenchmarkStats);
        out_info->network_stats_size = sizeof(KernelNetworkStats);
        out_info->debug_record_filter_size = sizeof(KernelDebugRecordFilter);
        out_info->debug_info_size = sizeof(KernelDebugInfo);
        out_info->collider_shape_query_size = sizeof(KernelColliderShapeQuery);
        out_info->collider_shape_view_size = sizeof(KernelColliderShapeView);
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
            KERNEL_CAPABILITY_AREA_EFFECT_WEAPONS |
            KERNEL_CAPABILITY_PROJECTILE_RESPONSE_MASKS |
            KERNEL_CAPABILITY_BEAM_WEAPONS |
            KERNEL_CAPABILITY_HOMING_PROJECTILES |
            KERNEL_CAPABILITY_LAN_DISCOVERY |
            KERNEL_CAPABILITY_GAMEPLAY_CATALOG |
            KERNEL_CAPABILITY_PROJECTILE_SPAWN_BATCH |
            KERNEL_CAPABILITY_DEBUG_RECORDS |
            KERNEL_CAPABILITY_COLLIDER_SHAPE_QUERY |
            KERNEL_CAPABILITY_BENCHMARK_STATS |
            KERNEL_CAPABILITY_NETWORK_STATS;
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
    const KernelGameplayCatalogDefinition* catalog) {
    return abi_call("Kernel_LoadGameplayCatalog", false, [&]() {
        return kernel != nullptr && catalog != nullptr &&
               kernel->engine->load_gameplay_catalog(*catalog);
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

bool Kernel_ServerCreateEntity(
    KernelHandle* kernel,
    const KernelServerEntityCreateInfo* create_info,
    uint32_t* out_net_id) {
    return abi_call("Kernel_ServerCreateEntity", false, [&]() {
        return kernel != nullptr && create_info != nullptr &&
               kernel->engine->server_create_entity(*create_info, out_net_id);
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

bool Kernel_ServerSetEntityVelocity(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelVec3* velocity) {
    return abi_call("Kernel_ServerSetEntityVelocity", false, [&]() {
        return kernel != nullptr && velocity != nullptr &&
               kernel->engine->server_set_entity_velocity(net_id, *velocity);
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

bool Kernel_ServerSubmitEntityInput(
    KernelHandle* kernel,
    uint32_t net_id,
    const PlayerInput* input) {
    return abi_call("Kernel_ServerSubmitEntityInput", false, [&]() {
        return kernel != nullptr && input != nullptr &&
               kernel->engine->server_submit_entity_input(net_id, *input);
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

bool Kernel_ServerGetAreaEffectState(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelAreaEffectState* out_state) {
    return abi_call("Kernel_ServerGetAreaEffectState", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->server_get_area_effect_state(net_id, out_state);
    });
}

bool Kernel_ServerGetBeamState(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelBeamState* out_state) {
    return abi_call("Kernel_ServerGetBeamState", false, [&]() {
        return kernel != nullptr &&
               kernel->engine->server_get_beam_state(net_id, out_state);
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
            weapon_mechanics->weapon_id >= KERNEL_MAX_WEAPONS ||
            weapon_mechanics->magazine_size == 0 ||
            weapon_mechanics->damage == 0 ||
            weapon_mechanics->cooldown_ticks == 0 ||
            weapon_mechanics->reload_ticks == 0 ||
            weapon_mechanics->fire_mode > KernelWeaponFireMode_Beam) {
            return false;
        }
        if (weapon_mechanics->fire_mode == KernelWeaponFireMode_Projectile) {
            return weapon_mechanics->projectile.struct_size >=
                       sizeof(KernelProjectileMechanicsDefinition) &&
                    weapon_mechanics->projectile.motion_model <=
                       KernelProjectileMotionModel_Homing &&
                   weapon_mechanics->projectile.hit_response <=
                       KernelProjectileHitResponse_Attach &&
                   weapon_mechanics->projectile.damage_shape <=
                       KernelProjectileDamageShape_PiercingSegment &&
                   weapon_mechanics->projectile.hit_response !=
                       KernelProjectileHitResponse_Bounce &&
                   weapon_mechanics->projectile.hit_response !=
                       KernelProjectileHitResponse_Attach &&
                   weapon_mechanics->projectile.max_hit_count > 0 &&
                   weapon_mechanics->projectile.speed > 0.0f &&
                   weapon_mechanics->projectile.lifetime_seconds > 0.0f &&
                   (weapon_mechanics->projectile.motion_model !=
                            KernelProjectileMotionModel_Homing
                        ? weapon_mechanics->projectile.homing.struct_size == 0
                        : weapon_mechanics->projectile.homing.struct_size >=
                                  sizeof(KernelHomingMechanicsDefinition) &&
                              weapon_mechanics->projectile.homing.homing_mode ==
                                  KernelHomingMode_FireAndForget &&
                              weapon_mechanics->projectile.homing.sync_mode ==
                                  KernelProjectileSyncMode_HybridDeterministicThenSnapshot &&
                              weapon_mechanics->projectile.homing.lock_on_range > 0.0f &&
                              weapon_mechanics->projectile.homing.lose_target_range >=
                                  weapon_mechanics->projectile.homing.lock_on_range &&
                              weapon_mechanics->projectile.homing.lock_cone_degrees > 0.0f &&
                              weapon_mechanics->projectile.homing.lock_cone_degrees <= 180.0f &&
                              weapon_mechanics->projectile.homing
                                      .max_turn_rate_degrees_per_second > 0.0f &&
                              weapon_mechanics->projectile.homing.acceleration > 0.0f &&
                              weapon_mechanics->projectile.homing.max_speed > 0.0f);
        }
        if (weapon_mechanics->fire_mode == KernelWeaponFireMode_AreaEffect) {
            return weapon_mechanics->area_effect.struct_size >=
                       sizeof(KernelAreaEffectMechanicsDefinition) &&
                   weapon_mechanics->area_effect.radius > 0.0f &&
                   weapon_mechanics->area_effect.damage_per_interval > 0 &&
                   weapon_mechanics->area_effect.damage_interval_ticks > 0 &&
                   weapon_mechanics->area_effect.lifetime_ticks > 0 &&
                   weapon_mechanics->area_effect.spawn_distance >= 0.0f;
        }
        if (weapon_mechanics->fire_mode == KernelWeaponFireMode_Beam) {
            return weapon_mechanics->beam.struct_size >=
                       sizeof(KernelBeamMechanicsDefinition) &&
                   weapon_mechanics->beam.length > 0.0f &&
                   weapon_mechanics->beam.radius > 0.0f &&
                   weapon_mechanics->beam.damage_per_second > 0 &&
                   weapon_mechanics->beam.lifetime_ticks > 0;
        }
        if (weapon_mechanics->fire_mode != KernelWeaponFireMode_Beam &&
            weapon_mechanics->max_range <= 0.0f) {
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
