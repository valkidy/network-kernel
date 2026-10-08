# 水球武器（泡泡封鎖）實作計劃書

狀態：**設計已定案；P0 驗證完成（§9）；**P1–P6 全部完成（2026-10-08）**，見各節的實作紀錄。**
分支：`claude/water-bubble`，從 `main`（7dc17f9，item-weapon 已 merge，ABI 101）開。
最後更新：2026-10-08。

本文件整理 2026-10-08 的可行性評估與七輪設計討論。第 2 節是決策，第 3 節是 kernel
改動，第 9 節是 P0 驗證結果。

---

## 1. 目標與範圍

設計目標（使用者原話整理）：

> 水球武器，被擊中的單體目標會被包覆在泡泡內，無法做任何動作，逐漸上升（類似
> knock out，但方向往上漂浮），直到 N ticks 後泡泡破掉，目標自然掉落到地面。

在範圍內：
- kernel：三個綁在 status 上的新 action、`apply_status` 的 strength 檢查、懸浮移動
  共用 solver、破掉後接一段落下的 lockout、所有選目標的路徑都跳過被包住的單位、
  本機玩家 prediction。
- game_server：水球的 weapon / projectile / collider / status / graph 的 catalog 內容。

不在範圍內：
- **泡泡的外觀**：client presentation，Unity 端從 status 自己畫。
- **Unity package build / stage / bump 與 C# mirror**：由使用者處理。
- **merge 到 `feat-unity-plugin`**：由使用者處理。

---

## 2. 決策（全部已定案）

| # | 決策 | 備註 |
|---|---|---|
| D1 | 泡泡是一個 **status effect**，持續 N ticks（`duration_ticks`）；server 上沒有泡泡實體 | P0 修正：`StatusEffectStatePacket` 只送給擁有者，遠端 client 要靠新的 visual flag 知道誰被包住（§3.7） |
| D2 | 控制效果拆成**三個綁在 status 上的 action**，從 status 的 `on_apply` 呼叫：`apply_block_actions`、`apply_suspend_movement`、`apply_untargetable` | 照 `apply_speed_modifier` 的寫法：必須帶 `status_instance_id`，status 到期或被移除時一起清掉 |
| D3 | 兩個 graph：① 命中 graph（水球 projectile 的 `on_collision`）只做 `apply_status`；② status 的 `on_apply` graph 做三個控制 action | 持續時間只有一個來源：status 的 `duration_ticks` |
| D4 | 泡泡裡**不受傷、不會被打**：子彈直接穿過；範圍效果（爆炸、擊退、pull、治療、其他 status）也跳過 | 由 `apply_untargetable` 提供 |
| D5 | 上升時保留一點水平飄移，方向用**水球飛行的方向**乘上 status 設定的 `drift` 速度 | 不用隨機，保持 deterministic |
| D6 | 泡泡破掉時**水平速度歸零、直直落下** | |
| D7 | **落下期間不能動作，但可以被打** | `untargetable` 隨 status 結束；落下的封鎖由 §3.4 的 lockout 提供 |
| D8 | 落下沿用現有的 `ImpulseLockout`（`recovering = false` 那一段） | 落地解除、`KnockdownProfile` 起身、擋 AI 寫速度、送 anchor，全部現成。P2 實作時多加了一個 server 端的 `free_fall` 旗標，只有 hover 會讀（見 §3.4 實作紀錄） |
| D9 | 上升那一段**不用** `ImpulseLockout`，另做一個綁 status instance 的 `Suspended` modifier | 生命週期要跟 status 一致 |
| D10 | `apply_status` 加**可選的** `strength` 欄位；有寫時只有 `strength > impulse_resistance` 才掛上 status | 沒寫就不檢查，現有 status 行為不變 |
| D11 | 飛船（`37_hive_airship`，resistance 10）只受傷害、免疫泡泡；水球的 strength ≤ 10 | 傷害是命中 graph 裡另一個 `apply_damage` 節點，不受 D10 影響。巢、帳篷是 prop，本來就不受影響 |
| D12 | 只有 actor 會被包 | 跟 `apply_pull` 一樣：prop 沒有 airtime |
| D13 | 玩家和 AI 在 server 上處理完全一樣；泡泡外觀是 client presentation | |
| D14 | 本機玩家的 prediction 要**自己算出上升軌跡**；server 和 client 呼叫**同一個 solver** | 見 §3.6 |
| D15 | **AI 選目標時跳過被包住的單位** | 見 §3.5 |
| D16 | **drone（hover controller）也會被包住，泡泡破掉後墜落到地面**；sentry（grounded controller）也支援。只排除被 strength 擋掉的單位（飛船） | 取代 P0 時「只支援 character controller」的建議。見 §3.3、§3.4 |
| D17 | drone 的落下重力寫在 drone 自己的 `movement.gravity`（改成 -9.81），不加在 status 上 | 採用第七輪的建議 (b)，使用者未反對。hover 平常不讀這個值，飛行不受影響 |
| D18 | drone 掉進坑裡、往下找不到地面時：lockout 依上限解除，drone **停在當時的高度** | 接受。`step_hover` 看不到地面時本來就維持高度 |
| D19 | drone 落地時**可以跟地面單位疊在一起** | 接受。drone 的碰撞不含其他單位，幾秒後它就飛回 9 m |

