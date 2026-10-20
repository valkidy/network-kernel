# 玩家 Hover 模式實作計劃書

狀態：**方案 A 已選；Q1–Q5 已定案（2026-10-21）；P0 驗證完成，假設成立（§5.1）；P0–P5 全部完成（§5.1–§5.6）；Q6 定案為 (a)。剩下 Unity 端和 C# mirror，由使用者處理。**
分支：計畫書在 `claude/player-hover-plan`，從 `main`（a5544579，ABI 108，snapshot schema 31）開。
實作時另開 `claude/player-hover`。
最後更新：2026-10-21。

---

## 1. 目標與範圍

使用者的需求：

> 玩家使用道具後切換成 hover 狀態，期間離地一段固定高度移動，到期後恢復成一般角色。

已經決定的事（2026-10-21）：

| # | 決定 |
|---|---|
| 1 | 方案 A：**status 驅動**。道具套上一個 status，status 存在的期間就是 hover。 |
| 2 | 轉向 60° 限制**先不做**，等測過手感再決定要不要加。 |
| 3 | 第一版**不開放升降操作**。高度固定為 status 寫的 clearance。 |
| 4 | Knockdown、Leap、ActorLanded 在 hover 時不作用，可以接受。 |
| 5 | 死亡時會播 splatter effect，屍體離不離地看不出來。 |
| 6 | Procedural locomotion 不會配 hover。 |
| 7 | Water bubble 照現有機制運作。 |

在範圍內：
- kernel：status 綁定的新 action `apply_hover_movement`、authority 端的移動 step、owner snapshot 欄位、visual flag、本機玩家 prediction。
- game_server：catalog loader 支援新 action；hover 的 status、道具、prop 的 catalog 內容。

不在範圍內：
- 轉向限制、升降操作。
- Unity 端的外觀和動畫（client 讀 `KERNEL_VISUAL_FLAG_HOVERING` 自己畫）。
- **C# mirror、Unity package 的 build / stage / bump**：使用者處理。
- **merge 到 `feat-unity-plugin`**：使用者處理。

---

## 2. 核心設計：hover 是 Character controller 的一種模式

**不切換 `MovementState::controller_type`。** 玩家在 hover 期間仍然是 `kCharacter`，只是每個 tick 改用 `step_character_at_velocity` 來 step，就跟 water bubble 的 suspension 一樣（`player_movement.cc` 的 Character 分支）。速度這樣算：

```
v.xz = desired_horizontal                     // 和平常一樣：input、knockback 的水平分量、stagger 時歸零等
below = 從 capsule 底部往下 probe 到地面的距離   // 探測距離 max(3h, h+10)，和 kHover 相同
v.y  = hit ? clamp((h - below) / dt, -vs, vs) // h = hover_height，vs = vertical_speed
           : 0                                // 探不到地面（懸崖、坑洞）就維持目前高度
ground_state 強制設成 kAirborne
```

為什麼不直接把 `controller_type` 換成 `kHover`：

| 換成 kHover 會遇到的問題 | 留在 Character 就不會有 |
|---|---|
| client prediction 只支援 Character（`kernel.cc` 的 `build_local_character_movement_config`）。遇到 hover，`predict_local_input` 會直接 return，而且沒有任何提示。 | prediction 繼續走 CharacterVirtual，只是換一個速度 |
| Jolt 的 CharacterVirtual 在 hover 期間不會被 step，位置會停在原地；切回來時要 `reset_character` | CharacterVirtual 一直被 step，切回來不需要任何處理 |
| `sync_prediction_actor_proxies` 會跳過非 Character 的 actor，其他 client 預測時不會被它擋住 | 阻擋 proxy 不變 |
| 進出 hover 時要換 controller，到期的 tick 兩邊都得做 | 到期就停止覆寫，下一個 tick 自動變回一般的 `step_character` |

代價是 hover 的貼牆滑動改由 CharacterVirtual 處理，不走 `step_hover` 的兩段 sweep。手感可能和 drone 有一點差異，這點在 P0 量測。

### 2.1 每個 tick 的優先順序（authority 端和 prediction 端相同）

