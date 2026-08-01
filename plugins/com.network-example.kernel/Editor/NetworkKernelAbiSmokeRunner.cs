using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Threading;
using UnityEditor;
using UnityEngine;
using NetworkExample.Kernel.Client;
using NetworkExample.Kernel.Host;
using NetworkExample.Kernel.Presentation;

namespace NetworkExample.Kernel.Editor
{
    public static class NetworkKernelAbiSmokeRunner
    {
        private static uint nextActionInstanceId = 1;

        [MenuItem("Network Example/Run Kernel ABI Smoke")]
        public static void Run()
        {
            KernelAbi.ValidateNativeAbi();
            GameServerAbi.ValidateNativeAbi();
            KernelAbiInfo info = KernelAbi.GetInfo();
            Require(KernelConstants.AbiVersion == 65, "Managed kernel ABI version was not v65.");
            Require(
                RenderEntityState.StructSize == 144,
                "Managed RenderEntityState layout was not 144 bytes.");
            RequireClientPredictionFailureStateTransition();
            Require(
                (info.capability_flags & KernelConstants.CapabilityEntityLifecycleEvents) != 0,
                "Kernel lifecycle event capability was missing.");
            Require(
                (info.capability_flags & KernelConstants.CapabilityVisionStateQuery) != 0,
                "Kernel vision state query capability was missing.");
            Require(
                (info.capability_flags & KernelConstants.CapabilityGameplayCatalogSync) != 0,
                "Kernel gameplay catalog sync capability was missing.");
            Require(
                (info.capability_flags & KernelConstants.CapabilityControlPlaneRpc) != 0,
                "Kernel control-plane RPC capability was missing.");
            Require(
                (info.capability_flags & KernelConstants.CapabilityActionTimeline) != 0 &&
                (info.capability_flags & KernelConstants.CapabilityActionIntents) != 0,
                "Kernel action timeline capabilities were missing.");
            Require(
                (info.capability_flags & KernelConstants.CapabilityItemPropSystem) != 0,
                "Kernel item/prop system capability was missing.");
            Require(
                info.gameplay_catalog_manifest_size ==
                    KernelGameplayCatalogManifest.StructSize,
                "Kernel gameplay catalog manifest size mismatch.");
            Require(
                info.gameplay_catalog_sync_status_size ==
                    KernelGameplayCatalogSyncStatus.StructSize,
                "Kernel gameplay catalog sync status size mismatch.");
            Require(
                KernelConstants.CollisionLayerAgentVision == 0x00000010U,
                "Kernel agent vision collision layer mismatch.");
            Require(
                KernelColliderShapeType.Cone == (KernelColliderShapeType)4,
                "Kernel cone collider shape type mismatch.");
            Require(
                KernelColliderPurpose.Vision == (KernelColliderPurpose)(1U << 3),
                "Kernel vision collider purpose mismatch.");
            Require(
                KernelVec4.StructSize == 16,
                "KernelVec4 layout size mismatch.");
            Require(
                KernelPhysicsConfig.StructSize == 12 &&
                KernelSessionRulesConfig.StructSize == 8,
                "Kernel configuration layout size mismatch.");
            Require(
                KernelMovementDefinition.StructSize == 44,
                "Kernel movement definition layout size mismatch.");
            Require(
                KernelBoneLocalTransform.StructSize == 40 &&
                KernelSkeletonRenderState.StructSize == 40 &&
                KernelSkeletonRenderStateResult.StructSize == 48 &&
                KernelSkeletonBindingDefinition.StructSize == 488,
                "Kernel skeleton ABI layout size mismatch.");
            Require(
                (info.capability_flags &
                 KernelConstants.CapabilitySkeletonRenderStates) != 0,
                "Kernel skeleton render-state capability was missing.");
            RequireSkeletonBindingValidation();
            RequireSkeletonAbsolutePoseApplication();
            Require(
                (KernelConstants.VisualFlagHpUnknown & KernelConstants.VisualFlagDead) == 0,
                "Kernel HpUnknown visual flag overlapped Dead.");
            Require(
                KernelConstants.VisualFlagGrounded == 0x00000010U &&
                KernelConstants.VisualFlagFalling == 0x00000020U &&
                KernelConstants.VisualFlagLanded == 0x00000040U &&
                KernelEventType.ActorLanded == (KernelEventType)12,
                "Kernel grounding presentation ABI mismatch.");
            RequireLANDiscovery();
            byte[] catalogBundleBytes = LoadGameplayCatalogBundleBytes();

            using (var kernel = new Kernel(KernelConfig.CreateDefault(KernelMode.ListenServer)))
            {
                Require(
                    kernel.SetPhysicsConfig(new KernelPhysicsConfig()),
                    "Kernel_SetPhysicsConfig failed.");
                Require(
                    kernel.SetSessionRules(new KernelSessionRulesConfig
                    {
                        actor_blocking_mode = KernelActorBlockingMode.Disabled,
                    }),
                    "Kernel_SetSessionRules failed.");
                using (var gameServer = new GameServer(
                    kernel,
                    catalogBundleBytes,
                    "gameplay_catalog.yaml",
                    out KernelGameplayCatalogLoadResult loadResult))
                {
                    Require(
                        loadResult.status == KernelConstants.GameplayCatalogLoadStatusSuccess,
                        "GameServer bundle load did not report success.");
                    Require(
                        kernel.StartListenServer(7777),
                        "Kernel_StartListenServer failed.");
                    Require(
                        gameServer.QueryWeaponTemplate(
                            4,
                            out GameServerWeaponTemplateInfo templateInfo),
                        "GameServer_QueryWeaponTemplate failed.");
                    Require(templateInfo.valid != 0, "GameServer weapon template was not valid.");
                    Require(
                        templateInfo.mechanics.fire_mode ==
                            (byte)KernelWeaponFireMode.Projectile &&
                        templateInfo.mechanics.projectile_template_id == 4,
                        "GameServer weapon template did not expose area-effect projectile mechanics.");
                }

                Require(
                    kernel.TryGetLocalPlayerInfo(out KernelLocalPlayerInfo localInfo) &&
                    localInfo.player_net_id != 0 &&
                    kernel.LocalPlayerNetId == localInfo.player_net_id &&
                    kernel.IsClientReady,
                    "Kernel_GetLocalPlayerInfo returned no local player.");

                var createInfo = new KernelServerEntityCreateInfo
                {
                    entity_type = KernelEntityType.Actor,
                    actor_type = KernelActorType.Agent,
                    position = new KernelVec3(6.0f, 0.0f, 0.0f),
                    rotation = new KernelQuat(0.0f, 0.0f, 0.0f, 1.0f),
                    animation_state = 4,
                    visual_flags = 0,
                };
                Require(
                    kernel.ServerCreateEntity(createInfo, out uint enemyNetId) &&
                    enemyNetId != 0,
                    "Kernel_ServerCreateEntity failed.");

                RequireAbi26CatalogDiagnostics(kernel);
                Require(
                    kernel.ServerSetEntityActorTemplate(enemyNetId, 404),
                    "Kernel_ServerSetEntityActorTemplate failed.");
                ConfigureCombatAndWeapons(kernel, enemyNetId);
                RequireAbi26VisionDiagnostics(kernel, enemyNetId);
                Require(
                    kernel.ServerGetEntityState(
                        enemyNetId,
                        out KernelServerEntityState enemyState) &&
                    enemyState.valid != 0 &&
                    enemyState.hp == 240 &&
                    enemyState.max_hp == 240 &&
                    enemyState.actor_template_id == 404,
                    "Kernel_ServerSetEntityCombatState did not update enemy health.");
                Require(
                    kernel.ServerSetEntityHealth(enemyNetId, 123),
                    "Kernel_ServerSetEntityHealth failed.");
                Require(
                    kernel.ServerGetEntityState(
                        enemyNetId,
                        out enemyState) &&
                    enemyState.valid != 0 &&
                    enemyState.hp == 123 &&
                    enemyState.max_hp == 240,
                    "Kernel_ServerSetEntityHealth did not update hp only.");
                RequireControlPlaneRpc(kernel, enemyNetId);

                Require(
                    kernel.ServerSetEntityActorTemplate(
                        localInfo.player_net_id,
                        404),
                    "Kernel_ServerSetEntityActorTemplate failed for local player.");

                var states = new RenderEntityState[16];
                uint renderCount = 0;
                for (int physicsTick = 0; physicsTick < 8; ++physicsTick)
                {
                    kernel.Update(1.0f / 30.0f);
                    Array.Clear(states, 0, states.Length);
                    renderCount = kernel.GetRenderStates(states);
                    if (renderCount > 0 &&
                        HasActiveRenderEntity(states, enemyNetId))
                    {
                        break;
                    }
                }
                Require(
                    renderCount > 0,
                    "Kernel_GetRenderStates returned no states after waiting " +
                    "for first-physics finalization.");
                Require(
                    HasActiveRenderEntity(states, enemyNetId),
                    $"Kernel_GetRenderStates missed active enemy. {DescribeRenderStates(states)}");
                Require(
                    HasRenderEntityHpUnknown(states, enemyNetId),
                    $"Kernel_GetRenderStates should keep listen-server client HP unknown. {DescribeRenderStates(states)}");
                Require(
                    kernel.GetRenderStatesAtTime(33333, states) > 0,
                    "Kernel_GetRenderStatesAtTime returned no states.");

                RequireProjectileTemplateFire(kernel, enemyNetId, 4);
                RequireProjectileTemplateFire(kernel, enemyNetId, 5);
                RequireHomingFire(kernel, enemyNetId);

                Require(
                    kernel.ServerDestroyEntity(enemyNetId, KernelDespawnReason.Destroyed),
                    "Kernel_ServerDestroyEntity failed.");
                RequireDestroyedLifecycleEvent(kernel, enemyNetId);
            }

            using (var host = new NetworkHost())
            {
                Require(
                    host.Start(
                        7778,
                        catalogBundleBytes,
                        "gameplay_catalog.yaml",
                        out KernelGameplayCatalogLoadResult loadResult) &&
                    loadResult.status == KernelConstants.GameplayCatalogLoadStatusSuccess,
                    "NetworkHost.Start with bundle failed.");
                Require(
                    host.Kernel.TryGetGameplayCatalogSyncStatus(
                        out KernelGameplayCatalogSyncStatus hostSyncStatus) &&
                    hostSyncStatus.manifest.catalog_version == loadResult.catalog_version &&
                    hostSyncStatus.manifest.catalog_hash == loadResult.catalog_hash &&
                    hostSyncStatus.manifest.bundle_size == catalogBundleBytes.Length &&
                    hostSyncStatus.manifest.entry_path == "gameplay_catalog.yaml" &&
                    hostSyncStatus.manifest.content_namespace == "default",
                    "NetworkHost did not publish the loaded gameplay catalog bundle.");
                var hostEvents = new KernelEvent[16];
                for (int tick = 0; tick < 8 && host.EnemyCount != 10; ++tick)
                {
                    host.Update(1.0f / 30.0f, hostEvents);
                }
                Require(
                    host.IsLocalClientReady &&
                    host.LocalPlayerNetId != 0 &&
                    host.EnemyCount == 10,
                    $"NetworkHost smoke failed: ready={host.IsLocalClientReady} " +
                    $"localPlayer={host.LocalPlayerNetId} enemyCount={host.EnemyCount}.");
                Require(
                    host.GameServer.QueryWeaponTemplate(
                        6,
                        out GameServerWeaponTemplateInfo homingTemplate) &&
                    homingTemplate.valid != 0 &&
                    homingTemplate.mechanics.fire_mode ==
                        (byte)KernelWeaponFireMode.Projectile &&
                    homingTemplate.mechanics.projectile_template_id == 6,
                    "NetworkHost GameServer homing template query failed.");
            }

            RequireExternalGameplayCatalogSyncIfConfigured();

            Debug.Log("Network kernel ABI 65 smoke passed.");
        }

