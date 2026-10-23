# AI controller 重構實作計劃書

狀態：**設計已定案（§2）；P0 驗證完成（§11）；P1–P6 完成（§6.0、§7.0、§8.0、§9），在 `claude/ai-controller-p1`（2026-10-24）；P7 依量測結果暫不需要。**
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
| D7 | **巡邏隊連動**：任一成員進入 Alert → 巡邏隊 Halted（cursor 暫停），其他成員以同一個注意點進入 Alert；全員回到 Idle 才繼續前進 | cursor 暫停**已經存在**（`group.holding`，V3）；新的只有「其他成員一起進 Alert」 |
| D8 | **各 state 收到 Damaged**：Idle → Alert；Alert → 更新注意點；Chasing / Attack → 目前目標仍可見就不換，看不到才換成攻擊者；Search → 換成攻擊者並進 Alert；**Return → 只轉頭，不追** | |
| D9 | **`pursuer` pending**：要做時另開一種 controller，所有覆寫（不換目標、不 forget）關在它自己的 state graph 裡；catalog 不允許它加入巡邏隊或當 garrison | 未決：目標死亡（重生在死亡點上方，會變守屍）、目標失效（重連會換 net_id）、卡住（hover 中的 player、navmesh 到不了的位置） |
| D10 | **hunter 兩段式搜尋**。broad phase：全場共用一張 hostile 位置表（排除躲在帳篷裡、死亡中、斷線的 player），hunter 每 60 ticks 選一次目標，以 `net_id` 錯開；進入 Seek 時立刻查一次。narrow phase：沿用 kernel vision | broad phase 只給**目的地**，攻擊仍要先被 vision 看到。換目標門檻：新目標要近 20% |
| D11 | **narrow phase 先用距離開關 vision**：最近的目標 > vision range + 10 m 就 `Kernel_ServerClearEntityVisionConfig`，進入範圍再 Set | V4：1300 agents 時 vision 只佔 kernel tick 約 13%，切換成本可忽略；kernel 降頻（K3）**不做** |
| D12 | **追擊時目標位置依 id 從 actor 狀態讀取**，vision 只用來確認「還看不看得到」 | Chasing 不會因為 vision 降頻追著過期的位置 |
| D13 | **skill slot**：每個單位最多 8 個，格式為 `{use: {type, value}, when, cooldown_ticks, weight, once}`。type：`weapon`、`item`、`sp_attack` | **不分 passive / active**：原本的 passive（對自己掛 status）改成「在某條件下使用 item」 |
| D14 | **skill 挑選**：每個 tick，從條件成立、cooldown 結束、有彈、且**沒有進行中 action** 的 skill 裡依 `weight` 挑一個；隨機數用每個 agent 自己的 seed | 沒有任何 skill 符合 → Chasing |
| D15 | **`when` 條件**：持續條件（`state`、`range_meters`、`hp_below`、`missing_status`）每個 tick 檢查；事件條件（`on_spawn`、`on_state_enter`、`on_damaged`）只在事件發生的 tick 成立 | |
| D16 | **`sp_attack_templates/`** 收納特殊組合攻擊。`kind` 是封閉清單，每種都要有 kernel 實作；目前是 `leap`、`charge` | 跳撲 = leap 移動 + 落地前用 loadout 的近戰（`on_landing.weapon`，需在 loadout 裡）；衝撞 = 蓄力 + 直線衝刺 + 自帶的接觸武器（`contact_weapon`，不需在 loadout 裡） |
| D17 | **`when` 與 `cooldown_ticks` 放在 skill 上**，不放進 sp_attack template | 同一個 sp_attack 給不同單位時觸發距離可以不同 |
| D18 | **AI 使用 item 不扣數量**：skill 上的 item 使用視為無限 | V2：kernel 照常扣數量。P5 由 game_server 在使用後補回（`Kernel_ServerCreateInventoryItem`），不改 kernel |
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
| K1 | 受傷事件：每 tick 的（受害者、攻擊者、位置、weapon_id），供 game_server 讀取。**獨立的 batch export**，不走 `Kernel_PollEvents`（V6） | P3 | 新增 export，用 capability flag，**不 bump** |
| K2 | ~~AI 使用 item 的進入點~~ **不需要**：`Kernel_ServerSubmitGameplayRequest` 已可用（V2） | — | — |
| K3 | ~~vision 每 agent 降頻~~ **不做**（V4：上限只省約 13% kernel tick） | — | — |
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

