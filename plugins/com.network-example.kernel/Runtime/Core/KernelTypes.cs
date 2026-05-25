using System;
using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    public static class KernelConstants
    {
        public const uint AbiVersion = 12;
        public const int MaxWeapons = 7;

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

        public const uint CollisionLayerPlayer = 0x00000001U;
        public const uint CollisionLayerEnemy = 0x00000002U;
        public const uint CollisionLayerProjectile = 0x00000004U;
        public const uint CollisionLayerAreaEffect = 0x00000008U;
        public const uint CollisionMaskDamageable = CollisionLayerPlayer | CollisionLayerEnemy;

        public const uint VisualFlagMoving = 0x00000001U;
        public const uint VisualFlagReloading = 0x00000002U;
        public const uint VisualFlagDead = 0x00000004U;
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
        Explosion = 1,
        PiercingSegment = 2,
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
        public ulong capability_flags;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KernelLocalPlayerInfo
    {
        public uint peer_id;
        public uint player_net_id;
        [MarshalAs(UnmanagedType.I1)]
        public bool has_welcome;
        [MarshalAs(UnmanagedType.I1)]
        public bool connected;
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
        [MarshalAs(UnmanagedType.I1)]
        public bool valid;

        public static uint StructSize => (uint)Marshal.SizeOf<KernelServerEntityState>();
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
        public float explosion_radius;
        public KernelVec3 gravity;
        public uint collision_mask;
        public uint max_hit_count;
        public KernelHomingMechanicsDefinition homing;

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
        [MarshalAs(UnmanagedType.I1)]
        public bool valid;

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
        [MarshalAs(UnmanagedType.I1)]
        public bool valid;

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
        [MarshalAs(UnmanagedType.I1)]
        public bool valid;

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