1. **dead**：hover 不作用，照一般規則下墜（見 §6 Q1）。
2. **Sheltered**：hover 不作用，人留在入帳篷時被放的位置（見 §6 Q2）。
3. **suspension（water bubble）**：照現有機制，由 suspension 接管。
4. **bubble 破掉後的 free-fall drop lockout**：hover 不作用，Character 靠重力落地。落地時照現有規則處理：有 `KnockdownProfile` 就進入 recovery，否則解除 lockout。lockout 結束後，hover 從地面以 `vs` 升回 clearance。**這跟 hive_airship 目前的行為一致。**
5. **hover status 存在**：依上面的公式 step。knockback 和 pull 只有水平分量會生效，垂直分量被丟掉，跟 `kHover` agent 一樣。
6. 其他情況：一般的 `step_character`。

### 2.2 會跟著改變的行為（已經接受）

- 永遠是 `kAirborne`，所以不會出現 `landed_this_tick`、ActorLanded，也不會因為 knockback 落地而進入 knockdown。唯一的例外是 §2.1 第 4 點的 bubble drop。
- 沒有 `step_height` 和 slope 的限制：遇到箱子、帳篷、牆，都以 `vs` 往上爬過去。
- 飛過懸崖時維持高度，所以 kill floor（`claude/fall-death`，還沒進 main）不會觸發。
- hover 到期時：如果人在空中，會用一般 Character 的方式掉下來，這次落地是正常落地。

---

## 3. Kernel 改動（ABI 108 → 109，snapshot schema 31 → 32）

### 3.1 ABI
- `KernelEntityTriggerActionType_ApplyHoverMovement = 17`。限 status 的 on_apply graph 使用，目標只能是 status 自己的 subject，這點和 `_ApplySuspendMovement` 相同。
- 在 `KernelActionDefinition` 的最後面加上 `float hover_height_meters; float hover_vertical_speed;`。兩者都必須是 finite 且 > 0，上限另訂常數（建議 `KERNEL_MAX_HOVER_HEIGHT 20.0f`；速度沿用 `KERNEL_MAX_SUSPEND_SPEED`）。
  **因為 struct 的 layout 變了，所有 managed mirror 都會跟著位移**，C# mirror 的部分由使用者處理。
- 新增 `KERNEL_VISUAL_FLAG_HOVERING`，暫定 `0x00004000`。目前 visual flag 用到 `0x2000`；動工時要再 grep 確認這個 bit 沒被佔用，原因見 memory：capability bits 不連續。
- 在 `kernel_types.h` 的 ABI 歷史註解裡補上 109 這一條。
- 動工前要先檢查其他 `claude/*` 分支有沒有已經佔用 ABI 109 或相同的 catalog id。

### 3.2 World / simulation
- `ActiveStatusEffect` 新增 `hovers = false; float hover_height; float hover_vertical_speed;`，跟 `suspends_movement` 一樣放在 instance 上。這樣不管是到期、被移除、被取代、還是復活時清空 status，hover 都會跟著結束。
- `world.h`：新增 `active_hover(world, entity)`，寫法對照 `active_suspension`。
- `player_movement.cc`：在 Character 分支裡依 §2.1 的順序接上 hover 的 step。`probe_ground` 目前在 anonymous namespace 裡，要搬到 `movement_solver`（或一個共用的 header），讓 prediction 也能呼叫。
- `ActorImpulseBatch` 的 `gravity_y`：處於 hover 的 Character 也要送 0（`kernel.cc` 現在只看 `kHover`）。不改的話，遠端 client 會把 hover 中的玩家外插成一條拋物線。

### 3.3 Replication
- **owner snapshot**（照 schema 29 送 suspension 的做法）：新增 `has_hover`、`hover_height`、`hover_vertical_speed`、`hover_until_tick`。snapshot schema 升到 32。
- snapshot builder：hover 期間設 `KERNEL_VISUAL_FLAG_HOVERING`，所有 client 都會收到。
- 依照「schema bump touch points」那條 memory，有三個測試把 ABI 和 schema 版本寫死了，要一起改。

### 3.4 本機 prediction（`step_local_character_prediction`）
- 新增 `predicted_hover_*`（height、speed、until_tick），從 owner snapshot 取值，寫法對照 `predicted_suspension_*`。
- 照 §2.1 的同一個順序選要用哪種 step。probe 用 `prediction_physics_world_` 和共用的 `probe_ground`。
- **已知會有的修正：** 道具使用是 server 端處理的，client 要等 snapshot 到了才知道已經開始 hover，前面幾個 tick 會被 reconcile 修正一次。suspension 也有一樣的情況，沿用同樣的處理。到期 tick 已經知道，所以 client 會在同一個 tick 切回來。

---

