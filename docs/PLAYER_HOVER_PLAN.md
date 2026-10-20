# 玩家 Hover 模式實作計劃書

狀態：**方案 A 已選；Q1–Q5 已定案（2026-10-21）；P0 驗證完成，假設成立（§5.1）；P1 完成（§5.2）；P2 尚未開始。**
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

## 6. 待決事項

| # | 問題 | 建議 |
|---|---|---|
| Q1 | 死亡時 hover 要不要作用 | **定案**：**不作用，屍體照常下墜。** 這符合現有「死者會掉下來」的規則，復活位置也會在地面上；反正 splatter 會蓋掉畫面 |
| Q2 | 在帳篷裡（Sheltered）時要不要作用 | **定案**：**不作用。** 離開帳篷後，如果 status 還在，就從地面升回去 |
| Q3 | 高度、上升速度、持續時間 | **定案**：3 m、4 m/s、**30 s**。測過手感後再調 |
| Q4 | hover 期間能不能使用武器和道具 | **定案**：**可以**。kernel 不需要做任何限制 |
| Q5 | 重複使用道具時 | **定案**：`replace`：重新計時 |
| Q6 | 敵人近戰能不能打到 hover 中的玩家 | P2 時量測近戰和 charge 的接觸距離是 2D 還是 3D 計算。如果高度 3 m 讓它們打不到，再回報來決定 |

---

## 7. Token / cost 約束

- 先做 smoke test：P0 一旦不成立，就不往下做。
- 只讀跟這次改動直接相關的檔案；不做大範圍搜尋；只在需要時跑相關的測試 target，不跑完整的 build 和 test。
- 如果範圍擴大（例如 Q6 需要改 AI），先停下來說明。
- 每個階段結束時回報：讀了哪些檔案、改了哪些檔案、搜尋了哪些東西、跑了哪些 build 和 test 指令。

## 8. 交付給使用者

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
