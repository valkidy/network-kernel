using System;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using UnityEditor;
using UnityEngine;
using NetworkExample.Kernel.Client;
using NetworkExample.Kernel.Host;

namespace NetworkExample.Kernel.Editor
{
    public static class NetworkKernelAbiSmokeRunner
    {
        [MenuItem("Network Example/Run Kernel ABI Smoke")]
        public static void Run()
        {
            KernelAbi.ValidateNativeAbi();
            GameServerAbi.ValidateNativeAbi();
            RequireLANDiscovery();

            using (var kernel = new Kernel(KernelConfig.CreateDefault(KernelMode.ListenServer)))
            {
                Require(kernel.StartListenServer(7777), "Kernel_StartListenServer failed.");

                using (var gameServer = new GameServer(kernel))
                {
                    Require(
                        gameServer.QueryWeaponTemplate(
                            4,
                            out GameServerWeaponTemplateInfo templateInfo),
                        "GameServer_QueryWeaponTemplate failed.");
                    Require(templateInfo.valid, "GameServer weapon template was not valid.");
                    Require(
                        templateInfo.mechanics.fire_mode ==
                            (byte)KernelWeaponFireMode.AreaEffect,
                        "GameServer weapon template did not expose area-effect mechanics.");
                }

                Require(
                    kernel.TryGetLocalPlayerInfo(out KernelLocalPlayerInfo localInfo) &&
                    localInfo.player_net_id != 0 &&
                    kernel.LocalPlayerNetId == localInfo.player_net_id &&
                    kernel.IsClientReady,
                    "Kernel_GetLocalPlayerInfo returned no local player.");

                var createInfo = new KernelServerEntityCreateInfo
                {
                    entity_type = KernelEntityType.Enemy,
                    position = new KernelVec3(6.0f, 0.0f, 0.0f),
                    rotation = new KernelQuat(0.0f, 0.0f, 0.0f, 1.0f),
                    animation_state = 4,
                    visual_flags = 8,
                };
                Require(
                    kernel.ServerCreateEntity(createInfo, out uint enemyNetId) &&
                    enemyNetId != 0,
                    "Kernel_ServerCreateEntity failed.");

                RequireAbi15Diagnostics(kernel, enemyNetId);
                ConfigureCombatAndWeapons(kernel, enemyNetId);
                Require(
                    kernel.ServerGetEntityState(
                        enemyNetId,
                        out KernelServerEntityState enemyState) &&
                    enemyState.valid &&
                    enemyState.hp == 240 &&
                    enemyState.max_hp == 240,
                    "Kernel_ServerSetEntityCombatState did not update enemy health.");

                var states = new RenderEntityState[16];
                kernel.Update(0.0f);
                Require(kernel.GetRenderStates(states) > 0, "Kernel_GetRenderStates returned no states.");
                Require(
                    HasRenderEntityMaxHealth(states, enemyNetId, 240),
                    "Kernel_GetRenderStates missed enemy health.");
                Require(
                    kernel.GetRenderStatesAtTime(33333, states) > 0,
                    "Kernel_GetRenderStatesAtTime returned no states.");

                RequireAreaEffectFire(kernel, enemyNetId);
                RequireBeamFire(kernel, enemyNetId);
                RequireHomingFire(kernel, enemyNetId);

                Require(
                    kernel.ServerDestroyEntity(enemyNetId, KernelDespawnReason.Destroyed),
                    "Kernel_ServerDestroyEntity failed.");
            }

            using (var host = new NetworkHost())
            {
                Require(host.Start(7778), "NetworkHost.Start failed.");
                var hostEvents = new KernelEvent[16];
                host.Update(1.0f / 30.0f, hostEvents);
                Require(
                    host.IsLocalClientReady &&
                    host.LocalPlayerNetId != 0 &&
                    host.EnemyCount == 1,
                    "NetworkHost smoke failed.");
                Require(
                    host.GameServer.QueryWeaponTemplate(
                        6,
                        out GameServerWeaponTemplateInfo homingTemplate) &&
                    homingTemplate.valid &&
                    homingTemplate.mechanics.projectile.motion_model ==
                        (byte)KernelProjectileMotionModel.Homing,
                    "NetworkHost GameServer homing template query failed.");
            }

            Debug.Log("Network kernel ABI 15 smoke passed.");
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

        private static void ConfigureCombatAndWeapons(Kernel kernel, uint enemyNetId)
        {
            KernelCombatStateDefinition combat = KernelCombatStateDefinition.Create();
            combat.hp = 240;
            combat.max_hp = 240;
            combat.active_weapon_id = 3;
            combat.hitbox_center = new KernelVec3(0.0f, 0.8f, 0.0f);
            combat.hitbox_half_extents = new KernelVec3(0.4f, 0.8f, 0.4f);
            combat.ammo[3] = 3;
            combat.reserve_ammo[3] = 6;
            combat.ammo[4] = 1;
            combat.reserve_ammo[4] = 1;
            combat.ammo[5] = 2;
            combat.reserve_ammo[5] = 2;
            combat.ammo[6] = 2;
            combat.reserve_ammo[6] = 2;
            Require(
                kernel.ServerSetEntityCombatState(enemyNetId, combat),
                "Kernel_ServerSetEntityCombatState failed.");

            SetAndQueryWeapon(kernel, enemyNetId, RocketWeapon(), 3);
            SetAndQueryWeapon(kernel, enemyNetId, AreaEffectWeapon(), 4);
            SetAndQueryWeapon(kernel, enemyNetId, BeamWeapon(), 5);
            SetAndQueryWeapon(kernel, enemyNetId, HomingWeapon(), 6);
        }

        private static void RequireAbi15Diagnostics(Kernel kernel, uint enemyNetId)
        {
            var colliderTemplates = new[]
            {
                new KernelColliderTemplateDefinition
                {
                    struct_size = KernelColliderTemplateDefinition.StructSize,
                    template_id = 101,
                    shape_type = (byte)KernelColliderShapeType.Sphere,
                    center = new KernelVec3(0.0f, 0.5f, 0.0f),
                    radius = 0.75f,
                    purpose_flags = (uint)KernelColliderPurpose.Hit,
                    layer_mask = KernelConstants.CollisionLayerEnemy,
                },
            };
            var colliderBindings = new[]
            {
                new KernelColliderBindingDefinition
                {
                    struct_size = KernelColliderBindingDefinition.StructSize,
                    entity_type = (ushort)KernelEntityType.Enemy,
                    collider_template_id = 101,
                    local_position = new KernelVec3(0.0f, 0.1f, 0.0f),
                    local_rotation = new KernelQuat(0.0f, 0.0f, 0.0f, 1.0f),
                },
            };
            var catalog = new KernelGameplayCatalog
            {
                CatalogVersion = 15,
                CatalogHash = 0x1500UL,
                ColliderTemplates = colliderTemplates,
                ColliderBindings = colliderBindings,
            };

            Require(kernel.LoadGameplayCatalog(catalog), "Kernel_LoadGameplayCatalog failed.");

            var shapeQuery = new KernelColliderShapeQuery
            {
                struct_size = KernelColliderShapeQuery.StructSize,
                entity_net_id = enemyNetId,
                purpose_mask = (uint)KernelColliderPurpose.Hit,
            };
            var shapes = new KernelColliderShapeView[4];
            Require(
                kernel.QueryColliderShapes(shapeQuery, shapes) == 1 &&
                shapes[0].entity_net_id == enemyNetId &&
                shapes[0].collider_template_id == 101 &&
                shapes[0].shape_type == (byte)KernelColliderShapeType.Sphere,
                "Kernel_QueryColliderShapes failed.");

            Require(
                kernel.TryGetBenchmarkStats(out KernelBenchmarkStats benchmarkStats) &&
                benchmarkStats.catalog_version == 15 &&
                benchmarkStats.catalog_hash == 0x1500UL,
                "Kernel_GetBenchmarkStats failed.");
            Require(
                kernel.TryGetNetworkStats(out KernelNetworkStats networkStats) &&
                networkStats.struct_size == KernelNetworkStats.StructSize,
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

        private static KernelWeaponMechanicsDefinition RocketWeapon()
        {
            return new KernelWeaponMechanicsDefinition
            {
                struct_size = KernelWeaponMechanicsDefinition.StructSize,
                weapon_id = 3,
                fire_mode = (byte)KernelWeaponFireMode.Projectile,
                magazine_size = 3,
                damage = 5,
                cooldown_ticks = 30,
                reload_ticks = 30,
                projectile = new KernelProjectileMechanicsDefinition
                {
                    struct_size = KernelProjectileMechanicsDefinition.StructSize,
                    motion_model = (byte)KernelProjectileMotionModel.Linear,
                    hit_response = (byte)KernelProjectileHitResponse.Destroy,
                    damage_shape = (byte)KernelProjectileDamageShape.Explosion,
                    speed = 35.0f,
                    lifetime_seconds = 2.5f,
                    explosion_radius = 3.0f,
                    collision_mask = KernelConstants.CollisionMaskDamageable,
                    max_hit_count = 1,
                },
            };
        }

        private static KernelWeaponMechanicsDefinition AreaEffectWeapon()
        {
            return new KernelWeaponMechanicsDefinition
            {
                struct_size = KernelWeaponMechanicsDefinition.StructSize,
                weapon_id = 4,
                fire_mode = (byte)KernelWeaponFireMode.AreaEffect,
                magazine_size = 2,
                damage = 7,
                cooldown_ticks = 10,
                reload_ticks = 30,
                area_effect = new KernelAreaEffectMechanicsDefinition
                {
                    struct_size = KernelAreaEffectMechanicsDefinition.StructSize,
                    radius = 2.5f,
                    damage_per_interval = 7,
                    damage_interval_ticks = 3,
                    lifetime_ticks = 9,
                    spawn_distance = 1.5f,
                    collision_mask = KernelConstants.CollisionMaskDamageable,
                },
            };
        }

        private static KernelWeaponMechanicsDefinition BeamWeapon()
        {
            return new KernelWeaponMechanicsDefinition
            {
                struct_size = KernelWeaponMechanicsDefinition.StructSize,
                weapon_id = 5,
                fire_mode = (byte)KernelWeaponFireMode.Beam,
                magazine_size = 2,
                damage = 30,
                cooldown_ticks = 1,
                reload_ticks = 30,
                beam = new KernelBeamMechanicsDefinition
                {
                    struct_size = KernelBeamMechanicsDefinition.StructSize,
                    length = 6.0f,
                    radius = 0.25f,
                    damage_per_second = 30,
                    lifetime_ticks = 2,
                    collision_mask = KernelConstants.CollisionLayerEnemy,
                },
            };
        }

        private static KernelWeaponMechanicsDefinition HomingWeapon()
        {
            KernelWeaponMechanicsDefinition weapon = RocketWeapon();
            weapon.weapon_id = 6;
            weapon.projectile.motion_model = (byte)KernelProjectileMotionModel.Homing;
            weapon.projectile.damage_shape = (byte)KernelProjectileDamageShape.DirectHit;
            weapon.projectile.explosion_radius = 0.0f;
            weapon.projectile.collision_mask = KernelConstants.CollisionLayerEnemy;
            weapon.projectile.homing = new KernelHomingMechanicsDefinition
            {
                struct_size = KernelHomingMechanicsDefinition.StructSize,
                homing_mode = (byte)KernelHomingMode.FireAndForget,
                sync_mode = (byte)KernelProjectileSyncMode.HybridDeterministicThenSnapshot,
                boost_ticks = 1,
                lock_on_range = 25.0f,
                lose_target_range = 30.0f,
                lock_cone_degrees = 75.0f,
                max_turn_rate_degrees_per_second = 360.0f,
                acceleration = 20.0f,
                max_speed = 40.0f,
            };
            return weapon;
        }

        private static void RequireAreaEffectFire(Kernel kernel, uint enemyNetId)
        {
            uint areaNetId = FireAndFindSpawn(kernel, enemyNetId, 4, KernelEntityType.AreaEffect);
            Require(
                kernel.ServerGetAreaEffectState(areaNetId, out KernelAreaEffectState state) &&
                state.valid &&
                state.radius == 2.5f &&
                state.damage_interval_ticks == 3,
                "Kernel_ServerGetAreaEffectState failed.");
        }

        private static void RequireBeamFire(Kernel kernel, uint enemyNetId)
        {
            uint beamNetId = FireAndFindSpawn(kernel, enemyNetId, 5, KernelEntityType.Beam);
            Require(
                kernel.ServerGetBeamState(beamNetId, out KernelBeamState state) &&
                state.valid &&
                state.shooter_net_id == enemyNetId &&
                state.length == 6.0f,
                "Kernel_ServerGetBeamState failed.");
        }

        private static void RequireHomingFire(Kernel kernel, uint enemyNetId)
        {
            uint projectileNetId = FireAndFindSpawn(kernel, enemyNetId, 6, KernelEntityType.Projectile);
            Require(
                kernel.ServerGetHomingState(projectileNetId, out KernelHomingState state) &&
                state.valid &&
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
            var input = new PlayerInput
            {
                buttons = (uint)InputButton.Fire,
                selected_weapon = selectedWeapon,
                aim_dir = new KernelVec3(1.0f, 0.0f, 0.0f),
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

        private static bool HasRenderEntityMaxHealth(
            RenderEntityState[] states,
            uint netId,
            ushort maxHp)
        {
            for (int index = 0; index < states.Length; ++index)
            {
                if (states[index].net_id == netId)
                {
                    return states[index].max_hp == maxHp && states[index].hp <= maxHp;
                }
            }

            return false;
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