## 4. Game_server / catalog 改動

- `gameplay_config.cc`：加上 action `apply_hover_movement`，參數為 `height_meters`、`vertical_speed_meters_per_second`。另外要做驗證，並把它納入 hash。
- status `1006_status_effect_hover.yaml`：channel `hover`，`replace_policy: replace`，on_apply 綁定一個新的 graph `action_hover_apply`（裡面是 `apply_hover_movement`，目標是 `event.subject`）。
- 道具 `3036_fungible_hover_potion.yaml`：照 `3002_fungible_potion` 的寫法，`on_item_used` 綁定 `action_apply_status_if_target`（`target: event.target`、`status: hover`）。掉在地上時的 prop 外觀先沿用 potion 的 entity template，id 在動工時再確認。
- 數值暫定：高度 3 m、`vs` 4 m/s、持續 900 ticks（30 s）。見 §6 Q3。

---

## 5. 階段

| 階段 | 內容 | 完成條件 |
|---|---|---|
| **P0 驗證** | 不改 ABI。在測試裡直接呼叫 `step_character_at_velocity` 搭配 clearance 速度，用合成的幾何（平地、箱子、牆、懸崖）量測，v2 地圖留到 P2 再跑 | (a) 平地維持在 h ± 0.05 m；(b) 遇到 1 m 的箱子會爬上去再降回來；(c) server 和 prediction 用同一組 input 時位置一致。只要有一項失敗，就停下來回報，§2 要重新評估 |
| P1 | §3.1 ABI，加上 §4 loader 的部分 | 載入 catalog 時接受新 action，非法的值會被拒絕 |
| P2 | §3.2 authority 端的移動 | 玩家套上 status 後升到 h 並維持；到期後落地；bubble、knockback、stagger、death、shelter 都照 §2.1 的順序處理 |
| P3 | §3.3 replication | owner 收到 hover 相關欄位，visual flag 在 hover 期間成立；remote impulse 的 `gravity_y` 為 0 |
| P4 | §3.4 prediction | 雙 process 測試：hover 期間 reconcile 的誤差在門檻內，不會出現 `fail_client_prediction` |
| P5 | §4 catalog | 使用道具會進入 hover，到期會恢復 |

每個階段只跑跟該階段有關的測試 target。以 main 已知會紅的測試集合當基準（見「main carries a red baseline」那條 memory），比對失敗的是哪些測試，而不是要求全綠。

---

### 5.1 P0 結果（2026-10-21，`player_hover_p0_test`）

只用 `PhysicsWorld` 加 `movement_solver`，沒有 World 也沒有動 ABI。clearance probe 是從 `probe_ground` / `step_hover` 複製過來的。

| 情境 | 結果 |
|---|---|
| (a) 平地 | 第 22 個 tick 到達 3.0 m（3 m ÷ 4 m/s 約 23 ticks），之後的維持誤差是 **0.0000 m** |
| (b) 1 m 箱子 | 經過箱頂中段時 y 在 3.93–4.00，有跟上箱頂的高度；離開後回到 3.0 m，沒有被卡住 |
| 比 clearance 高的牆（5 m） | **會被擋住**（停在 x = -1.37，貼著牆面），不會往上爬。probe 要等到人已經在牆頂上方才看得到牆頂，`kHover` 的 agent 也一樣。地圖上比 3 m 高的牆會擋住 hover 中的玩家 |
| 懸崖 | 經過邊緣時**下沉 0.18 m**（capsule 的圓底打到邊角，量到的離地距離偏大），之後維持在 2.82 m 不再變。這是 probe 本來就有的行為，airship 也會這樣 |
| (d) 切換 | 同一個 CharacterVirtual 中途不 reset：hover 結束後直接接回 `step_character`，22 個 tick 後落地（理論值 23），落地後 y = 0 |
| (c) 兩個 world 的一致性 | 跑 300 ticks、方向一直在變的 input，誤差 **0.000000 m**；對照組只改一個 tick 的 input，就出現 0.24 m 的差距，證明這個比對抓得到差異 |

結論：「留在 Character、只換速度」這個做法成立，可以進入 P1。真正的雙 process 一致性要到 P4 才驗。

### 5.2 P1 結果（2026-10-21）

ABI 108 → 109。動工前檢查過本地 56 個分支，都沒有佔用 109、action type 17 或 visual flag `0x4000`。