### 6.0 拆分與進度

P1 實作時發現範圍比原本的「catalog 重整」大（garrison 走路線是新的 controller 邏輯；生成來源
覆寫要穿過四個 director），所以拆成四步，每步一個 commit：

| 步驟 | 內容 | 行為改變 | 狀態 |
|---|---|---|---|
| P1a | 新格式與舊格式並存（新格式在載入時轉成舊格式的形狀，再用原本的 parser）；每個 agent 帶 `AgentControllerRole`，controller 依（template, role）綁定 | 無 | ✅ `claude/ai-controller-p1` |
| P1b | 生成來源覆寫：四個 director 回傳 net_id → controller spec；驗證；`patrol_route` → `controller.route` | 無 | ✅ `claude/ai-controller-p1` |
| P1c | garrison 走路線（共用 helper）、刪除 `passive_patrol` 與 x 軸來回、轉換 21–23、39 與 director 100–102、更新測試 | **有** | ✅ `claude/ai-controller-p1` |
| P1d | 所有 template 改用新格式、刪除舊格式（parser 直接讀新 key） | 無 | ✅ `claude/ai-controller-p1` |

P1a 的對應（舊 → role）：`controller: sentry` → `kSentry`（`stationary` = 不是 passive_patrol）；
`controller: chaser` → `kGarrison`。kernel 的 `ai_controller_type` 不變（sentry → Sentry，其他 →
Chaser），**不需要 ABI 改動**。`hunter` 在 P6 之前會被拒絕；`controllers.garrison` 目前只接受空區塊。

P1b 的實作：`AgentControllerSpec`（role + 世界座標 route）。spawn_group 接受 `controller: {type, route}`，
spawner 與 world rule 只接受 `controller: {type}`（world rule 把 agent 散在圓周上，spawner 的路線座標系
要到 P1c 才需要）；巡邏隊固定把成員設成 patrol。`type` 只接受 sentry / garrison（patrol 只能由巡邏隊
產生，hunter 等 P6）。載入時檢查：sentry 只能用在 sentry template、garrison / patrol 不能用在 sentry
template、route 在 P1c 之前只能給 legacy passive_patrol walker。**參數覆寫延後**：garrison 在 P2 之前
沒有參數，巡邏隊成員用 template 的 `controllers.patrol`，目前沒有可以覆寫的東西。

P1c 的實作：路線行走移到 `agent_steering::route_patrol_move`，chaser 在 Idle、沒有巡邏隊位置、
有路線時走路線（速度沿用 `locomotion.input_magnitude`，garrison 自己的參數等 P2）；打完後從所在位置
直接走向原本要去的轉角（navmesh 重新尋路與 leash 是 P2 的）。只有 garrison 會被掛上路線。world rule
的 `controller.route` 是相對每個 agent 出生點的位移。21–23、39 改寫成新格式的 garrison（停在 8 m、
10 m 再追，射程不設限，因為武器是 spammer）。director 100、102 用 ±30 m 的相對路線，101 的三個
長條巨人用世界座標的兩點路線。`passive_patrol` / `patrol_extent_x_meters` / `patrol_input_magnitude`
在載入時會被拒絕，並提示改用 garrison + route。

行為改變與測試：這四種單位看到目標就會接戰（`ai_controller_spec_test` 量了「走路線 → 接戰 → 遺忘
→ 回到路線」）；`pass_giant_route_test` 的長條巨人偏離容許值從 0.5 m 放寬到 2 m（路段改走 navmesh，
轉角可能偏離直線約 1 m）；`agent_cpu_bench` 的步態表中，四足單位不再自己走，而是追擊原點的玩家；
`agent_sentry_per_agent_config_test` 與 `agent_sentry_gravity_test` 改成不依賴被動巡邏。
`gameplay_config_test` 在 main 上本來就無法 build，沒有改。

P1d 的實作：17 個 legacy agent template 用腳本轉換（保留註解）；`perception` / `locomotion` /
`controllers.patrol` 由原本的 parser 直接讀，轉換函式刪除。驗證方式：轉換前後與 parser 改寫後各傾印一次
所有 actor template 的 AI 欄位與 catalog hash，三份完全相同。舊 key（`controller` / `sentry` / `chaser` /
`patrol`）在載入時被拒絕並提示新名稱；director 仍可寫 `controller: director`。`shipping_catalog_flow_test`
與 bench 不需要改。`ai_role_based_format_test` 改為檢查所有 agent template 都是新格式，以及 13 種錯誤寫法
會被拒絕。

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