---

## 3. Kernel 改動

### 3.1 K1：三個綁在 status 上的 action

新增 `KernelEntityTriggerActionType`：

| 值 | Action | 參數 | 效果 |
|---|---|---|---|
| 12 | `apply_block_actions` | 無 | 擋新動作（`KernelLocalActionResultReason_StatusBlocked = 17`），並**打斷正在進行的動作** |
| 13 | `apply_suspend_movement` | `rise_speed`、`drift_speed` | 不受重力、等速上升，水平以 `drift_speed` 沿飄移方向移動；輸入和 AI 寫速度都不能改 |
| 14 | `apply_untargetable` | 無 | 所有選目標的路徑跳過這個單位（§3.5） |

共同規則（照 `apply_speed_modifier`）：
- 必須帶 `status_instance_id`，否則驗證失敗。也就是只能從 status 的 `on_apply` 呼叫，目標只能是 status 自己的 subject（`self` 或 `event.subject`）。
  game_server 載入、kernel 載入、執行期三層都會擋。

**P1 實作紀錄（`apply_block_actions`）**：
- 和計劃原本寫的不同：**旗標直接放在 `ActiveStatusEffect::blocks_actions` 上**，沒有另外存一份清單。
  status 不論從哪裡結束（到期、`remove_status`、同 channel 被取代、revive 全部清掉），旗標都跟著那個 instance 消失，
  不用像 speed modifier 那樣在五個移除點各補一段清除。P2、P3 的 suspend / untargetable 也照這個做法。
- 打斷改照 `interrupt_sheltered_actions` 的做法，不用 stagger 的 counter：每個 tick 只要單位身上有擋動作的 status，
  就打斷 Windup / Active 的動作，跳過 recovery。因為新動作本來就被擋，第一個 tick 之後就沒有東西可打斷。
- `action_block_reason` 的優先順序：Sheltered → **StatusBlocked** → Staggered → KnockedBack。
- ABI 在 P1 就升到 102（只加 enum 值，`KernelActionDefinition` 大小不變）。P2、P3 的 struct 變動都併進同一個 102，不再另外升版。
- 測試：`//engine/src/tests/simulation_tests:status_action_block_test`（執行期）、`//game_server:status_action_block_catalog_test`（載入）。

### 3.2 K2：`apply_status` 的 strength 檢查

- `apply_status` 加一個可選的 `strength`。有寫時，`strength <= impulse_resistance` 就不掛 status，跟 `apply_impulse`、`apply_pull` 一樣是嚴格大於。
- 沒寫就跳過檢查。現有 status（burn、poison、speed_up、slow）不受影響。
- 檢查放在 `apply_status`，不放在三個控制 action 上。否則 status 還是會掛上去：client 會在飛船上畫泡泡，`untargetable` 也可能已經生效。

**P4 實作紀錄（K2）**：
- `KernelActionDefinition` 加了 `status_strength`（接在 `suspend_drift_speed` 後面，仍在 ABI 102 內）。YAML 寫法是 apply_status 的 `strength: 10.0`，
  和 apply_pull 一樣是**字面數字**，不能由 binding 改；0、負數、非數字都會被拒絕。沒寫就是 0，代表不檢查。
- 檢查放在 commit 一開始的過濾（P3 加的那一段）：`strength <= impulse_resistance` 的 apply_status 被拿掉，同一個 batch 的其他指令（例如傷害）照常執行。
  resistance 不是有限數字時一律擋下，跟 apply_impulse 的規則一樣。
- 測試：`//engine/src/tests/simulation_tests:status_strength_test`（resistance 10 對 strength 10 只受傷不掛 status、9.5 會掛、沒有 resistance 會掛、
  無限大不掛、沒寫 strength 時 resistance 1000 也會掛、不合法的 strength 不能編譯）；catalog 測試確認 YAML 的 strength 編進去了，
  而且正式 catalog 裡飛船（template 37）的 resistance 確實是 10。

### 3.3 K3：懸浮移動（`Suspended`）

存在 `StatusEffectState`，內容是 `status_instance_id`、`rise_speed`、`drift_velocity`（水平向量）。

**飄移方向怎麼來（D5）**：P0 確認 status 的 `on_apply` **拿不到**命中方向（§9-2）。做法：
`apply_status` 加一個可選的 `direction` 來源（graph 寫 `direction: event.direction`），
存進 `ActiveStatusEffect`；`prepare_status_lifecycle_trigger` 建 event 時填進 `event.direction`，
status 的 `on_apply` graph 就能用 `event.direction` 傳給 `apply_suspend_movement`。

移動計算（[player_movement.cc:502](../engine/src/simulation/src/player_movement.cc) 那一串 `if / else if`）：
- 優先順序：dead → sheltered → **suspended** → impulse lockout → stagger → input。
- 被包住的單位：`velocity = (drift_x, rise_speed, drift_z)`，**這一 tick 不加重力**
  （[player_movement.cc:766](../engine/src/simulation/src/player_movement.cc)），也不做往下的落地檢查。
