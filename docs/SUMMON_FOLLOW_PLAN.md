# 召喚友軍巨人與 follow 行為 實作計劃書

基準：main 10d32e16。相關：`docs/AI_BEHAVIOR_PLAN.md`（behavior class、`set_behavior`、guard anchor、
友軍巨人 entity 40）。舊對話「玩家寵物 AI agent」（2026-08-30）只做到分析，沒有實作；本計劃沿用它的部分
結論，其餘依現在的架構重新決定（§8）。

---

## 1. 目標與範圍

- **召喚道具**：玩家丟出一個道具，落地後生出 `allied_gingerbread_giant`。
- **巨人知道是誰召喚它**：生出來的巨人能找到召喚它的玩家。
- **預設行為**：(a) 攻擊周圍的敵對陣營；(b) 跟隨召喚者（新的 `follow` 行為）。

不在範圍內：追擊避障（另案）、Unity 端的道具 / prop view 與 C# mirror（使用者處理）。

---

## 2. 現況（main 10d32e16）

| 項目 | 現況 | 位置 |
|---|---|---|
| 丟道具生 entity | 已有範例：道具 3005 丟出 prop 206，`on_collision` 用 `action_spawn_entity_and_damage_self` 生出 `chaser_grunt` 後自毀 | `entity_templates/206_prop_test_bottle_spawn_enemy.yaml` |
| spawn 的位置 | `spawn_placement: clear` 會在附近地面找一個 hit volume 不撞地形與 static obstacle 的位置 | `kernel_types.h` `KERNEL_SPAWN_PLACEMENT_CLEAR` |
| 碰撞事件的 instigator | 丟出的 prop 碰撞時 `event.instigator` 刻意為 0（爆炸才打得到丟的人）；載入 catalog 時就拒絕在碰撞事件用 `event.instigator` | `action_graph.cc:181`、`systems.cc` W6 註解 |
| 丟的人記在哪 | `ThrownPropMotion.thrower_net_id`；碰撞時 graph 的 provenance `owner_peer` = 丟的人的 peer | `systems.cc`（W6） |
| spawn 的 owner_peer | 從 `owner` 參數那個 entity 複製 `owner_peer`；道具 prop 的 `owner_peer` 是 0，所以生出來的也是 0 | `systems.cc:1714` |
| game_server 看得到 owner | `KernelServerEntityState.owner_peer`，每 tick 的 actor snapshot 已有 | `kernel_types.h` |
| 友軍巨人 | entity 40，camp / layer / 揮擊 mask 都在玩家這一側，`default_behavior: guard` | `entity_templates/40_allied_gingerbread_giant.yaml` |
| 行為系統 | behavior class + `set_behavior` + guard anchor（navmesh 路徑走回崗位） | `agent_behavior.{h,cc}`、`agent_runtime_manager.cc` |
| 速度 | 玩家 5 m/s；巨人 2.5 m/s、轉向 45°/s | `1_player.yaml`、`39/40_*giant.yaml` |

---

## 3. 已決事項

| # | 決定 | 理由 |
|---|---|---|
| S1 | **召喚者的找法採 D**：`spawn_entity` 的 owner 沒有 peer 時，改用 graph provenance 的 `owner_peer` | 保留「丟出去、落地才出現」；只改 spawn 一處，比「所有丟出的 prop 都帶 owner」影響小 |
| S2 | **follow 做成 behavior class**（`FollowBehavior`），不另寫 controller | 跟隨中要照常攻擊敵人（目標 (a)），共用 chaser 的狀態機；舊對話的獨立 `AgentPetController` 要重做這些 |
| S3 | **主人位置從 actor snapshot 讀**，不靠 vision | 同陣營的主人不會是 vision 的目標（舊對話的結論，仍成立） |
| S4 | **主人 = `owner_peer` 對應的玩家**，在 manager 同步 actor 時反查 | snapshot 已有 `owner_peer`，不需要額外查詢（舊對話的結論，仍成立） |

---

## 4. engine 改動與 `owner_peer` 稽核

### 4.1 改動

- **E1 spawn 繼承 peer**：`systems.cc:1714` 改為 owner entity 的 `owner_peer` 為 0 時，用
  `spawn.provenance.owner_peer`。套用範圍見待決 Q1。
- **E2 防禦動作只配對玩家**：`DamagePipeline` 的 dodge / parry 依「目標的 `owner_peer`」配對，而且套用在
  **所有**傷害上（`damage_pipeline.cc:66、213、219、314、326`）。召喚的巨人帶了玩家的 peer 後，會共享玩家的
  閃避與格擋窗口。改為只有玩家 entity 才帶 `target_peer`（其他為 0）。

兩者都不改 ABI、snapshot 或 packet schema。

### 4.2 agent 帶玩家 `owner_peer` 的影響（逐一確認過）

| 位置 | 影響 | 處理 |
|---|---|---|
| `action_system.cc:18` `input_entity`、`weapon_system.cc:899`、`player_movement.cc:469` | 依 peer 找 entity 時只看 `PlayerTag` | 無影響 |
| `damage_pipeline.cc` dodge / parry | 巨人共享玩家的閃避格擋 | E2 修正 |
| `kernel.cc:7771` client lock-on 候選、`lock_strike.cc:61` | 略過射手自己 peer 的 entity | 玩家鎖不到自己的召喚物：正是想要的。**但若 E1 也套用在敵人身上**（測試瓶 3005），玩家會鎖不到那隻敵人（見 Q1） |
| `kernel.cc:14421` legged locomotion | 這 tick 沒有指名給它的 input 時，退而用 peer 相同的 input | AI 每 tick 都會送 input，理論上不會走到；巨人是 legged，要有測試蓋到 |
| `kernel.cc:10258、17622`、`15803` | 只針對 projectile / 丟出的 prop | 無影響 |