- `KernelEntityTriggerActionType_ApplyHoverMovement = 17`
- `KernelActionDefinition` 在 `lock_origin_lift` 後面加了 `hover_height_meters` 和 `hover_vertical_speed`，struct 大了 8 bytes；`KernelActionTriggerDefinition` 有 8 個 action，大了 64 bytes
- `KERNEL_MAX_HOVER_HEIGHT 20.0f`、`KERNEL_VISUAL_FLAG_HOVERING 0x4000`（P1 只定義了常數，P3 才會真正設定它）

驗證分三關，跟 suspension 一樣：game_server loader、kernel 的 trigger compile（`action_graph.cc`）、command preflight（`systems.cc`）。高度和速度**都不接受 0**，這點跟 suspension 不同。套用時只把數值記在 status instance 上，不會清掉 knockback，也不會跳過在帳篷裡的人（Sheltered），每個 tick 怎麼處理交給 P2 的移動 step。

YAML 寫法：
```yaml
- type: apply_hover_movement
  target: params.target
  height_meters: 3.0
  vertical_speed_meters_per_second: 4.0
```

測試（`status_action_block_catalog_test`）：
- 在 on_apply 裡可以正常載入，而且 kernel 接受
- 放在 on_tick 會被拒絕
- 高度為 0 或超過 20、速度為 0 或超過 20、少了速度、多出 suspension 的欄位、未知欄位，都會被拒絕
- 繞過 loader、直接把非法的數值（包括 NaN）塞給 kernel，也會被拒絕

`kernel_c_header_test` 新增的 static assert 都有編譯過；執行期在 `sizeof(RenderEntityState) == 160u` 失敗，這是 main 上本來就紅的。

### 5.3 P2 結果（2026-10-21）

- `world.h`：新增 `active_hover()`。
- `movement_solver`：新增 `hover_vertical_velocity()`，伺服器和 P4 的 prediction 會呼叫同一個函式。
- `player_movement.cc` 的 Character 分支照 §2.1 的順序判斷：dead、Sheltered、suspension、drop/recovery lockout 都排在 hover 前面；controller 不是 Character 時，hover status 不起作用。

**跟 §3.2 不同的地方：** `probe_ground` 沒有搬走。改成在 `movement_solver` 裡寫一份相同的 probe（大約 20 行重複的程式碼）。這樣 `kHover` agent 的程式碼完全沒動，現有 drone 和 airship 的行為不會受影響。

測試（`player_hover_movement_test`）：

| 情境 | 結果 |
|---|---|
| §9 回歸：沒有 hover status 時，跑完 walk、knockback、stagger、bubble 一整串 | 跟改動前錄下的固定數值**完全相同**（position -6532.393735189、velocity -756.999998592、grounded 141）。多掛一個沒有 hover 的 status 時也相同 |
| 上升並維持高度 | 第 22 個 tick 到達 3.0 m，維持誤差 0.0000 m，3 秒內正好走了 15 m（5 m/s）；**沒有任何 ActorLanded**。對照組：真正落地時確實會發出這個事件 |
| status 結束 | 22 個 tick 後落地 |
| 死亡 | 往下掉並落地 |
| Sheltered | 待在原地不升起；離開帳篷後升到 3 m |
| Bubble | 升到 4.5 m，破掉後 28 個 tick 落地，drop lockout 解除後再升回 3.0 m |
| Knockback (6, 8, 0) | 水平推了正好 4.0 m（6 m/s × 20 ticks），垂直變化 0；lockout 結束後玩家重新拿回操控 |
| Stagger | 水平不動，高度維持 |
| kGrounded controller | 套上 hover status 也不會升起 |
| 走完整的 status 套用路徑 | apply_status 觸發 on_apply，再由 apply_hover_movement 把數值記在 instance 上，`expire_tick` 正確；人在帳篷裡時也會記上 |

`simulation_tests/...` 全部跑一遍（`--keep_going`）：只有 `damage_pipeline_test` 和 `hitscan_test` 兩個紅，都是 main 原本就紅的，失敗行和已知紀錄一致，而且這兩個測試都不會呼叫移動程式。