- AI 寫速度的入口（[systems.cc:3201](../engine/src/simulation/src/systems.cc)）跟 lockout 一樣，被包住時拒絕。
- 套用當下如果有 `ImpulseLockout`（正在被擊飛），就移除它，泡泡取代原本的擊退速度。

**三種 controller 的上升怎麼走（D16）**：

| controller | 誰在用 | 上升 |
|---|---|---|
| character | 玩家、所有 gingerbread、chaser | 共用 solver 直接給 Jolt 速度，**繞過** `ground_following_velocity`（否則在地上時 `vy` 會被坡度重算掉）；Jolt 會擋天花板 |
| grounded | 4 個 sentry | 不走原本「往上直接加位移」那段（會穿過天花板），改用 `step_hover` 裡現成的 `sweep`，各方向都有碰撞 |
| hover | drone | 不走 `step_hover`（它會把高度拉回 9 m），一樣改用 `sweep` |

`sweep` 在 [player_movement.cc](../engine/src/simulation/src/player_movement.cc) 的 `step_hover` 上方，被擋時停在碰撞點前面。

### 3.4 K4：泡泡破掉 → 接一段落下的 lockout

`Suspended` 被移除時（status 到期，或被 `remove_status`），如果單位還在空中：
1. 速度設成 `(0, 0, 0)`（D6）。
2. arm 一個一般的 `ImpulseLockout`：`armed_tick = now`；`until_tick` 用現有的
   `knockback_flight_ticks`，從目前高度到**往下找到的實際地面高度**算出要幾個 tick，再加一點餘量。
3. 送一個 `ActorImpulseRecord` anchor：速度 0、單位自己的 `movement.gravity`。
   **`floor_y` 一律填往下找到的實際地面**，不是「被打中時的高度」：drone 被打中時在 9 m 高，
   用舊的定義的話，遠端 client 只會畫到 9 m 就停下來。
4. 之後全部是現有流程：落地解除；如果 actor 有設 `KnockdownProfile`，就進起身階段。中途落在屋頂上會提早解除。

**hover（drone）的落下要新寫一段**：hover 沒有重力也沒有落地狀態，所以 lockout 期間
（而且是泡泡造成的 lockout）不走 `step_hover`，改走一段新的落下計算：每 tick `vy += g·dt`，
往下 `sweep`，碰到可站立的地面就設成 grounded、`landed_this_tick = true`，讓 lockout 依現有規則解除。
- **不能借用 grounded 那段 code**：它第一次執行時會往下找最遠 10 km 內的地面，直接把單位貼上去
  （`has_last_queried_position` 那段，[player_movement.cc:689](../engine/src/simulation/src/player_movement.cc)）。
  hover 從來沒設過這個值，drone 會在落下的第一個 tick 瞬間貼到地上。
- 落地、lockout 解除後，`step_hover` 接手，以 `vertical_speed`（drone 是 3 m/s）飛回 9 m，大約 3 秒。
  想讓它先在地上停一下，可以加 `KnockdownProfile`。
- 往下找不到地面（坑、懸崖外）：lockout 依上限解除，drone 停在當時的高度（D18）。
- 落地時可能跟地面單位疊在一起（D19）。

grounded（sentry）的落下不用新寫：原本那段就有重力和落地檢查，而且 sentry 已經設過 `has_last_queried_position`。

這段封鎖屬於 `suspend_movement` 的語意（「懸浮結束一定是一段不能操控的落下」），
不是 `block_actions` 的語意。所以以後就算單獨用 `suspend_movement`，落下也一樣會被鎖住。

### P2 實作紀錄（K3 + K4）

- **飄移方向**：`apply_status` 加了可選的 `direction:`（`KernelActionDefinition::status_direction_authored` + `direction_source`），
  可以綁 `event.direction`、`event.subject_direction` 或 vec3 預設值。方向存在 `ActiveStatusEffect::applied_direction`，
  只有 status 的 **`on_apply`** 會在 `event.direction` 看到它（`on_tick` / `on_expire` 看到的還是 0）。
  `apply_suspend_movement` 在 graph 執行時就把它換算成水平飄移速度。
- **懸浮狀態**和 P1 的旗標一樣，直接放在 `ActiveStatusEffect` 上（`suspends_movement`、`suspend_rise_speed`、`suspend_drift_velocity`），
  有多個時取最新的那個。另外用一個 server 端的標記 `HeldInSuspension` 記住「這個單位被懸浮過」，用來偵測泡泡破掉。
- **上升**：
  - character controller：`movement_solver::step_character_at_velocity`，是從 `step_character` 拆出來的共用 solver，P5 的 client 預測會呼叫同一個。
  - grounded / hover：`step_suspended`，先水平 `sweep` 再垂直 `sweep`，頂到屋頂就停。
  - 懸浮期間 ground state 一律是空中，輸入、AI 寫速度（`set_velocity`）、進建築都被擋；正在被擊飛時中彈，泡泡會取代擊退；建築裡的住客不會被包。