        private static void RequireSkeletonBindingValidation()
        {
            var root = new GameObject("KernelSkeletonBindingSmoke");
            try
            {
                Transform first = new GameObject("FirstBone").transform;
                Transform second = new GameObject("SecondBone").transform;
                first.SetParent(root.transform, false);
                second.SetParent(first, false);
                KernelSkeletonBinding binding =
                    root.AddComponent<KernelSkeletonBinding>();
                binding.SkeletonAssetId = 7;
                binding.SkeletonContentHash = 0x1234UL;
                binding.Bones = new[] { first, second };
                var state = new KernelSkeletonRenderState
                {
                    skeleton_asset_id = 7,
                    skeleton_content_hash = 0x1234UL,
                    bone_count = 2,
                };
                Require(
                    binding.TryValidate(state, out string error),
                    $"Valid KernelSkeletonBinding was rejected: {error}");

                binding.Bones = new[] { first, first };
                Require(
                    !binding.TryValidate(state, out error) &&
                    error.Contains("duplicates"),
                    "Duplicate KernelSkeletonBinding bones were not rejected.");

                RequireMonsterSimV4AutoBinding();
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(root);
            }
        }

        private static void RequireMonsterSimV4AutoBinding()
        {
            var root = new GameObject("MonsterSimV4AutoBindingSmoke");
            try
            {
                var expectedBones = new Transform[41];
                for (int index = 0; index < expectedBones.Length; ++index)
                {
                    var bone = new GameObject(
                        KernelSkeletonBinding.GetMonsterSimV4BoneName(index));
                    int parentIndex =
                        KernelSkeletonBinding.GetMonsterSimV4ParentBoneIndex(index);
                    bone.transform.SetParent(
                        parentIndex < 0 ? root.transform : expectedBones[parentIndex],
                        false);
                    expectedBones[index] = bone.transform;
                }

                KernelSkeletonBinding binding =
                    root.AddComponent<KernelSkeletonBinding>();
                Require(
                    binding.TryAutoMap(out string error),
                    $"Monster v4 automatic bone mapping failed: {error}");
                Require(
                    binding.Bones.Length == expectedBones.Length,
                    "Monster v4 automatic bone mapping returned the wrong count.");
                for (int index = 0; index < expectedBones.Length; ++index)
                {
                    Require(
                        binding.Bones[index] == expectedBones[index],
                        $"Monster v4 automatic bone mapping mismatched index {index}.");
                }
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(root);
            }
        }

