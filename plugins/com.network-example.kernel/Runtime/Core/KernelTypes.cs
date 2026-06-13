using System;
using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    public static class KernelConstants
    {
        public const uint AbiVersion = 17;
        public const int BuildInfoTextSize = 128;
        public const int LANDiscoveryTextSize = 128;
        public const int GameplayCatalogLoadPathSize = 128;
        public const int GameplayCatalogLoadFieldSize = 64;
        public const int GameplayCatalogLoadDiagnosticSize = 256;
        public const ushort LANDiscoveryDefaultPort = 47777;
        public const int MaxWeapons = 7;
        public const byte DebugWildcardU8 = 0xff;

        public const uint GameplayCatalogLoadStatusFailed = 0;
        public const uint GameplayCatalogLoadStatusSuccess = 1;
        public const uint GameplayCatalogLoadErrorNone = 0;
        public const uint GameplayCatalogLoadErrorInvalidArgument = 1;
        public const uint GameplayCatalogLoadErrorInvalidYaml = 2;
        public const uint GameplayCatalogLoadErrorUnsupportedCatalogVersion = 3;
        public const uint GameplayCatalogLoadErrorUnknownField = 4;
        public const uint GameplayCatalogLoadErrorMissingRequiredField = 5;
        public const uint GameplayCatalogLoadErrorInvalidFieldType = 6;
        public const uint GameplayCatalogLoadErrorInvalidEnumValue = 7;
        public const uint GameplayCatalogLoadErrorDuplicateTemplateId = 8;
        public const uint GameplayCatalogLoadErrorDuplicateTemplateName = 9;
        public const uint GameplayCatalogLoadErrorMissingTemplateReference = 10;
        public const uint GameplayCatalogLoadErrorInvalidNumericRange = 11;
        public const uint GameplayCatalogLoadErrorInvalidArchivePath = 12;
        public const uint GameplayCatalogLoadErrorDuplicateArchiveEntry = 13;
        public const uint GameplayCatalogLoadErrorMissingBundleEntry = 14;
        public const uint GameplayCatalogLoadErrorKernelRejectedCatalog = 15;
        public const uint GameplayCatalogLoadErrorUnknown = 255;
        public const uint GameplayCatalogLoadSourceUnknown = 0;
        public const uint GameplayCatalogLoadSourceFilesystem = 1;
        public const uint GameplayCatalogLoadSourceBundle = 2;
        public const uint GameplayCatalogTemplateKindUnknown = 0;
        public const uint GameplayCatalogTemplateKindCatalog = 1;
        public const uint GameplayCatalogTemplateKindWeapon = 2;
        public const uint GameplayCatalogTemplateKindProjectile = 3;
        public const uint GameplayCatalogTemplateKindActor = 4;
        public const uint GameplayCatalogTemplateKindCollider = 5;

        public const ulong CapabilityClientMode = 0x0000000000000001UL;
        public const ulong CapabilityListenServerMode = 0x0000000000000002UL;
        public const ulong CapabilityDedicatedServerMode = 0x0000000000000004UL;
        public const ulong CapabilityInputSubmission = 0x0000000000000008UL;
        public const ulong CapabilityRenderStates = 0x0000000000000010UL;
        public const ulong CapabilityEventPolling = 0x0000000000000020UL;
        public const ulong CapabilityClientPrediction = 0x0000000000000040UL;
        public const ulong CapabilitySnapshotInterpolation = 0x0000000000000080UL;
        public const ulong CapabilityLagCompensatedHitscan = 0x0000000000000100UL;
        public const ulong CapabilityLocalPlayerInfo = 0x0000000000000200UL;
        public const ulong CapabilityServerEntityCreate = 0x0000000000000400UL;
        public const ulong CapabilityServerEntityDestroy = 0x0000000000000800UL;
        public const ulong CapabilityServerEntityTransformWrite = 0x0000000000001000UL;
        public const ulong CapabilityServerEntityVelocityWrite = 0x0000000000002000UL;
        public const ulong CapabilityServerEntityStateWrite = 0x0000000000004000UL;
        public const ulong CapabilityServerEntityQuery = 0x0000000000008000UL;
        public const ulong CapabilityServerRelevanceFilter = 0x0000000000010000UL;
        public const ulong CapabilityLagCompensatedProjectile = 0x0000000000020000UL;
        public const ulong CapabilityEventPresentationTime = 0x0000000000040000UL;
        public const ulong CapabilityRenderStatesAtTime = 0x0000000000080000UL;
        public const ulong CapabilityServerMechanicsConfig = 0x0000000000100000UL;
        public const ulong CapabilityWeaponMetadataQuery = 0x0000000000200000UL;
        public const ulong CapabilityAreaEffectWeapons = 0x0000000000400000UL;
        public const ulong CapabilityProjectileResponseMasks = 0x0000000000800000UL;
        public const ulong CapabilityBeamWeapons = 0x0000000001000000UL;
        public const ulong CapabilityHomingProjectiles = 0x0000000002000000UL;
        public const ulong CapabilityLANDiscovery = 0x0000000004000000UL;
        public const ulong CapabilityGameplayCatalog = 0x0000000008000000UL;
        public const ulong CapabilityProjectileSpawnBatch = 0x0000000010000000UL;
        public const ulong CapabilityDebugRecords = 0x0000000020000000UL;
        public const ulong CapabilityColliderShapeQuery = 0x0000000040000000UL;
        public const ulong CapabilityBenchmarkStats = 0x0000000080000000UL;
        public const ulong CapabilityNetworkStats = 0x0000000100000000UL;

        public const uint CollisionLayerPlayer = 0x00000001U;
        public const uint CollisionLayerEnemy = 0x00000002U;
        public const uint CollisionLayerProjectile = 0x00000004U;
        public const uint CollisionLayerAreaEffect = 0x00000008U;
        public const uint CollisionMaskDamageable = CollisionLayerPlayer | CollisionLayerEnemy;

        public const uint VisualFlagMoving = 0x00000001U;
        public const uint VisualFlagReloading = 0x00000002U;
        public const uint VisualFlagDead = 0x00000004U;
    }

    public static class NetworkKernelPackageInfo
    {
        public const string Name = "com.network-example.kernel";
        public const string Version = "0.6.5";
    }

    public enum KernelMode
    {
        Client = 0,
        ListenServer = 1,
        DedicatedServer = 2,
    }

    public enum KernelEventType
    {
        Connected = 0,
        Disconnected = 1,
        PlayerJoined = 2,
        PlayerLeft = 3,
        EntitySpawned = 4,
        EntityDestroyed = 5,
        FireConfirmed = 6,
        HitConfirmed = 7,
        DamageApplied = 8,
        Explosion = 9,
        MissionStateChanged = 10,
        Error = 11,
    }

    public enum KernelDespawnReason : uint
    {
        Destroyed = 0,
        OutOfRange = 1,
        Disconnected = 2,
    }

    public enum KernelEntityType : ushort
    {
        Unknown = 0,
        Player = 1,
        Enemy = 2,
        Projectile = 3,
        AreaEffect = 4,
        Beam = 5,
    }

    [Flags]
    public enum InputButton : uint
    {
        MoveJump = 1U << 0,
        Fire = 1U << 1,
        Reload = 1U << 2,
        Sprint = 1U << 3,
        Interact = 1U << 4,
        Ability1 = 1U << 5,
        Dodge = 1U << 6,
        Parry = 1U << 7,
    }

    public enum KernelWeaponFireMode : byte
    {
        Hitscan = 0,
        Shotgun = 1,
        Projectile = 2,
        AreaEffect = 3,
        Beam = 4,
    }

    public enum KernelProjectileMotionModel : byte
    {
        Linear = 0,
        Parabolic = 1,
        Homing = 2,
    }

    public enum KernelProjectileSyncMode : byte
    {
        LocalPredictedDeterministic = 0,
        HybridDeterministicThenSnapshot = 1,
        ServerSnapshotOnly = 2,
    }

    public enum KernelMissileGuidancePhase : byte
    {
        Boost = 0,
        Guided = 1,
        LostTarget = 2,
        Expired = 3,
    }

    public enum KernelHomingMode : byte
    {
        FireAndForget = 0,
    }

    public enum KernelProjectileHitResponse : byte
    {
        Destroy = 0,
        Continue = 1,
        Bounce = 2,
        Attach = 3,
    }

    public enum KernelProjectileDamageShape : byte
    {
        DirectHit = 0,
        PiercingSegment = 2,
    }

    public enum KernelProjectileKind : byte
    {
        Projectile = 0,
        AreaEffect = 1,
    }

    public enum KernelProjectileImpactAction : byte
    {
        None = 0,
        SpawnProjectile = 1,
    }

    public enum KernelProjectileDamageFalloff : byte
    {
        None = 0,
        Linear = 1,
    }

    public enum KernelColliderShapeType : byte
    {
        Aabb = 0,
        Sphere = 1,
        OrientedBox = 2,
        Segment = 3,
    }

    [Flags]
    public enum KernelColliderPurpose : uint
    {
        Hit = 1U << 0,
        Damage = 1U << 1,
        Trigger = 1U << 2,
    }

    [Flags]
    public enum KernelDebugRecordType : uint
    {
        Hit = 1U << 0,
        Projectile = 1U << 1,
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelAbiInfo
    {
        public uint struct_size;
        public uint abi_version;
        public uint kernel_config_size;
        public uint player_input_size;
        public uint render_entity_state_size;
        public uint kernel_event_size;
        public uint local_player_info_size;
        public uint server_entity_create_info_size;
        public uint server_entity_state_size;
        public uint weapon_mechanics_definition_size;
        public uint projectile_mechanics_definition_size;
        public uint combat_state_definition_size;
        public uint area_effect_mechanics_definition_size;
        public uint area_effect_state_size;
        public uint beam_mechanics_definition_size;
        public uint beam_state_size;
        public uint homing_mechanics_definition_size;
        public uint homing_state_size;
        public uint lan_discovery_server_config_size;
        public uint lan_discovery_query_config_size;
        public uint lan_discovery_result_size;
        public ulong capability_flags;
        public uint gameplay_catalog_definition_size;
        public uint gameplay_catalog_load_result_size;
        public uint projectile_template_definition_size;
        public uint collider_template_definition_size;
        public uint collider_binding_definition_size;
        public uint benchmark_stats_size;
        public uint network_stats_size;
        public uint debug_record_filter_size;
        public uint debug_info_size;
        public uint collider_shape_query_size;
        public uint collider_shape_view_size;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct KernelBuildInfo
    {
        public uint struct_size;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.BuildInfoTextSize)]
        public string module_name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.BuildInfoTextSize)]
        public string module_file_name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.BuildInfoTextSize)]
        public string module_version;
        public uint protocol_version;
        public uint snapshot_schema_version;
        public uint packet_schema_version;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.BuildInfoTextSize)]
        public string git_commit;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.BuildInfoTextSize)]
        public string build_timestamp;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.BuildInfoTextSize)]
        public string build_platform;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.BuildInfoTextSize)]
        public string build_config;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.BuildInfoTextSize)]
        public string compiler_info;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelLocalPlayerInfo
    {
        public uint peer_id;
        public uint player_net_id;
        public uint has_welcome;
        public uint connected;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct KernelLANDiscoveryServerConfig
    {
        public uint struct_size;
        public ushort discovery_port;
        public ushort server_endpoint_port;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.LANDiscoveryTextSize)]
        public string server_name;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelLANDiscoveryServerConfig>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelLANDiscoveryQueryConfig
    {
        public uint struct_size;
        public ushort discovery_port;
        public uint timeout_ms;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelLANDiscoveryQueryConfig>();
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct KernelLANDiscoveryResult
    {
        public uint struct_size;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.LANDiscoveryTextSize)]
        public string server_name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.LANDiscoveryTextSize)]
        public string server_endpoint_ip;
        public ushort server_endpoint_port;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.LANDiscoveryTextSize)]
        public string module_version;
        public uint protocol_version;
        public uint snapshot_schema_version;
        public uint packet_schema_version;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.LANDiscoveryTextSize)]
        public string git_commit;
        public uint compatible;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelLANDiscoveryResult>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelVec2
    {
        public float x;
        public float y;

        public KernelVec2(float x, float y)
        {
            this.x = x;
            this.y = y;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelVec3
    {
        public float x;
        public float y;
        public float z;

        public KernelVec3(float x, float y, float z)
        {
            this.x = x;
            this.y = y;
            this.z = z;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelQuat
    {
        public float x;
        public float y;
        public float z;
        public float w;

        public KernelQuat(float x, float y, float z, float w)
        {
            this.x = x;
            this.y = y;
            this.z = z;
            this.w = w;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct TickConfig
    {
        public uint server_tick_rate;
        public uint snapshot_rate;
        public uint history_ms;
        public uint max_ticks_per_update;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelConfig
    {
        public KernelMode mode;
        public TickConfig tick;
        public uint max_render_states;
        public uint max_events;

        public static KernelConfig CreateDefault(KernelMode mode)
        {
            return new KernelConfig
            {
                mode = mode,
                tick = new TickConfig
                {
                    server_tick_rate = 30,
                    snapshot_rate = 15,
                    history_ms = 500,
                    max_ticks_per_update = 4,
                },
                max_render_states = 256,
                max_events = 256,
            };
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PlayerInput
    {
        public uint input_seq;
        public ulong client_action_time_us;
        public uint client_action_id;
        public KernelVec2 move;
        public KernelVec2 look_delta;
        public KernelVec3 aim_dir;
        public uint buttons;
        public byte selected_weapon;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RenderEntityState
    {
        public ulong entity_id;
        public uint net_id;
        public KernelEntityType entity_type;
        public uint owner_peer;
        public KernelVec3 position;
        public KernelQuat rotation;
        public KernelVec3 velocity;
        public ushort hp;
        public ushort max_hp;
        public ushort animation_state;
        public uint visual_flags;
        public uint spawn_tick;
        public uint client_action_id;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelServerEntityCreateInfo
    {
        public uint struct_size;
        public KernelEntityType entity_type;
        public uint owner_peer;
        public KernelVec3 position;
        public KernelQuat rotation;
        public ushort animation_state;
        public uint visual_flags;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelServerEntityCreateInfo>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelServerEntityState
    {
        public uint struct_size;
        public uint net_id;
        public KernelEntityType entity_type;
        public uint owner_peer;
        public KernelVec3 position;
        public KernelQuat rotation;
        public KernelVec3 velocity;
        public ushort hp;
        public ushort max_hp;
        public ushort animation_state;
        public uint visual_flags;
        public uint valid;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelServerEntityState>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelColliderTemplateDefinition
    {
        public uint struct_size;
        public uint template_id;
        public byte shape_type;
        public byte reserved0;
        public ushort reserved1;
        public KernelVec3 center;
        public KernelVec3 half_extents;
        public float radius;
        public uint purpose_flags;
        public uint layer_mask;
        public float segment_length;
        public float scatter_degrees;
        public uint lifetime_ticks;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelColliderTemplateDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelColliderBindingDefinition
    {
        public uint struct_size;
        public ushort entity_type;
        public ushort reserved0;
        public uint collider_template_id;
        public KernelVec3 local_position;
        public KernelQuat local_rotation;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelColliderBindingDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelProjectileTemplateDefinition
    {
        public uint struct_size;
        public uint projectile_template_id;
        public byte weapon_id;
        public byte motion_model;
        public byte sync_mode;
        public byte hit_response;
        public byte damage_shape;
        public byte projectile_kind;
        public byte impact_action;
        public uint impact_destroy_self;
        public byte damage_falloff;
        public byte reserved0;
        public ushort damage;
        public float speed;
        public float lifetime_seconds;
        public KernelVec3 gravity;
        public uint collider_template_id;
        public uint collision_mask;
        public uint max_hit_count;
        public uint impact_projectile_template_id;
        public uint lifetime_ticks;
        public uint damage_interval_ticks;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelProjectileTemplateDefinition>();
    }

    public struct KernelGameplayCatalog
    {
        public uint CatalogVersion;
        public ulong CatalogHash;
        public KernelProjectileTemplateDefinition[] ProjectileTemplates;
        public KernelColliderTemplateDefinition[] ColliderTemplates;
        public KernelColliderBindingDefinition[] ColliderBindings;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct KernelGameplayCatalogLoadResult
    {
        public uint struct_size;
        public uint status;
        public uint catalog_version;
        public ulong catalog_hash;
        public uint projectile_template_count;
        public uint collider_template_count;
        public uint collider_binding_count;
        public uint error_code;
        public uint source_kind;
        public uint template_kind;
        public uint template_id;
        public uint field_id;
        public int line;
        public int column;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.GameplayCatalogLoadPathSize)]
        public string path;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.GameplayCatalogLoadFieldSize)]
        public string field;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = KernelConstants.GameplayCatalogLoadDiagnosticSize)]
        public string diagnostic;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelGameplayCatalogLoadResult>();
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct KernelGameplayCatalogDefinition
    {
        public uint struct_size;
        public uint catalog_version;
        public ulong catalog_hash;
        public IntPtr projectile_templates;
        public uint projectile_template_count;
        public IntPtr collider_templates;
        public uint collider_template_count;
        public IntPtr collider_bindings;
        public uint collider_binding_count;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelGameplayCatalogDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelBenchmarkStats
    {
        public uint struct_size;
        public uint catalog_version;
        public ulong catalog_hash;
        public uint total_entity_count;
        public uint projectile_count;
        public uint event_spawn_projectile_count;
        public uint snapshot_only_projectile_count;
        public uint hybrid_projectile_count;
        public float event_spawn_ratio;
        public float snapshot_only_ratio;
        public float hybrid_ratio;
        public ulong render_solver_cost_us;
        public ulong projectile_solver_cost_us;
        public ulong hybrid_correction_cost_us;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelBenchmarkStats>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelNetworkStats
    {
        public uint struct_size;
        public ulong snapshot_bytes_sent;
        public ulong event_bytes_sent;
        public ulong reliable_bytes_sent;
        public ulong unreliable_bytes_sent;
        public uint packet_count_sent;
        public uint average_packet_size;
        public uint max_packet_size;
        public ulong packet_serialization_cost_us;
        public ulong packet_deserialization_cost_us;
        public ulong rtt_us;
        public ulong jitter_us;
        public float loss_ratio;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelNetworkStats>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelHitDebugInfo
    {
        public uint source_net_id;
        public uint target_net_id;
        public byte weapon_id;
        public byte reserved0;
        public ushort reserved1;
        public KernelVec3 position;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelProjectileDebugInfo
    {
        public uint projectile_net_id;
        public uint owner_net_id;
        public uint owner_peer;
        public byte weapon_id;
        public byte motion_model;
        public byte sync_mode;
        public byte reserved0;
        public KernelVec3 position;
        public KernelVec3 velocity;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct KernelDebugInfoData
    {
        [FieldOffset(0)]
        public KernelHitDebugInfo hit;

        [FieldOffset(0)]
        public KernelProjectileDebugInfo projectile;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelDebugInfo
    {
        public uint struct_size;
        public uint tick;
        public byte record_type;
        public byte flags;
        public ushort reserved0;
        public KernelDebugInfoData data;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelDebugInfo>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelDebugRecordFilter
    {
        public uint struct_size;
        public uint record_type_mask;
        public uint source_net_id;
        public uint target_net_id;
        public uint projectile_net_id;
        public byte weapon_id;
        public byte motion_model;
        public byte sync_mode;
        public byte reserved0;
        public uint min_tick;
        public uint max_tick;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelDebugRecordFilter>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelColliderShapeQuery
    {
        public uint struct_size;
        public ushort entity_type_filter;
        public ushort reserved0;
        public uint entity_net_id;
        public uint purpose_mask;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelColliderShapeQuery>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelColliderShapeView
    {
        public uint struct_size;
        public uint entity_net_id;
        public ushort entity_type;
        public ushort reserved0;
        public uint collider_template_id;
        public byte shape_type;
        public byte reserved1;
        public ushort reserved2;
        public KernelVec3 world_center;
        public KernelVec3 half_extents;
        public float radius;
        public uint purpose_flags;
        public uint layer_mask;
        public uint collider_id;
        public uint owner_net_id;
        public KernelQuat world_rotation;
        public KernelVec3 segment_start;
        public KernelVec3 segment_end;
        public uint lifetime_ticks;
        public uint remaining_ticks;
        public uint has_resolved_damage;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelColliderShapeView>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelHomingMechanicsDefinition
    {
        public uint struct_size;
        public byte homing_mode;
        public byte sync_mode;
        public ushort reserved0;
        public uint boost_ticks;
        public float lock_on_range;
        public float lose_target_range;
        public float lock_cone_degrees;
        public float max_turn_rate_degrees_per_second;
        public float acceleration;
        public float max_speed;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelHomingMechanicsDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelProjectileMechanicsDefinition
    {
        public uint struct_size;
        public byte motion_model;
        public byte hit_response;
        public byte damage_shape;
        public byte reserved0;
        public float speed;
        public float lifetime_seconds;
        public KernelVec3 gravity;
        public uint collision_mask;
        public uint max_hit_count;
        public KernelHomingMechanicsDefinition homing;
        public uint projectile_template_id;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelProjectileMechanicsDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelAreaEffectMechanicsDefinition
    {
        public uint struct_size;
        public float radius;
        public ushort damage_per_interval;
        public uint damage_interval_ticks;
        public uint lifetime_ticks;
        public float spawn_distance;
        public uint collision_mask;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelAreaEffectMechanicsDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelBeamMechanicsDefinition
    {
        public uint struct_size;
        public float length;
        public float radius;
        public ushort damage_per_second;
        public uint lifetime_ticks;
        public uint collision_mask;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelBeamMechanicsDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelWeaponMechanicsDefinition
    {
        public uint struct_size;
        public byte weapon_id;
        public byte fire_mode;
        public ushort magazine_size;
        public ushort damage;
        public uint cooldown_ticks;
        public uint reload_ticks;
        public float max_range;
        public byte pellet_count;
        public float pellet_spread;
        public KernelProjectileMechanicsDefinition projectile;
        public KernelAreaEffectMechanicsDefinition area_effect;
        public KernelBeamMechanicsDefinition beam;
        public uint segment_collider_template_id;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelWeaponMechanicsDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelBeamState
    {
        public uint struct_size;
        public uint net_id;
        public uint owner_peer;
        public uint shooter_net_id;
        public KernelVec3 origin;
        public KernelVec3 direction;
        public float length;
        public float radius;
        public ushort damage_per_second;
        public uint expire_tick;
        public byte source_code;
        public uint collision_mask;
        public uint valid;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelBeamState>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelAreaEffectState
    {
        public uint struct_size;
        public uint net_id;
        public uint owner_peer;
        public float radius;
        public ushort damage_per_interval;
        public uint damage_interval_ticks;
        public uint expire_tick;
        public byte source_code;
        public uint collision_mask;
        public uint valid;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelAreaEffectState>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelHomingState
    {
        public uint struct_size;
        public uint net_id;
        public uint owner_peer;
        public uint shooter_net_id;
        public uint target_net_id;
        public byte homing_mode;
        public byte sync_mode;
        public byte guidance_phase;
        public byte reserved0;
        public uint boost_ticks;
        public uint guidance_start_tick;
        public float lock_on_range;
        public float lose_target_range;
        public float lock_cone_degrees;
        public float max_turn_rate_degrees_per_second;
        public float acceleration;
        public float max_speed;
        public uint valid;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelHomingState>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelCombatStateDefinition
    {
        public uint struct_size;
        public ushort hp;
        public ushort max_hp;
        public byte active_weapon_id;
        public float move_speed_meters_per_second;
        public KernelVec3 hitbox_center;
        public KernelVec3 hitbox_half_extents;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxWeapons)]
        public ushort[] ammo;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxWeapons)]
        public ushort[] reserve_ammo;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelCombatStateDefinition>();

        public static KernelCombatStateDefinition Create()
        {
            return new KernelCombatStateDefinition
            {
                struct_size = StructSize,
                ammo = new ushort[KernelConstants.MaxWeapons],
                reserve_ammo = new ushort[KernelConstants.MaxWeapons],
            };
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelEvent
    {
        public KernelEventType type;
        public uint tick;
        public uint net_id;
        public uint peer_id;
        public uint code;
        public ulong event_time_us;
        public ulong presentation_time_us;
    }
}