### 7.0 拆分與進度

| 步驟 | 內容 | 行為改變 | 狀態 |
|---|---|---|---|
| P2a | `kChasing`、`kSearch` 兩個新狀態；Alert / Search 的注意點；chaser 與 sentry 都做 Search | **有** | ✅ `claude/ai-controller-p1` |
| P2b | garrison 的 leash 與 Return（`controllers.garrison.leash_meters` / `leash_resume_meters`） | 只有寫了 leash 的 template（目前沒有） | ✅ `claude/ai-controller-p1` |

P2a 的實作與決定：
- 狀態仍是 `AgentSentryState`（新值接在後面：`kChasing = 4`、`kSearch = 5`）；`is_pursuing` = Chasing 或
  Attack，`is_engaged` 多了 Chasing 與 Search（巡邏隊在成員搜尋時仍然停下）。`kSeek` 留給 P6 再加。
- 事件沒有另外做成資料結構：每個 tick 開頭由 perception 算出（看得到 = SawTarget，`lost_target_ticks`
  ≥ `forget_ticks` = ForgetTimeout，leash 判斷照舊）。
- Chasing / Attack 每個 tick 依 `attack_range_meters`（或跳撲的落地時間、蓄力中）切換，直接設定狀態、
  不經 `transition_to`，以免重設計數。sentry 沒有射程，看得到就是 Attack。
- Search：chaser 走向最後看到目標的位置（腳下），到了就原地環顧；sentry 轉向那個位置。再次看到 →
  chaser 回到追擊（不重新 Alert）、sentry 回到 Attack；超過 `forget_ticks` → Idle（巡邏隊成員 → Return）。
- **Alert 中失去目標不進 Search**：照舊等到 forget 後放棄，避免目標進出視野就跳過警告。
- 失去目標到放棄的時間從 2 × `forget_ticks`（Attack 等一次、再到 Alert 等一次）變成 1 × `forget_ticks`。
P2b 的實作與決定：
- leash 的錨點與巡邏隊成員相同：開始追擊（Idle → Alert）時的位置，不是路線上最近的點。這樣「追擊能把
  單位拉多遠」只看這次追擊，實作也與巡邏隊共用。
- 超過 leash → Return：直線走回錨點（不經 navmesh），途中不接戰；回到 `leash_resume_meters`（至少 1 m）
  以內 → Idle，有路線的 garrison 接著走路線。
- `leash_meters` 0 = 沒有 leash（預設）。shipping catalog 沒有任何 template 寫 garrison leash，所以 P2b
  不改變現有單位的行為；要讓巨人等單位有 leash，在 template 的 `controllers.garrison` 加上即可。
- 載入時檢查：非負、有限，且 `leash_resume_meters` < `leash_meters`。

- 測試：`agent_chaser_controller_test` 的「失去視野原地停下」改成 Search（走向最後位置、再看到就恢復、
  forget 後 Idle）；射程外的狀態從 kAttack 改成 kChasing。`agent_sentry_controller_test` 改成 Attack →
  Search → Idle。`agent_runtime_manager_test` 在 main 上第 228 行就失敗，後面對 kAttack 的檢查無法驗證。

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

### 8.0 拆分與進度

| 步驟 | 內容 | 狀態 |
|---|---|---|
| P3a | K1：`Kernel_ServerQueryDamageRecords` | ✅ `claude/ai-controller-p1` |
| P3b | perception 的 Damaged 事件與各 state 的處理（D6、D8） | ✅ `claude/ai-controller-p1` |
| P3c | 巡邏隊成員共享注意點（D7） | ✅ `claude/ai-controller-p1` |

P3a（K1）的實作：
- `KernelDamageRecord`（32 bytes：tick、受害者、攻擊者、命中位置、傷害）；export
  `Kernel_ServerQueryDamageRecords(kernel, out, max)`：回傳**最近一次 `Kernel_Update`** 實際套用的命中
  （免疫、屍體、躲在帳篷裡的都不算），每次 update 取代上一次的內容；寫入至多 `max` 筆，回傳總數。
