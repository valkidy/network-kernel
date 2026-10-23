# AI controller 重構實作計劃書

狀態：**設計已定案（§2），P0 尚未開始。**
分支：計劃在 `claude/ai-controller-plan`，從 `main`（84cc4d40，ABI 109）開。
各階段的實作各自開 `claude/ai-controller-p<N>` 分支。
最後更新：2026-10-23。

本文件整理 2026-10-21～23 的設計討論：controller 分類、state 與事件、hunter 的兩段式
搜尋、skill slot、敵人使用 item、special attack。第 2 節是決策，第 3 節是現況，
第 5 節起是分階段的實作。

---

## 1. 目標與範圍

目標：

- 敵人的行為由「角色」決定（砲台、駐守、巡邏、增援 / 追擊），同一個 entity template
  可以在不同場合扮演不同角色。
- 所有角色共用一套 state 與事件，各角色只決定自己怎麼處理每個 state。
- 單位可以被攻擊喚醒（under attack），不只靠 vision。
- 攻擊方式（武器、item、special attack）統一成最多 8 個 skill slot，依條件挑選。

在範圍內：

- game_server：controller、state machine、perception、skill 挑選、巡邏隊的連動、
  catalog 格式與驗證。
- kernel：受傷事件、AI 使用 item 的進入點、（之後）vision 降頻與傷害倍率。

不在範圍內：

- **`pursuer`（追擊到自己被消滅為止）**：pending，見 §2 D9。
- **Unity 端**：C# mirror、AI 使用 item 的表現、新的動畫狀態，由使用者處理。
- **Unity package build / stage / bump、merge 到 `feat-unity-plugin`**：由使用者處理。

---

## 2. 決策