        private static void RequireSkeletonAbsolutePoseApplication()
        {
            var root = new GameObject("KernelSkeletonAbsolutePoseSmoke");
            try
            {
                Transform bone = new GameObject("AbsolutePoseBone").transform;
                bone.SetParent(root.transform, false);
                Vector3 bindPosition = new Vector3(10.0f, 20.0f, 30.0f);
                Quaternion bindRotation = Quaternion.Euler(25.0f, 0.0f, 10.0f);
                Vector3 bindScale = new Vector3(1.5f, 2.0f, 2.5f);
                bone.localPosition = bindPosition;
                bone.localRotation = bindRotation;
                bone.localScale = bindScale;

                KernelSkeletonPoseApplicator applicator =
                    CreateSkeletonPoseApplicator(root, bone);
                KernelSkeletonRenderState state = SkeletonState(
                    KernelConstants.SkeletonPoseFlagProcedural);
                SkeletonRenderStateBuffer firstPose = SkeletonBuffer(
                    state,
                    BoneTransform(
                        new Vector3(3.0f, 4.0f, 5.0f),
                        Quaternion.identity,
                        new Vector3(2.0f, 2.0f, 4.0f)));
                Require(
                    applicator.TryApply(state, firstPose, out string error),
                    $"First absolute skeleton pose was rejected: {error}");
                RequireVectorApproximately(
                    bone.localPosition,
                    bindPosition,
                    "First procedural pose was added to the FBX bind position.");
                RequireQuaternionApproximately(
                    bone.localRotation,
                    bindRotation,
                    "First procedural pose was multiplied into the FBX bind rotation.");
                RequireVectorApproximately(
                    bone.localScale,
                    bindScale,
                    "First procedural pose changed the FBX bind scale.");

                Quaternion nativeYaw = Quaternion.AngleAxis(30.0f, Vector3.up);
                SkeletonRenderStateBuffer secondPose = SkeletonBuffer(
                    state,
                    BoneTransform(
                        new Vector3(4.0f, 6.0f, 8.0f),
                        nativeYaw,
                        new Vector3(4.0f, 1.0f, 4.0f)));
                Require(
                    applicator.TryApply(state, secondPose, out error),
                    $"Second absolute skeleton pose was rejected: {error}");
                RequireVectorApproximately(
                    bone.localPosition,
                    bindPosition + new Vector3(-1.0f, 2.0f, 3.0f),
                    "Native position delta did not use the X-reflected basis.");
                Quaternion reflectedYaw =
                    new Quaternion(nativeYaw.x, -nativeYaw.y, -nativeYaw.z, nativeYaw.w);
                RequireQuaternionApproximately(
                    bone.localRotation,
                    bindRotation * reflectedYaw,
                    "Native rotation delta was not applied after the FBX bind rotation.");
                Require(
                    Quaternion.Angle(
                        bone.localRotation,
                        reflectedYaw * bindRotation) > 0.1f,
                    "Skeleton rotation smoke did not distinguish bind * delta from delta * bind.");
                RequireVectorApproximately(
                    bone.localScale,
                    Vector3.Scale(bindScale, new Vector3(2.0f, 0.5f, 1.0f)),
                    "Native absolute scale was not converted to a reference-relative ratio.");
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(root);
            }

            RequireSkeletonZeroReferenceScale();
            RequireSkeletonReferenceReset();
        }

        private static void RequireSkeletonZeroReferenceScale()
        {
            var root = new GameObject("KernelSkeletonZeroScaleSmoke");
            try
            {
                Transform bone = new GameObject("ZeroScaleBone").transform;
                bone.SetParent(root.transform, false);
                bone.localScale = new Vector3(2.0f, 3.0f, 4.0f);
                KernelSkeletonPoseApplicator applicator =
                    CreateSkeletonPoseApplicator(root, bone);
                KernelSkeletonRenderState state = SkeletonState(
                    KernelConstants.SkeletonPoseFlagProcedural);
                Require(
                    applicator.TryApply(
                        state,
                        SkeletonBuffer(
                            state,
                            BoneTransform(
                                Vector3.zero,
                                Quaternion.identity,
                                new Vector3(0.0f, 1.0e-10f, 2.0f))),
                        out string error),
                    $"Zero-scale skeleton reference was rejected: {error}");
                Require(
                    applicator.TryApply(
                        state,
                        SkeletonBuffer(
                            state,
                            BoneTransform(
                                Vector3.zero,
                                Quaternion.identity,
                                new Vector3(5.0f, 6.0f, 4.0f))),
                        out error),
                    $"Skeleton pose after zero-scale reference was rejected: {error}");
                RequireVectorApproximately(
                    bone.localScale,
                    new Vector3(2.0f, 3.0f, 8.0f),
                    "Zero or near-zero reference scale produced an unsafe ratio.");
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(root);
            }
        }

        private static void RequireSkeletonReferenceReset()
        {
            Vector3 bindPosition = new Vector3(7.0f, 8.0f, 9.0f);
            Vector3[] nativeReferences =
            {
                new Vector3(1.0f, 2.0f, 3.0f),
                new Vector3(20.0f, 30.0f, 40.0f),
            };
            for (int index = 0; index < nativeReferences.Length; ++index)
            {
                var root = new GameObject($"KernelSkeletonRespawnSmoke{index}");
                try
                {
                    Transform bone = new GameObject("RespawnBone").transform;
                    bone.SetParent(root.transform, false);
                    bone.localPosition = bindPosition;
                    KernelSkeletonPoseApplicator applicator =
                        CreateSkeletonPoseApplicator(root, bone);
                    KernelSkeletonRenderState state = SkeletonState(
                        KernelConstants.SkeletonPoseFlagProcedural);
                    Require(
                        applicator.TryApply(
                            state,
                            SkeletonBuffer(
                                state,
                                BoneTransform(
                                    nativeReferences[index],
                                    Quaternion.identity,
                                    Vector3.one)),
                            out string error),
                        $"Respawn reference pose {index} was rejected: {error}");
                    RequireVectorApproximately(
                        bone.localPosition,
                        bindPosition,
                        "A new skeleton instance reused a stale native reference pose.");
                }
                finally
                {
                    UnityEngine.Object.DestroyImmediate(root);
                }
            }
        }

        private static KernelSkeletonPoseApplicator CreateSkeletonPoseApplicator(
            GameObject root,
            Transform bone)
        {
            KernelSkeletonBinding binding = root.AddComponent<KernelSkeletonBinding>();
            binding.SkeletonAssetId = 7;
            binding.SkeletonContentHash = 0x1234UL;
            binding.AutoMapKnownSkeleton = false;
            binding.PreservePrefabBindPose = true;
            binding.Bones = new[] { bone };
            return root.AddComponent<KernelSkeletonPoseApplicator>();
        }

