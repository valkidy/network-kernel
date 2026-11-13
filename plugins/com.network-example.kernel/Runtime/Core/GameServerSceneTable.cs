using System;
using System.Collections.Generic;
using System.Text;

namespace NetworkExample.Kernel
{
    // One level slot a SCENE_STATE lists.
    public readonly struct GameServerSceneSlot
    {
        public readonly uint Slot;
        public readonly uint Generation;
        public readonly ushort TemplateId;
        public readonly byte Source;
        public readonly ulong RecipeHash;
        public readonly string Name;

        public GameServerSceneSlot(
            uint slot, uint generation, ushort templateId, byte source, ulong recipeHash, string name)
        {
            Slot = slot;
            Generation = generation;
            TemplateId = templateId;
            Source = source;
            RecipeHash = recipeHash;
            Name = name;
        }
    }

    // One way between the camp and the party's level in a SCENE_STATE: a
    // level_gate_entrance portal while Open, only announced while Pending.
    // EntranceForward is the way a player walks into it, Exit where its
    // arrivals step out (the exit prop's place while Open; zero for the level
    // gate until its level is ready) and ExitForward the way they face. The
    // exit prop's net id is EntranceNetId + 1.
    public readonly struct GameServerSceneGate
    {
        public const int WireBytes = 53;

        public readonly byte State;
        public readonly uint EntranceNetId;
        public readonly KernelVec3 Entrance;
        public readonly KernelVec3 EntranceForward;
        public readonly KernelVec3 Exit;
        public readonly KernelVec3 ExitForward;

        public GameServerSceneGate(
            byte state,
            uint entranceNetId,
            KernelVec3 entrance,
            KernelVec3 entranceForward,
            KernelVec3 exit,
            KernelVec3 exitForward)
        {
            State = state;
            EntranceNetId = entranceNetId;
            Entrance = entrance;
            EntranceForward = entranceForward;
            Exit = exit;
            ExitForward = exitForward;
        }

        public bool IsOpen => State == GameServerConstants.SceneGateOpen;
        public bool IsPending => State == GameServerConstants.SceneGatePending;
        public bool IsAnnounced => State != GameServerConstants.SceneGateNone;

        internal static GameServerSceneGate Read(byte[] p, int at)
        {
            return new GameServerSceneGate(
                p[at],
                BitConverter.ToUInt32(p, at + 1),
                ReadVec3(p, at + 5),
                ReadVec3(p, at + 17),
                ReadVec3(p, at + 29),
                ReadVec3(p, at + 41));
        }

        private static KernelVec3 ReadVec3(byte[] p, int at)
        {
            return new KernelVec3(
                BitConverter.ToSingle(p, at),
                BitConverter.ToSingle(p, at + 4),
                BitConverter.ToSingle(p, at + 8));
        }
    }

    // The whole scene state. It is sent on joining, on every change and again
    // every few seconds, so reconcile to the latest instead of following steps:
    // load each listed slot not held at that generation, answer SendSceneReady,
    // and unload any held slot that is no longer listed.
    public sealed class GameServerSceneState
    {
        // flow, slot, generation, outcome, owner, defeated, target; then the
        // two gates, then slot_count (GameServer ABI 6).
        private const int FixedBytes = 15;
        private const int SlotCountAt = FixedBytes + 2 * GameServerSceneGate.WireBytes;
        private const int HeaderBytes = SlotCountAt + 1;
        private const int SlotHeaderBytes = 17;
        // Half the width of a level gate's opening (the level_gate_gate
        // collider), for callers still sizing a marker by GateRadius.
        private const float LevelGateHalfWidth = 1.15f;

        public byte Flow { get; private set; }
        public uint LevelSlot { get; private set; }
        public uint LevelGeneration { get; private set; }
        public byte Outcome { get; private set; }
        // Camp -> level: Pending from the level being asked for, Open from
        // SceneFlowGateOpen until the objective is met.
        public GameServerSceneGate LevelGate { get; private set; }
        // Level -> camp: Pending from the level being ready, Open in
        // SceneFlowReturnOpen.
        public GameServerSceneGate ReturnGate { get; private set; }
        public uint OwnerNetId { get; private set; }
        public ushort EnemiesDefeated { get; private set; }
        public ushort EnemiesTarget { get; private set; }
        public IReadOnlyList<GameServerSceneSlot> Slots { get; private set; }