| # | 決策 | 備註 |
|---|---|---|
| D1 | **controller = 角色**，由**生成來源**決定：spawner、巡邏隊、director `spawn_group` 可以覆寫；沒寫就用 template 的 `default_controller`；巡邏隊成員固定是 `patrol` | template 只描述能力與各 controller 的參數。參數合併順序：程式預設 → template `ai.controllers.<type>` → 生成來源的 `controller:` 區塊 |
| D2 | controller 類型：`sentry`（固定砲台）、`garrison`（駐守，可帶路線）、`patrol`（巡邏隊成員）、`hunter`（增援 / 特殊目標） | `hunter` 由原本的「增援」和「特殊目標」合併，用 `target_source: inherited \| nearest_hostile` 區分 |
| D3 | **state 與事件分開**。state（互斥）：Idle、Alert、Chasing、Attack、Search、Return，hunter 另有 Seek。事件：SawTarget、LostSight、ForgetTimeout、Damaged、LeashExceeded、LeashResumed | 每個 controller 對每個 state 各自實作，也可以設成不反應 |
| D4 | **Chasing** = 目標已知，但沒有任何 active skill 的條件成立。**Search** = 失去視野、還在 `forget_ticks` 內，朝最後看到的位置。**Return** = 超過 leash，回 anchor，途中不接戰 | sentry 的 Chasing 是原地轉向（目標仍在視野內）；失去視野後的原地掃視屬於 Search |
| D5 | **leash anchor 屬於 controller**：sentry 無；patrol 為開始追擊的位置，回隊伍位置；garrison 為蟲巢或路線上最近的點；hunter 無 leash | |
| D6 | **Alert 記注意點（attention point），不記目標 id**。被攻擊時：停下、轉向攻擊者或傷害來源；攻擊者為 0 時用傷害來源位置。Damaged 進入的 Alert 用 `alert_on_damage_ticks`（建議 15），Sighted 用 `alert_ticks` | Alert 中再次 Damaged 只更新注意點，不重設計時 |
| D7 | **巡邏隊連動**：任一成員進入 Alert → 巡邏隊 Halted（cursor 暫停），其他成員以同一個注意點進入 Alert；全員回到 Idle 才繼續前進 | 放在 `patrol_director` |
| D8 | **各 state 收到 Damaged**：Idle → Alert；Alert → 更新注意點；Chasing / Attack → 目前目標仍可見就不換，看不到才換成攻擊者；Search → 換成攻擊者並進 Alert；**Return → 只轉頭，不追** | |
| D9 | **`pursuer` pending**：要做時另開一種 controller，所有覆寫（不換目標、不 forget）關在它自己的 state graph 裡；catalog 不允許它加入巡邏隊或當 garrison | 未決：目標死亡（重生在死亡點上方，會變守屍）、目標失效（重連會換 net_id）、卡住（hover 中的 player、navmesh 到不了的位置） |
| D10 | **hunter 兩段式搜尋**。broad phase：全場共用一張 hostile 位置表（排除躲在帳篷裡、死亡中、斷線的 player），hunter 每 60 ticks 選一次目標，以 `net_id` 錯開；進入 Seek 時立刻查一次。narrow phase：沿用 kernel vision | broad phase 只給**目的地**，攻擊仍要先被 vision 看到。換目標門檻：新目標要近 20% |
| D11 | **narrow phase 先用距離開關 vision**：最近的目標 > vision range + 10 m 就 `Kernel_ServerClearEntityVisionConfig`，進入範圍再 Set | kernel 降頻（每 N ticks 算一次）等量過再決定（§9 P7） |
| D12 | **追擊時目標位置依 id 從 actor 狀態讀取**，vision 只用來確認「還看不看得到」 | Chasing 不會因為 vision 降頻追著過期的位置 |
| D13 | **skill slot**：每個單位最多 8 個，格式為 `{use: {type, value}, when, cooldown_ticks, weight, once}`。type：`weapon`、`item`、`sp_attack` | **不分 passive / active**：原本的 passive（對自己掛 status）改成「在某條件下使用 item」 |
| D14 | **skill 挑選**：每個 tick，從條件成立、cooldown 結束、有彈、且**沒有進行中 action** 的 skill 裡依 `weight` 挑一個；隨機數用每個 agent 自己的 seed | 沒有任何 skill 符合 → Chasing |
| D15 | **`when` 條件**：持續條件（`state`、`range_meters`、`hp_below`、`missing_status`）每個 tick 檢查；事件條件（`on_spawn`、`on_state_enter`、`on_damaged`）只在事件發生的 tick 成立 | |
| D16 | **`sp_attack_templates/`** 收納特殊組合攻擊。`kind` 是封閉清單，每種都要有 kernel 實作；目前是 `leap`、`charge` | 跳撲 = leap 移動 + 落地前用 loadout 的近戰（`on_landing.weapon`，需在 loadout 裡）；衝撞 = 蓄力 + 直線衝刺 + 自帶的接觸武器（`contact_weapon`，不需在 loadout 裡） |
| D17 | **`when` 與 `cooldown_ticks` 放在 skill 上**，不放進 sp_attack template | 同一個 sp_attack 給不同單位時觸發距離可以不同 |
| D18 | **AI 使用 item 不扣數量**：skill 上的 item 使用視為無限 | 避免 fungible item 被 AI 用完 |
| D19 | **刪除被動巡邏**（`passive_patrol`：只走路、不看人）。原本使用它的 21–23、39 改成 garrison + route，會接戰 | gameplay 用不到；x 軸來回改寫成兩點 route。見 §6.3 |

---

## 3. 現況（main 84cc4d40）