---

## 5. 召喚道具（catalog）

- **prop**：新的瓶子 prop，`on_collision` → `action_spawn_entity_and_damage_self`，
  `template: allied_gingerbread_giant`、`owner: self`（E1 讓它繼承丟的人的 peer）、`spawn_placement: clear`。
- **item**：throwable，參考 3005 的寫法。
- 整批驗證：spawn 前會先驗證整批動作，巨人 template 只要有一項不合法就整批放棄（巨人沒出來、瓶子也沒自毀，
  卡在地上）。要有測試蓋到。
- id 依當時 catalog 的空號決定，並和其他分支對號碼。

---

## 6. follow 行為

- **角色**：新增 `kFollow`；資料是 owner 的 net_id。
- **主人怎麼來**：
  - template 的 `default_behavior: follow`：agent 被發現時，用它的 `owner_peer` 找對應的玩家；找不到就退回
    guard（站在當下位置）。
  - `set_behavior` 可直接給 owner（spec 新增 `owner_net_id`）。
- **沒看到敵人時**（`on_idle`）：離主人超過 `resume_meters` 就走過去，進入 `stop_meters` 內就停下並四處張望
  （雙門檻，避免在邊界抖動）。走路用 navmesh 路徑，由 manager 規劃：主人移動超過一定距離或每 `repath_ticks`
  重算一次（同 guard 走回崗位、hunter 的 Seek）。
- **看到敵人時**：共用狀態機，照常接戰（目標 (a)）。
- **leash**：以主人為中心。追擊把它拉離主人超過 `leash_meters` 就放手，Return 走回主人身邊。
- **主人消失**：依待決 Q3。
- **YAML 草案**：

```yaml
ai:
  default_behavior: follow        # 只有 player_side template 能寫
  behaviors:
    follow: {stop_meters: 6.0, resume_meters: 10.0, leash_meters: 25.0, repath_ticks: 30}
```

- **限制**：巨人 2.5 m/s、轉向 45°/s，玩家 5 m/s，一定會落後；是否補救見 Q4。
- 舊對話中寵物的「繞圈」「特殊 Idle 動畫」巨人用不到；以後需要時可加在 `behaviors.follow`。

---

## 7. 階段

| 階段 | 內容 | ABI / schema | 驗證 |
|---|---|---|---|
| F1 | E1、E2 | 不變 | kernel 測試：丟瓶子生出的 entity 帶丟的人的 peer（依 Q1 的範圍）；玩家閃避時召喚物照樣受傷；3005 的敵人依 Q1 的決定 |
| F2 | 召喚道具（catalog） | 不變；**catalog 新增 template，`bundle.bytes` 要更新** | 丟道具 → 落地 → 生出友軍巨人、帶丟的人的 peer；template 不合法時的整批放棄 |
| F3 | `FollowBehavior` | 不變；**`behaviors.follow` 是新的 catalog key**：Unity package 要重建（見 memory：catalog format change needs package） | 跟隨（停 / 走的雙門檻、跟著主人轉彎）、跟隨中接戰、leash 拉回、主人消失的處理、legged 巨人不吃到玩家的 input |
| F4（選做） | remote command 的 Follow、Unity 端 | 指令 enum 加值（C# mirror 要跟） | 依需要 |

---

## 8. 和舊對話「玩家寵物 AI agent」的差異

| 面向 | 舊對話（2026-08-30） | 本計劃 |
|---|---|---|
| 放在哪裡 | 新的 `AgentPetController` + kernel `KernelAiControllerType_Pet` | `FollowBehavior`，不改 kernel enum |
| 找主人 | 推薦先「綁最近的玩家」；「瓶子帶 owner」因撿取權限風險擱置 | D：spawn 繼承 provenance peer。W6 之後才有這條路 |
| 撿取權限風險 | `item_gameplay_system.cc:553` 會讓 prop 只有丟的人能撿 | **當時讀錯**：那行檢查的是「發出請求的人是否控制 instigator」，與 prop 的 owner 無關 |
| `event.instigator` | 碰撞事件不能用（載入時拒絕） | 仍成立 |
| 整批驗證放棄 | 要有測試 | 仍成立 |
| 每 tick 一個 input | 要注意 | chaser 已保證 |

---

## 9. 待決事項

| # | 問題 | 選項 |
|---|---|---|
| Q1 | E1 套用範圍 | (a) 一律套用：測試瓶 3005 生出的敵人也會帶玩家的 peer，玩家鎖不到它；(b) **只有生出 `player_side` actor 時才套用**（建議），敵人不受影響 |
| Q2 | 跟隨距離 | `stop_meters` / `resume_meters` / `leash_meters` 的預設值（草案 6 / 10 / 25 m） |
| Q3 | 主人死亡或斷線 | (a) 原地轉 guard；(b) 主人死亡時守在原地，復活後繼續跟；(c) 消失 / despawn |
| Q4 | 巨人跟不上 | (a) 接受落後；(b) 離太遠時提高移動速度；(c) 離太遠時傳送到主人附近 |
| Q5 | 召喚的限制 | 每個玩家同時最多幾隻？道具是否消耗、冷卻多久？巨人有沒有存在時間上限？ |
| Q6 | 重連 | 重連後 peer 和 net_id 都會變，舊的巨人要不要認回新的主人？ |
| Q7 | remote command | 要不要能下 Follow 指令（F4）？ |