        private static KernelSkeletonRenderState SkeletonState(uint poseFlags)
        {
            return new KernelSkeletonRenderState
            {
                entity_net_id = 1,
                skeleton_asset_id = 7,
                skeleton_content_hash = 0x1234UL,
                first_bone_transform = 0,
                bone_count = 1,
                pose_tick = 2,
                pose_flags = poseFlags,
                pose_time_us = 66666,
            };
        }

        private static SkeletonRenderStateBuffer SkeletonBuffer(
            KernelSkeletonRenderState state,
            KernelBoneLocalTransform transform)
        {
            var buffer = new SkeletonRenderStateBuffer(1, 1);
            buffer.States[0] = state;
            buffer.BoneTransforms[0] = transform;
            MethodInfo commit = typeof(SkeletonRenderStateBuffer).GetMethod(
                "Commit",
                BindingFlags.Instance | BindingFlags.NonPublic);
            Require(commit != null, "Skeleton smoke could not access buffer commit.");
            commit.Invoke(
                buffer,
                new object[]
                {
                    new KernelSkeletonRenderStateResult
                    {
                        struct_size = KernelSkeletonRenderStateResult.StructSize,
                        status = KernelConstants.SkeletonRenderStatusSuccess,
                        required_state_count = 1,
                        required_bone_transform_count = 1,
                        written_state_count = 1,
                        written_bone_transform_count = 1,
                        source_tick = state.pose_tick,
                    },
                });
            return buffer;
        }

        private static KernelBoneLocalTransform BoneTransform(
            Vector3 position,
            Quaternion rotation,
            Vector3 scale)
        {
            return new KernelBoneLocalTransform
            {
                local_position = new KernelVec3(position.x, position.y, position.z),
                local_rotation = new KernelQuat(
                    rotation.x,
                    rotation.y,
                    rotation.z,
                    rotation.w),
                local_scale = new KernelVec3(scale.x, scale.y, scale.z),
            };
        }

        private static void RequireVectorApproximately(
            Vector3 actual,
            Vector3 expected,
            string message)
        {
            Require(
                (actual - expected).sqrMagnitude <= 1.0e-8f,
                $"{message} expected={expected} actual={actual}");
        }

        private static void RequireQuaternionApproximately(
            Quaternion actual,
            Quaternion expected,
            string message)
        {
            Require(
                Quaternion.Angle(actual, expected) <= 0.001f,
                $"{message} expected={expected} actual={actual}");
        }

        private static void RequireControlPlaneRpc(Kernel kernel, uint enemyNetId)
        {
            string requestJson =
                "{\"jsonrpc\":\"2.0\",\"id\":101,\"method\":\"world.set_entity_health\",\"params\":{\"net_id\":" +
                enemyNetId +
                ",\"hp\":77}}";
            Require(
                kernel.InvokeRpcCommand(requestJson, out ulong requestId) && requestId != 0,
                "Kernel_InvokeRpcCommand failed.");

            kernel.Update(1.0f / 30.0f);
            Require(
                kernel.TryPollRpcResponse(requestId, out string responseJson) &&
                !string.IsNullOrEmpty(responseJson) &&
                responseJson.Contains("\"result\""),
                "Kernel_PollRpcResponse did not return a successful response.");

            Require(
                kernel.ServerGetEntityState(enemyNetId, out KernelServerEntityState state) &&
                state.valid != 0 &&
                state.hp == 77,
                "Control-plane RPC did not update entity health.");
        }

        private static void RequireLANDiscovery()
        {
            const string ServerName = "Editor LAN Discovery Smoke";
            const ushort ServerEndpointPort = 7799;
            ushort discoveryPort = AllocateUdpPort();

            using (var discovery = new LANDiscovery())
            {
                Require(
                    discovery.StartServer(ServerEndpointPort, ServerName, discoveryPort),
                    "LANDiscovery.StartServer failed.");
                Require(
                    discovery.RefreshServerList(discoveryPort, 250),
                    "LANDiscovery.RefreshServerList failed.");

                var servers = new LANDiscoveredServer[4];
                uint serverCount = 0;
                for (int attempt = 0; attempt < 40 && serverCount == 0; ++attempt)
                {
                    Thread.Sleep(25);
                    serverCount = discovery.GetDiscoveredServers(servers);
                }

                Require(serverCount > 0, "LANDiscovery did not return any servers.");
                Require(servers[0].ServerName == ServerName, "LANDiscovery server name mismatch.");
                Require(!string.IsNullOrEmpty(servers[0].Ip), "LANDiscovery server IP was empty.");
                Require(servers[0].Port == ServerEndpointPort, "LANDiscovery server port mismatch.");
                Require(!string.IsNullOrEmpty(servers[0].ModuleVersion), "LANDiscovery module version was empty.");
                Require(servers[0].ProtocolVersion != 0, "LANDiscovery protocol version was empty.");
                Require(servers[0].SnapshotSchemaVersion != 0, "LANDiscovery snapshot schema version was empty.");
                Require(servers[0].PacketSchemaVersion != 0, "LANDiscovery packet schema version was empty.");
                Require(!string.IsNullOrEmpty(servers[0].GitCommit), "LANDiscovery git commit was empty.");
                Require(servers[0].Compatible, "LANDiscovery result was not compatible.");

                discovery.ClearResults();
                Require(discovery.GetDiscoveredServers(servers) == 0, "LANDiscovery.ClearResults failed.");
                discovery.StopServer();
                discovery.StopServer();
            }
        }

        private static ushort AllocateUdpPort()
        {
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                return (ushort)((IPEndPoint)socket.Client.LocalEndPoint).Port;
            }
        }

        private static void RequireExternalGameplayCatalogSyncIfConfigured()
        {
            string address = Environment.GetEnvironmentVariable(
                "NETWORK_KERNEL_SYNC_SMOKE_ADDRESS");
            if (string.IsNullOrWhiteSpace(address))
            {
                Debug.Log(
                    "External gameplay catalog sync smoke skipped: " +
                    "NETWORK_KERNEL_SYNC_SMOKE_ADDRESS is not set.");
                return;
            }

            string cacheDirectory = Path.Combine(
                Path.GetTempPath(),
                "network-example-editor-catalog-sync-" + Guid.NewGuid().ToString("N"));
            try
            {
                RequireCatalogSyncConnection(address, cacheDirectory, false);
                RequireCatalogSyncConnection(address, cacheDirectory, true);
            }
            finally
            {
                try
                {
                    Directory.Delete(cacheDirectory, true);
                }
                catch
                {
                }
            }
        }