- capability `KERNEL_CAPABILITY_SERVER_DAMAGE_RECORDS = 0x0010000000000000`（檢查過所有本地分支沒有人用）；
  兩份 export 清單（`.def` 與 `BUILD.bazel`）都加了；**ABI 不 bump**。
- **攻擊者是「誰開的火」，不是「什麼造成的傷害」**：傷害的 source 是投射物或光束本身，而投射物在傷害套用
  之前就已經被銷毀。kernel 每個 tick 在投射物模擬前、銷毀前各記一次「投射物 / 光束 → 射手」，保留 300
  ticks；記錄時用它轉換。近戰、hitscan 的 source 就是角色本身；沒有射手的爆炸（瓶子）是 0。
- 不走 `Kernel_PollEvents`：dedicated server 每 tick 只讀 32 個事件且每個都寫 info log，而且
  `DamageApplied` 事件沒有攻擊者 id 與位置。
- 測試：`damage_records_test`（光束無人機燒玩家：記錄的攻擊者是無人機而不是光束；沒有命中的 update 回報 0
  筆）；`damage_record_shooter_test`（kernel 層：投射物銷毀後仍能查到射手、沒有射手的爆炸是 0、近戰是角色
  本身、未設定 struct_size 的緩衝會被拒絕）；`dynamic_abi_smoke_test` 與 `kernel_c_header_test` 涵蓋新
  export 與 bit。用真的投射物打玩家需要 session 的 rewind 歷史，所以投射物的部分在 kernel 層測。
P3b 的實作與決定：
- `PerceptionFrame` 每個 tick 查一次 damage records（全體共用，和 vision 一樣），在 `Kernel_Update` 之前讀，
  所以是上一次 update 的命中：命中的那個 tick，受害者在下一個 tick 反應。每個受害者取最後一筆。
- `SentryPerceptionSnapshot` 多了 `damaged`、`attacker_id`、`damage_source`：攻擊者還是活著的角色時，
  來源是攻擊者**現在**的位置；攻擊者是 0 或已經不在，來源是命中位置。
- `perception.alert_on_damage_ticks`（預設 15）：被打而進入的 Alert 用這個時間（`alerted_by_damage`）；
  看到而進入的仍用 `alert_ticks`。
- 處理（D8）：Idle / Search → Alert（被打）；Alert 或追擊中但看不到 → 注意點移到來源（Search 會走過去）；
  看得到目標 → 不換；Return → 只轉頭。sentry 在 Attack 中被打不理會。
- Alert 中看不到目標時，chaser 與 sentry 都會轉向注意點（包括「看到後又失去」的 Alert，原本是站著不動）。
- 測試：`damage_reaction_test`（友方光束砲台從背後打 gingerbread：被打之前一直是 Idle；下一個 tick 進入
  被打的 Alert，注意點就是砲台；14 ticks 後轉身追擊砲台）。`ai_controller_spec_test` 的「遺忘後回到路線」
  量測改成 2 秒（Alert 中轉向會讓它停在不同的位置，可能剛好在路線端點轉身）。

P3c 的實作：在 `patrol_group_runtime` 判斷 `holding` 的同一個迴圈裡，找到第一個「交戰中而且有注意點」的成員；
同隊所有 Idle 的成員進入 Alert，注意點相同，alert 的種類（被打 / 看到）也跟著來源。正在 Return 的成員不受影響。
被叫醒的成員之後靠自己的 perception：看不到就在 forget 後回到 Idle，只要戰鬥還在就會再被叫醒，所以會一直朝那個
方向看。測試：`patrol_group_runtime_test` 新增一段（共享注意點、Return 的成員不動、沒有注意點的交戰不叫醒任何人）。

- 既有紅燈（與本步驟無關，main 上相同）：`kernel_c_header_test`（`RenderEntityState` 大小）、
  `damage_pipeline_test`、`hitscan_test`（事件數量）。

1. **K1**：kernel 收集每 tick 的受傷事件，新增 export（capability flag）。
   攻擊者可能是 0（瓶子爆炸、area effect）。
2. perception 加上本 tick 的受傷事件；轉成 Damaged 事件。
3. 依 D8 處理每個 state；Alert 用 `alert_on_damage_ticks`。
4. **D7**：cursor 暫停已存在（`patrol_group_runtime.cc` 的 `group.holding`，看 `is_engaged`）。
   新增：成員進入 Alert 時，把注意點傳給同隊其他 Idle 成員；`is_engaged` 改用新的 state 清單。

