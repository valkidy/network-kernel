using System;
using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    public static class KernelConstants
    {
        public const uint AbiVersion = 8;

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