**Q6 計算結果**（從 code 和 catalog 數值算出來，沒有在遊戲裡實測）：
- AI 判斷要不要出手，看的是**平面距離**（`agent_chaser_controller.cc`，`horizontal_distance` 小於等於 `attack_range_meters`）。
- 近戰實際命中是一顆 **3D 球**：球心在敵人腳底往上 1.0 m（`projectile_launch_position`），claw 和 slam 的半徑是 2.2 m，charge horns 是 1.0 m。
- 玩家 hover 在 3 m 時，hitbox 底部在 3.0 m，跟球心差了 2.0 m。claw 和 slam 只有在**水平距離小於約 1.27 m**時才打得到，但所有近戰敵人都會在 `stop_distance_meters: 1.8` 的地方停下來。也就是說，只有玩家自己飛到敵人正上方時才會被打中。charge horns 完全打不到。
- 遠程攻擊不受影響：drone beam、各種 projectile 照樣打得到。

### 5.4 P3 結果（2026-10-21）

Snapshot schema 31 → 32。

- **`MovementState::hovering`**：移動 step 每個 tick 都會重寫這個值，表示這個 tick 是不是 hover 在移動它。visual flag 和 knockback anchor 都讀它。
- **visual flag**：`hovering` 為 true 時設 `HOVERING`，**取代** `FALLING`/`GROUNDED`。不這樣做的話，Unity 會一直以為 hover 中的玩家在下墜，播下墜動畫。`kHover` 的 drone 和 airship 不受影響，它們沒有 status hover。
- **owner snapshot**：actor record 新增 flag bit 11（`kActorSnapshotHasHover`），內容是高度 f32、速度 f32、到期 tick u32，共 12 bytes，只有存在時才佔空間。只要 status 還在就送，就算這個 tick 移動它的不是 hover（例如被 bubble 包住）也一樣，P4 的 prediction 自己照 §2.1 的順序判斷。只送給 owner，帶有 hover 的 agent 會改用完整的 actor record。decoder 會拒絕超出範圍的數值和 NaN。
- **send budget**：大小估算也加上了這 12 bytes。
- **`ActorImpulseBatch`**：`movement.hovering` 為 true 時，`gravity_y = 0`。

**P3 測試抓到 P2 的一個 bug**：被 bubble 包住的 Character 也會走進 Character 分支，hover 的判斷條件原本沒把 suspension 算進去，導致 `hovering` 被設成 true。移動結果本身沒錯，因為 suspension 的速度選擇排在前面，但 visual flag 和 anchor 的重力會錯。修正方式是把 `suspension == nullptr` 加進判斷條件。

測試：
- `player_hover_movement_test`：snapshot 的部分包括 HOVERING 有設、FALLING/GROUNDED 沒設、owner 欄位正確；在 bubble 裡時沒有 HOVERING 但 `has_hover` 還在；status 結束後全部清掉。§9 回歸的固定數值照樣吻合。
- `hover_roundtrip_test`：player 和 agent 的 roundtrip；同時有 suspension 和 hover 的 roundtrip；剛好多 12 bytes，大小估算也一致；6 種非法數值都會被拒絕。
- `hover_end_to_end_test`：兩個 engine 加上 loopback。只有 owner 收到 hover 欄位；`hovering` 時 knockback anchor 的 `gravity_y` 是 0，對照組是 -9.81。
- `protocol_tests/...`、`kernel_tests/...`、`simulation_tests/...` 全部跑一遍：紅了 8 個，名單跟 main 已知的紅燈完全一致；跟封包有關的兩個，失敗行都跟 hover 無關。

### 5.5 P4 結果（2026-10-21）

`step_local_character_prediction` 判斷 hover 的順序跟伺服器完全一樣：
- **不是 dead**：看 owner snapshot 的 DEAD flag，存在 `predicted_local_dead_`。prediction 原本沒有死亡的對應狀態，這是新加的。
- **不在帳篷裡**：`predicted_shelter_net_id_` 為 0。
- **不在 suspension 中**。
- **不在非 knockback 的 lockout 中**：bubble 結束後的 drop 和落地後的倒地起身都算這一類。snapshot 用 `KNOCKED_BACK` 區分它們跟真正的 knockback；伺服器設這個 flag 的條件是 `!free_fall && !recovering`，所以兩邊的判斷剛好對得上。
- **`prediction_tick < hover_until_tick`**：伺服器在同一個 tick 裡會先跑 `simulate_status_effects`，再跑移動，所以到期那個 tick 已經沒有 hover。

高度用的是跟伺服器同一個 `hover_vertical_velocity()`。owner snapshot 的 hover 欄位是在重播 pending input 之前就套用的（`adopt_authoritative_hover`）。本機畫出來的角色，hover 時會設 `HOVERING`，取代 `GROUNDED`/`FALLING`。