- **破掉 → 落下**：`settle_status_suspensions(World&, tick, dt)` 在每個 tick 的 status 結算之後、movement 之前執行。
  只要單位有 `HeldInSuspension` 但已經沒有懸浮中的 status，就把速度歸零，往下最多找 50 m 的地面，
  用 `knockback_flight_ticks` 算出落地 tick（加 2 當餘量），arm 一個 `ImpulseLockout{free_fall = true}`，並回傳 anchor 要用的 `floor_y`。
  engine 那層的包裝負責把 anchor 排進 `queue_actor_impulse`。死掉、已經在地上、或重力不是往下的單位不會被 arm。
- **hover 的落下**：lockout 有 `free_fall` 時改走 `step_free_fall`（先加重力再移動，往下 `probe_ground` 找可以站的地面）。
  落地那一 tick 設成 grounded，讓 lockout 依現有規則解除，下一 tick `step_hover` 再接手飛回原高度。
- **anchor 的重力**：`flush_actor_impulses` 對 hover 單位只有在 `free_fall` 時才送真的重力，否則送 0。
  原因是 drone 的 YAML 重力改成 -9.81 之後，一般擊退的 anchor 如果照送，遠端 client 會畫出 drone 在下墜。
- **drone YAML**：`36_beam_drone.yaml` 的 `gravity` 改成 `{0, -9.81, 0}`（D17）。
- 測試：`//engine/src/tests/simulation_tests:status_suspension_test`（7 項）。catalog 載入的情況加在 `//game_server:status_action_block_catalog_test`。
- **延到 P6**：「sentry 類 AI 推不動」要 server 真的跑起來（`set_velocity` 要求 `running_`），放到 P6 的 e2e。

### 3.5 K5：不會被打（`untargetable`）與 AI 跳過

要擋住的每一條路徑都各寫一個測試：

| 路徑 | 位置 | 做法 |
|---|---|---|
| projectile 碰撞 | `projectile_system.cc` `query_projectile_collision_hits_impl` | 見下方 |
| hitscan（含 rewind） | `weapon_system.cc` `find_hitscan_target` | 見下方；rewind 用的是 history Hitbox，要確認被包住的狀態也會記進 history |
| 近戰 | `weapon_system.cc` `apply_melee_damage` | 見下方 |
| beam | `beam_system.cc` `simulate_beams` | 見下方 |
| 範圍效果 | `area_effect_system.cc` `simulate_area_effects` | 收集目標時跳過 |
| targeted strike | `weapon_system.cc` | 選目標時跳過 |
| AI 視野候選 | [kernel.cc:13021](../engine/src/kernel/src/kernel.cc) | 跟死掉的單位、建築住客一樣 `continue`（D15） |

**做法（P0 已確認，§9-3）**：被包住時，在三個地方跟「死掉」走同一個入口，但**只關 hitbox**：
1. `push_collider_into_physics`（[kernel.cc:4866](../engine/src/kernel/src/kernel.cc)）：死掉時整個 entity 的 collider 都關掉（含移動 capsule、limb）。
   被包住時只關 `kActorHitbox` 那一種。projectile、live hitscan、近戰、beam、範圍效果都是查 physics world 的 `kDamageable`，一次全擋。
2. history（[history_buffer.cc:116](../engine/src/sync/src/history_buffer.cc)）：`alive` 改成「活著而且沒被包住」，rewind 的 hitscan 才不會打到被包住當下的位置。
3. 傷害入口：rewind 時射手看到的是「還沒被包住」的畫面時，射擊仍然可能命中；傷害送到時已經被包住，要在 `apply_damage_applications` 丟掉，照 `DamageImmunity` 的做法（不扣血、不 stagger、不發 hit event）。

另外：standalone collision world（測試用，[world.cc:282](../engine/src/world/src/world.cc)）也要同步加這個條件，否則測試結果跟正式 server 不一致。

**P3 實作紀錄（K5）**：
- `apply_untargetable` 是 action type 14，旗標同樣放在 `ActiveStatusEffect::untargetable`。查詢函式 `status_untargetable(const World&, entt::entity)`
  放在 **world 層**（`world.h`），不放在 simulation，因為 history 和 standalone collision world 都在 simulation 下面，依賴方向不能反過來。
- 計劃寫的三處都做了：kernel 的 `push_collider_into_physics` 只關 `kActorHitbox` 和 `kActorLimb`（移動 capsule 保留）、
  standalone collision world、history 的 `alive`、傷害入口照 `Sheltered` 的做法整筆丟掉。AI 視野候選照死亡單位的做法 `continue`。
- **多做了一處**：`execute_action_graph_commands` 一開始就把「打到 untargetable 單位」的傷害、health change、擊退、pull、apply_status 指令拿掉，
  只有真的有要拿掉的指令時才複製。這擋住了「在被包住之前就排好的 event」和「graph 直接指名目標」這兩種漏網的情況。
  status 自己的 lifecycle batch 不受影響（例如再生的 `on_tick`）。負的 health change 本來就走傷害 pipeline，所以也會在傷害入口被丟掉。