測試：

- 巡邏中的成員被背後攻擊（攻擊者不在視野內）→ 停下、轉向、整隊 Halted。
- 瓶子爆炸（攻擊者 0）→ 轉向爆炸位置。
- Return 中被攻擊 → 只轉頭，不追。
- Chasing 中被第二個 player 攻擊，原目標仍可見 → 不換目標。

---

## 9. P4–P7

### P4：skill slot（weapon 與 sp_attack）

**拆分與進度**：

| 步驟 | 內容 | 行為改變 | 狀態 |
|---|---|---|---|
| P4a | `sp_attack_templates/` 與每個 agent 必填的 `ai.skills`；載入時解析成原本的 weapon / attack range / leap / charge | 無 | ✅ `claude/ai-controller-p1` |
| P4b | 多個 weapon skill、依權重與 seed 挑選、切換武器、Chasing / Attack 依「有沒有 skill 搆得到」 | 只有寫了多個武器的 template（目前沒有） | ✅ `claude/ai-controller-p1` |

P4a 的實作與決定：
- **skill 用 weapon id 引用武器**（`{type: weapon, value: 10}`），和 `weapon_slots` 一致。武器的 `name` 是給人看的
  （例如 spammer 的 name 是 "Projectile Spammer"），對不上檔名，所以沒有用名稱。sp_attack 用 template 的 `name`。
- skill 的順序就是 slot（最多 8 個），沒有另外的 `slot:` 欄位。`when` 目前只有 `range_meters {min, max}`
  （max 0 = 不限）；`state`、`hp_below` 等條件等 P5 一起做。
- `locomotion.attack_range_meters`、`locomotion.leap`、`locomotion.charge`、`perception.weapon_slot` 移除：前三個
  在載入時被拒絕並提示改用 skill，`weapon_slot` 沒有任何 template 使用。
- sp_attack template：leap = `leap {landing_distance_meters, apex_meters, max_horizontal_speed_meters_per_second}`
  + `on_landing {weapon, lead_ticks}`；charge = `windup {ticks, animation}` + `charge {speed, overshoot}` +
  `contact_weapon`。`windup.animation` 只接受數字（template 不屬於任何角色，沒有 idle / chasing 可指）。
- P4a 的限制（P4b 解除）：每個 agent 恰好一個 weapon skill，且沒有 min 距離與 cooldown；leap / charge 各最多一個；
  sentry 不能有 range 也不能有 sp_attack；leap 的 `on_landing.weapon` 必須是 agent 的 weapon skill。
- 驗證：轉換前後傾印所有 actor template 的武器、射程、leap、charge 欄位，完全相同；原本的 leap / charge / 射程
  驗證在解析後照常執行（例如射程小於 resume 距離仍會被拒絕）。
- `lead_ticks == commit_offset_ticks + 1` 的檢查**沒有做**：需要從武器找到 action template 的 commit offset，
  留到 P4b。
- 新目錄加進了 bundle 與 52 個測試 target 的 data glob，以及 legged locomotion 測試 catalog。
- 測試：`ai_skills_test`（解析結果、13 種錯誤寫法、一個合法的對照編輯）。

P4b 的實作與決定：
- 只有 chaser 會在武器之間挑；sentry 仍是恰好一個、沒有射程與 cooldown 的武器（沒有距離可以依據）。
- 每個 weapon skill 有自己的 executor 設定（拋物線瞄準、蓄力時間來自那把武器），由 manager 建立。
- 挑選（D14）：只在 agent **空閒**時（沒有 Windup / Active 的 action、沒有在 reload，否則 kernel 不會換武器，
  而且換武器會取消 hold）、而且只在剛開火之後或手上的武器不能用時重新挑；在射程內、cooldown 已結束、有子彈的
  skill 依 `weight` 隨機挑，亂數以 net id 為種子（可重現）。都不能用就保留手上的，繼續靠近。
- **cooldown 從 action 真的開始時算**（看到新的、不是 reload 的 action instance），不是從送出 input 時算：最初
  的版本在 recovery 中換成火箭、input 被忽略、cooldown 卻開始了，30 秒只射出 1 發火箭。
- 跳撲在空中時強制使用 `on_landing.weapon` 那個 skill。
- Attack / Chasing：**任何**武器在射程內就是 Attack（即使在 cooldown，例如火箭剛射出）；開火則要手上的武器
  在射程內而且 cooldown 結束。