        private static void RequireCatalogSyncConnection(
            string address,
            string cacheDirectory,
            bool expectCacheHit)
        {
            using (var client = new NetworkClient())
            {
                Require(
                    client.Start(
                        address,
                        new GameplayCatalogSyncOptions
                        {
                            CacheDirectory = cacheDirectory,
                            MaxBundleBytes =
                                KernelConstants.GameplayCatalogSyncDefaultMaxBundleSize,
                            Timeout = TimeSpan.FromSeconds(10),
                        }),
                    "Catalog sync client startup failed.");

                var clientEvents = new KernelEvent[32];
                for (int attempt = 0;
                     attempt < 5000 &&
                     (!client.IsReady ||
                      client.ConnectionState != NetworkClientConnectionState.Ready);
                     ++attempt)
                {
                    client.Update(1.0f / 120.0f, clientEvents);
                    if (client.ConnectionState == NetworkClientConnectionState.Failed)
                    {
                        throw new InvalidOperationException(
                            $"Catalog sync client failed: {client.CatalogSyncResult.Error} " +
                            $"{client.CatalogSyncResult.ErrorMessage}");
                    }
                    Thread.Sleep(2);
                }

                Require(
                    client.IsReady &&
                    client.ConnectionState == NetworkClientConnectionState.Ready,
                    $"Catalog sync client did not become ready; state={client.ConnectionState}.");
                Require(
                    client.CatalogSyncResult.CacheHit == expectCacheHit,
                    $"Catalog sync cache expectation failed; expected={expectCacheHit} " +
                    $"actual={client.CatalogSyncResult.CacheHit}.");
                Require(
                    client.CatalogSyncResult.LoadResult.status ==
                        KernelConstants.GameplayCatalogLoadStatusSuccess,
                    "Catalog sync client did not load the gameplay catalog.");
            }
        }

        private static byte[] LoadGameplayCatalogBundleBytes()
        {
            TextAsset catalogBundle = Resources.Load<TextAsset>(
                "gameplay_catalog_bundle/bundle");
            if (catalogBundle != null)
            {
                return catalogBundle.bytes;
            }

            UnityEditor.PackageManager.PackageInfo packageInfo =
                UnityEditor.PackageManager.PackageInfo.FindForAssembly(
                    typeof(Kernel).Assembly);
            if (packageInfo != null && !string.IsNullOrEmpty(packageInfo.resolvedPath))
            {
                string path = Path.Combine(
                    packageInfo.resolvedPath,
                    "Runtime",
                    "Resources",
                    "gameplay_catalog_bundle",
                    "bundle.bytes");
                if (File.Exists(path))
                {
                    return File.ReadAllBytes(path);
                }
            }

            throw new InvalidOperationException("Gameplay catalog bundle resource was missing.");
        }

        private static void ConfigureCombatAndWeapons(Kernel kernel, uint enemyNetId)
        {
            KernelCombatStateDefinition combat = KernelCombatStateDefinition.Create();
            combat.hp = 240;
            combat.max_hp = 240;
            combat.active_weapon_slot = 0;
            combat.weapon_slot_count = 4;
            combat.collider_template_id = 101;
            combat.hitbox_center = new KernelVec3(0.0f, 0.8f, 0.0f);
            combat.hitbox_half_extents = new KernelVec3(0.4f, 0.8f, 0.4f);
            combat.weapon_ids[0] = 3;
            combat.ammo[0] = 3;
            combat.reserve_magazines[0] = 6;
            combat.weapon_ids[1] = 4;
            combat.ammo[1] = 1;
            combat.reserve_magazines[1] = 1;
            combat.weapon_ids[2] = 5;
            combat.ammo[2] = 2;
            combat.reserve_magazines[2] = 2;
            combat.weapon_ids[3] = 6;
            combat.ammo[3] = 2;
            combat.reserve_magazines[3] = 2;
            Require(
                kernel.ServerSetEntityCombatState(enemyNetId, combat),
                "Kernel_ServerSetEntityCombatState failed.");

            SetAndQueryWeapon(kernel, enemyNetId, RocketWeapon(), 3);
            SetAndQueryWeapon(kernel, enemyNetId, AreaEffectWeapon(), 4);
            SetAndQueryWeapon(kernel, enemyNetId, BeamWeapon(), 5);
            SetAndQueryWeapon(kernel, enemyNetId, HomingWeapon(), 6);
        }