| 項目 | 現況 | 位置 |
|---|---|---|
| AI controller | 只有 `sentry` 和 `chaser` 兩種；drone / airship 是 chaser + hover 移動 | `agent_sentry_controller.cc`、`agent_chaser_controller.cc` |
| controller 綁定 | 寫死在 entity template 的 `ai.controller`；chaser 是否在巡邏隊由生成時的 slot 決定（`has_slot`） | `gameplay_config.cc` |
| 被動巡邏 | `sentry` + `passive_patrol`：只走路不看人；`patrol_route` 只掛得上 passive_patrol 的 agent，其他類型只印 warn | `agent_sentry_controller.cc:66`、`agent_runtime_manager.cc:308` |
| state | `AgentSentryState`：kIdle、kAlert、kAttack、kReturn | `agent_runtime.h:33` |
| perception | 只有 vision 的結果（是否看到、目標 id、目標位置、視線方向），沒有受傷資訊 | `ai_perception_adapter.h:26` |
| 武器 | AI config 只有一個 `weapon_id` | `agent_runtime.h:99` |
| 跳撲 / 衝撞 | 參數在 `chaser.leap` / `chaser.charge`；先試衝撞再試跳撲 | `agent_chaser_controller.cc:123,169,494` |
| vision | kernel 每 tick 對所有掛 vision config 的 agent 計算（16 m grid）；有 Set / Clear export | `kernel.cc:14810`、`kernel_api.h:633` |
| 尋路 | chaser 直線走，不用 navmesh；巡邏隊用 `patrol_navigation` 分段尋路 | `patrol_navigation.cc` |
| status | `status_effect_templates/`：channel + `duration_ticks`（必須 > 0）+ `replace_policy` + action graph；有 speed modifier，**沒有傷害倍率** | `gameplay_config.cc:2945` |
| item 使用 | item 有 `use`（數量、cooldown）與 `on_item_used` graph；**AI 觸發使用的進入點未確認** | `KernelPlayerInput` 沒有 item 欄位 |

---

## 4. Kernel 改動總覽

| # | 改動 | 階段 | ABI |
|---|---|---|---|
| K1 | 受傷事件：每 tick 的（受害者、攻擊者、位置、weapon_id），供 game_server 讀取 | P3 | 新增 export，用 capability flag，**不 bump** |
| K2 | AI 使用 item 的進入點（若現有路徑只有 client 封包） | P5 | 同上，不 bump；P0 確認 |
| K3 | vision 每 agent 降頻 | P7（量過才做） | export 不 bump；若改 template 欄位則要更新 `bundle.bytes` |
| K4 | 傷害倍率（attack up） | 獨立軌（§10） | 改到傷害公式與 status，**很可能 bump** |

snapshot / packet schema：P1–P6 都不需要改。新的 animation_state 值（例如 Alert 轉身）
只是數值，不改 schema。

---

## 5. P0：驗證（不寫功能碼）

每一項都要有量測或測試結果，記在 §11。

| # | 要確認的事 | 方法 | 影響 |
|---|---|---|---|
| V1 | kernel 移動 AI 時，會不會套用 `speed_up` / `slow` 的 speed modifier | 測試：agent 掛 speed_up，量 2 秒的位移 | 不會的話，item 掛 speed_up 對 AI 無效，P5 要加一項 kernel 改動 |
| V2 | 玩家使用 item 的完整路徑；AI 能不能走同一條 | 讀 code，找到 `on_item_used` 的觸發點 | 決定 K2 要不要做 |
| V3 | `patrol_director` 在成員戰鬥時，cursor 會不會暫停 | 讀 code + 測試 | D7 的改動量 |
| V4 | `Kernel_ServerSetEntityVisionConfig` / `Clear` 的成本，Set 之後下一個 tick 就有結果嗎 | `agent_cpu_bench` 加一組每 60 ticks 切換 | D11 是否可行 |
| V5 | 基準：現有的 AI + kernel tick 成本 | `bazel run -c opt //game_server:agent_cpu_bench`（200 m 與 1 km） | 之後各階段對照 |
| V6 | 受傷事件在 kernel 哪裡最容易收集（`apply_melee_damage`、projectile 命中、area effect） | 讀 code | K1 的範圍 |

**注意**：效能量測一定要用 `-c opt`，dbg build 慢 27 倍。

---

## 6. P1：catalog 結構重整（除被動巡邏外，行為不變）

