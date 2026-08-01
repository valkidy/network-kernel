#include <array>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <dlfcn.h>

#include "game_server/public/game_server_api.h"
#include "kernel/public/kernel_api.h"

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

bool all_digits(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
        if (!std::isdigit(static_cast<unsigned char>(*cursor))) {
            return false;
        }
    }
    return true;
}

bool has_version_revision_suffix(const char* value) {
    const std::string text = value == nullptr ? "" : value;
    constexpr const char* kPrefix = "0.7.0+r";
    if (text.rfind(kPrefix, 0) != 0 || text.size() == std::strlen(kPrefix)) {
        return false;
    }
    return all_digits(text.c_str() + std::strlen(kPrefix));
}

template <typename Signature>
Signature* load_symbol(void* library, const char* name) {
    dlerror();
    void* symbol = dlsym(library, name);
    const char* error = dlerror();
    assert(error == nullptr);
    assert(symbol != nullptr);
    return reinterpret_cast<Signature*>(symbol);
}

std::filesystem::path runfiles_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    assert(test_srcdir != nullptr);
    assert(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace;
}

std::filesystem::path find_shared_library() {
    const std::filesystem::path root = runfiles_root();
    const std::vector<std::filesystem::path> candidates = {
        root / "engine/src/kernel/signed/libnetwork_kernel.dylib",
        root / "engine/src/kernel/libnetwork_kernel.so",
    };

    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    assert(false);
    return {};
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += "'";
    return quoted;
}

void verify_exported_symbols(
    [[maybe_unused]] const std::filesystem::path& library_path) {
#if defined(__APPLE__)
    const std::string command = "nm -gU " + shell_quote(library_path.string());
    FILE* pipe = popen(command.c_str(), "r");
    assert(pipe != nullptr);

    std::array<char, 1024> line{};
    std::vector<std::string> unexpected_symbols;
    while (fgets(line.data(), static_cast<int>(line.size()), pipe) != nullptr) {
        std::string text = line.data();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        if (text.empty()) {
            continue;
        }
        const std::size_t symbol_begin = text.find_last_of(' ');
        const std::string symbol =
            symbol_begin == std::string::npos ? text : text.substr(symbol_begin + 1);
        if (symbol.rfind("_Kernel_", 0) != 0 &&
            symbol.rfind("_GameServer_", 0) != 0) {
            unexpected_symbols.push_back(symbol);
        }
    }

    const int status = pclose(pipe);
    assert(status == 0);
    assert(unexpected_symbols.empty());
#endif
}

}  // namespace