- 測試：`//engine/src/tests/simulation_tests:status_untargetable_test`（live 物理查詢、rewind、範圍效果、beam、傷害入口、commit 過濾、status 生命週期，8 項）、
  `//engine/src/tests/kernel_tests:status_untargetable_kernel_test`（kernel 正式的物理世界、丟出去的 prop 的 `on_collision`、AI 視野，3 項）。
  projectile、live hitscan、近戰都查同一個物理世界的 hitbox，由「物理世界不含 hitbox」那兩項涵蓋，沒有各寫一項。

### 3.6 K6：共用 solver 與本機玩家 prediction

- 共用 solver 放在 `movement_solver`：輸入 `Suspended` 的參數，輸出這一 tick 的速度和位移。
  server 的移動計算和 client 的 `step_local_character_prediction` 都呼叫它（D14）。
- 飄移方向是每次命中才決定的，status packet 只有 status id 和 tick，不包含這個資訊；
  impulse anchor 又刻意不送給擁有者。所以**擁有者的 snapshot 要加一組 suspend 欄位**：
  `rise_speed`、`drift_velocity`、結束的 tick。做法照現有的 `impulse_lockout_*` 欄位
  （[snapshot.h:66](../engine/src/sync/public/snapshot.h)），一樣只送給擁有者。
- client 端照 `predicted_impulse_lockout_*`（[kernel.h:1356](../engine/src/kernel/src/kernel.h)）加對應的 prediction 狀態。
  上升時用共用 solver；破掉後交給現有的 lockout prediction。

### 3.7 K7：遠端 client 的外觀與軌跡

**外觀（P0 修正）**：`StatusEffectStatePacket` 只送給擁有者
（[kernel.cc:15783](../engine/src/kernel/src/kernel.cc) `session->player != target` 就不送），
遠端 client 不知道別的單位被包住。改用一個新的 visual flag `kVisualFlagEncased = 0x0400`，
照 `kVisualFlagStaggered` 的做法每 tick 設定；snapshot 的 `visual_flags` 在線上是 u16，
目前只用到 `0x0200`，加 bit 不改格式。被包住時 client 端自己的 prediction 碰撞世界
（[kernel.cc:5330](../engine/src/kernel/src/kernel.cc)，現在只排除 `kVisualFlagDead`）也要排除這個 flag，
否則本機預測的子彈會打中泡泡裡的單位。

**軌跡**：

不用新的 packet。被包住時送一個 `ActorImpulseRecord`：
`velocity = (drift_x, rise_speed, drift_z)`、`gravity_y = 0`。
現有的 `knockback_flight_position_at` 在重力為 0 時算出來就是準確的等速直線移動；
`knockback_flight_ticks` 在重力為 0 時回傳 `lockout_ticks - 1`，也正確。
破掉時再送 §3.4 的第二個 anchor。

**P5 實作紀錄（K6 + K7）**：
- **擁有者 snapshot**：`EntitySnapshot` 加 `has_suspension`、`suspension_velocity`、`suspension_until_tick`（= status 的 `expire_tick`）。
  線上是 actor record 的 flag bit `1u << 10`，16 bytes，只有懸浮期間才佔位置，而且跟 lockout 一樣只送給擁有者。
  帶著它的 agent 會改用完整的 actor record。**snapshot schema 28 → 29**。
- **client 預測**：`step_local_character_prediction` 在 `prediction_tick < suspension_until_tick` 時改走 `step_character_at_velocity`，
  也就是 server 用的同一個 solver。到了結束 tick，client 自己 arm 落下（速度歸零，lockout 上限先給 `KERNEL_MAX_IMPULSE_LOCKOUT_TICKS`，落地就解除），
  不用等一個來回。server 的 lockout 隨 snapshot 到了以後，由現有的 `adopt_authoritative_impulse_lockout` 接手。
  懸浮資料本身每次都直接採用 snapshot 的值（`adopt_authoritative_suspension`），因為開始和結束都只由 server 決定。
- **遠端 client**：
  - 顯示旗標 `KERNEL_VISUAL_FLAG_SUSPENDED = 0x400`（`kVisualFlagSuspended`），在 settle 裡每個 tick 重設。
    因為 game_server 的 `set_state` 會整個覆寫 `visual_flags`，而 settle 在那些指令之後執行。計劃原本取名 `Encased`，改成比較通用的名字。
  - client 預測子彈用的碰撞世界會排除帶這個旗標的單位。**注意**：這裡用「懸浮」代替「打不到」，對水球來說兩者同時成立，但兩者在設計上是不同的 action。
  - 懸浮開始時送一個 anchor：速度等於懸浮速度、重力 0、持續到 status 結束。`flush_actor_impulses` 對懸浮中的單位不需要 lockout。
    泡泡破掉時再送 P2 的落下 anchor。
