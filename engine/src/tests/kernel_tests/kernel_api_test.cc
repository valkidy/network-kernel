#include <array>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

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
    constexpr const char* kPrefix = "0.6.4+r";
    if (text.rfind(kPrefix, 0) != 0 || text.size() == std::strlen(kPrefix)) {
        return false;
    }
    return all_digits(text.c_str() + std::strlen(kPrefix));
}

}  // namespace

int main() {
    KernelAbiInfo abi_info{};
    assert(Kernel_GetAbiInfo(&abi_info, sizeof(abi_info)));
    assert(abi_info.struct_size == sizeof(KernelAbiInfo));
    assert(abi_info.abi_version == KERNEL_ABI_VERSION);
    assert(abi_info.kernel_config_size == sizeof(KernelConfig));
    assert(abi_info.player_input_size == sizeof(PlayerInput));
    assert(abi_info.render_entity_state_size == sizeof(RenderEntityState));
    assert(abi_info.kernel_event_size == sizeof(KernelEvent));
    assert(abi_info.server_entity_create_info_size ==
           sizeof(KernelServerEntityCreateInfo));
    assert(abi_info.server_entity_state_size == sizeof(KernelServerEntityState));
    assert(abi_info.weapon_mechanics_definition_size ==
           sizeof(KernelWeaponMechanicsDefinition));
    assert(abi_info.projectile_mechanics_definition_size ==
           sizeof(KernelProjectileMechanicsDefinition));
    assert(abi_info.area_effect_mechanics_definition_size ==
           sizeof(KernelAreaEffectMechanicsDefinition));
    assert(abi_info.beam_mechanics_definition_size ==
           sizeof(KernelBeamMechanicsDefinition));
    assert(abi_info.area_effect_state_size == sizeof(KernelAreaEffectState));
    assert(abi_info.beam_state_size == sizeof(KernelBeamState));
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
    assert(abi_info.gameplay_catalog_definition_size ==
           sizeof(KernelGameplayCatalogDefinition));
    assert(abi_info.gameplay_catalog_load_result_size ==
           sizeof(KernelGameplayCatalogLoadResult));
    assert(abi_info.projectile_template_definition_size ==
           sizeof(KernelProjectileTemplateDefinition));
    assert(abi_info.collider_template_definition_size ==
           sizeof(KernelColliderTemplateDefinition));
    assert(abi_info.collider_binding_definition_size ==
           sizeof(KernelColliderBindingDefinition));
    assert(abi_info.benchmark_stats_size == sizeof(KernelBenchmarkStats));
    assert(abi_info.network_stats_size == sizeof(KernelNetworkStats));
    assert(abi_info.debug_record_filter_size == sizeof(KernelDebugRecordFilter));
    assert(abi_info.debug_info_size == sizeof(KernelDebugInfo));
    assert(abi_info.collider_shape_query_size == sizeof(KernelColliderShapeQuery));
    assert(abi_info.collider_shape_view_size == sizeof(KernelColliderShapeView));
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_CLIENT_MODE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LISTEN_SERVER_MODE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_DEDICATED_SERVER_MODE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_INPUT_SUBMISSION) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_RENDER_STATES) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_EVENT_POLLING) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_CLIENT_PREDICTION) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SNAPSHOT_INTERPOLATION) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LAG_COMPENSATED_HITSCAN) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LOCAL_PLAYER_INFO) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_CREATE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_DESTROY) != 0);
    assert(
        (abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_TRANSFORM_WRITE) !=
        0);
    assert(
        (abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_VELOCITY_WRITE) !=
        0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_STATE_WRITE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_QUERY) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_RELEVANCE_FILTER) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LAG_COMPENSATED_PROJECTILE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_EVENT_PRESENTATION_TIME) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_RENDER_STATES_AT_TIME) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_MECHANICS_CONFIG) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_WEAPON_METADATA_QUERY) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_AREA_EFFECT_WEAPONS) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_PROJECTILE_RESPONSE_MASKS) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_BEAM_WEAPONS) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_HOMING_PROJECTILES) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LAN_DISCOVERY) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_GAMEPLAY_CATALOG) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_PROJECTILE_SPAWN_BATCH) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_DEBUG_RECORDS) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_COLLIDER_SHAPE_QUERY) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_BENCHMARK_STATS) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_NETWORK_STATS) != 0);
    assert(abi_info.local_player_info_size == sizeof(KernelLocalPlayerInfo));
    assert(KERNEL_ABI_VERSION == 17u);
    assert(KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_FAILED == 0u);
    assert(KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_SUCCESS == 1u);
    assert(KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNSUPPORTED_CATALOG_VERSION == 3u);
    assert(KERNEL_MAX_WEAPONS == 7u);
    assert(KERNEL_LAN_DISCOVERY_DEFAULT_PORT == 47777u);
    assert(offsetof(PlayerInput, client_action_time_us) > offsetof(PlayerInput, input_seq));
    assert(offsetof(PlayerInput, client_action_id) > offsetof(PlayerInput, client_action_time_us));
    assert(offsetof(KernelEvent, event_time_us) > offsetof(KernelEvent, code));
    assert(offsetof(KernelEvent, presentation_time_us) > offsetof(KernelEvent, event_time_us));
    assert(offsetof(RenderEntityState, entity_id) == 0u);
    assert(offsetof(RenderEntityState, net_id) > offsetof(RenderEntityState, entity_id));
    assert(offsetof(RenderEntityState, hp) > offsetof(RenderEntityState, velocity));
    assert(offsetof(RenderEntityState, max_hp) > offsetof(RenderEntityState, hp));
    assert(!Kernel_GetAbiInfo(nullptr, sizeof(abi_info)));
    assert(!Kernel_GetAbiInfo(&abi_info, sizeof(abi_info) - 1));

    KernelBuildInfo build_info{};
    require(Kernel_GetBuildInfo(&build_info, sizeof(build_info)));
    require(build_info.struct_size == sizeof(KernelBuildInfo));
    require(std::strcmp(build_info.module_name, "network_kernel") == 0);
    require(build_info.module_file_name[0] != '\0');
    require(has_version_revision_suffix(build_info.module_version));
    require(build_info.protocol_version != 0);
    require(build_info.snapshot_schema_version != 0);
    require(build_info.packet_schema_version != 0);
    require(build_info.git_commit[0] != '\0');
    require(std::strcmp(build_info.git_commit, "unknown") != 0);
    require(std::strcmp(build_info.module_version, build_info.git_commit) != 0);
    require(all_digits(build_info.build_timestamp));
    require(build_info.build_platform[0] != '\0');
    require(build_info.build_config[0] != '\0');
    require(build_info.compiler_info[0] != '\0');
    require(!Kernel_GetBuildInfo(nullptr, sizeof(build_info)));
    require(!Kernel_GetBuildInfo(&build_info, sizeof(build_info) - 1));

    assert(Kernel_Create(nullptr) == nullptr);
    assert(!Kernel_StartClient(nullptr, "127.0.0.1:9"));
    assert(!Kernel_StartListenServer(nullptr, 7777));
    assert(!Kernel_StartDedicatedServer(nullptr, 7777));
    KernelLANDiscoveryHandle* discovery = Kernel_LANDiscovery_Create();
    assert(discovery != nullptr);
    Kernel_LANDiscovery_Destroy(discovery);
    Kernel_LANDiscovery_Destroy(nullptr);
    assert(!Kernel_LANDiscovery_StartServer(nullptr, nullptr));
    Kernel_LANDiscovery_StopServer(nullptr);
    assert(!Kernel_LANDiscovery_Query(nullptr, nullptr));
    assert(Kernel_LANDiscovery_PollResults(nullptr, nullptr, 0) == 0);
    Kernel_LANDiscovery_ClearResults(nullptr);
    Kernel_Update(nullptr, 1.0f / 30.0f);
    Kernel_SubmitInput(nullptr, 1, nullptr);
    assert(!Kernel_LoadGameplayCatalog(nullptr, nullptr));
    assert(Kernel_GetRenderStates(nullptr, nullptr, 0) == 0);
    assert(Kernel_GetRenderStatesAtTime(nullptr, 0, nullptr, 0) == 0);
    assert(Kernel_PollEvents(nullptr, nullptr, 0) == 0);
    KernelBenchmarkStats benchmark_stats{};
    benchmark_stats.struct_size = sizeof(benchmark_stats);
    assert(!Kernel_GetBenchmarkStats(nullptr, &benchmark_stats));
    KernelNetworkStats network_stats{};
    network_stats.struct_size = sizeof(network_stats);
    assert(!Kernel_GetNetworkStats(nullptr, &network_stats));
    KernelDebugRecordFilter debug_filter{};
    debug_filter.struct_size = sizeof(debug_filter);
    std::array<KernelDebugInfo, 4> debug_records{};
    for (KernelDebugInfo& debug_record : debug_records) {
        debug_record.struct_size = sizeof(KernelDebugInfo);
    }
    assert(Kernel_PollDebugRecords(
               nullptr,
               &debug_filter,
               debug_records.data(),
               static_cast<std::uint32_t>(debug_records.size())) == 0);
    KernelColliderShapeQuery collider_query{};
    collider_query.struct_size = sizeof(collider_query);
    std::array<KernelColliderShapeView, 4> collider_shapes{};
    for (KernelColliderShapeView& shape : collider_shapes) {
        shape.struct_size = sizeof(KernelColliderShapeView);
    }
    assert(Kernel_QueryColliderShapes(
               nullptr,
               &collider_query,
               collider_shapes.data(),
               static_cast<std::uint32_t>(collider_shapes.size())) == 0);
    KernelLocalPlayerInfo local_info{};
    assert(!Kernel_GetLocalPlayerInfo(nullptr, &local_info));
    assert(!Kernel_GetLocalPlayerInfo(nullptr, nullptr));
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = 2;
    create_info.position = KernelVec3{1.0f, 0.0f, 0.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t created_net_id = 0;
    assert(!Kernel_ServerCreateEntity(nullptr, &create_info, &created_net_id));
    assert(!Kernel_ServerDestroyEntity(nullptr, 1, KernelDespawnReason_Destroyed));
    assert(!Kernel_ServerSetEntityTransform(
        nullptr,
        1,
        &create_info.position,
        &create_info.rotation));
    assert(!Kernel_ServerSetEntityVelocity(nullptr, 1, &create_info.position));
    assert(!Kernel_ServerSetEntityState(nullptr, 1, 2, 3));
    PlayerInput server_entity_input{};
    assert(!Kernel_ServerSubmitEntityInput(nullptr, 1, &server_entity_input));
    KernelCombatStateDefinition combat_state{};
    combat_state.struct_size = sizeof(combat_state);
    assert(!Kernel_ServerSetEntityCombatState(nullptr, 1, &combat_state));
    KernelWeaponMechanicsDefinition weapon_mechanics{};
    weapon_mechanics.struct_size = sizeof(weapon_mechanics);
    assert(!Kernel_ServerValidateMechanicsConfig(nullptr));
    assert(!Kernel_ServerSetEntityWeaponMechanics(nullptr, 1, &weapon_mechanics));
    assert(!Kernel_ServerClearEntityWeaponMechanics(nullptr, 1, 0));
    assert(!Kernel_ServerGetEntityWeaponMechanics(nullptr, 1, 0, &weapon_mechanics));
    KernelAreaEffectState area_effect_state{};
    area_effect_state.struct_size = sizeof(area_effect_state);
    assert(!Kernel_ServerGetAreaEffectState(nullptr, 1, &area_effect_state));
    KernelBeamState beam_state{};
    beam_state.struct_size = sizeof(beam_state);
    assert(!Kernel_ServerGetBeamState(nullptr, 1, &beam_state));
    KernelHomingState homing_state{};
    homing_state.struct_size = sizeof(homing_state);
    assert(!Kernel_ServerGetHomingState(nullptr, 1, &homing_state));
    KernelServerEntityState server_state{};
    server_state.struct_size = sizeof(server_state);
    assert(!Kernel_ServerGetEntityState(nullptr, 1, &server_state));
    assert(Kernel_ServerQueryEntities(nullptr, 0, &server_state, 1) == 0);

    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    KernelProjectileTemplateDefinition projectile_template{};
    projectile_template.struct_size = sizeof(projectile_template);
    projectile_template.projectile_template_id = 3;
    projectile_template.weapon_id = 3;
    projectile_template.motion_model = KernelProjectileMotionModel_Linear;
    projectile_template.sync_mode = KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
    projectile_template.speed = 35.0f;
    projectile_template.lifetime_seconds = 2.5f;
    projectile_template.gravity = KernelVec3{0.0f, 0.0f, 0.0f};
    projectile_template.collider_template_id = 10;
    projectile_template.damage = 5;
    projectile_template.damage_shape = KernelProjectileDamageShape_DirectHit;
    projectile_template.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    projectile_template.max_hit_count = 1;
    KernelColliderTemplateDefinition collider_template{};
    collider_template.struct_size = sizeof(collider_template);
    collider_template.template_id = 10;
    collider_template.shape_type = KernelColliderShapeType_Aabb;
    collider_template.half_extents = KernelVec3{0.25f, 0.25f, 0.25f};
    collider_template.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;
    collider_template.purpose_flags = KernelColliderPurpose_Damage;
    KernelColliderBindingDefinition collider_binding{};
    collider_binding.struct_size = sizeof(collider_binding);
    collider_binding.entity_type = 2;
    collider_binding.collider_template_id = 10;
    collider_binding.local_position = KernelVec3{0.0f, 0.8f, 0.0f};
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 3;
    catalog.catalog_hash = 0x1122334455667788ull;
    catalog.projectile_templates = &projectile_template;
    catalog.projectile_template_count = 1;
    catalog.collider_templates = &collider_template;
    catalog.collider_template_count = 1;
    catalog.collider_bindings = &collider_binding;
    catalog.collider_binding_count = 1;
    assert(Kernel_LoadGameplayCatalog(kernel, &catalog));
    benchmark_stats = KernelBenchmarkStats{};
    benchmark_stats.struct_size = sizeof(benchmark_stats);
    assert(Kernel_GetBenchmarkStats(kernel, &benchmark_stats));
    assert(benchmark_stats.catalog_version == 3);
    assert(benchmark_stats.catalog_hash == 0x1122334455667788ull);
    assert(Kernel_GetNetworkStats(kernel, &network_stats));
    assert(Kernel_PollDebugRecords(
               kernel,
               &debug_filter,
               debug_records.data(),
               static_cast<std::uint32_t>(debug_records.size())) == 0);
    assert(Kernel_GetLocalPlayerInfo(kernel, &local_info));
    assert(local_info.peer_id == 0);
    assert(local_info.player_net_id == 0);
    assert(local_info.has_welcome == 0u);
    assert(local_info.connected == 0u);
    assert(!Kernel_GetLocalPlayerInfo(kernel, nullptr));
    assert(!Kernel_StartClient(kernel, nullptr));
    assert(!Kernel_StartClient(kernel, ""));
    assert(!Kernel_ServerCreateEntity(kernel, &create_info, &created_net_id));
    assert(Kernel_StartDedicatedServer(kernel, 7777));

    assert(Kernel_ServerCreateEntity(kernel, &create_info, &created_net_id));
    assert(created_net_id != 0);
    server_state = KernelServerEntityState{};
    server_state.struct_size = sizeof(server_state);
    assert(Kernel_ServerGetEntityState(kernel, created_net_id, &server_state));
    assert(server_state.hp == 0);
    assert(server_state.max_hp == 0);

    combat_state.hp = 240;
    combat_state.max_hp = 240;
    combat_state.active_weapon_id = 3;
    combat_state.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    combat_state.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    combat_state.ammo[3] = 3;
    combat_state.reserve_ammo[3] = 6;
    combat_state.ammo[4] = 1;
    combat_state.reserve_ammo[4] = 1;
    combat_state.ammo[5] = 2;
    combat_state.reserve_ammo[5] = 2;
    combat_state.ammo[6] = 2;
    combat_state.reserve_ammo[6] = 2;
    assert(!Kernel_ServerSetEntityCombatState(kernel, created_net_id, nullptr));
    assert(Kernel_ServerSetEntityCombatState(kernel, created_net_id, &combat_state));
    const std::uint32_t collider_count = Kernel_QueryColliderShapes(
        kernel,
        &collider_query,
        collider_shapes.data(),
        static_cast<std::uint32_t>(collider_shapes.size()));
    assert(collider_count == 1);
    assert(collider_shapes[0].entity_net_id == created_net_id);
    assert(collider_shapes[0].entity_type == 2);
    assert(collider_shapes[0].collider_template_id == 10);
    assert(collider_shapes[0].shape_type == KernelColliderShapeType_Aabb);
    assert(collider_shapes[0].collider_id != 0);
    assert(collider_shapes[0].owner_net_id == created_net_id);
    assert(collider_shapes[0].world_center.y == 0.8f);
    assert(collider_shapes[0].half_extents.x == 0.25f);
    assert(collider_shapes[0].remaining_ticks == 0);

    KernelColliderTemplateDefinition changed_collider_template{};
    changed_collider_template.struct_size = sizeof(changed_collider_template);
    changed_collider_template.template_id = 11;
    changed_collider_template.shape_type = KernelColliderShapeType_Sphere;
    changed_collider_template.radius = 3.0f;
    changed_collider_template.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;
    changed_collider_template.purpose_flags = KernelColliderPurpose_Damage;
    std::array<KernelColliderTemplateDefinition, 2> changed_collider_templates = {
        collider_template,
        changed_collider_template,
    };
    KernelColliderBindingDefinition changed_collider_binding{};
    changed_collider_binding.struct_size = sizeof(changed_collider_binding);
    changed_collider_binding.entity_type = 2;
    changed_collider_binding.collider_template_id = 11;
    KernelGameplayCatalogDefinition changed_catalog{};
    changed_catalog.struct_size = sizeof(changed_catalog);
    changed_catalog.catalog_version = 4;
    changed_catalog.catalog_hash = 0x8877665544332211ull;
    changed_catalog.projectile_templates = &projectile_template;
    changed_catalog.projectile_template_count = 1;
    changed_catalog.collider_templates = changed_collider_templates.data();
    changed_catalog.collider_template_count =
        static_cast<std::uint32_t>(changed_collider_templates.size());
    changed_catalog.collider_bindings = &changed_collider_binding;
    changed_catalog.collider_binding_count = 1;
    assert(Kernel_LoadGameplayCatalog(kernel, &changed_catalog));
    for (KernelColliderShapeView& shape : collider_shapes) {
        shape = KernelColliderShapeView{};
        shape.struct_size = sizeof(KernelColliderShapeView);
    }
    assert(Kernel_QueryColliderShapes(
               kernel,
               &collider_query,
               collider_shapes.data(),
               static_cast<std::uint32_t>(collider_shapes.size())) == 1);
    assert(collider_shapes[0].collider_template_id == 10);
    assert(collider_shapes[0].shape_type == KernelColliderShapeType_Aabb);
    assert(collider_shapes[0].half_extents.x == 0.25f);
    assert(collider_shapes[0].radius != 3.0f);

    weapon_mechanics.weapon_id = 3;
    weapon_mechanics.fire_mode = KernelWeaponFireMode_Projectile;
    weapon_mechanics.magazine_size = 3;
    weapon_mechanics.damage = 5;
    weapon_mechanics.cooldown_ticks = 30;
    weapon_mechanics.reload_ticks = 30;
    weapon_mechanics.projectile.struct_size = sizeof(KernelProjectileMechanicsDefinition);
    weapon_mechanics.projectile.projectile_template_id = 3;
    weapon_mechanics.projectile.speed = 35.0f;
    weapon_mechanics.projectile.lifetime_seconds = 2.5f;
    weapon_mechanics.projectile.motion_model = KernelProjectileMotionModel_Linear;
    weapon_mechanics.projectile.hit_response = KernelProjectileHitResponse_Destroy;
    weapon_mechanics.projectile.damage_shape = KernelProjectileDamageShape_DirectHit;
    weapon_mechanics.projectile.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    weapon_mechanics.projectile.max_hit_count = 1;
    assert(Kernel_ServerValidateMechanicsConfig(&weapon_mechanics));
    assert(!Kernel_ServerSetEntityWeaponMechanics(kernel, created_net_id, nullptr));
    assert(Kernel_ServerSetEntityWeaponMechanics(
        kernel,
        created_net_id,
        &weapon_mechanics));
    KernelWeaponMechanicsDefinition queried_weapon{};
    queried_weapon.struct_size = sizeof(queried_weapon);
    assert(Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        created_net_id,
        3,
        &queried_weapon));
    assert(queried_weapon.weapon_id == 3);
    assert(queried_weapon.projectile.hit_response == KernelProjectileHitResponse_Destroy);
    assert(queried_weapon.projectile.damage_shape == KernelProjectileDamageShape_DirectHit);
    assert(queried_weapon.projectile.collision_mask == KERNEL_COLLISION_MASK_DAMAGEABLE);
    KernelWeaponMechanicsDefinition zero_projectile_mask = weapon_mechanics;
    zero_projectile_mask.projectile.collision_mask = 0;
    assert(Kernel_ServerValidateMechanicsConfig(&zero_projectile_mask));
    assert(Kernel_ServerSetEntityWeaponMechanics(
        kernel,
        created_net_id,
        &zero_projectile_mask));

    KernelWeaponMechanicsDefinition invalid_reserved_response = weapon_mechanics;
    invalid_reserved_response.projectile.hit_response = KernelProjectileHitResponse_Bounce;
    assert(!Kernel_ServerValidateMechanicsConfig(&invalid_reserved_response));
    invalid_reserved_response.projectile.hit_response = KernelProjectileHitResponse_Attach;
    assert(!Kernel_ServerValidateMechanicsConfig(&invalid_reserved_response));

    KernelWeaponMechanicsDefinition homing_weapon = weapon_mechanics;
    homing_weapon.weapon_id = 6;
    homing_weapon.projectile.projectile_template_id = 6;
    homing_weapon.projectile.motion_model = KernelProjectileMotionModel_Homing;
    homing_weapon.projectile.damage_shape = KernelProjectileDamageShape_DirectHit;
    homing_weapon.projectile.homing.struct_size = sizeof(KernelHomingMechanicsDefinition);
    homing_weapon.projectile.homing.homing_mode = KernelHomingMode_FireAndForget;
    homing_weapon.projectile.homing.sync_mode =
        KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
    homing_weapon.projectile.homing.boost_ticks = 1;
    homing_weapon.projectile.homing.lock_on_range = 25.0f;
    homing_weapon.projectile.homing.lose_target_range = 30.0f;
    homing_weapon.projectile.homing.lock_cone_degrees = 75.0f;
    homing_weapon.projectile.homing.max_turn_rate_degrees_per_second = 360.0f;
    homing_weapon.projectile.homing.acceleration = 20.0f;
    homing_weapon.projectile.homing.max_speed = 40.0f;
    assert(Kernel_ServerValidateMechanicsConfig(&homing_weapon));
    assert(Kernel_ServerSetEntityWeaponMechanics(kernel, created_net_id, &homing_weapon));
    queried_weapon = KernelWeaponMechanicsDefinition{};
    queried_weapon.struct_size = sizeof(queried_weapon);
    assert(Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        created_net_id,
        6,
        &queried_weapon));
    assert(queried_weapon.projectile.motion_model == KernelProjectileMotionModel_Homing);
    assert(queried_weapon.projectile.homing.lock_on_range == 25.0f);

    KernelWeaponMechanicsDefinition invalid_homing = homing_weapon;
    invalid_homing.projectile.homing.max_turn_rate_degrees_per_second = 0.0f;
    assert(!Kernel_ServerValidateMechanicsConfig(&invalid_homing));
    invalid_homing = homing_weapon;
    invalid_homing.projectile.homing.homing_mode = 99;
    assert(!Kernel_ServerValidateMechanicsConfig(&invalid_homing));
    invalid_homing = homing_weapon;
    invalid_homing.projectile.homing.sync_mode =
        KernelProjectileSyncMode_ServerSnapshotOnly;
    assert(!Kernel_ServerValidateMechanicsConfig(&invalid_homing));

    KernelWeaponMechanicsDefinition invalid_weapon = weapon_mechanics;
    invalid_weapon.struct_size = sizeof(invalid_weapon) - 1;
    assert(!Kernel_ServerValidateMechanicsConfig(&invalid_weapon));
    assert(!Kernel_ServerSetEntityWeaponMechanics(
        kernel,
        created_net_id,
        &invalid_weapon));

    KernelWeaponMechanicsDefinition area_weapon{};
    area_weapon.struct_size = sizeof(area_weapon);
    area_weapon.weapon_id = 4;
    area_weapon.fire_mode = KernelWeaponFireMode_AreaEffect;
    area_weapon.magazine_size = 2;
    area_weapon.damage = 7;
    area_weapon.cooldown_ticks = 10;
    area_weapon.reload_ticks = 30;
    area_weapon.area_effect.struct_size = sizeof(KernelAreaEffectMechanicsDefinition);
    area_weapon.area_effect.radius = 2.5f;
    area_weapon.area_effect.damage_per_interval = 7;
    area_weapon.area_effect.damage_interval_ticks = 3;
    area_weapon.area_effect.lifetime_ticks = 9;
    area_weapon.area_effect.spawn_distance = 1.5f;
    area_weapon.area_effect.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    assert(Kernel_ServerValidateMechanicsConfig(&area_weapon));
    assert(Kernel_ServerSetEntityWeaponMechanics(kernel, created_net_id, &area_weapon));
    queried_weapon = KernelWeaponMechanicsDefinition{};
    queried_weapon.struct_size = sizeof(queried_weapon);
    assert(Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        created_net_id,
        4,
        &queried_weapon));
    assert(queried_weapon.fire_mode == KernelWeaponFireMode_AreaEffect);
    assert(queried_weapon.area_effect.radius == 2.5f);
    assert(queried_weapon.area_effect.damage_interval_ticks == 3);
    KernelWeaponMechanicsDefinition zero_area_mask = area_weapon;
    zero_area_mask.area_effect.collision_mask = 0;
    assert(Kernel_ServerValidateMechanicsConfig(&zero_area_mask));
    assert(Kernel_ServerSetEntityWeaponMechanics(
        kernel,
        created_net_id,
        &zero_area_mask));

    KernelWeaponMechanicsDefinition beam_weapon{};
    beam_weapon.struct_size = sizeof(beam_weapon);
    beam_weapon.weapon_id = 5;
    beam_weapon.fire_mode = KernelWeaponFireMode_Beam;
    beam_weapon.magazine_size = 2;
    beam_weapon.damage = 30;
    beam_weapon.cooldown_ticks = 1;
    beam_weapon.reload_ticks = 30;
    beam_weapon.beam.struct_size = sizeof(KernelBeamMechanicsDefinition);
    beam_weapon.beam.length = 6.0f;
    beam_weapon.beam.radius = 0.25f;
    beam_weapon.beam.damage_per_second = 30;
    beam_weapon.beam.lifetime_ticks = 2;
    beam_weapon.beam.collision_mask = KERNEL_COLLISION_LAYER_ENEMY;
    assert(Kernel_ServerValidateMechanicsConfig(&beam_weapon));
    assert(Kernel_ServerSetEntityWeaponMechanics(kernel, created_net_id, &beam_weapon));
    queried_weapon = KernelWeaponMechanicsDefinition{};
    queried_weapon.struct_size = sizeof(queried_weapon);
    assert(Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        created_net_id,
        5,
        &queried_weapon));
    assert(queried_weapon.fire_mode == KernelWeaponFireMode_Beam);
    assert(queried_weapon.beam.length == 6.0f);
    assert(queried_weapon.beam.damage_per_second == 30);

    KernelWeaponMechanicsDefinition invalid_beam = beam_weapon;
    invalid_beam.beam.radius = 0.0f;
    assert(!Kernel_ServerValidateMechanicsConfig(&invalid_beam));
    invalid_beam = beam_weapon;
    invalid_beam.beam.collision_mask = 0;
    assert(Kernel_ServerValidateMechanicsConfig(&invalid_beam));
    assert(Kernel_ServerSetEntityWeaponMechanics(kernel, created_net_id, &invalid_beam));

    KernelVec3 enemy_position{5.0f, 0.0f, 0.0f};
    KernelQuat enemy_rotation{0.0f, 0.0f, 0.0f, 1.0f};
    assert(Kernel_ServerSetEntityTransform(
        kernel,
        created_net_id,
        &enemy_position,
        &enemy_rotation));
    KernelVec3 enemy_velocity{1.0f, 0.0f, 0.0f};
    assert(Kernel_ServerSetEntityVelocity(kernel, created_net_id, &enemy_velocity));
    assert(Kernel_ServerSetEntityState(kernel, created_net_id, 7, 0x12345678u));
    server_entity_input.buttons = 0;
    server_entity_input.selected_weapon = 3;
    server_entity_input.aim_dir = KernelVec3{-1.0f, 0.0f, 0.0f};
    assert(Kernel_ServerSubmitEntityInput(
        kernel,
        created_net_id,
        &server_entity_input));
    server_state = KernelServerEntityState{};
    server_state.struct_size = sizeof(server_state);
    assert(Kernel_ServerGetEntityState(kernel, created_net_id, &server_state));
    assert(server_state.valid != 0u);
    assert(server_state.net_id == created_net_id);
    assert(server_state.entity_type == 2);
    assert(server_state.animation_state == 7);
    assert(
        server_state.visual_flags ==
        (0x12345678u | KERNEL_VISUAL_FLAG_MOVING));
    assert(server_state.position.x == 5.0f);
    assert(server_state.velocity.x == 1.0f);
    assert(server_state.hp == 240);
    assert(server_state.max_hp == 240);
    std::array<KernelServerEntityState, 4> queried_states{};
    for (KernelServerEntityState& queried_state : queried_states) {
        queried_state.struct_size = sizeof(KernelServerEntityState);
    }
    assert(Kernel_ServerQueryEntities(
               kernel,
               2,
               queried_states.data(),
               static_cast<std::uint32_t>(queried_states.size())) == 1);
    assert(queried_states[0].net_id == created_net_id);
    assert(queried_states[0].hp == 240);
    assert(queried_states[0].max_hp == 240);
    assert(Kernel_ServerQueryEntities(
               kernel,
               0,
               queried_states.data(),
               static_cast<std::uint32_t>(queried_states.size())) == 1);

    PlayerInput input{};
    input.input_seq = 1;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    Kernel_SubmitInput(kernel, 1, &input);
    Kernel_Update(kernel, 1.0f / 30.0f);
    server_state = KernelServerEntityState{};
    server_state.struct_size = sizeof(server_state);
    assert(Kernel_ServerGetEntityState(kernel, created_net_id, &server_state));
    assert(server_state.position.x > 5.0f);
    assert(server_state.hp == 240);
    assert(server_state.max_hp == 240);

    std::array<RenderEntityState, 8> states{};
    assert(Kernel_GetRenderStates(kernel, nullptr, states.size()) == 0);
    assert(Kernel_GetRenderStates(kernel, states.data(), 0) == 0);
    assert(Kernel_GetRenderStatesAtTime(kernel, 0, nullptr, states.size()) == 0);
    assert(Kernel_GetRenderStatesAtTime(kernel, 0, states.data(), 0) == 0);
    const std::uint32_t render_count =
        Kernel_GetRenderStates(kernel, states.data(), states.size());
    assert(render_count == 1);
    assert(states[0].net_id == created_net_id);
    assert(states[0].entity_id != 0);
    assert(states[0].owner_peer == 0);
    assert(states[0].position.x > 5.0f);
    assert(states[0].velocity.x == 1.0f);
    assert(states[0].hp == 240);
    assert(states[0].max_hp == 240);
    assert(states[0].animation_state == 7);
    assert(
        states[0].visual_flags ==
        (0x12345678u | KERNEL_VISUAL_FLAG_MOVING));
    assert(states[0].spawn_tick == 0);
    assert(states[0].client_action_id == 0);
    const std::uint32_t render_at_time_count =
        Kernel_GetRenderStatesAtTime(kernel, 33333, states.data(), states.size());
    assert(render_at_time_count == 1);
    assert(states[0].net_id == created_net_id);
    assert(states[0].hp == 240);
    assert(states[0].max_hp == 240);

    server_entity_input.buttons = InputButton_Fire;
    server_entity_input.selected_weapon = 4;
    server_entity_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    assert(Kernel_ServerSubmitEntityInput(
        kernel,
        created_net_id,
        &server_entity_input));
    Kernel_Update(kernel, 1.0f / 30.0f);
    std::array<KernelEvent, 16> area_events{};
    const std::uint32_t area_event_count =
        Kernel_PollEvents(kernel, area_events.data(), area_events.size());
    std::uint32_t area_net_id = 0;
    for (std::uint32_t index = 0; index < area_event_count; ++index) {
        if (area_events[index].type == KernelEventType_EntitySpawned &&
            area_events[index].code == 4u) {
            area_net_id = area_events[index].net_id;
        }
    }
    assert(area_net_id != 0);
    area_effect_state = KernelAreaEffectState{};
    area_effect_state.struct_size = sizeof(area_effect_state);
    assert(Kernel_ServerGetAreaEffectState(kernel, area_net_id, &area_effect_state));
    assert(area_effect_state.valid != 0u);
    assert(area_effect_state.radius == 2.5f);
    assert(area_effect_state.damage_per_interval == 7);
    assert(area_effect_state.damage_interval_ticks == 3);
    assert(area_effect_state.collision_mask == 0);

    server_entity_input.buttons = InputButton_Fire;
    server_entity_input.selected_weapon = 5;
    server_entity_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    assert(Kernel_ServerSubmitEntityInput(
        kernel,
        created_net_id,
        &server_entity_input));
    Kernel_Update(kernel, 1.0f / 30.0f);
    std::array<KernelEvent, 16> beam_events{};
    const std::uint32_t beam_event_count =
        Kernel_PollEvents(kernel, beam_events.data(), beam_events.size());
    std::uint32_t beam_net_id = 0;
    for (std::uint32_t index = 0; index < beam_event_count; ++index) {
        if (beam_events[index].type == KernelEventType_EntitySpawned &&
            beam_events[index].code == 5u) {
            beam_net_id = beam_events[index].net_id;
        }
    }
    assert(beam_net_id != 0);
    beam_state = KernelBeamState{};
    beam_state.struct_size = sizeof(beam_state);
    assert(Kernel_ServerGetBeamState(kernel, beam_net_id, &beam_state));
    assert(beam_state.valid != 0u);
    assert(beam_state.shooter_net_id == created_net_id);
    assert(beam_state.length == 6.0f);
    assert(beam_state.radius == 0.25f);
    assert(beam_state.damage_per_second == 30);
    assert(beam_state.collision_mask == 0);

    server_entity_input.buttons = InputButton_Fire;
    server_entity_input.selected_weapon = 6;
    server_entity_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    assert(Kernel_ServerSubmitEntityInput(
        kernel,
        created_net_id,
        &server_entity_input));
    Kernel_Update(kernel, 1.0f / 30.0f);
    std::array<KernelEvent, 16> homing_events{};
    const std::uint32_t homing_event_count =
        Kernel_PollEvents(kernel, homing_events.data(), homing_events.size());
    std::uint32_t homing_net_id = 0;
    for (std::uint32_t index = 0; index < homing_event_count; ++index) {
        if (homing_events[index].type == KernelEventType_EntitySpawned &&
            homing_events[index].code == 3u) {
            homing_net_id = homing_events[index].net_id;
        }
    }
    assert(homing_net_id != 0);
    homing_state = KernelHomingState{};
    homing_state.struct_size = sizeof(homing_state);
    assert(Kernel_ServerGetHomingState(kernel, homing_net_id, &homing_state));
    assert(homing_state.valid != 0u);
    assert(homing_state.shooter_net_id == created_net_id);
    assert(homing_state.guidance_phase <= KernelMissileGuidancePhase_LostTarget);
    assert(homing_state.lock_on_range == 25.0f);
    assert(homing_state.max_speed == 40.0f);

    std::array<KernelEvent, 16> events{};
    assert(Kernel_PollEvents(kernel, nullptr, events.size()) == 0);
    assert(Kernel_PollEvents(kernel, events.data(), 0) == 0);
    Kernel_PollEvents(kernel, events.data(), events.size());
    assert(Kernel_ServerDestroyEntity(
        kernel,
        created_net_id,
        KernelDespawnReason_Destroyed));

    assert(!Kernel_ServerClearEntityWeaponMechanics(kernel, created_net_id, 3));

    Kernel_Destroy(kernel);
    return 0;
}
