# Network Kernel Development History

This document records the major features delivered since the repository was
created. It is a chronological development history, not a list of planned
milestones.

- **Covered period:** 2026-05-05 to 2026-07-23
- **Source of truth:** Git history on the current branch
- **Selection rule:** Functional, architecture, platform, and integration
  milestones are included. Routine fixes and small tuning changes are omitted
  unless they completed or materially changed a capability.

## Major Feature Timeline

| Date | Major update | Delivered features | Representative commits |
|---|---|---|---|
| 2026-05-05 | Repository and simulation foundation | Initialized the C++20/Bazel workspace; added the headless fixed-tick simulation and unified dedicated-server/listen-host runtime modes. | `0098354`, `d31c70c` |
| 2026-05-06 | Real networking and authoritative combat | Added GameNetworkingSockets transport, client/server packet flow, the shared-library target, and the first server-authoritative combat loop. | `c78e0f9`, `267d1e8`, `fa12939` |
| 2026-05-07 | Stable engine boundary and latency handling | Completed the public C ABI and added lag compensation, client prediction/reconciliation, and render smoothing. | `ad1ed1b`, `aa9c296` |
| 2026-05-09 | Game-server API and scalable replication | Established the server gameplay API boundary; added entity lifecycle and state mutation APIs, reliable/persistent state replication, and per-client area-of-interest relevance. | `8653f01`, `cb1e4b2`, `ea17cdc`, `d80db4b`, `a1b48a2` |
| 2026-05-10 | Game Server v1 and Unity bridge | Delivered the first integrated game-server runtime and a C ABI bridge for Unity-side server integration. | `0beb916`, `e1a1d24` |
| 2026-05-12 – 2026-05-13 | Projectile synchronization and presentation | Expanded the ABI with projectile synchronization metadata and improved projectile prediction, latency behavior, correction, and presentation. | `b7fc25b`, `365711c` |
| 2026-05-16 | Server-authoritative defensive actions and damage | Added server-originated pending damage, dodge/parry input, compensated player-fired projectiles, replay, and render-timeline-aligned damage/event presentation. | `f848a0e`, `5ce3c80` |
| 2026-05-18 | Clock synchronization and interpolation | Added client/server clock conversion, synchronization-policy verification, and time-based render-state interpolation. | `ed84aaa`, `6d095fd` |
| 2026-05-20 | Deterministic predicted projectiles | Implemented deterministic projectile simulation, local snapshot fast-forward, late-snapshot anti-jitter handling, and historical server-side projectile hit detection. | `96a3ed4`, `09c1bc8` |
| 2026-05-22 – 2026-05-23 | Hybrid AI tree runtime | Introduced the hybrid AI tree framework, YAML-defined behavior data, runtime execution, enemy AI integration, flee behavior, and entity health access. | `b6bfdd3`, `98fd2cd`, `7b5e71b`, `197e1ac`, `b699f10` |
| 2026-05-24 – 2026-05-26 | Authoritative weapon and collision foundation | Reworked engine/game-server responsibilities around an authoritative weapon core; added weapon templates, area effects, beam weapons, homing, collision queries, damage processing, and projectile interactions. | `1f927c0`, `e778d60`, `9708032`, `b126fc9`, `56bd832`, `9b1bd43`, `7bccaff`, `0d592a4` |
| 2026-05-27 – 2026-06-01 | Cross-platform native packaging | Added cross-platform OpenSSL/zlib wrappers, MinGW support, a Windows `network_kernel.dll` target, binary stripping, and macOS code signing. | `523be3a`, `e177eec`, `f0bb3b5`, `d6d4a35` |
| 2026-06-02 | Runtime discovery and diagnostics | Added version diagnostics and the native LAN discovery module. | `f0c6c7f`, `83d7058` |
| 2026-06-07 – 2026-06-08 | Gameplay catalog and sync benchmarking | Expanded weapon/projectile synchronization, collider and gameplay catalog metadata, runtime statistics, benchmark APIs, and a projectile synchronization benchmark. | `9411f67`, `1838a46`, `0a096dd` |
| 2026-06-09 – 2026-06-14 | Data-driven templates and snapshot budgets | Added configuration-bundle loading, actor templates, data-driven projectile policies and spawn responses, plus budgeted/refined snapshot sections and overflow diagnostics. | `0da4b74`, `00a1ac0`, `b73828d`, `d287703`, `6af6ecb`, `76ff0f4` |
| 2026-06-19 – 2026-06-20 | Vision system and actor unification | Implemented native vision and vision-collider templates, unified actor modeling, replicated vision debug data and actor templates, and enforced reliable template-metadata synchronization. | `08156c5`, `68d835c`, `71db974`, `213faaf`, `31cc6fe`, `911b248` |
| 2026-06-23 – 2026-06-27 | Kernel modularization and control plane | Split the kernel into systems; added monitored command queues, game-server command routing, server-synchronized gameplay catalogs, JSON-RPC 2.0 control-plane APIs, and entity health queries. | `c5fee71`, `b69d678`, `c685390`, `6f120c5`, `c7326db`, `043df34` |
| 2026-07-01 | Generalized projectile collision shapes | Replaced projectile-specific collision assumptions with generalized collision-shape data and tick-parameter conversion. | `440c862`, `c6c6080` |
| 2026-07-05 – 2026-07-07 | ECS AI framework and director | Delivered the refactored AI framework MVP, ECS AI runtime, director/world-rule execution, generic hostile semantics, and YAML-driven sentry configuration. | `665cef0`, `c1894de`, `927589e`, `f1b61c4`, `cce6c4b` |
| 2026-07-09 | Magazine-based ammunition | Replaced the flat reserve-ammo model with reserve magazines and added reload behavior coverage. | `9417858` |
| 2026-07-10 | Bundle-first dedicated server | Made gameplay bundles the authoritative dedicated-server startup path and documented the gameplay synchronization policy. | `913e283` |
| 2026-07-12 | Data-driven collider synchronization | Added native data-driven collider-template synchronization across the server/client boundary. | `fa66f2d` |
| 2026-07-13 – 2026-07-14 | Generalized action system | Added action-template data contracts and queries, compact action timeline synchronization, phase-derived animation hints, owner correction, remote presentation, cadence enforcement, and a generalized action runtime. | `aeb3f61`, `52b194e`, `59cb5ba`, `ae5bfe3`, `3e50541`, `765526d` |
| 2026-07-15 | Production physics and navigation stack | Integrated Jolt Physics and Recast Navigation, added server-side mesh asset cooking, and migrated runtime collision to the new stack. | `2180127`, `8b8e4b7`, `d09e4c1` |
| 2026-07-16 | Character physics and local prediction | Added grounding, kinematic bodies, Jolt `CharacterVirtual`, actor-blocking filters, and local character prediction. | `6530bcc`, `f093c85` |
| 2026-07-17 | Static worlds and deterministic projectile collision | Added gameplay-catalog-controlled static collision loading, ABI v43 load options, fixed-tick input guards, local predicted deterministic projectile collision, terrain grounding, and the grenade launcher. | `3556738`, `a1b176a`, `407e0c1`, `929eada`, `bab7a8c` |
| 2026-07-18 – 2026-07-23 | Session and presentation resilience | Preserved client sessions across relevance tombstones, corrected projectile expiry semantics, made predicted render smoothing time-based, separated the local presentation cursor, and reliably merged local actor metadata into predicted render states. | `118b5c3`, `4dc8d9f`, `24def2e`, `8f86e39`, `0515cee` |

## Current State

As of 2026-07-23, the repository provides a server-authoritative multiplayer
kernel with dedicated-server and listen-host modes, real network transport,
prediction and interpolation, data-driven gameplay catalogs, authoritative
weapons and projectiles, ECS/director AI, Jolt-based character and world
collision, native control-plane APIs, and Unity-facing native plugin packaging.