- 載入檢查：至少一個武器要能從停下的距離打到（否則會停在打不到的地方）；`chase.attack_range_meters` 改成
  「從停下的距離能打多遠」，原本 resume 距離的檢查照舊有意義。leap 的 `on_landing.weapon` 必須是 agent 的
  其中一個 weapon skill。
- **沒有做**：skill 的 min 距離超出視野範圍的檢查（要查 vision cone 的 collider range）、`lead_ticks ==
  commit_offset + 1` 的檢查；cooldown 只對武器 skill，sp_attack 的 cooldown 仍由原本的 leap / charge 計時。
- 測試：`ai_weapon_choice_test`（catalog 複本中給法師加上火箭：榴彈權重 3、火箭權重 1 + 150 ticks cooldown；
  對 12 m 外的玩家 30 秒：16 發榴彈、4 發火箭，火箭不超過 cooldown 允許的 7 發）。`ai_skills_test` 改為允許多個
  武器、拒絕停下距離打不到與 sentry 多武器。
- 效能：1 km 的 AI 時間約 +6%（446 µs，原本約 420 µs），kernel 不變，總計仍約 7% tick。

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

**狀態**：✅ `claude/ai-controller-p1`（2026-10-24）。

P5 的實作與決定：
- `use: {type: item, value: <item 名稱>}`（item 的 `name` 是機器名稱，例如 `fungible_hover_potion`）。
  item 必須是 consumable，且不能是武器。
- `when`（全部都要成立）：持續條件 `state`、`hp_below`（0–1 的比例）、`missing_status`（status 名稱）；
  事件條件 `on_state_enter`、`on_spawn`、`on_damaged`（只在發生的那個 tick 成立）。另有 `cooldown_ticks` 與
  `once`（一生只用一次；只有 item skill 可以用）。武器與 sp_attack 的 `when` 仍只有 `range_meters`。
- 載入檢查：item skill 一定要有 `when`；只有持續條件時，必須另有 `cooldown_ticks`、`once` 或
  `missing_status`，否則每個 tick 都會用。狀態名稱、status 名稱、item 名稱都要存在。
- 執行：`AgentRuntimeManager::use_item_skills`，在 controller 之後跑（這個 tick 進入的狀態算數）。第一次需要時
  為 agent 建立 inventory；每次使用前，若 item 不在或數量為 0 就補一個（D18：AI 不會用完）；以 server
  （peer 0）送出 Consume request。`missing_status` 只在需要時查 status。
- **kernel 改動（不 bump ABI，沒有新 export）**：agent 的 **Committed Consume** 不再記錄到
  `processed_gameplay_requests_` 與 `pending_gameplay_request_outcomes_`。兩者只在 reset 時清空，dedicated server
  上沒有人 poll；AI 頻繁使用 item 會讓它們一直變大，duplicate 檢查也會跟著變慢。agent 的其他 request 與所有
  被拒絕的 request 照常記錄（`weapon_item_test` 依賴 agent 撿武器被拒絕的 outcome）。**既有問題**：玩家的
  request 在 dedicated server 上同樣只增不減，這次沒有處理。
- 使用 item 會中斷 agent 當下的 action（`PendingActionInterrupt`），和玩家相同。
- **沒有新增**出貨用的 AI item 或讓任何 template 使用 item：這是遊戲設計的決定。測試在 catalog 複本中建立。
- 測試：`ai_item_skills_test`（catalog 複本中的 speed tonic：進入 chasing 的那個 tick 就使用；之後每次
  `speed_up` 4 秒結束就再用，360 ticks 內 3 次，每次都有補貨；outcome 佇列沒有留下東西）；`ai_skills_test`
  新增 2 個合法與 8 個錯誤的 item 寫法。既有紅燈（不改的程式碼上也一樣）：`hover_potion_test`（hover 時間
  在 dbdc197a 改成 1800）、`loadout_test`。

1. 透過 `Kernel_ServerSubmitGameplayRequest`（`requester_peer = 0`、`instigator = agent`、
   `KernelDomainAction_Consume`）使用 item；Committed 後補回數量（D18）。
2. agent 生成時依 template 建立 inventory，放入 skill 引用的 item。
3. `use.type: item`；使用時不扣數量（D18）。
4. 事件條件 `on_spawn`、`on_state_enter`、`on_damaged`，持續條件 `hp_below`、
   `missing_status`。