int main() {
    const std::filesystem::path library_path = find_shared_library();
    verify_exported_symbols(library_path);

    void* library = dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    assert(library != nullptr);

    auto* kernel_get_abi_info =
        load_symbol<bool(KernelAbiInfo*, std::uint32_t)>(library, "Kernel_GetAbiInfo");
    auto* kernel_get_build_info =
        load_symbol<bool(KernelBuildInfo*, std::uint32_t)>(
            library,
            "Kernel_GetBuildInfo");
    auto* kernel_create =
        load_symbol<KernelHandle*(const KernelConfig*)>(library, "Kernel_Create");
    auto* kernel_destroy =
        load_symbol<void(KernelHandle*)>(library, "Kernel_Destroy");
    [[maybe_unused]] auto* kernel_invoke_rpc =
        load_symbol<bool(
            KernelHandle*,
            const char*,
            std::uint32_t,
            KernelRpcRequestId*)>(
            library,
            "Kernel_InvokeRpcCommand");
    [[maybe_unused]] auto* kernel_poll_rpc_response =
        load_symbol<bool(
            KernelHandle*,
            KernelRpcRequestId,
            char*,
            std::uint32_t,
            std::uint32_t*)>(
            library,
            "Kernel_PollRpcResponse");
    [[maybe_unused]] auto* kernel_start_client =
        load_symbol<bool(KernelHandle*, const char*)>(library, "Kernel_StartClient");
    [[maybe_unused]] auto* kernel_start_client_catalog_sync =
        load_symbol<bool(
            KernelHandle*,
            const char*,
            const KernelGameplayCatalogSyncClientConfig*)>(
            library,
            "Kernel_StartClientCatalogSync");
    [[maybe_unused]] auto* kernel_set_gameplay_catalog_sync_bundle =
        load_symbol<bool(
            KernelHandle*,
            const KernelGameplayCatalogSyncServerConfig*,
            KernelGameplayCatalogManifest*)>(
            library,
            "Kernel_SetGameplayCatalogSyncBundle");
    [[maybe_unused]] auto* kernel_get_gameplay_catalog_sync_status =
        load_symbol<bool(KernelHandle*, KernelGameplayCatalogSyncStatus*)>(
            library,
            "Kernel_GetGameplayCatalogSyncStatus");
    [[maybe_unused]] auto* kernel_request_gameplay_catalog_bundle =
        load_symbol<bool(KernelHandle*)>(
            library,
            "Kernel_RequestGameplayCatalogBundle");
    [[maybe_unused]] auto* kernel_copy_gameplay_catalog_bundle =
        load_symbol<bool(KernelHandle*, std::uint8_t*, std::uint32_t, std::uint32_t*)>(
            library,
            "Kernel_CopyGameplayCatalogBundle");
    [[maybe_unused]] auto* kernel_continue_client_handshake =
        load_symbol<bool(KernelHandle*)>(
            library,
            "Kernel_ContinueClientHandshake");
    [[maybe_unused]] auto* kernel_load_gameplay_catalog =
        load_symbol<bool(
            KernelHandle*,
            const KernelGameplayCatalogDefinition*,
            const KernelGameplayCatalogLoadOptions*)>(
            library,
            "Kernel_LoadGameplayCatalog");
    [[maybe_unused]] auto* kernel_start_dedicated_server =
        load_symbol<bool(KernelHandle*, std::uint16_t)>(
            library,
            "Kernel_StartDedicatedServer");
    auto* kernel_start_listen_server =
        load_symbol<bool(KernelHandle*, std::uint16_t)>(
            library,
            "Kernel_StartListenServer");
    auto* kernel_update =
        load_symbol<void(KernelHandle*, float)>(library, "Kernel_Update");
    auto* kernel_submit_player_input =
        load_symbol<void(KernelHandle*, std::uint32_t, const KernelPlayerInput*)>(
            library,
            "Kernel_SubmitPlayerInput");
    auto* kernel_get_render_states =
        load_symbol<std::uint32_t(KernelHandle*, RenderEntityState*, std::uint32_t)>(
            library,
            "Kernel_GetRenderStates");
    auto* kernel_get_render_states_at_time =
        load_symbol<std::uint32_t(
            KernelHandle*,
            std::uint64_t,
            RenderEntityState*,
            std::uint32_t)>(library, "Kernel_GetRenderStatesAtTime");
    auto* kernel_get_skeleton_render_states =
        load_symbol<std::uint32_t(
            KernelHandle*,
            KernelSkeletonRenderState*,
            std::uint32_t,
            KernelBoneLocalTransform*,
            std::uint32_t,
            KernelSkeletonRenderStateResult*)>(
            library,
            "Kernel_GetSkeletonRenderStates");
    [[maybe_unused]] auto* kernel_get_skeleton_render_states_at_time =
        load_symbol<std::uint32_t(
            KernelHandle*,
            std::uint64_t,
            KernelSkeletonRenderState*,
            std::uint32_t,
            KernelBoneLocalTransform*,
            std::uint32_t,
            KernelSkeletonRenderStateResult*)>(
            library,
            "Kernel_GetSkeletonRenderStatesAtTime");
    [[maybe_unused]] auto* kernel_get_projectile_templates =
        load_symbol<std::uint32_t(
            KernelHandle*,
            KernelProjectileTemplateDefinition*,
            std::uint32_t)>(library, "Kernel_GetProjectileTemplates");
    [[maybe_unused]] auto* kernel_get_action_template =
        load_symbol<bool(
            KernelHandle*,
            std::uint32_t,
            KernelActionTemplateDefinition*)>(library, "Kernel_GetActionTemplate");
    [[maybe_unused]] auto* kernel_get_actor_templates =
        load_symbol<std::uint32_t(
            KernelHandle*,
            KernelActorTemplateDefinition*,
            std::uint32_t)>(library, "Kernel_GetActorTemplates");
    [[maybe_unused]] auto* kernel_get_collider_templates =
        load_symbol<std::uint32_t(
            KernelHandle*,
            KernelColliderTemplateDefinition*,
            std::uint32_t)>(library, "Kernel_GetColliderTemplates");
    [[maybe_unused]] auto* kernel_get_collider_bindings =
        load_symbol<std::uint32_t(
            KernelHandle*,
            KernelColliderBindingDefinition*,
            std::uint32_t)>(library, "Kernel_GetColliderBindings");
    [[maybe_unused]] auto* kernel_query_vision_state =
        load_symbol<std::uint32_t(
            KernelHandle*,
            const KernelVisionStateQuery*,
            KernelVisionStateView*,
            std::uint32_t)>(library, "Kernel_QueryVisionState");
    auto* kernel_poll_events =
        load_symbol<std::uint32_t(KernelHandle*, KernelEvent*, std::uint32_t)>(
            library,
            "Kernel_PollEvents");
    auto* kernel_poll_entity_lifecycle_events =
        load_symbol<std::uint32_t(
            KernelHandle*,
            KernelEntityLifecycleEvent*,
            std::uint32_t)>(library, "Kernel_PollEntityLifecycleEvents");
    auto* kernel_poll_local_action_results =
        load_symbol<std::uint32_t(
            KernelHandle*,
            KernelLocalActionResult*,
            std::uint32_t)>(library, "Kernel_PollLocalActionResults");
    auto* kernel_poll_remote_action_presentation_events =
        load_symbol<std::uint32_t(
            KernelHandle*,
            KernelRemoteActionPresentationEvent*,
            std::uint32_t)>(library, "Kernel_PollRemoteActionPresentationEvents");
    auto* kernel_get_local_player_info =
        load_symbol<bool(KernelHandle*, KernelLocalPlayerInfo*)>(
            library,
            "Kernel_GetLocalPlayerInfo");
    auto* kernel_lan_discovery_create =
        load_symbol<KernelLANDiscoveryHandle*()>(
            library,
            "Kernel_LANDiscovery_Create");
    auto* kernel_lan_discovery_destroy =
        load_symbol<void(KernelLANDiscoveryHandle*)>(
            library,
            "Kernel_LANDiscovery_Destroy");
    [[maybe_unused]] auto* kernel_lan_discovery_start_server =
        load_symbol<bool(KernelLANDiscoveryHandle*, const KernelLANDiscoveryServerConfig*)>(
            library,
            "Kernel_LANDiscovery_StartServer");
    [[maybe_unused]] auto* kernel_lan_discovery_stop_server =
        load_symbol<void(KernelLANDiscoveryHandle*)>(
            library,
            "Kernel_LANDiscovery_StopServer");
    [[maybe_unused]] auto* kernel_lan_discovery_query =
        load_symbol<bool(KernelLANDiscoveryHandle*, const KernelLANDiscoveryQueryConfig*)>(
            library,
            "Kernel_LANDiscovery_Query");
    [[maybe_unused]] auto* kernel_lan_discovery_poll_results =
        load_symbol<std::uint32_t(
            KernelLANDiscoveryHandle*,
            KernelLANDiscoveryResult*,
            std::uint32_t)>(library, "Kernel_LANDiscovery_PollResults");
    [[maybe_unused]] auto* kernel_lan_discovery_clear_results =
        load_symbol<void(KernelLANDiscoveryHandle*)>(
            library,
            "Kernel_LANDiscovery_ClearResults");
    auto* kernel_server_create_entity =
        load_symbol<bool(
            KernelHandle*,
            const KernelServerEntityCreateInfo*,
            std::uint32_t*)>(library, "Kernel_ServerCreateEntity");
    auto* kernel_server_destroy_entity =
        load_symbol<bool(KernelHandle*, std::uint32_t, std::uint32_t)>(
            library,
            "Kernel_ServerDestroyEntity");
    [[maybe_unused]] auto* kernel_server_set_entity_transform =
        load_symbol<bool(
            KernelHandle*,
            std::uint32_t,
            const KernelVec3*,
            const KernelQuat*)>(library, "Kernel_ServerSetEntityTransform");
    [[maybe_unused]] auto* kernel_server_set_entity_velocity =
        load_symbol<bool(KernelHandle*, std::uint32_t, const KernelVec3*)>(
            library,
            "Kernel_ServerSetEntityVelocity");
    auto* kernel_server_set_entity_state =
        load_symbol<bool(KernelHandle*, std::uint32_t, std::uint16_t, std::uint32_t)>(
            library,
            "Kernel_ServerSetEntityState");
    auto* kernel_server_set_entity_health =
        load_symbol<bool(KernelHandle*, std::uint32_t, std::uint16_t)>(
            library,
            "Kernel_ServerSetEntityHealth");
    [[maybe_unused]] auto* kernel_server_submit_entity_input =
        load_symbol<bool(KernelHandle*, std::uint32_t, const KernelPlayerInput*)>(
            library,
            "Kernel_ServerSubmitEntityInput");
    auto* kernel_server_set_entity_combat_state =
        load_symbol<bool(
            KernelHandle*,
            std::uint32_t,
            const KernelCombatStateDefinition*)>(
            library,
            "Kernel_ServerSetEntityCombatState");
    [[maybe_unused]] auto* kernel_server_set_entity_vision_config =
        load_symbol<bool(
            KernelHandle*,
            std::uint32_t,
            const KernelAgentVisionConfig*)>(
            library,
            "Kernel_ServerSetEntityVisionConfig");
    [[maybe_unused]] auto* kernel_server_clear_entity_vision_config =
        load_symbol<bool(KernelHandle*, std::uint32_t)>(
            library,
            "Kernel_ServerClearEntityVisionConfig");
    auto* kernel_server_set_entity_weapon_mechanics =
        load_symbol<bool(
            KernelHandle*,
            std::uint32_t,
            const KernelWeaponMechanicsDefinition*)>(
            library,
            "Kernel_ServerSetEntityWeaponMechanics");
    auto* kernel_server_clear_entity_weapon_mechanics =
        load_symbol<bool(KernelHandle*, std::uint32_t, std::uint8_t)>(
            library,
            "Kernel_ServerClearEntityWeaponMechanics");
    auto* kernel_server_validate_mechanics_config =
        load_symbol<bool(const KernelWeaponMechanicsDefinition*)>(
            library,
            "Kernel_ServerValidateMechanicsConfig");
    auto* kernel_server_get_entity_weapon_mechanics =
        load_symbol<bool(
            KernelHandle*,
            std::uint32_t,
            std::uint8_t,
            KernelWeaponMechanicsDefinition*)>(
            library,
            "Kernel_ServerGetEntityWeaponMechanics");
    [[maybe_unused]] auto* kernel_server_get_homing_state =
        load_symbol<bool(KernelHandle*, std::uint32_t, KernelHomingState*)>(
            library,
            "Kernel_ServerGetHomingState");
    auto* kernel_server_get_entity_state =
        load_symbol<bool(KernelHandle*, std::uint32_t, KernelServerEntityState*)>(
            library,
            "Kernel_ServerGetEntityState");
    [[maybe_unused]] auto* kernel_server_query_entities =
        load_symbol<std::uint32_t(
            KernelHandle*,
            std::uint16_t,
            KernelServerEntityState*,
            std::uint32_t)>(library, "Kernel_ServerQueryEntities");
    [[maybe_unused]] auto* kernel_submit_gameplay_request =
        load_symbol<bool(KernelHandle*, const KernelGameplayRequest*)>(
            library, "Kernel_SubmitGameplayRequest");
    [[maybe_unused]] auto* kernel_server_submit_gameplay_request =
        load_symbol<bool(KernelHandle*, const KernelGameplayRequest*)>(
            library, "Kernel_ServerSubmitGameplayRequest");
    [[maybe_unused]] auto* kernel_get_item_instance =
        load_symbol<bool(
            KernelHandle*, KernelItemInstanceId, KernelItemInstanceView*)>(
            library, "Kernel_GetItemInstance");
    [[maybe_unused]] auto* kernel_poll_gameplay_request_outcomes =
        load_symbol<std::uint32_t(
            KernelHandle*, KernelGameplayRequestOutcome*, std::uint32_t)>(
            library, "Kernel_PollGameplayRequestOutcomes");
    [[maybe_unused]] auto* kernel_poll_inventory_deltas =
        load_symbol<std::uint32_t(
            KernelHandle*,
            KernelInventoryContainerId,
            KernelInventoryDelta*,
            std::uint32_t)>(library, "Kernel_PollInventoryDeltas");
    [[maybe_unused]] auto* kernel_copy_owned_inventory_containers =
        load_symbol<std::uint32_t(
            KernelHandle*,
            std::uint32_t,
            KernelInventoryContainerView*,
            std::uint32_t)>(library, "Kernel_CopyOwnedInventoryContainers");
    auto* game_server_get_abi_info =
        load_symbol<bool(GameServerAbiInfo*, std::uint32_t)>(
            library,
            "GameServer_GetAbiInfo");
    auto* game_server_create =
        load_symbol<GameServerHandle*(KernelHandle*)>(library, "GameServer_Create");
    auto* game_server_create_with_weapon_template_directory =
        load_symbol<GameServerHandle*(KernelHandle*, const char*)>(
            library,
            "GameServer_CreateWithWeaponTemplateDirectory");
    auto* game_server_destroy =
        load_symbol<void(GameServerHandle*)>(library, "GameServer_Destroy");
    auto* game_server_handle_event =
        load_symbol<void(GameServerHandle*, const KernelEvent*)>(
            library,
            "GameServer_HandleEvent");
    auto* game_server_tick =
        load_symbol<void(GameServerHandle*, float)>(library, "GameServer_Tick");
    auto* game_server_get_enemy_count =
        load_symbol<std::uint32_t(GameServerHandle*)>(
            library,
            "GameServer_GetEnemyCount");
    auto* game_server_query_weapon_template =
        load_symbol<bool(GameServerHandle*, std::uint8_t, GameServerWeaponTemplateInfo*)>(
            library,
            "GameServer_QueryWeaponTemplate");
    auto* game_server_despawn_all =
        load_symbol<void(GameServerHandle*, std::uint32_t)>(
            library,
            "GameServer_DespawnAll");

    KernelAbiInfo abi_info{};
    assert(kernel_get_abi_info(&abi_info, sizeof(abi_info)));
    assert(abi_info.abi_version == KERNEL_ABI_VERSION);
    assert(abi_info.kernel_config_size == sizeof(KernelConfig));
    assert(abi_info.player_input_size == sizeof(KernelPlayerInput));
    assert(abi_info.render_entity_state_size == sizeof(RenderEntityState));
    assert(abi_info.bone_local_transform_size ==
           sizeof(KernelBoneLocalTransform));
    assert(abi_info.skeleton_render_state_size ==
           sizeof(KernelSkeletonRenderState));
    assert(abi_info.skeleton_render_state_result_size ==
           sizeof(KernelSkeletonRenderStateResult));
    assert((abi_info.capability_flags &
            KERNEL_CAPABILITY_SKELETON_RENDER_STATES) != 0u);
    KernelSkeletonRenderStateResult skeleton_result{};
    skeleton_result.struct_size = sizeof(skeleton_result);
    assert(kernel_get_skeleton_render_states(
               nullptr, nullptr, 0u, nullptr, 0u, &skeleton_result) == 0u);
    assert(skeleton_result.status ==
           KERNEL_SKELETON_RENDER_STATUS_INVALID_ARGUMENT);
    assert(abi_info.kernel_event_size == sizeof(KernelEvent));
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_ENTITY_LIFECYCLE_EVENTS) != 0);
    assert(kernel_poll_entity_lifecycle_events(nullptr, nullptr, 0) == 0);
    assert(kernel_poll_local_action_results(nullptr, nullptr, 0) == 0);
    assert(kernel_poll_remote_action_presentation_events(nullptr, nullptr, 0) == 0);
    assert(abi_info.local_player_info_size == sizeof(KernelLocalPlayerInfo));
    assert(abi_info.server_entity_create_info_size ==
           sizeof(KernelServerEntityCreateInfo));
    assert(abi_info.server_entity_state_size == sizeof(KernelServerEntityState));
    assert(abi_info.item_template_definition_size ==
           sizeof(KernelItemTemplateDefinition));
    assert(abi_info.gameplay_request_size == sizeof(KernelGameplayRequest));
    assert(abi_info.gameplay_request_outcome_size ==
           sizeof(KernelGameplayRequestOutcome));
    assert(abi_info.item_instance_view_size == sizeof(KernelItemInstanceView));
    assert(abi_info.inventory_container_view_size ==
           sizeof(KernelInventoryContainerView));
    assert(abi_info.inventory_delta_size == sizeof(KernelInventoryDelta));
    assert((abi_info.capability_flags &
            KERNEL_CAPABILITY_ITEM_PROP_SYSTEM) != 0u);
    assert(abi_info.weapon_mechanics_definition_size ==
           sizeof(KernelWeaponMechanicsDefinition));
    assert(abi_info.actor_template_definition_size ==
           sizeof(KernelActorTemplateDefinition));
    assert(abi_info.entity_template_definition_size ==
           sizeof(KernelEntityTemplateDefinition));
    assert(abi_info.entity_ai_definition_size ==
           sizeof(KernelEntityAiDefinition));
    assert(abi_info.action_template_definition_size ==
           sizeof(KernelActionTemplateDefinition));
    assert(abi_info.action_runtime_view_size == sizeof(KernelActionRuntimeView));
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_ACTION_TIMELINE) != 0);
    assert(abi_info.local_action_result_size == sizeof(KernelLocalActionResult));
    assert(
        abi_info.remote_action_presentation_event_size ==
        sizeof(KernelRemoteActionPresentationEvent));
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LOCAL_ACTION_RESULTS) != 0);
    assert(
        (abi_info.capability_flags &
         KERNEL_CAPABILITY_REMOTE_ACTION_PRESENTATION) != 0);
    assert(abi_info.action_intent_size == sizeof(KernelActionIntent));
    assert(abi_info.action_input_size == sizeof(KernelActionInput));
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_ACTION_INTENTS) != 0);
    assert(abi_info.projectile_mechanics_definition_size ==
           sizeof(KernelProjectileMechanicsDefinition));
    assert(abi_info.area_effect_mechanics_definition_size ==
           sizeof(KernelAreaEffectMechanicsDefinition));
    assert(abi_info.beam_mechanics_definition_size ==
           sizeof(KernelBeamMechanicsDefinition));
    assert(abi_info.homing_mechanics_definition_size ==
           sizeof(KernelHomingMechanicsDefinition));
    assert(abi_info.homing_state_size == sizeof(KernelHomingState));
    assert(abi_info.lan_discovery_server_config_size ==
           sizeof(KernelLANDiscoveryServerConfig));
    assert(abi_info.lan_discovery_query_config_size ==
           sizeof(KernelLANDiscoveryQueryConfig));
    assert(abi_info.lan_discovery_result_size ==
           sizeof(KernelLANDiscoveryResult));
    assert(abi_info.combat_state_definition_size ==
           sizeof(KernelCombatStateDefinition));
    assert(abi_info.gameplay_catalog_load_result_size ==
           sizeof(KernelGameplayCatalogLoadResult));
    assert(abi_info.gameplay_catalog_load_options_size ==
           sizeof(KernelGameplayCatalogLoadOptions));
    assert(abi_info.agent_vision_config_size == sizeof(KernelAgentVisionConfig));
    assert(abi_info.vision_state_query_size == sizeof(KernelVisionStateQuery));
    assert(abi_info.vision_state_view_size == sizeof(KernelVisionStateView));
    assert(
        abi_info.gameplay_catalog_manifest_size ==
        sizeof(KernelGameplayCatalogManifest));
    assert(
        abi_info.gameplay_catalog_sync_status_size ==
        sizeof(KernelGameplayCatalogSyncStatus));
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LISTEN_SERVER_MODE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LOCAL_PLAYER_INFO) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_CREATE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LAG_COMPENSATED_PROJECTILE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_EVENT_PRESENTATION_TIME) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_RENDER_STATES_AT_TIME) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_MECHANICS_CONFIG) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_WEAPON_METADATA_QUERY) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_PROJECTILE_RESPONSE_MASKS) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_HOMING_PROJECTILES) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LAN_DISCOVERY) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_VISION_STATE_QUERY) != 0);
    assert(
        (abi_info.capability_flags & KERNEL_CAPABILITY_GAMEPLAY_CATALOG_SYNC) !=
        0);
    assert(!kernel_get_abi_info(nullptr, sizeof(abi_info)));
    assert(!kernel_get_abi_info(&abi_info, sizeof(abi_info) - 1));

    KernelLANDiscoveryHandle* discovery = kernel_lan_discovery_create();
    assert(discovery != nullptr);
    kernel_lan_discovery_destroy(discovery);
    kernel_lan_discovery_destroy(nullptr);

    KernelBuildInfo build_info{};
    require(kernel_get_build_info(&build_info, sizeof(build_info)));
    require(build_info.struct_size == sizeof(KernelBuildInfo));
    require(std::string(build_info.module_name) == "network_kernel");
    require(build_info.module_file_name[0] != '\0');
    require(has_version_revision_suffix(build_info.module_version));
    require(build_info.protocol_version == 3u);
    require(build_info.snapshot_schema_version == 17u);
    require(build_info.packet_schema_version == 21u);
    require(build_info.git_commit[0] != '\0');
    require(std::string(build_info.git_commit) != "unknown");
    require(std::string(build_info.module_version) != build_info.git_commit);
    require(all_digits(build_info.build_timestamp));
    require(build_info.build_platform[0] != '\0');
    require(build_info.build_config[0] != '\0');
    require(build_info.compiler_info[0] != '\0');
    require(!kernel_get_build_info(nullptr, sizeof(build_info)));
    require(!kernel_get_build_info(&build_info, sizeof(build_info) - 1));
    GameServerAbiInfo game_server_abi_info{};
    assert(game_server_get_abi_info(
        &game_server_abi_info,
        sizeof(game_server_abi_info)));
    assert(game_server_abi_info.abi_version == GAME_SERVER_ABI_VERSION);
    assert(game_server_abi_info.weapon_template_info_size ==
           sizeof(GameServerWeaponTemplateInfo));
    assert(game_server_abi_info.gameplay_catalog_load_result_size ==
           sizeof(KernelGameplayCatalogLoadResult));
    assert(
        (game_server_abi_info.capability_flags &
         GAME_SERVER_CAPABILITY_ENEMY_MANAGER) != 0);
    assert(
        (game_server_abi_info.capability_flags &
         GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_QUERY) != 0);
    assert(!game_server_get_abi_info(nullptr, sizeof(game_server_abi_info)));
    assert(!game_server_get_abi_info(
        &game_server_abi_info,
        sizeof(game_server_abi_info) - 1));
    KernelLocalPlayerInfo local_info{};
    assert(!kernel_get_local_player_info(nullptr, &local_info));

    assert(kernel_create(nullptr) == nullptr);

    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;

    KernelHandle* kernel = kernel_create(&config);
    assert(kernel != nullptr);
    assert(kernel_start_listen_server(kernel, 7777));
    GameServerHandle* game_server = game_server_create(kernel);
    assert(game_server != nullptr);
    assert(game_server_create_with_weapon_template_directory(kernel, nullptr) == nullptr);
    GameServerWeaponTemplateInfo template_info{};
    template_info.struct_size = sizeof(template_info);
    assert(game_server_query_weapon_template(game_server, 4, &template_info));
    assert(template_info.mechanics.fire_mode == KernelWeaponFireMode_Projectile);
    assert(template_info.mechanics.projectile_template_id != 0u);
    assert(kernel_get_local_player_info(kernel, &local_info));
    assert(local_info.peer_id == 1);
    assert(local_info.player_net_id != 0);
    assert(local_info.has_welcome != 0u);
    assert(local_info.connected != 0u);
    std::array<KernelEvent, 16> events{};
    std::uint32_t event_count = kernel_poll_events(
        kernel,
        events.data(),
        static_cast<std::uint32_t>(events.size()));
    for (std::uint32_t index = 0; index < event_count; ++index) {
        game_server_handle_event(game_server, &events[index]);
    }
    KernelEvent joined_event{};
    joined_event.type = KernelEventType_PlayerJoined;
    joined_event.net_id = local_info.player_net_id;
    joined_event.peer_id = local_info.peer_id;
    game_server_handle_event(game_server, &joined_event);
    KernelCombatStateDefinition local_combat{};
    local_combat.struct_size = sizeof(local_combat);
    local_combat.hp = 100;
    local_combat.max_hp = 100;
    local_combat.active_weapon_slot = 0;
    local_combat.weapon_slot_count = 1;
    local_combat.weapon_ids[0] = 0;
    local_combat.collider_template_id = 1;
    local_combat.move_speed_meters_per_second = 5.0f;
    local_combat.hitbox_center = KernelVec3{0.0f, 0.9f, 0.0f};
    local_combat.hitbox_half_extents = KernelVec3{0.35f, 0.9f, 0.35f};
    assert(kernel_server_set_entity_combat_state(
        kernel,
        local_info.player_net_id,
        &local_combat));

    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = 1;
    create_info.actor_type = KernelActorType_Agent;
    create_info.position = KernelVec3{2.0f, 0.0f, 0.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t enemy = 0;
    assert(kernel_server_create_entity(kernel, &create_info, &enemy));
    assert(enemy != 0);
    KernelCombatStateDefinition combat_state{};
    combat_state.struct_size = sizeof(combat_state);
    combat_state.hp = 240;
    combat_state.max_hp = 240;
    combat_state.active_weapon_slot = 0;
    combat_state.weapon_slot_count = 1;
    combat_state.weapon_ids[0] = 3;
    combat_state.collider_template_id = 2;
    combat_state.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    combat_state.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    combat_state.ammo[0] = 3;
    combat_state.reserve_magazines[0] = 6;
    assert(kernel_server_set_entity_combat_state(kernel, enemy, &combat_state));
    KernelWeaponMechanicsDefinition rocket{};
    rocket.struct_size = sizeof(rocket);
    rocket.weapon_id = 3;
    rocket.fire_mode = KernelWeaponFireMode_Projectile;
    rocket.magazine_size = 3;
    rocket.reserve_magazines = 6;
    rocket.damage = 5;
    rocket.fire_action_template_id = 1001u;
    rocket.reload_action_template_id = 2001u;
    rocket.projectile_template_id = 3;
    assert(kernel_server_validate_mechanics_config(&rocket));
    assert(!kernel_server_set_entity_weapon_mechanics(kernel, enemy, &rocket));
    KernelWeaponMechanicsDefinition queried_weapon{};
    queried_weapon.struct_size = sizeof(queried_weapon);
    assert(!kernel_server_get_entity_weapon_mechanics(
        kernel, enemy, 3, &queried_weapon));
    assert(kernel_server_set_entity_state(kernel, enemy, 4, 8));
    KernelServerEntityState server_state{};
    server_state.struct_size = sizeof(server_state);
    assert(kernel_server_get_entity_state(kernel, enemy, &server_state));
    assert(server_state.net_id == enemy);
    assert(server_state.hp == 240);
    assert(server_state.max_hp == 240);
    assert(server_state.animation_state == 4);
    assert(kernel_server_set_entity_health(kernel, enemy, 123));
    server_state = KernelServerEntityState{};
    server_state.struct_size = sizeof(server_state);
    assert(kernel_server_get_entity_state(kernel, enemy, &server_state));
    assert(server_state.hp == 123);
    assert(server_state.max_hp == 240);
    assert(kernel_server_destroy_entity(
        kernel,
        enemy,
        KernelDespawnReason_Destroyed));
    assert(!kernel_server_clear_entity_weapon_mechanics(kernel, enemy, 3));

    KernelPlayerInput input{};
    input.input_seq = 1;
    input.client_action_time_us = 33333;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.action_intent = KernelActionIntent{
        1u, KernelActionBinding_PrimaryFire, 0u, 0u};
    kernel_submit_player_input(kernel, 1, &input);
    kernel_update(kernel, 1.0f / 30.0f);

    std::array<RenderEntityState, 16> states{};
    const std::uint32_t state_count = kernel_get_render_states(
        kernel,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    assert(state_count >= 1);
    bool saw_local_player_health = false;
    for (std::uint32_t index = 0; index < state_count; ++index) {
        if (states[index].net_id == local_info.player_net_id) {
            saw_local_player_health =
                states[index].hp == 100 && states[index].max_hp == 100;
        }
    }
    assert(saw_local_player_health);
    assert(kernel_get_render_states_at_time(
               kernel,
               33333,
               states.data(),
               static_cast<std::uint32_t>(states.size())) >= 1);

    event_count = kernel_poll_events(
        kernel,
        events.data(),
        static_cast<std::uint32_t>(events.size()));
    assert(event_count >= 1);
    for (std::uint32_t index = 0; index < event_count; ++index) {
        game_server_handle_event(game_server, &events[index]);
    }
    for (int frame = 0; frame < 3; ++frame) {
        game_server_tick(game_server, 1.0f / 30.0f);
        kernel_update(kernel, 1.0f / 30.0f);
        event_count = kernel_poll_events(
            kernel,
            events.data(),
            static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < event_count; ++index) {
            game_server_handle_event(game_server, &events[index]);
        }
    }
    game_server_tick(game_server, 1.0f / 30.0f);
    assert(game_server_get_enemy_count(game_server) == 2);
    game_server_despawn_all(game_server, KernelDespawnReason_Destroyed);
    game_server_tick(game_server, 1.0f / 30.0f);
    assert(game_server_get_enemy_count(game_server) == 0);
    kernel_update(kernel, 1.0f / 30.0f);

    game_server_destroy(game_server);
    kernel_destroy(kernel);
    assert(dlclose(library) == 0);
    return 0;
}