目標：只換格式與 controller 的決定方式。除了 §6.3 刪除被動巡邏造成的改變，現有測試的
結果必須完全一樣（以失敗集合為準，main 本來就有紅燈）。

### 6.1 template 格式

```yaml
ai:
  default_controller: garrison
  perception:
    alert_ticks: 90
    forget_ticks: 150
    idle_rotation_interval_ticks: 30
    idle_rotation_degrees: {min: 15.0, max: 30.0}
  locomotion:
    stationary: false
    stop_distance_meters: 1.8
    resume_distance_meters: 2.1
    input_magnitude: 1.0
  controllers:
    patrol:   {slot_radius_meters: 1.2, input_magnitude: 0.45, leash_meters: 18.0, leash_resume_meters: 6.0}
    garrison: {leash_meters: 15.0, leash_resume_meters: 5.0}
    hunter:   {target_source: inherited, on_target_lost: garrison}
```

### 6.2 生成來源覆寫

```yaml
# 蟲巢 spawner
spawner:
  controller:
    type: garrison
    route: [...]        # 相對蟲巢的座標
    loop: true

# director spawn_group
- type: spawn_group
  entity_template: gingerbread_giant
  controller:
    type: garrison
    route: [...]        # 世界座標，取代 patrol_route
```

### 6.3 現有內容的對應

| 現在 | P1 之後 |
|---|---|
| `ai.controller: sentry`（2、24、25、27） | `default_controller: sentry`，`locomotion.stationary: true` |
| `ai.controller: chaser`，在巡邏隊 | `patrol`（巡邏隊固定） |
| `ai.controller: chaser`，單獨生成（蟲巢、airship 投放、director） | `garrison`，沒有路線（行為 = 現在的單獨 chaser） |
| `sentry` + `passive_patrol`（21、22、23、39） | `garrison` + `route`；**會接戰**（見下方「唯一的行為改變」） |
| `passive_patrol` 的 x 軸來回（`patrol_extent_x_meters`） | **刪除**；需要來回走的，改寫成相對出生點的兩點 `route` |
| director 的 `patrol_route` | `spawn_group.controller.route` |

**刪除「只走路、不打人」的被動巡邏（D19）**：`passive_patrol`、`patrol_extent_x_meters`、
`patrol_input_magnitude` 與 sentry 的被動巡邏分支一併刪除，gameplay 用不到。

**唯一的行為改變**：21–23（locomotion 展示用）與 39（巨人）改成 garrison 後，看到目標
就會接戰；沒有目標時照舊沿路線走。要一起改的地方：

- director 100（`tripod_actor`）、102（`quadruped_actor`）的 x 軸來回 → 兩點 `route`。
- director 101 的三個巨人長條出生點 → 兩點 `route`；narrow pass 的 `patrol_route` →
  `controller.route`。
- `agent_cpu_bench` 的 gait 表（quadruped 靠 passive_patrol 自己走）→ 改成 garrison + route。
- `agent_sentry_per_agent_config_test`（測 passive 與非 passive 設定不互相污染）→ 改測
  「同一個 template、不同生成來源的 controller 參數不互相污染」，保留原本要防的 bug。
- `agent_sentry_gravity_test` 的 passive_patrol 分支 → 改成 garrison 走路線的分支。
- 依賴「展示用單位只走路」的 locomotion 測試，場上沒有目標時行為不變；
  有目標的測試要逐一確認。

### 6.4 驗證

- 不合法的組合：`stationary: true` 配會移動的 controller；`route` 寫在 garrison 以外；
  `hunter` 缺 `target_source`；`on_target_lost` 指回 hunter 自己但不是 `nearest_hostile`。
- `route` 掛在不會走路線的 controller 上，從執行時 warn 改成**載入時報錯**。

### 6.5 測試

- 現有的 sentry、chaser、巡邏隊、跳撲、衝撞、巨人路線（`pass_giant_route_test`）
  測試全部照舊通過。