        // The gate that is open now: the level gate, else the return gate.
        public bool TryGetOpenGate(out GameServerSceneGate gate)
        {
            gate = LevelGate.IsOpen ? LevelGate : ReturnGate;
            return gate.IsOpen;
        }

        [Obsolete("Read LevelGate and ReturnGate (GameServer ABI 6).")]
        public bool GateOpen => LevelGate.IsOpen || ReturnGate.IsOpen;

        [Obsolete("Read LevelGate.Entrance or ReturnGate.Entrance (GameServer ABI 6).")]
        public KernelVec3 GatePosition => TryGetOpenGate(out GameServerSceneGate gate) ? gate.Entrance : default;

        [Obsolete("A level gate is a portal, not a circle; this is half its opening's width.")]
        public float GateRadius => LevelGateHalfWidth;

        // False for any other message or a malformed body.
        public static bool TryDecode(in KernelGameMessage message, out GameServerSceneState state)
        {
            state = null;
            byte[] p = message.payload;
            int size = (int)message.payload_size;
            if (message.message_type != GameServerConstants.MessageSceneState ||
                p == null || size < HeaderBytes || size > p.Length)
            {
                return false;
            }

            int slotCount = p[SlotCountAt];
            var slots = new List<GameServerSceneSlot>(slotCount);
            int at = HeaderBytes;
            for (int index = 0; index < slotCount; ++index)
            {
                if (size - at < SlotHeaderBytes || size - at - SlotHeaderBytes < p[at + 16])
                {
                    return false;
                }
                int nameLength = p[at + 16];
                slots.Add(new GameServerSceneSlot(
                    p[at],
                    BitConverter.ToUInt32(p, at + 1),
                    BitConverter.ToUInt16(p, at + 5),
                    p[at + 7],
                    BitConverter.ToUInt64(p, at + 8),
                    Encoding.UTF8.GetString(p, at + 17, nameLength)));
                at += SlotHeaderBytes + nameLength;
            }
            if (at != size)
            {
                return false;
            }

            state = new GameServerSceneState
            {
                Flow = p[0],
                LevelSlot = p[1],
                LevelGeneration = BitConverter.ToUInt32(p, 2),
                Outcome = p[6],
                OwnerNetId = BitConverter.ToUInt32(p, 7),
                EnemiesDefeated = BitConverter.ToUInt16(p, 11),
                EnemiesTarget = BitConverter.ToUInt16(p, 13),
                LevelGate = GameServerSceneGate.Read(p, FixedBytes),
                ReturnGate = GameServerSceneGate.Read(p, FixedBytes + GameServerSceneGate.WireBytes),
                Slots = slots,
            };
            return true;
        }
    }

    // A prop asked to show this player an interface. For the level list,
    // ui_id is UiLevelSelect: on OpenUiOk show it, and send the choice with
    // SendSceneSelect.
    public readonly struct GameServerOpenUi
    {
        private const int BodyBytes = 9;

        public readonly uint UiId;
        public readonly uint PropNetId;
        public readonly byte Result;

        private GameServerOpenUi(uint uiId, uint propNetId, byte result)
        {
            UiId = uiId;
            PropNetId = propNetId;
            Result = result;
        }

        // False for any other message or a malformed body.
        public static bool TryDecode(in KernelGameMessage message, out GameServerOpenUi openUi)
        {
            openUi = default;
            if (message.message_type != GameServerConstants.MessageOpenUi ||
                message.payload == null || message.payload_size != BodyBytes)
            {
                return false;
            }
            openUi = new GameServerOpenUi(
                BitConverter.ToUInt32(message.payload, 0),
                BitConverter.ToUInt32(message.payload, 4),
                message.payload[8]);
            return true;
        }
    }

    // The scene table on a client (and a listen host's local player), keyed by
    // the kernel its catalog was loaded into. Slot 0 is the camp; levels are
    // slot 1 and up, in world space with the slot's offset included.
    // GameServerTerrain.Release frees every slot's copy.
    public static class GameServerSceneTable
    {
        // Whether the native library has the scene table at all.
        public static bool IsSupported =>
            (GameServerAbi.GetInfo().capability_flags & GameServerConstants.CapabilitySceneSlots) != 0UL;