- 測試：`//engine/src/tests/kernel_tests:suspension_end_to_end_test`（兩個 engine 加 loopback：rise anchor、旗標、擁有者才有懸浮資料、落下 anchor、旗標清除）、
  `//engine/src/tests/protocol_tests:suspension_roundtrip_test`、`client_mode_test` 的 `predicted_suspension_rises_then_drops_like_the_authority`。
- **沒做**：client 端不會在本機玩家被擋住時拒絕自己的動作預測。被擋住期間按下的動作會先預測、再被 server 用 `StatusBlocked` 糾正回來。
  擊退和 stagger 目前也是這樣處理。

---

## 4. Catalog 內容（game_server）

id 在開工時再分配，分配前要先查其他還沒 merge 的分支（memory `parallel-branches-collide-on-numbers`）。

- **status**：`<id>_status_effect_water_bubble.yaml`，`duration_ticks: N`，`on_apply` 綁 `action_status_water_bubble`。
- **graph ②** `action_status_water_bubble`：`apply_block_actions` + `apply_suspend_movement { rise_speed, drift_speed }` + `apply_untargetable`。
- **graph ①** 水球命中 graph：`apply_damage`（如果水球本身要有傷害）+ `apply_status { status: water_bubble, strength: ≤ 10 }`。
- **水球本體**：P2 發現 projectile 的 trigger 路徑**不支援 `apply_status`**，只有 entity（prop）的 trigger 支援。所以水球要照各種瓶子的做法，做成「丟出去的 prop」（item + prop + collider），用 prop 的 `on_collision` 對 `event.target` 執行 `apply_status { direction: event.direction }`。prop 一定要有 `on_collision`（memory `thrown-prop-needs-on-collision`）。

示意（欄位名稱以實作時為準）：

```yaml
# status_effect_templates/<id>_status_effect_water_bubble.yaml
id: <id>
name: water_bubble
kind: status_effect
channel: crowd_control_bubble
duration_ticks: 120
interval_ticks: 0
replace_policy: replace
triggers:
  on_apply:
    action_graph: action_status_water_bubble
    parameters:
      target: event.subject
      rise_speed: 1.0
      drift_speed: 0.3
```

**P6 實作紀錄（catalog 內容）**：

| 檔案 | id | 內容 |
|---|---|---|
| `status_effect_templates/1005_status_effect_water_bubble.yaml` | status 1005 | `water_bubble`，90 ticks（3 秒），`on_apply` → `action_status_water_bubble` |
| `action_graph_templates/action_status_water_bubble.yaml` | — | `apply_block_actions` + `apply_suspend_movement { rise_speed: 1.0, drift_speed: 0.5 }` + `apply_untargetable` |
| `action_graph_templates/action_encase_target_and_break_self_at_collision.yaml` | — | `apply_status { direction, strength: 10.0, when: event.has_target }` + 對自己扣 1 hp（每次碰撞都會，所以丟到地上也會破） |
| `entity_templates/224_prop_water_balloon.yaml` | prop 224 | `on_collision: actor \| terrain \| static_obstacle`，`direction: event.direction` |
| `item_templates/3014_fungible_water_balloon.yaml` | item 3014 | 照 `fungible_pull_bottle`：fungible、一組 3 個、`grenade_shell` 拋物線 |

- 玩家的道具欄最後面加上 3 顆水球（`1_player.yaml`），營地的整備選項也加上（`219_prop_initial_camp.yaml`）。id 開工前掃過全部本地分支，沒有撞號。
- **水球本身沒有傷害**：同一個 batch 裡的傷害會先進傷害 pipeline，等確認時目標已經被包住，反正會被丟掉。
- **碰撞遮罩是 `actor`（所有陣營）**，跟藥水一樣，所以丟到隊友會把隊友包住。這是一個設計選擇，要只打敵人的話改成敵方陣營的遮罩。
- **BUILD 的修正**：graph 現在會指名一個 status，所以凡是會載入整份 catalog 的測試，data 都要有 `status_effect_templates`。補了 8 個 game_server 測試 target，
  以及 bundle 裡的 `tests/test_catalogs/legged_locomotion/gameplay_catalog.yaml`（共用正式的 graph 和 entity 資料夾，原本沒有指定 `status_effect_template_dir`）。
- 測試：`//game_server:water_balloon_test`，玩家實際丟出正式的水球：
  - gingerbread：被包住、`set_velocity` 被拒絕、上升 2.97 m、落回地面、之後可以再設定速度
  - drone：被包住、升到 11.97 m、掉到地面（0.00）、再飛回 9.00 m
  - 飛船：水球在 (7.00, 13.21) 打中而破掉，但沒有被包住
  這同時涵蓋了 P2 延到這裡的「AI 推不動」。

---

## 5. 版本與交付