- 新增：同一個 `gingerbread` template 經由巡邏隊、蟲巢、director 生成，各自得到
  正確的 controller。
- 新增：合併順序（程式預設 → template → 生成來源）的單元測試。
- catalog 有改，`shipping_catalog_flow_test` 要跟著更新。

---

## 7. P2：state machine 重構

目標：所有 controller 共用 state 與事件（D3、D4、D5），各 controller 提供每個
state 的處理。

1. `AgentSentryState` 改成新的 state 清單：Idle、Alert、Chasing、Attack、Search、
   Return、Seek（Seek 先保留給 P6）。
2. 事件由 perception 產生：SawTarget、LostSight、ForgetTimeout、LeashExceeded、
   LeashResumed（Damaged 在 P3）。
3. Alert 改成記注意點（D6）；Search 記最後看到的位置。
4. 每個 controller 一個 handler 表：`sentry`、`garrison`、`patrol` 先做；`hunter` 在 P6。
5. Chasing 與 Attack 的分界暫時沿用現有的 `attack_range_meters`，P4 改成
   「是否有 skill 的條件成立」。

**行為改動**（要更新的測試）：

- Search 是新的：失去視野後會朝最後看到的位置移動，而不是原地等 forget。
- sentry 失去視野後轉向最後看到的位置。

測試：每個 controller 的完整流程（Idle → Alert → Chasing → Attack → LostSight →
Search → ForgetTimeout → Idle / Return）。

---

## 8. P3：受傷事件（under attack）

1. **K1**：kernel 收集每 tick 的受傷事件，新增 export（capability flag）。
   攻擊者可能是 0（瓶子爆炸、area effect）。
2. perception 加上本 tick 的受傷事件；轉成 Damaged 事件。
3. 依 D8 處理每個 state；Alert 用 `alert_on_damage_ticks`。
4. **D7**：`patrol_director` 加上 Halted；依 V3 的結果決定改動量。

測試：

- 巡邏中的成員被背後攻擊（攻擊者不在視野內）→ 停下、轉向、整隊 Halted。
- 瓶子爆炸（攻擊者 0）→ 轉向爆炸位置。
- Return 中被攻擊 → 只轉頭，不追。
- Chasing 中被第二個 player 攻擊，原目標仍可見 → 不換目標。

---

## 9. P4–P7

### P4：skill slot（weapon 與 sp_attack）

1. 新增 `sp_attack_templates/`（`$id_sp_attack_$name.yaml`），把 `chaser.leap` 和
   `chaser.charge` 的參數搬過去：
   - leap：`leap.*`、`on_landing: {weapon, lead_ticks}`
   - charge：`windup.*`、`charge.*`、`contact_weapon`
2. `ai.skills` 取代單一 `weapon_id`；`use.type` 為 `weapon` 或 `sp_attack`（item 在 P5）。
3. 多武器：依 skill 切換 active weapon；只在沒有進行中 action 時切換。
4. 依 D14 挑選；每個 agent 有自己的 seed。
5. Chasing / Attack 的分界改成「是否有 skill 條件成立」。

**catalog 驗證**：

- slot 0–7 不重複；`weapon` 必須在 loadout；`on_landing.weapon` 必須在 loadout；
  `contact_weapon` 不需要。
- `on_landing.lead_ticks == swing 的 commit_offset_ticks + 1`。
- 每個 skill 的 `range_meters` 要和 `[stop_distance, vision range]` 有交集；
  不相交就報錯。
- `stop_distance` 必須落在至少一個 skill 的範圍內（否則單位會停下不動）。
- `range_meters.min > resume_distance` 的 skill 標成「只有目標拉開距離時才會用」：
  警告，不報錯。

測試：mage 兩把武器（需要先把 rocket 加進 loadout）的挑選比例；跳撲、衝撞改用
sp_attack 後，`gingerbread_leap_test`、`gingerbread_charge_test` 照舊通過。

### P5：AI 使用 item