測試（`hover_prediction_test`）：
- **伺服器和 prediction 並排比對**：同一個場景（地面加一個 1 m 的箱子），伺服器用自己的 `simulate_actor_movement`，client 用 `step_local_character_prediction`，capsule 參數兩邊都取自 client 的 `build_local_character_movement_config`。流程是走路、第 20 tick 喝下、hover 飛過箱子再繞圈、第 260 tick 到期、落地，共 330 個 tick。**每個 tick 的誤差都是 0.000000 m**，`hovering` 兩邊也每個 tick 一致（共 240 個 tick 在 hover）。
- **對照組**：prediction 不知道 hover 時，誤差最大到 16.18 m。
- **順序**：dead 會掉下來；在帳篷裡時不 hover；bubble 的 rise 和 drop 期間都不 hover，落地後才升回 3 m；收到沒有 hover 的 snapshot 就清除。
- **本機畫面**：顯示 `HOVERING`，沒有 `FALLING`/`GROUNDED`。

**這次沒有驗到的**：snapshot 延遲造成的修正。道具是伺服器端使用的，client 要等 snapshot 到了才知道 hover 開始了，開頭那幾個 tick 會被 reconcile 修正一次，這跟 suspension 一樣。並排比對是在「snapshot 正好在套用的那個 tick 抵達」這個最好的情況下做的。實際的延遲要在 Unity 雙機上看。

回歸：`kernel_tests/...`、`simulation_tests/...`、`protocol_tests/...` 全部跑一遍，紅了 8 個，名單和失敗行都跟 P3 那次相同。`client_mode_test` 通過。

### 5.6 P5 結果（2026-10-21）

新增的 catalog 檔案（動工前查過所有分支，這些 id 都沒有被佔用）：
- `status_effect_templates/1006_status_effect_hover.yaml`：channel `hover`，900 ticks（30 s），`replace`。
- `action_graph_templates/derived/status_effect/action_hover_apply.yaml`：`apply_hover_movement` 3 m、4 m/s。
- `action_graph_templates/common/action_apply_status.yaml`（新的通用 graph）：無條件套用 status，不帶方向。
- `entity_templates/230_prop_hover_potion.yaml`：掉在地上的 prop。**不能丟**，所以沒有 on_collision。
- `item_templates/3036_fungible_hover_potion.yaml`：on_item_used 會把 hover 套在 `event.instigator`（喝的人）身上。
- 取得管道：玩家的初始道具欄加了 3 瓶（放在最後，不影響原本各 slot 的位置）、營地的 loadout 加了 3 瓶。照 §9，要關掉就把這兩處拿掉。

**跟 §4 不同的地方，經使用者同意擴大了範圍：**
1. **Kernel 原本不允許道具套 status。** `valid_item_graph_action`（`item_system.cc`）只接受 damage、spawn entity、spawn projectile、health change、refill weapon reserve 五種 action。現在加上了 `ApplyStatus`，條件跟 entity trigger 一樣：target 必須是事件裡有的對象、方向和 strength 都要合法。kernel 端另外用 `status_id_in_use` 確認 status 存在。ABI 沒有變（enum 本來就有），但**從此任何道具都可以套 status**。
2. **不能沿用 `action_apply_status_if_target`。** 它的 `direction` 是必填參數，而且帶有 `when: event.has_target`；但 Consume 的 event target 是 `request.target_net_id`，喝的人沒指定目標時是 0，status 就不會套上。所以改成新的 `action_apply_status`，target 用 `event.instigator`。

測試（`hover_potion_test`，用 shipped catalog、v2 地圖、真正的 dedicated server）：
- catalog 的內容跟 YAML 寫的一致，kernel 也接受。
- 繞過 loader 塞非法的值，kernel 都會拒絕：status id 9999、status id 0、target 200。
- 玩家站在營地平台上：喝之前 y = 4.785，flags GROUNDED；喝下 2 秒後 y = 7.785（正好高 3.000 m），flags 是 `0x4000`（只有 HOVERING）；30 秒到期前一個 tick 還在 3 m；到期 40 ticks 後 y = 4.785，flags `0x10`（GROUNDED）。

回歸：`//game_server/...` 加上 `item_system_test`，紅了 11 個。其中 6 個在已知的紅燈清單上。另外 5 個（`game_server_api`、`limb_collider`、`listen_host_pose_parity`、`patrol_catalog`、`stagger_config`）不在清單上，所以**切到 main 上實際跑過**，main 上也是紅的，失敗行完全一樣。也就是說 main 本身多了這幾個紅燈，不是 hover 造成的。