        private static void RequireAbi26CatalogDiagnostics(Kernel kernel)
        {
            const byte capsuleShapeType = 5;
            const uint movementPurpose = 1U << 4;
            var colliderTemplates = new[]
            {
                new KernelColliderTemplateDefinition
                {
                    struct_size = KernelColliderTemplateDefinition.StructSize,
                    template_id = 101,
                    shape_type = (byte)KernelColliderShapeType.Sphere,
                    center = new KernelVec3(0.0f, 0.5f, 0.0f),
                    shape_params = new KernelVec4(0.75f, 0.0f, 0.0f, 0.0f),
                    purpose_flags = (uint)KernelColliderPurpose.Hit,
                    layer_mask = KernelConstants.CollisionLayerHostile,
                },
                new KernelColliderTemplateDefinition
                {
                    struct_size = KernelColliderTemplateDefinition.StructSize,
                    template_id = 102,
                    shape_type = capsuleShapeType,
                    center = new KernelVec3(0.0f, 0.9f, 0.0f),
                    shape_params = new KernelVec4(0.9f, 0.4f, 0.0f, 0.0f),
                    purpose_flags = movementPurpose,
                    layer_mask = KernelConstants.CollisionLayerHostile,
                },
                new KernelColliderTemplateDefinition
                {
                    struct_size = KernelColliderTemplateDefinition.StructSize,
                    template_id = 303,
                    shape_type = (byte)KernelColliderShapeType.Cone,
                    shape_params = new KernelVec4(12.0f, 90.0f, 0.0f, 0.0f),
                    purpose_flags = (uint)KernelColliderPurpose.Vision,
                    layer_mask = KernelConstants.CollisionLayerAgentVision,
                },
            };
            var projectileTemplates = new[]
            {
                new KernelProjectileTemplateDefinition
                {
                    struct_size = KernelProjectileTemplateDefinition.StructSize,
                    projectile_template_id = 202,
                    weapon_id = 3,
                    mechanics = StandardProjectileMechanics(5, 30.0f, 60),
                },
                new KernelProjectileTemplateDefinition
                {
                    struct_size = KernelProjectileTemplateDefinition.StructSize,
                    projectile_template_id = 204,
                    weapon_id = 4,
                    mechanics = AreaEffectProjectileMechanics(),
                },
                new KernelProjectileTemplateDefinition
                {
                    struct_size = KernelProjectileTemplateDefinition.StructSize,
                    projectile_template_id = 205,
                    weapon_id = 5,
                    mechanics = BeamProjectileMechanics(),
                },
                new KernelProjectileTemplateDefinition
                {
                    struct_size = KernelProjectileTemplateDefinition.StructSize,
                    projectile_template_id = 206,
                    weapon_id = 6,
                    mechanics = HomingProjectileMechanics(),
                },
            };
            var actorTemplates = new[]
            {
                new KernelActorTemplateDefinition
                {
                    struct_size = KernelActorTemplateDefinition.StructSize,
                    actor_template_id = 404,
                    entity_type = KernelEntityType.Actor,
                    actor_type = KernelActorType.Agent,
                    collider_template_id = 101,
                    vision = new KernelAgentVisionConfig
                    {
                        struct_size = KernelAgentVisionConfig.StructSize,
                        camp = (byte)KernelAgentCamp.EnemySide,
                        vision_collider_template_id = 303,
                        max_visible_hostiles = KernelConstants.MaxVisibleHostiles,
                        max_visible_allies = KernelConstants.MaxVisibleAllies,
                        local_forward = new KernelVec3(-1.0f, 0.0f, 0.0f),
                    },
                },
            };
            var entityTemplates = new[]
            {
                new KernelEntityTemplateDefinition
                {
                    struct_size = KernelEntityTemplateDefinition.StructSize,
                    entity_template_id = 505,
                    entity_type = KernelEntityType.Actor,
                    actor_type = KernelActorType.Agent,
                    actor_template_id = 404,
                    collider_template_id = 101,
                    movement = new KernelMovementDefinition
                    {
                        struct_size = KernelMovementDefinition.StructSize,
                        controller_type = KernelMovementControllerType.Character,
                        movement_collider_template_id = 102,
                        gravity = new KernelVec3(0.0f, -9.81f, 0.0f),
                        max_slope_degrees = 45.0f,
                        step_height = 0.3f,
                        ground_probe_distance = 0.1f,
                        ground_snap_distance = 0.1f,
                    },
                },
            };
            var catalog = new KernelGameplayCatalog
            {
                CatalogVersion = 26,
                CatalogHash = 0x2600UL,
                ActorTemplates = actorTemplates,
                EntityTemplates = entityTemplates,
                ProjectileTemplates = projectileTemplates,
                ColliderTemplates = colliderTemplates,
                ActionTemplates = SmokeActionTemplates(),
            };

            Require(kernel.LoadGameplayCatalog(catalog), "Kernel_LoadGameplayCatalog failed.");
            RequireInvalidGameplayCatalogBundleFails(kernel);
            Require(
                kernel.GetColliderTemplates(null) == 3,
                "Kernel_GetColliderTemplates count failed.");
            var readColliders = new KernelColliderTemplateDefinition[3];
            Require(
                kernel.GetColliderTemplates(readColliders) == 3 &&
                readColliders[0].template_id == 101 &&
                readColliders[0].shape_type == (byte)KernelColliderShapeType.Sphere &&
                readColliders[0].shape_params.x == 0.75f &&
                readColliders[1].template_id == 102 &&
                readColliders[1].shape_type == capsuleShapeType &&
                readColliders[1].shape_params.x == 0.9f &&
                readColliders[1].shape_params.y == 0.4f &&
                readColliders[1].purpose_flags == movementPurpose &&
                readColliders[2].template_id == 303 &&
                readColliders[2].shape_type == (byte)KernelColliderShapeType.Cone &&
                readColliders[2].shape_params.x == 12.0f &&
                readColliders[2].shape_params.y == 90.0f,
                "Kernel_GetColliderTemplates read-back failed.");
            Require(
                kernel.GetProjectileTemplates(null) == 4,
                "Kernel_GetProjectileTemplates count failed.");
            var readProjectiles = new KernelProjectileTemplateDefinition[4];
            Require(
                kernel.GetProjectileTemplates(readProjectiles) == 4 &&
                readProjectiles[0].projectile_template_id == 202 &&
                readProjectiles[0].mechanics.collider_template_id == 101 &&
                readProjectiles[1].mechanics.projectile_type ==
                    (byte)KernelProjectileType.AreaEffect &&
                readProjectiles[2].mechanics.projectile_type ==
                    (byte)KernelProjectileType.Beam &&
                readProjectiles[3].mechanics.motion_model ==
                    (byte)KernelProjectileMotionModel.Homing,
                "Kernel_GetProjectileTemplates read-back failed.");
            Require(
                kernel.GetActorTemplates(null) == 1,
                "Kernel_GetActorTemplates count failed.");
            var readActors = new KernelActorTemplateDefinition[1];
            Require(
                kernel.GetActorTemplates(readActors) == 1 &&
                readActors[0].actor_template_id == 404 &&
                readActors[0].entity_type == KernelEntityType.Actor &&
                readActors[0].actor_type == KernelActorType.Agent &&
                readActors[0].collider_template_id == 101 &&
                readActors[0].vision.vision_collider_template_id == 303,
                "Kernel_GetActorTemplates read-back failed.");
            Require(
                kernel.GetColliderBindings(null) == 0,
                "Kernel_GetColliderBindings count was not empty.");
            var readBindings = new KernelColliderBindingDefinition[1];
            Require(
                kernel.GetColliderBindings(readBindings) == 0,
                "Kernel_GetColliderBindings read-back was not empty.");

            Require(
                kernel.TryGetBenchmarkStats(out KernelBenchmarkStats benchmarkStats) &&
                benchmarkStats.catalog_version == 26 &&
                benchmarkStats.catalog_hash == 0x2600UL,
                "Kernel_GetBenchmarkStats failed.");
            Require(
                kernel.TryGetNetworkStats(out KernelNetworkStats networkStats) &&
                networkStats.struct_size == KernelNetworkStats.StructSize &&
                networkStats.replication_metadata_timeout_count == 0 &&
                networkStats.replication_stale_snapshot_drop_count == 0,
                "Kernel_GetNetworkStats failed.");

            var debugFilter = new KernelDebugRecordFilter
            {
                struct_size = KernelDebugRecordFilter.StructSize,
                record_type_mask = (uint)KernelDebugRecordType.Hit |
                    (uint)KernelDebugRecordType.Projectile,
                weapon_id = KernelConstants.DebugWildcardU8,
                motion_model = KernelConstants.DebugWildcardU8,
                sync_mode = KernelConstants.DebugWildcardU8,
            };
            var debugRecords = new KernelDebugInfo[4];
            kernel.PollDebugRecords(debugFilter, debugRecords);
        }

        private static void RequireAbi26VisionDiagnostics(Kernel kernel, uint enemyNetId)
        {
            var visionConfig = new KernelAgentVisionConfig
            {
                camp = (byte)KernelAgentCamp.EnemySide,
                vision_collider_template_id = 303,
                max_visible_hostiles = KernelConstants.MaxVisibleHostiles,
                max_visible_allies = KernelConstants.MaxVisibleAllies,
                local_forward = new KernelVec3(-1.0f, 0.0f, 0.0f),
            };
            Require(
                kernel.ServerSetEntityVisionConfig(enemyNetId, visionConfig),
                "Kernel_ServerSetEntityVisionConfig failed.");
            kernel.Update(1.0f / 30.0f);
            var visionStates = new KernelVisionStateView[2];
            Require(
                kernel.QueryVisionState(null, visionStates) == 1 &&
                visionStates[0].agent_net_id == enemyNetId &&
                visionStates[0].vision_collider_template_id == 303 &&
                visionStates[0].resolved_collider_template_id == 101,
                "Kernel_QueryVisionState failed.");
        }