1. **K2**（依 V2 結果）：AI 使用 item 的進入點。
2. agent 生成時依 template 建立 inventory，放入 skill 引用的 item。
3. `use.type: item`；使用時不扣數量（D18）。
4. 事件條件 `on_spawn`、`on_state_enter`、`on_damaged`，持續條件 `hp_below`、
   `missing_status`。
5. 新增 AI 專用的 item（例如 `enemy_speed_tonic`），用現有的 `speed_up` status。

依 V1 結果：若 speed modifier 對 AI 無效，這一階段要加一項 kernel 改動。

測試：進入 Chasing 時使用 speed tonic，量測速度；`slow` 蓋掉 `speed_up` 後，
`missing_status` 讓它再用一次。

### P6：hunter

1. hostile 位置表（每 tick，全場共用，排除帳篷、死亡、斷線）。
2. `target_source`：`inherited`（生成時由呼叫者給目標 id）、`nearest_hostile`；
   換目標門檻 20%。
3. Seek state：沿 navmesh 走向目的地（沿用 `patrol_navigation` 的分段尋路），
   每 60 ticks 重算、以 `net_id` 錯開；進入 Seek 時立刻查一次。
4. D11：依距離開關 vision。
5. `on_target_lost`：`nearest_hostile` 或轉成 `garrison`（anchor = 當下位置）。
6. airship 的 on_alert spawner 改用 `controller: {type: hunter, target_source: inherited}`。

測試：hunter 在 vision 外找到 player 並走過去；目標死亡後換目標；vision 依距離
開關的次數與時機；`agent_cpu_bench` 加一組 hunter，與 V5 對照。

### P7：效能（量過才做）

- 尋路成本過高 → 每個目標一個 flow field，追同一目標的 hunter 共用。
- vision 仍是瓶頸 → K3。

---

## 10. 獨立軌：傷害倍率（attack up）

現在的規則是「命中傷害永遠由 projectile 決定」，所以 attack up 不能只掛 status。
需要在 kernel 新增 shooter 的傷害倍率，命中時套用，並綁在 status 上（status 結束時
移除）。這會改到傷害計算，很可能要 bump ABI；和 P1–P7 沒有相依，可以另外排程。

---

## 11. 待決與驗證紀錄

**待決**：

| # | 問題 |
|---|---|
| Q1 | passive 用途的 status 要和玩家的 debuff 共用 channel（互相取代），還是用獨立的 channel（疊加）？ |
| Q2 | `duration_ticks` 要不要允許 0 代表永久？還是一律靠 `missing_status` 補上？ |
| Q3 | broad phase 跳過躲在帳篷裡的 player：確認採用 |
| Q4 | `pursuer` 的三個未決問題（D9） |

**P0 驗證結果**：（尚未開始）

---

## 12. 交付

每個階段完成時回報：

- 分支名稱和 commit。
- 有沒有 ABI 或 snapshot schema 的變動。
- package 裡的 `bundle.bytes` 是否需要更新（P1、P4、P5、P6 都改 catalog，都需要）。

| 階段 | ABI | snapshot schema | bundle.bytes |
|---|---|---|---|
| P0 | — | — | — |
| P1 | — | — | 要 |
| P2 | — | — | — |
| P3 | export（capability，不 bump） | — | — |
| P4 | — | — | 要 |
| P5 | 可能新增 export（不 bump） | — | 要 |
| P6 | — | — | 要 |
| P7 | 視做法而定 | — | 視做法而定 |
| 傷害倍率 | 很可能 bump | — | 要 |

**成本控制**（每個階段都適用）：

- 先跑最小的 smoke test，再跑相關的測試；不跑整個 repository 的 build / test，
  除非另外要求。
- 只讀和該階段直接相關的檔案。
- 範圍擴大時先停下來說明原因。
- 完成時回報讀了哪些檔案、改了哪些檔案、做了哪些搜尋、跑了哪些 build / test 指令。
