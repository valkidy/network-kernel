using System;
using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    public static class KernelConstants
    {
        public const uint AbiVersion = 75;
        public const int BuildInfoTextSize = 128;
        public const int LANDiscoveryTextSize = 128;
        public const int GameplayCatalogEntryPathSize = 128;
        public const int GameplayCatalogContentNamespaceSize = 64;
        public const int GameplayCatalogSha256Size = 32;
        public const int GameplayCatalogLoadPathSize = 128;
        public const int GameplayCatalogLoadFieldSize = 64;
        public const int GameplayCatalogLoadDiagnosticSize = 256;
        public const ushort LANDiscoveryDefaultPort = 47777;
        public const uint GameplayCatalogSyncMaxBundleSize = 1024U * 1024U;
        public const uint GameplayCatalogSyncDefaultMaxBundleSize =
            GameplayCatalogSyncMaxBundleSize;
        public const uint GameplayCatalogSyncDefaultTimeoutMs = 30000U;
        public const uint StaticCollisionSceneMaxBytes = 16U * 1024U * 1024U;
        public const uint StaticCollisionLayerTerrain = 1U;
        public const int MaxWeaponSlots = 4;
        public const int MaxSkeletonLegs = 8;
        public const int MaxFootholdCandidates = 8;
        public const int MaxSkeletonColliders = 16;
        public const int MaxActionGraphActions = 8;
        public const int MaxPortableStateFields = 8;
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
        public const uint GameplayCatalogTemplateKindAction = 6;
        public const uint GameplayCatalogTemplateKindItem = 7;

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
        public const ulong CapabilityProjectileResponseMasks = 0x0000000000800000UL;
        public const ulong CapabilityHomingProjectiles = 0x0000000002000000UL;
        public const ulong CapabilityLANDiscovery = 0x0000000004000000UL;
        public const ulong CapabilityGameplayCatalog = 0x0000000008000000UL;
        public const ulong CapabilityProjectileSpawnBatch = 0x0000000010000000UL;
        public const ulong CapabilityDebugRecords = 0x0000000020000000UL;
        public const ulong CapabilityColliderShapeQuery = 0x0000000040000000UL;
        public const ulong CapabilityBenchmarkStats = 0x0000000080000000UL;
        public const ulong CapabilityNetworkStats = 0x0000000100000000UL;
        public const ulong CapabilityEntityLifecycleEvents = 0x0000000200000000UL;
        public const ulong CapabilityVisionStateQuery = 0x0000000400000000UL;
        public const ulong CapabilityGameplayCatalogSync = 0x0000000800000000UL;
        public const ulong CapabilityControlPlaneRpc = 0x0000001000000000UL;
        public const ulong CapabilityActionTimeline = 0x0000002000000000UL;
        public const ulong CapabilityLocalActionResults = 0x0000004000000000UL;
        public const ulong CapabilityRemoteActionPresentation = 0x0000008000000000UL;
        public const ulong CapabilityActionIntents = 0x0000010000000000UL;
        public const ulong CapabilityItemPropSystem = 0x0000020000000000UL;
        public const ulong CapabilitySkeletonRenderStates = 0x0000040000000000UL;
        public const ulong CapabilitySkeletonBindPose = 0x0000080000000000UL;

        public const uint SkeletonRenderStatusSuccess = 0;
        public const uint SkeletonRenderStatusInsufficientCapacity = 1;
        public const uint SkeletonRenderStatusInvalidArgument = 2;
        public const uint SkeletonPoseFlagBindPose = 0x00000001U;
        public const uint SkeletonPoseFlagProcedural = 0x00000002U;
        public const uint SkeletonRenderResultFlagAtTime = 0x00000001U;
        public const uint EntityComponentSkeleton = 0x00000200U;

        public const uint CollisionLayerPlayerSide = 0x00000001U;
        public const uint CollisionLayerHostileSide = 0x00000002U;
        public const uint CollisionLayerPlayer = CollisionLayerPlayerSide;
        public const uint CollisionLayerHostile = CollisionLayerHostileSide;
        public const uint CollisionLayerEnemy = CollisionLayerHostileSide;
        public const uint CollisionLayerProjectile = 0x00000004U;
        public const uint CollisionLayerAgentVision = 0x00000010U;
        public const uint CollisionLayerNeutral = 0x00000020U;
        public const uint CollisionLayerTerrain = 0x00000040U;
        public const uint CollisionLayerStaticObstacle = 0x00000080U;
        public const uint CollisionMaskNone = 0x00000000U;
        public const uint CollisionMaskDamageable =
            CollisionLayerPlayer | CollisionLayerHostile | CollisionLayerNeutral;
        public const uint CollisionMaskActor = CollisionMaskDamageable;
        public const uint CollisionMaskStaticWorld =
            CollisionLayerTerrain | CollisionLayerStaticObstacle;

        public const uint MovementLayerTerrain = 0x00000001U;
        public const uint MovementLayerStaticObstacle = 0x00000002U;
        public const uint MovementLayerActor = 0x00000008U;
        public const uint MovementMaskDefault =
            MovementLayerTerrain | MovementLayerStaticObstacle | MovementLayerActor;

        public const uint VisualFlagMoving = 0x00000001U;
        public const uint VisualFlagReloading = 0x00000002U;
        public const uint VisualFlagDead = 0x00000004U;
        public const uint VisualFlagHpUnknown = 0x00000008U;
        public const uint VisualFlagGrounded = 0x00000010U;
        public const uint VisualFlagFalling = 0x00000020U;
        public const uint VisualFlagLanded = 0x00000040U;
        public const uint VisualFlagAiming = 0x00000100U;
        public const uint VisualFlagFiring = 0x00000200U;
        public const uint MaxVisibleHostiles = 16;
        public const uint MaxVisibleAllies = 16;
        public const uint MaxVisibleNeutrals = 16;
    }

    public static class NetworkKernelPackageInfo
    {
        public const string Name = "com.network-example.kernel";
        public const string Version = "0.6.9";
    }

    public enum KernelMode
    {
        Client = 0,
        ListenServer = 1,
        DedicatedServer = 2,
    }

    public enum KernelActorBlockingMode : uint
    {
        Disabled = 0,
        Predicted = 1,
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
        ActorLanded = 12,
        HealthChanged = 13,
    }

    public enum KernelDespawnReason : uint
    {
        Destroyed = 0,
        OutOfRange = 1,
        Disconnected = 2,
        Expired = 3,
        CapacityEvicted = 4,
    }

    public enum KernelGameplayCatalogSyncState
    {
        Idle = 0,
        Connecting = 1,
        FetchingManifest = 2,
        ManifestReady = 3,
        Downloading = 4,
        BundleReady = 5,
        Handshaking = 6,
        Ready = 7,
        Failed = 8,
        Disconnected = 9,
    }

    public enum KernelGameplayCatalogSyncError
    {
        None = 0,
        Unsupported = 1,
        BundleUnavailable = 2,
        VersionMismatch = 3,
        InvalidManifest = 4,
        BundleTooLarge = 5,
        InvalidBundle = 6,
        Timeout = 7,
        Disconnected = 8,
        InvalidState = 9,
        Transport = 10,
    }

    public enum RenderEntityStatus : uint
    {
        Active = 0,
        Predicted = 1,
        Stale = 2,
    }

    public enum KernelEntityLifecycleEventType
    {
        OutOfRange = 0,
        Despawned = 1,
        Destroyed = 2,
    }

    public enum KernelEntityType : ushort
    {
        Unknown = 0,
        Actor = 1,
        Player = Actor,
        Prop = 2,
        Projectile = 3,
        Director = 5,
    }

    public enum KernelActorType : ushort
    {
        Unknown = 0,
        Player = 1,
        Agent = 2,
    }

    public enum KernelItemMode
    {
        Fungible = 0,
        Stateful = 1,
    }

    [Flags]
    public enum KernelItemCapabilityFlag : uint
    {
        Pickupable = 1U << 0,
        Deployable = 1U << 1,
        Carryable = 1U << 2,
        Consumable = 1U << 3,
        Throwable = 1U << 4,
        Interactable = 1U << 5,
    }

    public enum KernelDomainAction
    {
        None = 0,
        Consume = 1,
        Pickup = 2,
        Throw = 3,
        Place = 4,
        Carry = 5,
        Activate = 6,
    }

    public enum KernelItemResidencyKind
    {
        None = 0,
        Inventory = 1,
        World = 2,
        Terminal = 3,
    }

    public enum KernelWorldItemMode
    {
        Placed = 0,
        Carrying = 1,
        InFlight = 2,
    }

    public enum KernelItemThrowMode
    {
        None = 0,
        IdentityPreserving = 1,
        ConsumeAndSpawn = 2,
    }

    public enum KernelPortableStateType
    {
        Uint32 = 0,
        Float = 1,
        Bool = 2,
    }

    public enum KernelPortableStateProjection
    {
        None = 0,
        HealthCurrent = 1,
    }

    public enum KernelGameplayRequestStatus
    {
        NoAction = 0,
        Rejected = 1,
        Committed = 2,
    }

    public enum KernelGameplayGraphOutcome
    {
        NotSubmitted = 0,
        Succeeded = 1,
        FailedAfterCommit = 2,
    }

    public enum KernelGameplayRequestRejectionReason
    {
        None = 0,
        InvalidRequest = 1,
        UnknownInstigator = 2,
        UnknownTarget = 3,
        UnknownItem = 4,
        StaleReference = 5,
        InvalidContext = 6,
        MissingCapability = 7,
        NotAuthorized = 8,
        OutOfRange = 9,
        LineOfSight = 10,
        InventoryFull = 11,
        InvalidQuantity = 12,
        InvalidPlacement = 13,
        Claimed = 14,
        Cooldown = 15,
        GraphRejected = 16,
    }

    public enum KernelEntityTriggerActionType
    {
        None = 0,
        ApplyDamage = 1,
        SpawnEntity = 2,
        SpawnProjectile = 3,
        ApplyHealthChange = 4,
    }

    public enum KernelEntityRefSource
    {
        Self = 0,
        EventSubject = 1,
        EventTarget = 2,
        EventInstigator = 3,
    }

    public enum KernelEventVec3Source
    {
        Position = 0,
        Direction = 1,
    }

    public enum KernelActionConditionType
    {
        Always = 0,
        EventHasTarget = 1,
    }

    [Flags]
    public enum InputButton : uint
    {
        MoveJump = 1U << 0,
        Sprint = 1U << 3,
        Dodge = 1U << 6,
        Parry = 1U << 7,
        Aim = 1U << 8,
    }

    public enum KernelActionBinding : ushort
    {
        PrimaryFire = 0,
        Reload = 1,
    }

    public enum KernelActionTriggerMode : byte
    {
        Press = 0,
        Hold = 1,
    }

    public enum KernelActionPhase : byte
    {
        None = 0,
        Windup = 1,
        Active = 2,
        Recovery = 3,
    }

    public enum KernelLocalActionResultType : byte
    {
        Accepted = 0,
        Corrected = 1,
        Rejected = 2,
    }

    public enum KernelLocalActionResultReason : byte
    {
        None = 0,
        InvalidActionId = 1,
        MissingActor = 2,
        MissingTemplate = 3,
        Busy = 4,
        Reloading = 5,
        NoAmmo = 6,
        Cancelled = 7,
        TimedOut = 8,
        Dead = 9,
        WeaponChanged = 10,
        EffectFailed = 11,
        Cooldown = 12,
    }

    public enum KernelRemoteActionPresentationEventType : byte
    {
        FireCommit = 0,
        CastingCommit = 1,
        ReloadCommit = 2,
        HitReaction = 3,
        DeathTrigger = 4,
    }

    [Flags]
    public enum KernelActionTemplateFlag : byte
    {
        CancelOnRelease = 1 << 0,
        CancelOnDeath = 1 << 1,
        CancelOnWeaponChange = 1 << 2,
        CancelBeforeFirstCommit = 1 << 3,
    }

    public enum KernelNetworkStatsMode : byte
    {
        Default = 0,
        Off = 1,
        Basic = 2,
        Detailed = 3,
    }

    public enum KernelWeaponFireMode : byte
    {
        Hitscan = 0,
        Shotgun = 1,
        Projectile = 2,
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
        None = 1,
        PiercingSegment = 2,
    }

    public enum KernelProjectileType : byte
    {
        Standard = 0,
        AreaEffect = 1,
        Beam = 2,
    }

    public enum KernelProjectileCollisionQueryMode : byte
    {
        Auto = 0,
        Overlap = 1,
        Sweep = 2,
        Ray = 3,
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
        Cone = 4,
    }

    [Flags]
    public enum KernelColliderPurpose : uint
    {
        Hit = 1U << 0,
        Damage = 1U << 1,
        Trigger = 1U << 2,
        Vision = 1U << 3,
    }

    public enum KernelAgentCamp : byte
    {
        Unknown = 0,
        PlayerSide = 1,
        EnemySide = 2,
        Neutral = 3,
    }

    public enum KernelAgentRelation : byte
    {
        Self = 0,
        Ally = 1,
        Hostile = 2,
        Neutral = 3,
        Unknown = 4,
    }

    public enum KernelAiControllerType : uint
    {
        None = 0,
        Sentry = 1,
        Director = 2,
    }

    public enum KernelMovementControllerType : byte
    {
        None = 0,
        Grounded = 1,
        Kinematic = 2,
        Character = 3,
    }

    public enum KernelFootholdQueryType : byte
    {
        None = 0,
        Raycast = 1,
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
        public uint area_effect_mechanics_definition_size;
        public uint beam_mechanics_definition_size;
        public uint combat_state_definition_size;
        public uint homing_mechanics_definition_size;
        public uint homing_state_size;
        public uint lan_discovery_server_config_size;
        public uint lan_discovery_query_config_size;
        public uint lan_discovery_result_size;
        public ulong capability_flags;
        public uint gameplay_catalog_definition_size;
        public uint prop_population_rule_definition_size;
        public uint gameplay_catalog_load_result_size;
        public uint gameplay_catalog_load_options_size;
        public uint actor_template_definition_size;
        public uint projectile_template_definition_size;
        public uint collider_template_definition_size;
        public uint collider_binding_definition_size;
        public uint benchmark_stats_size;
        public uint network_stats_config_size;
        public uint network_stats_size;
        public uint debug_record_filter_size;
        public uint debug_info_size;
        public uint collider_shape_query_size;
        public uint collider_shape_view_size;
        public uint agent_vision_config_size;
        public uint vision_state_query_size;
        public uint vision_state_view_size;
        public uint gameplay_catalog_manifest_size;
        public uint gameplay_catalog_sync_status_size;
        public uint entity_template_definition_size;
        public uint entity_ai_definition_size;
        public uint action_template_definition_size;
        public uint action_runtime_view_size;
        public uint local_action_result_size;
        public uint remote_action_presentation_event_size;
        public uint action_intent_size;
        public uint action_input_size;
        public uint item_template_definition_size;
        public uint gameplay_request_size;
        public uint gameplay_request_outcome_size;
        public uint item_instance_view_size;
        public uint inventory_container_view_size;
        public uint inventory_delta_size;
        public uint bone_local_transform_size;
        public uint skeleton_render_state_size;
        public uint skeleton_render_state_result_size;
        public uint skeleton_asset_definition_size;
        public uint skeleton_binding_definition_size;
        public uint skeleton_leg_definition_size;
        public uint status_effect_view_size;
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
    public struct KernelVec4
    {
        public float x;
        public float y;
        public float z;
        public float w;

        public KernelVec4(float x, float y, float z, float w)
        {
            this.x = x;
            this.y = y;
            this.z = z;
            this.w = w;
        }

        public static uint StructSize => (uint)Marshal.SizeOf<KernelVec4>();
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
    public struct KernelNetworkStatsConfig
    {
        public KernelNetworkStatsMode mode;
        public byte reserved0;
        public ushort reserved1;
        public uint action_packet_budget_bytes;
        public uint remote_presentation_expiry_ms;
        public uint remote_presentation_client_budget_bytes_per_second;
        public uint remote_presentation_server_budget_bytes_per_second;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelNetworkStatsConfig>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelConfig
    {
        public KernelMode mode;
        public TickConfig tick;
        public uint max_render_states;
        public uint max_events;
        public KernelNetworkStatsConfig network_stats;

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
    public struct KernelPhysicsConfig
    {
        public uint struct_size;
        public uint physics_simulation;
        public uint physics_workers;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelPhysicsConfig>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelSessionRulesConfig
    {
        public uint struct_size;
        public KernelActorBlockingMode actor_blocking_mode;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelSessionRulesConfig>();
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct KernelStaticCollisionSceneConfig
    {
        public uint struct_size;
        public IntPtr artifact_bytes;
        public uint artifact_size;
        public uint scene_id;
        public uint collider_id;
        public uint collision_layer;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelStaticCollisionSceneConfig>();
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct KernelGameplayCatalogLoadOptions
    {
        public uint struct_size;
        public IntPtr static_collision_scene;
        public IntPtr out_static_scene_rejected;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelGameplayCatalogLoadOptions>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelActionDefinition
    {
        public byte action_type;
        public byte target_source;
        public ushort damage_amount;
        public uint spawn_entity_template_id;
        public uint spawn_projectile_template_id;
        public byte position_source;
        public byte direction_source;
        public byte owner_source;
        public byte reserved;
        public uint spawn_item_template_id;
        public uint spawn_item_quantity;
        public int health_change_amount;
        public uint condition_type;
        public float impulse_strength;
        public uint impulse_collision_mask;
        public KernelVec3 impulse_direction;
        public uint status_effect_id;
        public byte modifier_operation;
        public byte reserved1;
        public ushort reserved2;
        public float modifier_value;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelActionDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelActionTriggerDefinition
    {
        public uint struct_size;
        public byte action_type;
        public byte target_source;
        public ushort damage_amount;
        public uint spawn_entity_template_id;
        public uint spawn_projectile_template_id;
        public byte position_source;
        public byte direction_source;
        public byte owner_source;
        public byte reserved;
        public uint spawn_item_template_id;
        public uint spawn_item_quantity;
        public uint action_count;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxActionGraphActions)]
        public KernelActionDefinition[] actions;
        public int health_change_amount;
        public uint condition_type;
        public float impulse_strength;
        public uint impulse_collision_mask;
        public KernelVec3 impulse_direction;
        public uint status_effect_id;
        public byte modifier_operation;
        public byte reserved1;
        public ushort reserved2;
        public float modifier_value;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelActionTriggerDefinition>();

        public static KernelActionTriggerDefinition Create()
        {
            return new KernelActionTriggerDefinition
            {
                struct_size = StructSize,
                actions = new KernelActionDefinition[KernelConstants.MaxActionGraphActions],
            };
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelPortableStateFieldDefinition
    {
        public uint field_id;
        public byte type;
        public byte world_projection;
        public ushort reserved0;
        public uint uint32_default;
        public float float_default;
        public uint bool_default;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelItemThrowDefinition
    {
        public uint struct_size;
        public byte mode;
        public byte reserved0;
        public ushort reserved1;
        public uint trajectory_projectile_template_id;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelItemThrowDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelItemUseDefinition
    {
        public uint struct_size;
        public uint quantity_cost;
        public uint charge_field_id;
        public uint cooldown_ticks;
        public uint destroy_when_empty;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelItemUseDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelItemTemplateDefinition
    {
        public uint struct_size;
        public uint item_template_id;
        public byte item_mode;
        public byte reserved0;
        public ushort max_stack;
        public uint capability_flags;
        public uint entity_template_id;
        public float interaction_range;
        public uint line_of_sight_required;
        public uint line_of_sight_blocking_mask;
        public KernelItemThrowDefinition throw_policy;
        public KernelItemUseDefinition use_policy;
        public uint portable_state_field_count;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxPortableStateFields)]
        public KernelPortableStateFieldDefinition[] portable_state_fields;
        public KernelActionTriggerDefinition item_used_trigger;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelItemTemplateDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelPropInteractionDefinition
    {
        public uint struct_size;
        public uint capability_flags;
        public byte line_of_sight_required;
        public byte reserved0;
        public ushort reserved1;
        public float interaction_range;
        public uint line_of_sight_blocking_mask;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelPropInteractionDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelPropPopulationRuleDefinition
    {
        public uint struct_size;
        public uint population_group_id;
        public uint max_alive;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelPropPopulationRuleDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelPropDefinition
    {
        public uint struct_size;
        public KernelPropInteractionDefinition interaction;
        public float carry_offset_x;
        public float carry_offset_y;
        public float carry_offset_z;
        public uint throw_trajectory_projectile_template_id;
        public uint lifetime_ticks;
        public uint population_group_id;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelPropDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelActionIntent
    {
        public uint action_instance_id;
        public KernelActionBinding binding_id;
        public byte flags;
        public byte reserved;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelActionIntent>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelActionInput
    {
        public uint action_instance_id;
        public byte held;
        public byte flags;
        public ushort reserved;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelActionInput>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelPlayerInput
    {
        public uint input_seq;
        public ulong client_action_time_us;
        public KernelVec2 move;
        public KernelVec2 look_delta;
        public KernelVec3 aim_dir;
        public uint buttons;
        public byte selected_weapon;
        public KernelActionIntent action_intent;
        public KernelActionInput action_input;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelPlayerInput>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelLocalActionResult
    {
        public uint action_instance_id;
        public ushort confirmed_commit_count;
        public KernelLocalActionResultType result;
        public KernelLocalActionResultReason reason;
        public uint authoritative_tick;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelLocalActionResult>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelRemoteActionPresentationEvent
    {
        public uint actor_net_id;
        public uint action_template_id;
        public uint action_instance_id;
        public ushort first_commit_index;
        public ushort commit_count;
        public KernelRemoteActionPresentationEventType event_type;
        public byte flags;
        public ushort server_tick_delta;
        public uint status_effect_id;
        public uint status_instance_id;
        public uint status_channel_id;
        public uint duration_ticks;
        public ushort stack_count;
        public ushort reserved0;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelRemoteActionPresentationEvent>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelStatusEffectView
    {
        public uint struct_size;
        public uint status_effect_id;
        public uint status_instance_id;
        public uint status_channel_id;
        public uint instigator_net_id;
        public uint applied_tick;
        public uint expire_tick;
        public ushort stack_count;
        public ushort max_stacks;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelStatusEffectView>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelActionRuntimeView
    {
        public uint struct_size;
        public uint action_template_id;
        public uint action_instance_id;
        public KernelActionPhase phase;
        public byte reserved0;
        public ushort reserved1;
        public uint start_tick;
        public uint commit_count;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelActionRuntimeView>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RenderEntityState
    {
        public ulong entity_id;
        public uint net_id;
        public KernelEntityType entity_type;
        public KernelActorType actor_type;
        public uint owner_peer;
        public KernelVec3 position;
        public KernelQuat rotation;
        public KernelVec3 velocity;
        public ushort hp;
        public ushort max_hp;
        public ushort animation_state;
        public uint visual_flags;
        public uint spawn_tick;
        public uint action_instance_id;
        public RenderEntityStatus status;
        public uint template_id;
        public uint collider_template_id;
        public KernelActionRuntimeView action;
        public KernelVec3 aim_direction;
        public ulong item_instance_id;
        public byte world_item_mode;
        public byte reserved_item0;
        public ushort reserved_item1;
        public uint carrier_entity_id;

        /// <summary>
        /// Beams only: the far end of the beam in world space, already cut short
        /// at whatever stopped it. <see cref="position"/> is the near end, so the
        /// pair gives both the direction to orient a beam by and the length to
        /// scale it to. Zero for every other entity -- a beam is the only
        /// projectile with an extent rather than a point, and the only one whose
        /// rotation cannot be recovered from its velocity, because it has none.
        /// </summary>
        public KernelVec3 beam_end;

        public static uint StructSize => (uint)Marshal.SizeOf<RenderEntityState>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelBoneLocalTransform
    {
        public KernelVec3 local_position;
        public KernelQuat local_rotation;
        public KernelVec3 local_scale;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelBoneLocalTransform>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelSkeletonRenderState
    {
        public uint entity_net_id;
        public uint skeleton_asset_id;
        public ulong skeleton_content_hash;
        public uint first_bone_transform;
        public uint bone_count;
        public uint pose_tick;
        public uint pose_flags;
        public ulong pose_time_us;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelSkeletonRenderState>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelSkeletonRenderStateResult
    {
        public uint struct_size;
        public uint status;
        public uint required_state_count;
        public uint required_bone_transform_count;
        public uint written_state_count;
        public uint written_bone_transform_count;
        public uint source_tick;
        public uint flags;
        public ulong requested_render_time_us;
        public ulong evaluated_render_time_us;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelSkeletonRenderStateResult>();

        public static KernelSkeletonRenderStateResult Create()
        {
            return new KernelSkeletonRenderStateResult
            {
                struct_size = StructSize,
            };
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelServerEntityCreateInfo
    {
        public uint struct_size;
        public KernelEntityType entity_type;
        public KernelActorType actor_type;
        public uint owner_peer;
        public KernelVec3 position;
        public KernelQuat rotation;
        public ushort animation_state;
        public uint visual_flags;
        public uint actor_template_id;
        public uint entity_template_id;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelServerEntityCreateInfo>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelServerEntityActivateInfo
    {
        public uint struct_size;
        public uint subject_net_id;
        public uint instigator_net_id;
        public uint target_net_id;
        public uint action_instance_id;
        public ulong request_id;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelServerEntityActivateInfo>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelGameplayRequest
    {
        public uint struct_size;
        public uint requester_peer;
        public ulong request_id;
        public uint instigator_net_id;
        public byte domain_action;
        public byte reserved0;
        public ushort reserved1;
        public ulong selected_item_instance_id;
        public uint target_net_id;
        public uint requested_quantity;
        public KernelVec3 placement_position;
        public KernelVec3 throw_direction;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelGameplayRequest>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelGameplayRequestOutcome
    {
        public uint struct_size;
        public uint requester_peer;
        public ulong request_id;
        public byte status;
        public byte graph_outcome;
        public byte domain_action;
        public byte rejection_reason;
        public ulong item_instance_id;
        public uint prop_entity_id;
        public uint committed_quantity;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelGameplayRequestOutcome>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelItemInstanceView
    {
        public uint struct_size;
        public ulong item_instance_id;
        public uint item_template_id;
        public uint quantity;
        public byte residency;
        public byte world_mode;
        public ushort slot;
        public ulong inventory_container_id;
        public uint prop_entity_id;
        public uint carrier_entity_id;
        public uint terminal;
        public uint next_use_tick;
        public uint portable_state_field_count;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxPortableStateFields)]
        public KernelPortableStateFieldDefinition[] portable_state_fields;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelItemInstanceView>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelInventoryContainerView
    {
        public uint struct_size;
        public ulong inventory_container_id;
        public uint owner_entity_id;
        public uint slot_capacity;
        public uint occupied_slot_count;
        public ulong revision;
        public byte sync_state;
        public byte reserved0;
        public ushort reserved1;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelInventoryContainerView>();
    }

    public enum KernelInventorySyncState
    {
        NotAvailable = 0,
        Syncing = 1,
        Ready = 2,
        Desynced = 3,
    }

    public enum KernelInventoryDeltaType
    {
        Add = 0,
        Update = 1,
        Remove = 2,
        Move = 3,
    }

    [Flags]
    public enum KernelInventoryChangeFlag : uint
    {
        Quantity = 1U << 0,
        Cooldown = 1U << 1,
        PortableState = 1U << 2,
        All = Quantity | Cooldown | PortableState,
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelInventoryDelta
    {
        public uint struct_size;
        public ulong inventory_container_id;
        public ulong revision;
        public byte type;
        public byte reserved0;
        public ushort slot;
        public ushort previous_slot;
        public ushort changed_fields;
        public KernelItemInstanceView item;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelInventoryDelta>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelServerEntityState
    {
        public uint struct_size;
        public uint net_id;
        public KernelEntityType entity_type;
        public KernelActorType actor_type;
        public uint owner_peer;
        public KernelVec3 position;
        public KernelQuat rotation;
        public KernelVec3 velocity;
        public ushort hp;
        public ushort max_hp;
        public ushort animation_state;
        public uint visual_flags;
        public uint valid;
        public uint actor_template_id;
        public byte active_weapon_slot;
        public byte weapon_slot_count;
        public ushort reserved0;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxWeaponSlots)]
        public uint[] weapon_ids;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxWeaponSlots)]
        public ushort[] ammo;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxWeaponSlots)]
        public ushort[] reserve_magazines;
        public uint is_reloading;
        public uint reload_remaining_ticks;
        public KernelActionRuntimeView action;
        public KernelVec3 aim_direction;
        public uint item_template_id;
        public ulong item_instance_id;
        public byte world_item_mode;
        public byte reserved_item0;
        public ushort reserved_item1;
        public uint carrier_entity_id;

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
        public KernelVec4 shape_params;
        public uint purpose_flags;
        public uint layer_mask;
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
        public byte reserved0;
        public ushort reserved1;
        public KernelProjectileMechanicsDefinition mechanics;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelProjectileTemplateDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelActorTemplateDefinition
    {
        public uint struct_size;
        public uint actor_template_id;
        public KernelEntityType entity_type;
        public KernelActorType actor_type;
        public uint collider_template_id;
        public KernelAgentVisionConfig vision;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelActorTemplateDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelActionTemplateDefinition
    {
        public uint struct_size;
        public uint action_template_id;
        public KernelActionTriggerMode trigger_mode;
        public KernelActionTemplateFlag flags;
        public ushort ammo_cost_per_commit;
        public uint commit_offset_ticks;
        public uint commit_interval_ticks;
        public uint max_commit_count;
        public uint recovery_ticks;
        public uint hold_input_timeout_ticks;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelActionTemplateDefinition>();
    }

    public struct KernelGameplayCatalog
    {
        public uint CatalogVersion;
        public ulong CatalogHash;
        public KernelActorTemplateDefinition[] ActorTemplates;
        public KernelProjectileTemplateDefinition[] ProjectileTemplates;
        public KernelColliderTemplateDefinition[] ColliderTemplates;
        public KernelColliderBindingDefinition[] ColliderBindings;
        public KernelEntityTemplateDefinition[] EntityTemplates;
        public KernelActionTemplateDefinition[] ActionTemplates;
        public KernelItemTemplateDefinition[] ItemTemplates;
        public KernelPropPopulationRuleDefinition[] PropPopulationRules;
        public KernelSkeletonAssetDefinition[] SkeletonAssets;
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

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct KernelGameplayCatalogManifest
    {
        public uint struct_size;
        public uint catalog_version;
        public ulong catalog_hash;
        public uint bundle_size;
        [MarshalAs(
            UnmanagedType.ByValArray,
            SizeConst = KernelConstants.GameplayCatalogSha256Size,
            ArraySubType = UnmanagedType.U1)]
        public byte[] bundle_sha256;
        [MarshalAs(
            UnmanagedType.ByValTStr,
            SizeConst = KernelConstants.GameplayCatalogEntryPathSize)]
        public string entry_path;
        [MarshalAs(
            UnmanagedType.ByValTStr,
            SizeConst = KernelConstants.GameplayCatalogContentNamespaceSize)]
        public string content_namespace;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelGameplayCatalogManifest>();

        public static KernelGameplayCatalogManifest Create()
        {
            return new KernelGameplayCatalogManifest
            {
                struct_size = StructSize,
                bundle_sha256 = new byte[KernelConstants.GameplayCatalogSha256Size],
                entry_path = string.Empty,
                content_namespace = string.Empty,
            };
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelGameplayCatalogSyncClientConfig
    {
        public uint struct_size;
        public uint max_bundle_size;
        public uint timeout_ms;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelGameplayCatalogSyncClientConfig>();
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct KernelGameplayCatalogSyncServerConfig
    {
        public uint struct_size;
        public IntPtr bundle_bytes;
        public uint bundle_size;
        public IntPtr entry_path;
        public IntPtr content_namespace;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelGameplayCatalogSyncServerConfig>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelGameplayCatalogSyncStatus
    {
        public uint struct_size;
        public KernelGameplayCatalogSyncState state;
        public KernelGameplayCatalogSyncError error;
        public uint received_bundle_size;
        public KernelGameplayCatalogManifest manifest;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelGameplayCatalogSyncStatus>();

        public static KernelGameplayCatalogSyncStatus Create()
        {
            return new KernelGameplayCatalogSyncStatus
            {
                struct_size = StructSize,
                manifest = KernelGameplayCatalogManifest.Create(),
            };
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct KernelGameplayCatalogDefinition
    {
        public uint struct_size;
        public uint catalog_version;
        public ulong catalog_hash;
        public IntPtr actor_templates;
        public uint actor_template_count;
        public IntPtr projectile_templates;
        public uint projectile_template_count;
        public IntPtr collider_templates;
        public uint collider_template_count;
        public IntPtr collider_bindings;
        public uint collider_binding_count;
        public IntPtr entity_templates;
        public uint entity_template_count;
        public IntPtr action_templates;
        public uint action_template_count;
        public IntPtr item_templates;
        public uint item_template_count;
        public IntPtr prop_population_rules;
        public uint prop_population_rule_count;
        public IntPtr skeleton_assets;
        public uint skeleton_asset_count;
        public IntPtr status_effects;
        public uint status_effect_count;
        public IntPtr game_rules;
        public uint game_rule_count;
        public IntPtr game_rule_nodes;
        public uint game_rule_node_count;
        public IntPtr game_rule_edges;
        public uint game_rule_edge_count;
        public IntPtr game_rule_effects;
        public uint game_rule_effect_count;

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
        public ulong grounded_query_count;
        public ulong grounded_query_cost_us;
        public ulong kinematic_move_count;
        public ulong kinematic_move_cost_us;
        public ulong character_move_count;
        public ulong character_move_cost_us;

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
        public uint replication_metadata_timeout_count;
        public uint replication_stale_snapshot_drop_count;
        public uint collection_mode;
        public uint reserved0;
        public ulong input_bytes_sent;
        public ulong presentation_bytes_sent;
        public ulong session_bytes_sent;
        public ulong local_action_result_bytes_sent;
        public ulong remote_action_presentation_bytes_sent;
        public ulong local_action_results_generated;
        public ulong local_action_results_sent;
        public ulong local_action_results_accepted;
        public ulong local_action_results_corrected;
        public ulong local_action_results_rejected;
        public ulong local_action_result_server_duplicates_suppressed;
        public ulong local_action_result_client_duplicates_dropped;
        public ulong local_action_results_timed_out;
        public ulong local_action_result_latency_sample_count;
        public ulong local_action_result_latency_us_total;
        public ulong local_action_result_latency_us_max;
        public ulong local_action_result_batch_count;
        public ulong local_action_result_batch_record_count;
        public uint average_local_action_result_batch_size;
        public uint max_local_action_result_batch_size;
        public ulong remote_presentation_records_generated;
        public ulong remote_presentation_records_sent;
        public ulong remote_presentation_batch_count;
        public ulong remote_presentation_batch_record_count;
        public ulong remote_presentation_relevance_filtered;
        public ulong remote_presentation_budget_dropped;
        public ulong remote_presentation_stale_dropped;
        public ulong remote_presentation_duplicate_dropped;
        public uint average_remote_presentation_batch_size;
        public uint max_remote_presentation_batch_size;
        public ulong zero_action_instance_attempts;
        public ulong action_instance_collisions;
        public ulong inventory_delta_bytes_sent;
        public ulong inventory_snapshot_bytes_sent;
        public ulong prop_state_bytes_sent;
        public ulong inventory_resync_request_count;
        public ulong inventory_revision_gap_count;

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

    /// <summary>
    /// Filters active collider shape queries. Passing a null query to
    /// Kernel.QueryColliderShapes applies no filters; zero-valued entity type,
    /// entity net id, and purpose mask fields also mean no filter for that field.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct KernelColliderShapeQuery
    {
        public uint struct_size;
        public ushort entity_type_filter;
        public KernelActorType actor_type_filter;
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
        public KernelActorType actor_type;
        public uint collider_template_id;
        public byte shape_type;
        public byte reserved1;
        public ushort reserved2;
        public KernelVec3 world_center;
        public KernelVec4 shape_params;
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
    public struct KernelAgentVisionConfig
    {
        public uint struct_size;
        public byte camp;
        public byte reserved0;
        public ushort reserved1;
        public uint vision_collider_template_id;
        public uint max_visible_hostiles;
        public uint max_visible_allies;
        public uint max_visible_neutrals;
        public KernelVec3 local_origin;
        public KernelVec3 local_forward;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelAgentVisionConfig>();
    }

    /// <summary>
    /// Filters local runtime vision state queries. Passing a null query to
    /// Kernel.QueryVisionState applies no filters; zero-valued entity type and
    /// agent net id fields also mean no filter for that field.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct KernelVisionStateQuery
    {
        public uint struct_size;
        public ushort entity_type_filter;
        public KernelActorType actor_type_filter;
        public uint agent_net_id;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelVisionStateQuery>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelVisionStateView
    {
        public uint struct_size;
        public uint agent_net_id;
        public ushort entity_type;
        public byte camp;
        public byte actor_type;
        public KernelVec3 vision_origin;
        public KernelVec3 vision_forward;
        public uint vision_collider_template_id;
        public uint resolved_collider_template_id;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)]
        public uint[] visible_hostiles;
        public uint visible_hostile_count;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)]
        public uint[] visible_allies;
        public uint visible_ally_count;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)]
        public uint[] visible_neutrals;
        public uint visible_neutral_count;
        public uint current_target_candidate;
        public byte relation_to_current_target;
        public byte reserved1;
        public ushort reserved2;
        public uint last_seen_target;
        public KernelVec3 last_known_target_position;
        public float time_since_last_seen_target;
        public uint valid;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelVisionStateView>();
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
        public float max_turn_degrees_per_tick;
        public float acceleration;
        public float max_speed;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelHomingMechanicsDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelProjectileMechanicsDefinition
    {
        public uint struct_size;
        public byte projectile_type;
        public byte motion_model;
        public byte hit_response;
        public byte damage_shape;
        public byte sync_mode;
        public byte damage_falloff;
        public ushort damage;
        public float speed;
        public uint lifetime_ticks;
        public KernelVec3 gravity;
        public uint collider_template_id;
        public uint collision_mask;
        public uint max_hit_count;
        public KernelHomingMechanicsDefinition homing;
        public KernelAreaEffectMechanicsDefinition area_effect;
        public KernelBeamMechanicsDefinition beam;
        public KernelActionTriggerDefinition projectile_impact_trigger;
        public KernelActionTriggerDefinition expired_trigger;
        public byte collision_query_mode;
        public byte reserved0;
        public ushort reserved1;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelProjectileMechanicsDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelAreaEffectMechanicsDefinition
    {
        public uint struct_size;
        public float radius;
        public ushort damage_per_interval;
        public ushort reserved0;
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
        public ushort damage_per_tick;
        public ushort reserved0;
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
        public ushort reserve_magazines;
        public ushort damage;
        public float max_range;
        public byte pellet_count;
        public float pellet_spread;
        public uint projectile_template_id;
        public uint segment_collider_template_id;
        public uint fire_action_template_id;
        public uint reload_action_template_id;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelWeaponMechanicsDefinition>();
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
        public float max_turn_degrees_per_tick;
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
        public byte active_weapon_slot;
        public byte weapon_slot_count;
        public ushort reserved0;
        public uint collider_template_id;
        public float move_speed_meters_per_second;
        public KernelVec3 hitbox_center;
        public KernelVec3 hitbox_half_extents;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxWeaponSlots)]
        public uint[] weapon_ids;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxWeaponSlots)]
        public ushort[] ammo;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxWeaponSlots)]
        public ushort[] reserve_magazines;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelCombatStateDefinition>();

        public static KernelCombatStateDefinition Create()
        {
            return new KernelCombatStateDefinition
            {
                struct_size = StructSize,
                weapon_ids = new uint[KernelConstants.MaxWeaponSlots],
                ammo = new ushort[KernelConstants.MaxWeaponSlots],
                reserve_magazines = new ushort[KernelConstants.MaxWeaponSlots],
            };
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelMovementDefinition
    {
        public uint struct_size;
        public KernelMovementControllerType controller_type;
        public byte reserved0;
        public ushort reserved1;
        public uint movement_collider_template_id;
        public KernelVec3 gravity;
        public float max_slope_degrees;
        public float step_height;
        public float ground_probe_distance;
        public float ground_snap_distance;
        public float max_yaw_degrees_per_second;
        public uint movement_collision_mask;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelMovementDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelEntityAiDefinition
    {
        public uint struct_size;
        public KernelAiControllerType controller_type;
        public uint ai_profile_id;
        public uint tick_interval;
        public uint blackboard_id;
        public uint spawn_target_count;
        public uint spawn_entity_template_id;
        public uint spawn_actor_template_id;
        public KernelVec3 spawn_position;
        public float spawn_radius;
        public uint spawn_seed;
        public uint director_kind;
        public uint game_rule_definition_id;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelEntityAiDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelSkeletonAssetDefinition
    {
        public uint struct_size;
        public uint skeleton_asset_id;
        public ulong skeleton_content_hash;
        public IntPtr runtime_skeleton_data;
        public uint runtime_skeleton_size;
        public uint bone_count;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelSkeletonAssetDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelSkeletonLegDefinition
    {
        public uint leg_id;
        public uint hip_bone_index;
        public uint knee_bone_index;
        public uint foot_bone_index;
        public uint gait_group;
        public KernelVec3 pole_local;
        public KernelVec3 mid_axis_local;
        public float step_height_meters;
        public float max_reach_ratio;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelSkeletonLegDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelSkeletonColliderDefinition
    {
        public uint bone_index;
        public uint leg_index;
        public byte shape_type;
        public byte reserved0;
        public ushort hit_zone;
        public uint purpose_flags;
        public uint layer_mask;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelSkeletonColliderDefinition>();
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelSkeletonBindingDefinition
    {
        public uint struct_size;
        public uint skeleton_asset_id;
        public ulong skeleton_content_hash;
        public uint bone_count;
        public uint root_bone_index;
        public uint body_bone_index;
        public uint leg_count;
        public uint processing_order_count;
        public float input_deadzone;
        public float step_threshold_meters;
        public uint step_duration_ticks;
        public uint max_swinging_legs;
        public float body_follow_speed;
        public float slope_alignment;
        public KernelFootholdQueryType foothold_query_type;
        public byte reserved_foothold0;
        public ushort reserved_foothold1;
        public float foothold_query_start_height_meters;
        public float foothold_query_distance_meters;
        public uint foothold_candidate_count;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxFootholdCandidates)]
        public KernelVec2[] foothold_candidate_offsets;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxSkeletonLegs)]
        public KernelSkeletonLegDefinition[] legs;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxSkeletonLegs)]
        public uint[] processing_order;
        public float stance_crouch_meters;
        public uint collider_count;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = KernelConstants.MaxSkeletonColliders)]
        public KernelSkeletonColliderDefinition[] colliders;

        public static uint StructSize =>
            (uint)Marshal.SizeOf<KernelSkeletonBindingDefinition>();

        public static KernelSkeletonBindingDefinition Create()
        {
            return new KernelSkeletonBindingDefinition
            {
                struct_size = StructSize,
                foothold_candidate_offsets =
                    new KernelVec2[KernelConstants.MaxFootholdCandidates],
                legs = new KernelSkeletonLegDefinition[KernelConstants.MaxSkeletonLegs],
                processing_order = new uint[KernelConstants.MaxSkeletonLegs],
                colliders = new KernelSkeletonColliderDefinition[KernelConstants.MaxSkeletonColliders],
            };
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelEntityTemplateDefinition
    {
        public uint struct_size;
        public uint entity_template_id;
        public KernelEntityType entity_type;
        public KernelActorType actor_type;
        public uint actor_template_id;
        public uint component_flags;
        public uint collider_template_id;
        public ushort animation_state;
        public ushort reserved0;
        public uint visual_flags;
        public KernelCombatStateDefinition combat;
        public KernelAgentVisionConfig vision;
        public KernelEntityAiDefinition ai;
        public KernelMovementDefinition movement;
        public KernelActionTriggerDefinition activated_trigger;
        public KernelActionTriggerDefinition collision_trigger;
        public KernelActionTriggerDefinition health_depleted_trigger;
        public KernelActionTriggerDefinition destroy_entity_trigger;
        public KernelPropDefinition prop;
        public uint collision_trigger_mask;
        public KernelSkeletonBindingDefinition skeleton;
        public float impulse_resistance;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelEntityTemplateDefinition>();
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
        public int health_delta;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelEntityLifecycleEvent
    {
        public KernelEntityLifecycleEventType type;
        public uint tick;
        public uint net_id;
        public KernelDespawnReason reason;
        public KernelEntityType entity_type;
        public KernelActorType actor_type;
        public uint owner_peer;
    }
}