| 項目 | 變動 | 原因 |
|---|---|---|
| ABI | 101 → **102**（P1 已升） | 新的 action type 與欄位、`apply_status` 的 `strength` 與 `direction`、新的 result reason。101 是 item-weapon，已 merge 進 main |
| action type | **12 / 13 / 14** | 11 已被 item-weapon 的 `RefillWeaponReserve` 占用 |
| result reason | **17**（`StatusBlocked`，P1 已加） | 16 是 item-weapon 的 `ItemAction`。改名成 `StatusBlocked`，因為以後的暈眩也會用同一個 action |
| snapshot schema | 28 → **29** | 擁有者 snapshot 加 suspend 欄位（§3.6）。28 是 item-weapon，已在 main。`kPacketSchemaVersion`（main 上是 30）是另一個版本號，不用動 |
| visual flag | `0x0400` | 不改 snapshot 格式（§3.7） |
| drone YAML | `36_beam_drone.yaml` 的 `gravity` 改成 `{0, -9.81, 0}`（D17） | 註解也要改：hover 平常不讀，被包住後的落下會讀 |
| 要更新版本號的測試 | 3 個 | 見 memory `schema-bump-touch-points` |
| `bundle.bytes` | **需要更新** | 新的 status / graph / weapon / projectile / collider 都要讓 client 拿到 |
| C# mirror | 使用者處理 | 新的 enum 值、action 定義的欄位、snapshot 欄位 |

client 和 server 必須用同一版。

---

## 6. 實作階段

每個階段都在 `claude/water-bubble` 上 commit 並測好。

| 階段 | 內容 | 驗收 |
|---|---|---|
| **P0** | 開工前驗證（§9） | **完成 2026-10-08** |
| **P1** | K1 `apply_block_actions` + status 綁定 + 打斷 | **完成 2026-10-08。** 新動作被拒絕；進行中的動作被打斷；放在命中 graph 會被驗證拒絕；status 被移除或到期時解除；持續中的 beam 被打斷後消失 |
| **P2** | K3 + K4 懸浮與落下 | **完成 2026-10-08**（「sentry 類 AI 推不動」延到 P6）。上升高度剛好是 `rise_speed·N·dt`；飄移方向等於水球飛行方向；破掉時水平歸零；落地的 tick 跟預測一樣；落下期間不能動作；sentry 類 AI 推不動；被擊飛時中彈，泡泡取代擊退；**三種 controller 各一個屋頂下的測試，高度停在屋頂下方**；**drone：從 9 m 落到地面、落地後 lockout 解除並飛回 9 m；往下找不到地面時停在當時高度；anchor 的 `floor_y` 是地面高度** |
| **P3** | K5 untargetable + AI 跳過 | **完成 2026-10-08。** §3.5 每一條路徑各一個測試：子彈穿過、範圍效果跳過；落下期間可以被打；AI 視野看不到 |
| **P4** | K2 strength | **完成 2026-10-08。** 飛船（resistance 10）被打到只受傷不被包；沒寫 strength 的現有 status 行為不變 |
| **P5** | K6 + K7 prediction 與遠端軌跡 | **完成 2026-10-08。** 原本的驗收是「本機 prediction 的上升軌跡和 server 誤差為 0；遠端 anchor 重播誤差為 0」。實際驗證的是：client 預測的上升和飄移距離在 `v·t` 的 1 cm 以內（server 那邊在 P2 也是同樣的標準），兩邊呼叫同一個 solver；anchor 的速度、重力、tick 跟 server 完全相等。**沒有**逐 tick 比對 client 和 server 的軌跡 |
| **P6** | §4 catalog 內容 | **完成 2026-10-08。** e2e：玩家丟水球打中 AI，AI 上升 N ticks、落下、落地；打中 drone，drone 墜落後飛回 |

全部做完後交給使用者，附上：分支名稱、commit、ABI 與 snapshot schema 的變動、`bundle.bytes` 需要更新。

---

## 7. 測試策略

- smoke test 優先：每個階段只跑跟改動直接相關的 test target，不跑全部。
- `main` 本來就有一些紅的測試（memory `main-carries-nine-red-tests`）。
  開工前先記下紅的測試清單，最後比對「新增了哪些紅的」，不要用「全部綠」當標準。
- 斷言要避開 memory `test-assertions-can-be-vacuous` 列的三種陷阱。特別注意：
  `spawn_enemy` 沒有 catalog 時 hp 是 0，死掉的 actor hitbox 會被關掉，範圍查詢會直接找不到
  ——這會讓「untargetable 跳過」的測試永遠是綠的。每個「跳過」的測試都要有一個沒被包住的對照組，而且對照組要真的被打中。
- 需要 dedicated server 的測試要注意 port 衝突（memory `test-port-collisions`）。

---

## 8. 風險