        // The levels the kernel's catalog offers, by ascending id.
        public static GameServerSceneTemplateInfo[] GetTemplates(Kernel kernel)
        {
            IntPtr handle = RequireSupported(kernel);
            uint count = GameServerNative.GameServer_GetSceneTemplateCount(handle);
            var templates = new List<GameServerSceneTemplateInfo>((int)count);
            for (uint index = 0U; index < count; ++index)
            {
                var info = new GameServerSceneTemplateInfo { struct_size = GameServerSceneTemplateInfo.StructSize };
                if (GameServerNative.GameServer_GetSceneTemplate(handle, index, ref info))
                {
                    templates.Add(info);
                }
            }
            return templates.ToArray();
        }

        // Asks for a level. The server obeys only the owner, only in the camp.
        public static bool SendSceneSelect(Kernel kernel, ushort templateId)
        {
            RequireSupported(kernel);
            var body = new byte[2];
            PutU16(body, 0, templateId);
            return kernel.SendGameMessage(GameServerConstants.MessageSceneSelect, body, body.Length);
        }

        // Ends the party's level at once. The server obeys only the owner.
        public static bool SendSceneAbort(Kernel kernel)
        {
            RequireSupported(kernel);
            return kernel.SendGameMessage(GameServerConstants.MessageSceneAbort, null, 0);
        }

        // The slot's collision is in this client's prediction world.
        public static bool SendSceneReady(Kernel kernel, uint slot, uint generation)
        {
            RequireSupported(kernel);
            byte[] body = SlotBody(5, slot, generation);
            return kernel.SendGameMessage(GameServerConstants.MessageSceneReady, body, body.Length);
        }

        // This client could not build the slot; the party does not wait for it.
        public static bool SendSceneLoadFailed(Kernel kernel, uint slot, uint generation, byte reason)
        {
            RequireSupported(kernel);
            byte[] body = SlotBody(6, slot, generation);
            body[5] = reason;
            return kernel.SendGameMessage(GameServerConstants.MessageSceneLoadFailed, body, body.Length);
        }

        // Give every message to this; true when it was a recipe chunk.
        public static bool AcceptMessage(Kernel kernel, in KernelGameMessage message)
        {
            IntPtr handle = RequireSupported(kernel);
            KernelGameMessage copy = message;
            return GameServerNative.GameServer_ClientAcceptSceneMessage(handle, ref copy);
        }

        // For a slot whose source is SceneSourceRecipe: call every frame until
        // it stops returning SceneLoadPending. It fetches the recipe, asks again
        // if it does not arrive, and checks it against recipeHash.
        public static uint PumpRecipe(
            Kernel kernel, uint slot, uint generation, ulong recipeHash, float deltaSeconds)
        {
            IntPtr handle = RequireSupported(kernel);
            return GameServerNative.GameServer_ClientPumpSceneRecipe(
                handle, slot, generation, recipeHash, deltaSeconds);
        }

        // Starts composing on a worker. Add SceneLoadFromRecipe for a recipe slot.
        public static bool BeginLoad(
            Kernel kernel,
            byte[] bundleBytes,
            string entryPath,
            string sceneTemplate,
            uint slot,
            uint generation,
            uint flags)
        {
            IntPtr handle = RequireSupported(kernel);
            RequireBundle(bundleBytes);
            return GameServerNative.GameServer_ClientBeginLoadSceneSlot(
                handle, bundleBytes, (uint)bundleBytes.Length, entryPath, sceneTemplate, slot, generation, flags);
        }

        // Call on the Kernel.Update thread until it stops returning
        // SceneLoadPending: the call that finds the load finished commits it.
        public static uint PollLoad(Kernel kernel, uint slot, uint generation)
        {
            IntPtr handle = RequireSupported(kernel);
            return GameServerNative.GameServer_ClientPollSceneSlotLoad(handle, slot, generation);
        }

        // Loads on the calling thread, which stalls it: prefer BeginLoad.
        public static bool LoadNow(
            Kernel kernel, byte[] bundleBytes, string entryPath, string sceneTemplate, uint slot, uint flags)
        {
            IntPtr handle = RequireSupported(kernel);
            RequireBundle(bundleBytes);
            return GameServerNative.GameServer_ClientLoadSceneSlot(
                handle, bundleBytes, (uint)bundleBytes.Length, entryPath, sceneTemplate, slot, flags);
        }