## 6. 待決事項

| # | 問題 | 建議 |
|---|---|---|
| Q1 | 死亡時 hover 要不要作用 | **定案**：**不作用，屍體照常下墜。** 這符合現有「死者會掉下來」的規則，復活位置也會在地面上；反正 splatter 會蓋掉畫面 |
| Q2 | 在帳篷裡（Sheltered）時要不要作用 | **定案**：**不作用。** 離開帳篷後，如果 status 還在，就從地面升回去 |
| Q3 | 高度、上升速度、持續時間 | **定案**：3 m、4 m/s、**30 s**。測過手感後再調 |
| Q4 | hover 期間能不能使用武器和道具 | **定案**：**可以**。kernel 不需要做任何限制 |
| Q5 | 重複使用道具時 | **定案**：`replace`：重新計時 |
| Q6 | 敵人近戰能不能打到 hover 中的玩家 | **定案（a）**：接受，hover 本來就是一種躲近戰的手段。算出來的結果是打不到，除非玩家飛到敵人正上方（§5.3） |

---

## 7. Token / cost 約束

- 先做 smoke test：P0 一旦不成立，就不往下做。
- 只讀跟這次改動直接相關的檔案；不做大範圍搜尋；只在需要時跑相關的測試 target，不跑完整的 build 和 test。
- 如果範圍擴大（例如 Q6 需要改 AI），先停下來說明。
- 每個階段結束時回報：讀了哪些檔案、改了哪些檔案、搜尋了哪些東西、跑了哪些 build 和 test 指令。

## 8. 交付給使用者

**實際交付（2026-10-21）：** 分支 `claude/player-hover`，commit 從 096ca019（P0）到 8cb0f032（P5）。

- 分支和 commit。
- **ABI 109 和 snapshot schema 32 都有變動**，client 和 server 必須用同一版。
- **`bundle.bytes` 需要更新**，因為 catalog 新增了 status、item 和 graph。
- C# mirror 需要新增：`_ApplyHoverMovement`、`KernelActionDefinition` 的兩個欄位、`KERNEL_VISUAL_FLAG_HOVERING`。

---

## 9. 不 revert 也能關閉

**設計上的不變式：沒有 hover status，就不會跑到任何新的 code path。** 唯一的入口是 `active_hover(world, entity)`。它回傳 null 時，§2.1 會直接落到第 6 步（一般的 `step_character`），prediction 端也一樣。impulse 的 `gravity_y`、visual flag 和 owner snapshot 欄位，也只在 hover 期間才會不同。

所以關閉時只要讓 status 不會被套上：

| 層級 | 做法 | 需要重新 build 嗎 | 效果 |
|---|---|---|---|
| 1. 道具來源（建議） | 移除 `3036_fungible_hover_potion` 所有的取得管道：地圖上擺放的、營地庫存、掉落 | 不用。改 catalog，再重新產生 `bundle.bytes` | 玩家拿不到道具 |
| 2. 道具本身 | 把道具 `on_item_used` 的綁定移掉，或刪掉 status 1006 和 graph `action_hover_apply` | 不用。改 catalog | 道具用了也沒有效果，或整個道具不存在 |
| 3. 臨時調整 | 把 status 的 `duration_ticks` 調到很小，或把 `height_meters` 調低 | 不用 | 不完全關閉，用來比較手感 |

**關不掉的部分：** ABI 109 和 snapshot schema 32 的格式變動。這部分在功能上不會有作用（欄位一直是 0，flag 永遠不會被設），但 client 和 server 仍然必須用同一版。要回到 108 和 31，只能 revert。

**用測試保護這個不變式（P2、P3 一起交付）：**
- 回歸測試：同一個 scene、同一串 input，**沒有 hover status 時**，每個 tick 的玩家位置、速度、ground_state 和 visual flags，要跟改動前的固定數值完全一致。包括 knockback、bubble、stagger 各一種情境。
- `probe_ground` 從 anonymous namespace 搬到共用 header 是純粹的 refactor，現有 `kHover` agent（drone、airship）的測試必須保持原本的結果。
- 依照「Test assertions can be vacuous」那條 memory，每個測試都要附一個對照組（有 hover status 時位置確實不一樣），證明這個測試真的抓得到差異。