        private static void RequireQueryAllColliderShape(Kernel kernel, uint enemyNetId)
        {
            var shapes = new KernelColliderShapeView[4];
            uint count = kernel.QueryColliderShapes(null, shapes);
            for (int index = 0; index < Math.Min(shapes.Length, (int)count); ++index)
            {
                if (shapes[index].entity_net_id == enemyNetId &&
                    shapes[index].collider_template_id == 101 &&
                    shapes[index].shape_type == (byte)KernelColliderShapeType.Sphere)
                {
                    return;
                }
            }

            throw new InvalidOperationException(
                "Kernel_QueryColliderShapes query-all missed enemy collider.");
        }

        private static void RequireInvalidGameplayCatalogBundleFails(Kernel kernel)
        {
            byte[] invalidBundle = { 0x6e, 0x6b };
            Require(
                !kernel.LoadGameplayCatalogFromMemory(
                    invalidBundle,
                    "gameplay_catalog.yaml",
                    out KernelGameplayCatalogLoadResult result),
                "Kernel_LoadGameplayCatalogFromMemory unexpectedly accepted invalid bundle bytes.");
            Require(
                result.struct_size == KernelGameplayCatalogLoadResult.StructSize,
                "KernelGameplayCatalogLoadResult struct_size mismatch.");
            Require(
                result.status == KernelConstants.GameplayCatalogLoadStatusFailed,
                "KernelGameplayCatalogLoadResult status mismatch.");
        }

        private static void SetAndQueryWeapon(
            Kernel kernel,
            uint enemyNetId,
            KernelWeaponMechanicsDefinition weapon,
            byte weaponId)
        {
            Require(Kernel.ServerValidateMechanicsConfig(weapon), "Weapon mechanics validation failed.");
            Require(
                kernel.ServerSetEntityWeaponMechanics(enemyNetId, weapon),
                "Kernel_ServerSetEntityWeaponMechanics failed.");
            Require(
                kernel.ServerGetEntityWeaponMechanics(
                    enemyNetId,
                    weaponId,
                    out KernelWeaponMechanicsDefinition queried) &&
                queried.weapon_id == weaponId &&
                queried.fire_mode == weapon.fire_mode,
                "Kernel_ServerGetEntityWeaponMechanics failed.");
        }

        private static KernelProjectileMechanicsDefinition StandardProjectileMechanics(
            ushort damage,
            float speed,
            uint lifetimeTicks)
        {
            return new KernelProjectileMechanicsDefinition
            {
                struct_size = KernelProjectileMechanicsDefinition.StructSize,
                projectile_type = (byte)KernelProjectileType.Standard,
                motion_model = (byte)KernelProjectileMotionModel.Linear,
                hit_response = (byte)KernelProjectileHitResponse.Destroy,
                damage_shape = (byte)KernelProjectileDamageShape.DirectHit,
                sync_mode = (byte)KernelProjectileSyncMode.LocalPredictedDeterministic,
                damage = damage,
                speed = speed,
                lifetime_ticks = lifetimeTicks,
                collider_template_id = 101,
                collision_mask = KernelConstants.CollisionMaskDamageable,
                max_hit_count = 1,
                collision_query_mode = (byte)KernelProjectileCollisionQueryMode.Auto,
            };
        }

        private static KernelProjectileMechanicsDefinition AreaEffectProjectileMechanics()
        {
            KernelProjectileMechanicsDefinition mechanics =
                StandardProjectileMechanics(7, 0.0f, 9);
            mechanics.projectile_type = (byte)KernelProjectileType.AreaEffect;
            mechanics.area_effect = new KernelAreaEffectMechanicsDefinition
            {
                struct_size = KernelAreaEffectMechanicsDefinition.StructSize,
                radius = 2.5f,
                damage_per_interval = 7,
                damage_interval_ticks = 3,
                lifetime_ticks = 9,
                spawn_distance = 1.5f,
                collision_mask = KernelConstants.CollisionMaskDamageable,
            };
            return mechanics;
        }

        private static KernelProjectileMechanicsDefinition BeamProjectileMechanics()
        {
            KernelProjectileMechanicsDefinition mechanics =
                StandardProjectileMechanics(30, 0.0f, 2);
            mechanics.projectile_type = (byte)KernelProjectileType.Beam;
            mechanics.beam = new KernelBeamMechanicsDefinition
            {
                struct_size = KernelBeamMechanicsDefinition.StructSize,
                length = 6.0f,
                radius = 0.25f,
                damage_per_tick = 30,
                lifetime_ticks = 2,
                collision_mask = KernelConstants.CollisionLayerEnemy,
            };
            return mechanics;
        }

        private static KernelProjectileMechanicsDefinition HomingProjectileMechanics()
        {
            KernelProjectileMechanicsDefinition mechanics =
                StandardProjectileMechanics(5, 35.0f, 75);
            mechanics.motion_model = (byte)KernelProjectileMotionModel.Homing;
            mechanics.sync_mode =
                (byte)KernelProjectileSyncMode.HybridDeterministicThenSnapshot;
            mechanics.collision_mask = KernelConstants.CollisionLayerEnemy;
            mechanics.homing = new KernelHomingMechanicsDefinition
            {
                struct_size = KernelHomingMechanicsDefinition.StructSize,
                homing_mode = (byte)KernelHomingMode.FireAndForget,
                sync_mode = (byte)KernelProjectileSyncMode.HybridDeterministicThenSnapshot,
                boost_ticks = 1,
                lock_on_range = 25.0f,
                lose_target_range = 30.0f,
                lock_cone_degrees = 75.0f,
                max_turn_degrees_per_tick = 12.0f,
                acceleration = 20.0f,
                max_speed = 40.0f,
            };
            return mechanics;
        }

        private static KernelWeaponMechanicsDefinition RocketWeapon()
        {
            return ProjectileWeapon(3, 202, 5);
        }

        private static KernelWeaponMechanicsDefinition AreaEffectWeapon()
        {
            return ProjectileWeapon(4, 204, 7);
        }

        private static KernelWeaponMechanicsDefinition BeamWeapon()
        {
            return ProjectileWeapon(5, 205, 30);
        }

        private static KernelWeaponMechanicsDefinition HomingWeapon()
        {
            return ProjectileWeapon(6, 206, 5);
        }

        private static KernelWeaponMechanicsDefinition ProjectileWeapon(
            byte weaponId,
            uint projectileTemplateId,
            ushort damage)
        {
            return new KernelWeaponMechanicsDefinition
            {
                struct_size = KernelWeaponMechanicsDefinition.StructSize,
                weapon_id = weaponId,
                fire_mode = (byte)KernelWeaponFireMode.Projectile,
                magazine_size = 3,
                damage = damage,
                projectile_template_id = projectileTemplateId,
                fire_action_template_id = 1,
                reload_action_template_id = 2,
            };
        }

