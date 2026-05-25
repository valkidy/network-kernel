using System;
using NetworkExample.Kernel;
using NetworkExample.Kernel.Host;

public static class NetworkKernelManagedAbiSmoke
{
    public static int Main()
    {
        try
        {
            Run();
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }

    private static void Run()
    {
        KernelAbi.ValidateNativeAbi();
        GameServerAbi.ValidateNativeAbi();
        KernelAbiInfo info = KernelAbi.GetInfo();
        GameServerAbiInfo gameServerInfo = GameServerAbi.GetInfo();

        using (var kernel = new Kernel(KernelConfig.CreateDefault(KernelMode.ListenServer)))
        {
            Require(kernel.StartListenServer(7777), "Kernel_StartListenServer failed.");

            using (var gameServer = new GameServer(kernel))
            {
                Require(
                    gameServer.QueryWeaponTemplate(4, out GameServerWeaponTemplateInfo templateInfo),
                    "GameServer_QueryWeaponTemplate failed.");
                Require(templateInfo.valid, "GameServer weapon template was not valid.");
                Require(
                    templateInfo.mechanics.fire_mode == (byte)KernelWeaponFireMode.AreaEffect,
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
                kernel.ServerCreateEntity(createInfo, out uint enemyNetId) && enemyNetId != 0,
                "Kernel_ServerCreateEntity failed.");

            ConfigureCombatAndWeapons(kernel, enemyNetId);
            Require(
                kernel.ServerGetEntityState(enemyNetId, out KernelServerEntityState enemyState) &&
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
                host.GameServer.QueryWeaponTemplate(6, out GameServerWeaponTemplateInfo homingTemplate) &&
                homingTemplate.valid &&
                homingTemplate.mechanics.projectile.motion_model ==
                    (byte)KernelProjectileMotionModel.Homing,
                "NetworkHost GameServer homing template query failed.");
        }

        Console.WriteLine(
            "Network kernel managed ABI smoke passed: kernel_version={0} kernel_capabilities=0x{1:x16} game_server_version={2} game_server_capabilities=0x{3:x16}",
            info.abi_version,
            info.capability_flags,
            gameServerInfo.abi_version,
            gameServerInfo.capability_flags);
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