5. 新增 AI 專用的 item（例如 `enemy_speed_tonic`），用現有的 `speed_up` status。

V1 已確認 speed modifier 對 AI 的移動輸入有效，不需要 kernel 改動。

測試：進入 Chasing 時使用 speed tonic，量測速度；`slow` 蓋掉 `speed_up` 後，
`missing_status` 讓它再用一次。

### P6：hunter

**拆分與進度**：

| 步驟 | 內容 | 狀態 |
|---|---|---|
| P6a | `hunter` 角色與設定、全場 hostile 表、Seek、目標失去後的處理 | ✅ `claude/ai-controller-p1` |
| P6b | 依距離開關 vision（D11）、airship 的 on_alert 增援改成 `inherited` hunter、hunter 的效能量測 | ✅ `claude/ai-controller-p1` |

P6a 的實作與決定：
- 設定：`controllers.hunter {target_source: nearest_hostile|inherited, on_target_lost: nearest_hostile|garrison,
  repath_ticks}`；生成來源也可以寫 `controller: {type: hunter, target_source, ...}` 覆寫。`target_source` 等
  key 只能和 `type: hunter` 一起出現；hunter 不能用在 sentry template。
- `inherited`：spawner 會記下是哪個 carrier 生出這個單位，套用 spec 時取該 carrier **當下**的目標；沒有可以
  繼承的目標就視為已失去。
- broad phase：`AgentRuntimeManager::update_hunters` 每個 tick 從現成的 actor snapshot 建一次 hostile 表（活著
  的玩家），所有 hunter 共用；換目標要近 20% 以上；每 `repath_ticks`（預設 60）重新看一次目標位置並在 navmesh
  上尋路（沒有 navmesh 就直線），之後的重算以 net id 錯開。**目前沒有排除躲在帳篷裡的玩家**（Q3：server 的
  entity state 沒有可用的標記），hunter 會走到帳篷旁邊。
- 狀態：hunter 沒看到東西時從 Idle 進入 `kSeek`（新值 6），沿路徑走；看到任何敵對目標就和 Idle 一樣進入
  Alert；Search / Alert 遺忘後回到 Seek 並立刻重新尋路。在 Seek 中被打和 Idle 相同。Seek 不算交戰。
- 目標永久消失（死亡或離開世界）：`on_target_lost: garrison` → 角色變成 garrison 留在原地；`nearest_hostile`
  → 改找最近的。
- 測試：`ai_hunter_test`（離玩家 60 m、視野 26 m：Seek 175 ticks 後在 25.7 m 看到玩家，接著追擊；玩家被移除後
  變成 garrison）。`ai_role_based_format_test` 與 `ai_controller_spec_test` 改為接受 hunter，並新增 3 個錯誤寫法。

P6b 的實作與決定：
- vision 開關（D11）只對 hunter：最近的敵對目標超過 vision cone 的射程 + 10 m 就 Clear，進入範圍就 Set；沒有
  任何敵對目標時也關掉；hunter 變成其他角色（例如目標消失後變成 garrison）時一定恢復。cone 射程在發現 agent
  時讀一次。
- **出貨的行為改變**：hive airship（37）的 on_alert 增援改成 `controller: {type: hunter, target_source: inherited,
  on_target_lost: garrison}`——投下的 gingerbread 追擊 airship 當下的目標，目標消失後原地成為 garrison。
- 效能（臨時修改 bench、未 commit：1 km 的 1300 個 chaser_grunt 全部改成 hunter，玩家分散 250 m）：AI 434 →
  585 µs（+35%），kernel 1662 → 1911 µs（+15%），總計 6.7% → 7.9% tick；1300 個中 1234 個 vision 關閉。kernel
  增加主要是所有 hunter 都在移動（移動是 kernel 的主要成本），vision 關閉省下的部分被抵消；AI 增加看起來是每個
  hunter 每個 tick 都在轉向與移動（快取 cone 射程後沒有可見差異）。bench 的 1 km 地面和 catalog 的 navmesh 不
  一致，尋路大多退回直線，所以 navmesh 尋路的成本**沒有量到**。flow field（P7）暫時不需要。
- 測試：`ai_hunter_test` 檢查 vision 在 60 m 時關閉、靠近到 36 m 內打開、變成 garrison 後不會失明；
  `flying_units_test` 檢查 airship 投下的單位是 hunter、目標是 airship 正在打的玩家。