        private static KernelActionTemplateDefinition[] SmokeActionTemplates()
        {
            return new[]
            {
                new KernelActionTemplateDefinition
                {
                    struct_size = KernelActionTemplateDefinition.StructSize,
                    action_template_id = 1,
                    trigger_mode = KernelActionTriggerMode.Press,
                    flags = KernelActionTemplateFlag.CancelOnDeath |
                        KernelActionTemplateFlag.CancelOnWeaponChange,
                    ammo_cost_per_commit = 1,
                    max_commit_count = 1,
                    recovery_ticks = 1,
                },
                new KernelActionTemplateDefinition
                {
                    struct_size = KernelActionTemplateDefinition.StructSize,
                    action_template_id = 2,
                    trigger_mode = KernelActionTriggerMode.Press,
                    flags = KernelActionTemplateFlag.CancelOnDeath |
                        KernelActionTemplateFlag.CancelOnWeaponChange,
                    max_commit_count = 1,
                    recovery_ticks = 1,
                },
            };
        }

        private static void RequireClientPredictionFailureStateTransition()
        {
            Type clientType = typeof(NetworkClient);
            FieldInfo localPeerId = clientType.GetField(
                "localPeerId",
                BindingFlags.Instance | BindingFlags.NonPublic);
            FieldInfo localPlayerNetId = clientType.GetField(
                "localPlayerNetId",
                BindingFlags.Instance | BindingFlags.NonPublic);
            MethodInfo applyEvents = clientType.GetMethod(
                "ApplyEvents",
                BindingFlags.Instance | BindingFlags.NonPublic);
            Require(
                localPeerId != null &&
                localPlayerNetId != null &&
                applyEvents != null,
                "NetworkClient prediction failure smoke could not access managed state.");

            using (var client = new NetworkClient())
            {
                localPeerId.SetValue(client, 7U);
                localPlayerNetId.SetValue(client, 100U);
                var events = new[]
                {
                    new KernelEvent
                    {
                        type = KernelEventType.Error,
                        code = 31,
                    },
                };
                applyEvents.Invoke(client, new object[] { events, 1U });

                Require(
                    client.ConnectionState == NetworkClientConnectionState.Failed,
                    "Prediction failure did not mark NetworkClient failed.");
                Require(!client.IsReady, "Prediction failure left NetworkClient ready.");
                Require(
                    !client.IsDisconnected,
                    "Prediction failure was misreported as a transport disconnect.");
                Require(
                    client.LocalPeerId == 0 && client.LocalPlayerNetId == 0,
                    "Prediction failure did not clear managed local-player identifiers.");
                Require(
                    !client.TrySubmitInput(new KernelPlayerInput()),
                    "Prediction failure did not stop managed input submission.");
            }
        }

        private static void RequireProjectileTemplateFire(
            Kernel kernel,
            uint enemyNetId,
            byte weaponId)
        {
            FireAndFindSpawn(kernel, enemyNetId, weaponId, KernelEntityType.Projectile);
        }

        private static void RequireHomingFire(Kernel kernel, uint enemyNetId)
        {
            uint projectileNetId = FireAndFindSpawn(kernel, enemyNetId, 6, KernelEntityType.Projectile);
            Require(
                kernel.ServerGetHomingState(projectileNetId, out KernelHomingState state) &&
                state.valid != 0 &&
                state.shooter_net_id == enemyNetId &&
                state.lock_on_range == 25.0f,
                "Kernel_ServerGetHomingState failed.");
        }

        private static uint FireAndFindSpawn(
            Kernel kernel,
            uint enemyNetId,
            byte selectedWeapon,
            KernelEntityType entityType)
        {
            var input = new KernelPlayerInput
            {
                selected_weapon = selectedWeapon,
                aim_dir = new KernelVec3(1.0f, 0.0f, 0.0f),
                action_intent = new KernelActionIntent
                {
                    action_instance_id = nextActionInstanceId++,
                    binding_id = KernelActionBinding.PrimaryFire,
                },
            };
            Require(
                kernel.ServerSubmitEntityInput(enemyNetId, input),
                "Kernel_ServerSubmitEntityInput failed.");
            kernel.Update(1.0f / 30.0f);
            var events = new KernelEvent[32];
            uint eventCount = kernel.PollEvents(events);
            for (int index = 0; index < Math.Min(events.Length, (int)eventCount); ++index)
            {
                if (events[index].type == KernelEventType.EntitySpawned &&
                    events[index].code == (uint)entityType)
                {
                    return events[index].net_id;
                }
            }

            throw new InvalidOperationException("Expected spawned entity was not observed.");
        }

        private static bool HasActiveRenderEntity(
            RenderEntityState[] states,
            uint netId)
        {
            for (int index = 0; index < states.Length; ++index)
            {
                if (states[index].net_id == netId)
                {
                    return states[index].status == RenderEntityStatus.Active;
                }
            }

            return false;
        }

        private static string DescribeRenderStates(RenderEntityState[] states)
        {
            string description = "states=";
            for (int index = 0; index < states.Length; ++index)
            {
                if (states[index].net_id == 0)
                {
                    continue;
                }

                description +=
                    $"[{index}] net={states[index].net_id} hp={states[index].hp}/" +
                    $"{states[index].max_hp} status={states[index].status} " +
                    $"flags=0x{states[index].visual_flags:x8} " +
                    $"collider={states[index].collider_template_id};";
            }

            return description;
        }

        private static bool HasRenderEntityHpUnknown(
            RenderEntityState[] states,
            uint netId)
        {
            for (int index = 0; index < states.Length; ++index)
            {
                if (states[index].net_id == netId)
                {
                    return states[index].hp == 0 &&
                        states[index].max_hp == 0 &&
                        (states[index].visual_flags & KernelConstants.VisualFlagHpUnknown) != 0;
                }
            }

            return false;
        }

        private static void RequireDestroyedLifecycleEvent(
            Kernel kernel,
            uint netId)
        {
            var events = new KernelEntityLifecycleEvent[32];
            uint eventCount = kernel.PollEntityLifecycleEvents(events);
            for (int index = 0; index < Math.Min(events.Length, (int)eventCount); ++index)
            {
                if (events[index].net_id == netId &&
                    events[index].type == KernelEntityLifecycleEventType.Destroyed &&
                    events[index].reason == KernelDespawnReason.Destroyed)
                {
                    return;
                }
            }

            throw new InvalidOperationException(
                $"Kernel_PollEntityLifecycleEvents missed destroyed entity. {DescribeLifecycleEvents(events, eventCount)}");
        }

        private static string DescribeLifecycleEvents(
            KernelEntityLifecycleEvent[] events,
            uint eventCount)
        {
            string description = $"count={eventCount} events=";
            for (int index = 0; index < Math.Min(events.Length, (int)eventCount); ++index)
            {
                description +=
                    $"[{index}] net={events[index].net_id} type={events[index].type} " +
                    $"reason={events[index].reason} entity={events[index].entity_type};";
            }

            return description;
        }

        private static void Require(bool condition, string message)
        {
            if (!condition)
            {
                throw new InvalidOperationException(message);
            }
        }
    }
}