        // Pass SceneLoadRegisterCollision if the load did.
        public static bool Unload(Kernel kernel, uint slot, uint flags)
        {
            IntPtr handle = RequireSupported(kernel);
            return GameServerNative.GameServer_ClientUnloadSceneSlot(handle, slot, flags);
        }

        // False when the slot has no terrain.
        public static bool TryGetTerrainInfo(Kernel kernel, uint slot, out GameServerTerrainInfo info)
        {
            IntPtr handle = RequireSupported(kernel);
            info = new GameServerTerrainInfo { struct_size = GameServerTerrainInfo.StructSize };
            if (GameServerNative.GameServer_GetSceneTerrainInfo(handle, slot, ref info))
            {
                return true;
            }
            info = default;
            return false;
        }

        // As GameServerTerrain.CopyHeights, for one slot.
        public static bool CopyTerrainHeights(
            Kernel kernel, uint slot, uint x0, uint z0, uint width, uint depth, short[] heightsCm)
        {
            IntPtr handle = RequireSupported(kernel);
            if (heightsCm == null)
            {
                throw new ArgumentNullException(nameof(heightsCm));
            }
            return GameServerNative.GameServer_CopySceneTerrainHeights(
                handle, slot, x0, z0, width, depth, heightsCm, (uint)heightsCm.Length);
        }

        // Every stamp the slot's recipe placed; empty if it has no terrain.
        public static GameServerTerrainPlacement[] GetTerrainPlacements(Kernel kernel, uint slot)
        {
            IntPtr handle = RequireSupported(kernel);
            uint count = GameServerNative.GameServer_CopySceneTerrainPlacements(handle, slot, null, 0U);
            var placements = new GameServerTerrainPlacement[count];
            if (count == 0U)
            {
                return placements;
            }
            uint total = GameServerNative.GameServer_CopySceneTerrainPlacements(handle, slot, placements, count);
            if (total != count)
            {
                throw new InvalidOperationException(
                    $"Terrain placement count changed while copying ({count} -> {total}).");
            }
            return placements;
        }

        // False when the slot has no baked mesh (a terrain level, or empty).
        public static bool TryGetStaticMeshInfo(Kernel kernel, uint slot, out GameServerStaticSceneMeshInfo info)
        {
            IntPtr handle = RequireSupported(kernel);
            info = new GameServerStaticSceneMeshInfo { struct_size = GameServerStaticSceneMeshInfo.StructSize };
            if (GameServerNative.GameServer_GetSceneStaticMeshInfo(handle, slot, ref info))
            {
                return true;
            }
            info = default;
            return false;
        }

        // The slot's whole mesh, or false when it has none.
        public static bool TryCopyStaticMesh(
            Kernel kernel,
            uint slot,
            out GameServerStaticSceneMeshInfo info,
            out float[] positions,
            out uint[] indices)
        {
            positions = null;
            indices = null;
            if (!TryGetStaticMeshInfo(kernel, slot, out info))
            {
                return false;
            }
            var copiedPositions = new float[(long)info.vertex_count * 3];
            var copiedIndices = new uint[(long)info.triangle_count * 3];
            if (!GameServerNative.GameServer_CopySceneStaticMesh(
                    kernel.Handle,
                    slot,
                    copiedPositions,
                    (uint)copiedPositions.Length,
                    copiedIndices,
                    (uint)copiedIndices.Length))
            {
                return false;
            }
            positions = copiedPositions;
            indices = copiedIndices;
            return true;
        }

        private static byte[] SlotBody(int size, uint slot, uint generation)
        {
            var body = new byte[size];
            body[0] = (byte)slot;
            PutU16(body, 1, (ushort)generation);
            PutU16(body, 3, (ushort)(generation >> 16));
            return body;
        }

        private static void PutU16(byte[] body, int at, ushort value)
        {
            body[at] = (byte)value;
            body[at + 1] = (byte)(value >> 8);
        }

        private static void RequireBundle(byte[] bundleBytes)
        {
            if (bundleBytes == null)
            {
                throw new ArgumentNullException(nameof(bundleBytes));
            }
        }

        private static IntPtr RequireSupported(Kernel kernel)
        {
            if (kernel == null)
            {
                throw new ArgumentNullException(nameof(kernel));
            }
            if (!IsSupported)
            {
                throw new NotSupportedException(
                    "The native game server has no scene table (GAME_SERVER_CAPABILITY_SCENE_SLOTS).");
            }
            return kernel.Handle;
        }
    }
}