| # | 風險 | 處理 |
|---|---|---|
| R1 | 往上升穿過天花板：P0 確認 character controller 會擋，grounded 不會（§9-1） | **P2 已解決**：grounded 和 hover 的上升改用 `sweep`，三種 controller 的屋頂測試都通過 |
| R2 | drone 的落下是新寫的計算，誤用 grounded 那段會瞬間貼地（§3.4） | **P2 已解決**：新寫 `step_free_fall`；從 10 m 落地的 tick 跟 `knockback_flight_ticks` 的預測一致 |
| R6 | drone 的 beam 是持續好幾個 tick 的攻擊，打斷後有沒有真的停 | **P1 已解決**：打斷時 `release_action_resources` 會刪掉 beam 實體。測試 `a_block_ends_a_held_beam` 用一般玩家的 action 路徑驗證，AI 的 action intent 走同一條路 |
| R9 | main 上大約 15 個 game_server 測試（包含 `hover_controller_test`、`flying_units_test`）的 BUILD 沒有列 `locomotion_skeleton_assets`，gingerbread giant（template 39）加進 catalog 之後就全部載入失敗。原本就有的問題 | P2 暫時補上 dep 確認這兩個測試在 drone 改重力後都通過，然後還原。修正另外開成背景任務 |
| R8 | 舊的按鈕射擊路徑（`simulate_weapons` 在沒有 action commit 時，看 `InputButton_Fire` 直接產生 commit）不經過 `action_block_reason`，所以擋不住。stagger、擊退、進建築也一樣擋不住它，是原本就有的缺口 | 只有玩家會走這條路徑（`PlayerTag`），AI 用 action intent。P1 不處理；要不要補上由使用者決定 |
| R7 | drone 落下、落地的動畫 Unity 端還沒有 | client presentation，使用者處理 |
| R3 | 關掉 hitbox 的做法可能連帶影響移動碰撞或 rewind | §9-3 先確認；不行就改成在每條路徑各加檢查 |
| R4 | 被包住的單位升得太高、飄出地圖 | `rise_speed·N·dt` 是有上限的，authoring 時控制即可 |
| R5 | ABI 或 catalog id 跟其他還沒 merge 的分支撞號 | 開工時檢查，merge 時再檢查一次 |

---

## 9. P0 驗證結果（2026-10-08，只讀 code，沒有 build 或跑測試）

| # | 項目 | 結論 | 對計劃的影響 |
|---|---|---|---|
| 1 | 往上碰撞 | **character controller 會擋**；grounded 不會擋；hover 另外處理 | 見下方 9-1 |
| 2 | 飄移方向 | **拿不到**：status 的 `on_apply` event 是重新建的，只有 subject / instigator / target | §3.3 改成 `apply_status` 帶 `direction` |
| 3 | hitbox 開關 | 可行，但不能整個照抄「死掉」 | §3.5 改寫：只關 hitbox、history、傷害入口三處 |
| 4 | 版本號 | ABI 100、snapshot schema 27；item-weapon 分支已占 ABI 101、snapshot 28、action type 11 | §5 改成 ABI 102、snapshot 29、action 12–14 |
| 5 | status 送給誰 | **只送給擁有者** | §3.7 加 visual flag |

**9-1 往上碰撞**
- character controller（玩家和所有 gingerbread、chaser，catalog 裡 16 個 template）：走 Jolt `CharacterVirtual::ExtendedUpdate`，各方向都有碰撞，
  [movement_solver.cc](../engine/src/simulation/src/movement_solver.cc) 的註解也寫了「天花板會擋住上升」，被擋時 `vy` 會被夾回 0。
  Jolt 的 stick-to-floor 只在「往上速度 ≤ 0」時才把單位拉回地面，所以第一 tick 離地不會被拉回去。
  要注意：單位在地上時，`ground_following_velocity` 會用坡度重算 `vy`，所以套用時必須把 `ground_state` 設成空中（跟 impulse 一樣），共用 solver 也要繞過這個函式。
- grounded controller（4 個 sentry template）：往上位移是直接加上去，**沒有碰撞檢查**（[player_movement.cc:803](../engine/src/simulation/src/player_movement.cc)），會穿過天花板。
- hover controller（2 個 template：drone、飛船）：走 `step_hover`，自己維持高度。
- P0 時的建議是只支援 character controller。**第七輪改定（D16）**：三種都支援，grounded 和 hover 的上升改用 `sweep`，hover 的落下新寫一段（§3.3、§3.4）。

**9-3 hitbox 開關的三個細節**
- 死掉時，`push_collider_into_physics` 把該 entity 的**所有** collider 都關掉，包含移動 capsule 和 limb。被包住不能照抄，只能關 `kActorHitbox`。
- rewind hitscan 查的是 history frame，`alive` 在記錄當下就決定了。要把「被包住」也算進去，否則 rewind 射擊仍然打得到。
- rewind 的時間差代表「射手看到時還沒被包住」的射擊仍然可能命中。所以傷害入口要再檢查一次，這是 D4「不受傷」的最後一道保險。

**還沒用執行驗證的**：以上都是讀 code 的結論。character controller 擋天花板這點有 code 註解佐證，但我沒有實際跑過。P2 會有一個「在屋頂下被包住，高度停在屋頂下方」的測試。

## 10. Token / 成本限制

- smoke test 優先，不跑全部的 build / test，除非使用者要求。
- 只讀跟改動直接相關的檔案，不做大範圍的 repository 搜尋。
- 範圍擴大時（例如 §9 有一項不成立）先停下來說明原因。
- 完成時回報：讀了哪些檔案、改了哪些檔案、做了哪些搜尋、跑了哪些 build / test 指令。