- command queue 的修正已合併進這個分支（725c15a0，2026-10-24）。合併後重新量測（1 km、玩家分散）：出貨的 catalog
  AI 461 µs、kernel 1821 µs（7.3% tick）；全部改成 hunter 時 AI 640 µs、kernel 2462 µs（**9.8% tick**）。兩者都
  沒有 queue 警告。比合併前高，是因為之前每個 tick 約 23% 的 command 被丟掉，那些 agent 其實沒有在動；現在
  每個 hunter 都真的在移動。

**前置**：先處理 §11 的 simulation command queue 爆滿（1300 agents 時大量 command 被拒絕）。

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

**P0 驗證結果**（2026-10-23，`-c opt`，臨時測試與 bench 修改都沒有 commit）：

| # | 結果 | 對計劃的影響 |
|---|---|---|
| V1 | ✅ speed modifier 對 AI 有效。gingerbread agent 以 move input 前進：5.01 m/s → 掛 `speed_up` 後 **7.00 m/s** → 到期後 5.00 m/s | P5 不用改 kernel。只量了直接送 input 的路徑；經過 `AgentRuntimeManager` 的 controller 路徑在 P5 再確認 |
| V2 | ✅ AI 可以使用 item。`Kernel_ServerSubmitGameplayRequest`，`requester_peer = 0`（server 擁有 agent）、`instigator = agent`、Consume → Committed，status 掛到 agent 身上。**數量照扣**（2 → 1）。注意：weapon item 的取得有 `PlayerTag` 檢查，consume 沒有 | K2 刪除；D18 改由 game_server 補回數量 |
| V3 | ✅ cursor 暫停已存在：任一成員 `is_engaged`（Alert / Attack）時 `group.holding = true`，cursor 停止前進，也不會被回收 | D7 只剩「其他成員一起進 Alert」 |
| V4 | 1 km、1300 agents：kernel 1807 µs/tick，清掉所有 vision 後 1579 µs（**vision ≈ 228 µs，約 13%**）。對 1300 個 agent 全部 Clear 約 70 µs、全部 Set 約 75–88 µs；Set 之後的第一個 tick 沒有尖峰。200 m、52 agents：vision 約 7 µs | D11 照做（成本極低）；K3 不做 |
| V5 | 基準（open world）：200 m / 52 agents：ai 17.5 µs、kernel 119.7 µs。1 km / 1300 agents：ai 432 µs、kernel 1807 µs，佔 33 ms 的 **7.1%**（players 分散）/ 7.0%（同一隊） | 之後各階段對照這組數字 |
| V6 | 所有傷害都經過 `apply_damage_applications`（`damage_system.cc:164`），`ConfirmedDamage` 已有 `source_net_id`、`hit_position`。現有的 `KernelEventType_DamageApplied` 只有受害者與 `source_peer`，沒有攻擊者 id 與位置；而且 dedicated server 每 tick 只讀 32 個事件，每個都用 info 等級寫 log | K1 做成獨立的 batch export，直接從 `ConfirmedDamage` 收集 |

**P0 順帶發現**（不在本計劃範圍，另外處理）：

- **1 km 的 bench 裡，simulation command queue 爆滿**：`depth=1536 capacity=2048`，
  `rejected_count` 在 sample 期間累積到 14 萬以上。1300 個 agent 每 tick 送的 command
  超過 queue 容量，被拒絕的 command（很可能包含 AI 的移動 / 攻擊）就這樣不見了。hunter
  數量多的時候會直接碰到這個問題，P6 之前要先處理。
- **預設的 `agent_cpu_bench` 在 G 表的「20 agents」那一列中斷**：`unknown prop
  population_group: portal`。main 上就是這樣（與本計劃無關），所以 V5 只用 open world 的數字。

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
| P5 | — | — | 要 |
| P6 | — | — | 要 |
| P7 | 視做法而定 | — | 視做法而定 |
| 傷害倍率 | 很可能 bump | — | 要 |

**成本控制**（每個階段都適用）：

- 先跑最小的 smoke test，再跑相關的測試；不跑整個 repository 的 build / test，
  除非另外要求。
- 只讀和該階段直接相關的檔案。
- 範圍擴大時先停下來說明原因。
- 完成時回報讀了哪些檔案、改了哪些檔案、做了哪些搜尋、跑了哪些 build / test 指令。
