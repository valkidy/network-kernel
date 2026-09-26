# Remote Presentation — Next Implementation Plan

Status: **Planned（下一單）**

- 基準分支：`claude/thrown-prop-end-timing`（`2dc3e27`）
- 同步政策：`docs/NETCODE_SYNC_POLICY.md`，尤其是 § Remote Entities、
  § Render Clock and AI Movement Intent (Next)
- 延後項目：§ Homing Projectiles (Deferred)（不在本文件範圍）

本文件把「遠端呈現 jitter」這條工作線上尚未處理的項目整理成可實作的計畫：
共用畫面時鐘（W1）、AI 移動意圖同步（W7），以及討論中發現、尚未排程的
W2–W6 與 W5a。每一項都寫明現況（含程式碼位置）、設計、改動範圍、版本影響、
測試與驗收標準。

---

## 1. 背景：已完成的部分

| 項目 | 內容 | Commit | 版本影響 |
|---|---|---|---|
| 量測 | dedicated server 每 150 tick 輸出 `tick cadence:`；`snapshot_bandwidth_benchmark` 表 C/D（各距離帶的 p50/p90 與凍結比例） | `2f0a196` | 無 |
| 步驟 1 | 遠端 actor 用自己的前後樣本跨缺口內插，缺下一個樣本時水平外推最多 0.25 s | `19bbf20` | 無 |
| 步驟 2 | 擊退 anchor：`ActorImpulseBatch`（reliable），client 依 solver 的逐 tick 公式重播飛行 | `274ed2e` | packet schema 25 |
| a | 投擲 prop 的結束、server-only projectile 的開始與結束，改依畫面時間生效 | `2eb4a18` | 無 |
| b | 以 anchor 繪製的飛行中 prop 不再進入 snapshot send set | `2eb4a18` | 無 |

已知的量測數據（`-c opt`，2026-09-26）：

- 每個 snapshot 固定只能放 32 隻 idle agent，或 19 隻戰鬥中的 agent。
- 40 隻戰鬥中：中距離帶 49% 的區間缺樣本；80 隻時近距離帶 50%；200 隻時 80–93%。
- server tick：實際遊玩約 60 隻 agent 時，平均 2–4 ms、最慢 9.4 ms，`late=0`。
  每隻 agent 的成本是 `agent_cpu_bench` 的 15–20 倍，另案調查中。

---

## 2. 工作項目總覽

| ID | 項目 | 修改層級 | ABI | 封包 / snapshot | 前置條件 |
|---|---|---|---|---|---|
| G0 | Unity 驗證步驟 1、2、a、b | — | — | — | — |
| **W1** | 共用畫面時鐘可有上限地超前最新 snapshot | client | 無 | 無 | G0 |
| W2 | 戰鬥事件依畫面時間釋放 | server | 無 | 無（欄位已存在） | 建議與 W1 同批 |
| W3 | 戰鬥事件依 relevance 過濾 | server | 無 | 無 | 與 W2 同一段送出程式碼 |
| W4 | 受擊呈現事件的預算 | server / 設定 | 視方案而定 | 視方案而定 | G0 量測 |
| W5 | client 預測 projectile 碰撞時納入 actor | client | 無 | 無 | G0 確認可見度 |
| W5a | 預測 projectile 壽命到期時在本地結束 | client | 無 | 無 | 無 |
| W6 | 自己投擲的預測 | client + server | 可能 | packet 升版 | 獨立一單 |
| G1 | bench：畫出位置 vs 真實位置誤差 | test | — | — | W1 |
| G2 | Hermite 內插、提高 snapshot 預算 | client / 設定 | 無 | 無 | G1 |
| **W7** | AI 移動意圖同步 | game_server + kernel + client | **升版** | packet 升版 | W1、G1、G2 |

建議順序：

```text
G0 ─→ W1 ─┬─→ G1 ─→ G2 ─→ W7（先做 patrol，再 sentry，chaser 最後）
          └─→ W2 + W3
G0 ─→ W4（先看量測，有掉才動）
G0 ─→ W5（確認玩家看得出來再做）
W5a：不依賴 G0 或 W1，可提前或與 W1 並行
W6：獨立一單，時機由內容需求決定
```

---

## 3. G0 — Unity 驗證（前置）

驗證清單見先前的討論，重點是區分兩種停頓：

| 現象 | 代表什麼 |
|---|---|
| 所有東西同時停住、再同時跳動 | snapshot 串流整體遲到，W1 要處理的問題 |
| 只有個別物件停住再跳 | 缺樣本。應該已被步驟 1、2、a 解決；若仍出現，是回歸 |

要收集的資料：

- server 的 `tick cadence:` 行
- client `KernelNetworkStats` 的 `rtt_us`、`jitter_us`、`loss_ratio`、
  `remote_presentation_budget_dropped`、`remote_presentation_stale_dropped`
- 每幀狀態為 `RenderEntityStatus_Stale` 的遠端 actor 數量

**過關條件：** 列出剩下的現象，並依第 2 節的順序調整優先權。

---

## 4. W1 — 共用畫面時鐘

### 4.1 現況

`KernelEngine::client_render_server_time_us` 先算出目標時間：
`server_now - 2 個 snapshot interval`。沒有 clock sync 時，則是
`最新 tick - 2 個 interval`。算完後，再把結果 **clamp 到 buffer 的
[最舊, 最新] snapshot 之間**。

所有「世界時間軸」上的東西都讀這同一個時間：

| 使用者 | 讀取方式 |
|---|---|
| actor / prop / projectile 內插 | `build_interpolated_snapshot` → `build_interpolated_snapshot_for_server_time` |
| 擊退 anchor | 經由上一行的 bridging（`apply_knockback_anchor`） |
| 投擲 prop 的曲線 | `rebuild_render_states_from_snapshot` 裡的 `thrown_render_time_us` |
| 延後的刪除事件 | `render_server_time_us_` → `release_deferred_flight_despawns` |
| server-only projectile 的出現時機 | 同一個函式裡的 `before_its_spawn` |
| 依畫面時間釋放的事件 | `current_render_time_us_`（取整到 tick）→ `release_presentable_events` 與 `release_remote_action_presentation_events` |
| 遠端腿部重建 | `step_follower_locomotion_tick`（逐 tick 呼叫同一個內插函式） |

問題有兩個：

1. **串流整體遲到超過內插延遲（133 ms）時**，時間停在最新 snapshot，
   所有東西一起停住，下一個 snapshot 到了再一起跳。實測一個投擲 prop：
   停在 24.0 m，tick 44 的 snapshot 一到就跳到 27.2 m。
2. **時鐘偏移是用每秒一次的 ping 樣本，以係數 0.25 平滑**
   （`apply_client_clock_offset_sample`）。偏移估計往回修正時，畫面時間
   **會倒退**；往前修正時會一口氣跳過去。

### 4.2 設計

新增一個由 client 持有的 `RenderClock`，取代「每次重新計算再 clamp」的做法。
上表所有使用者都只讀它的輸出。

```text
每次 update（client_render_time_us 前進 dt）：
    target   = server_now_estimate - interpolation_delay
    ceiling  = newest_snapshot_time + overrun_cap
    rate     = 1.0
    若 render < target - catch_up_threshold：rate = 1.1（追上去）
    若 render > target：                  rate = 0.9（放慢，但不倒退）
    render   = min(render + dt * rate, ceiling)
    若 |render - target| > hard_reset_threshold（例如 1 s）：render = clamp(target)
```

參數（初始值，實作時可調）：

| 參數 | 初始值 | 依據 |
|---|---|---|
| `overrun_cap` | 0.25 s | 與 actor 外推上限 `kMaxRemoteActorExtrapolationSeconds` 一致 |
| `catch_up_threshold` | 1 個 tick | 避免在目標附近來回振盪 |
| rate 範圍 | 0.9–1.1 | 肉眼不易察覺的速度變化 |
| `hard_reset_threshold` | 1 s | 斷線重連、長時間暫停 |

**動態內插延遲（第二階段）：**

```text
interpolation_delay = clamp(1 interval + k * jitter_us, 2 intervals, 4 intervals)
```

延遲改變時不直接套用，而是由上面的 rate 機制慢慢收斂過去。
`jitter_us` 已經由 clock sync 在量（`network_stats_.jitter_us`）。

**超過最新 snapshot 時，各使用者的行為：**

| 使用者 | 行為 |
|---|---|
| 遠端 actor | 步驟 1 已處理「只有前一個樣本」的情況，會外推最多 0.25 s |
| 擊退 anchor | 繼續沿曲線，直到 `end_tick` |
| 投擲 prop | 繼續沿曲線 |
| server-only projectile | 維持最後一筆（可接受） |
| 延後的事件 | 用同一個時鐘釋放，順序不變 |

**必須是同一個時鐘。** 如果只讓投擲 prop 用未 clamp 的時間，瓶子會先飛過
撞擊點，才等到還在用 clamp 時間的刪除事件把它移除。

### 4.3 改動範圍

`engine/src/kernel/src/kernel.h/.cc`：

- 新增 `RenderClock` 狀態：`render_us`、`has`、`last_client_time_us`。
- `client_render_server_time_us` 改成讀 `RenderClock`。
- `rebuild_render_states_from_snapshot` 在每幀開始時推進時鐘。
- `current_render_time_us_` 改由時鐘推導（仍取整到 tick，給既有的事件釋放使用）。
- `apply_client_clock_offset_sample` 不變；它只影響 `target`，不直接影響畫面時間。
- reset 路徑（呼叫 `client_snapshot_buffer_.clear()` 的兩處）也要重設時鐘。

### 4.4 版本影響

無 ABI、packet、snapshot 變動。若要把「時鐘超前或放慢的次數」加進
`KernelNetworkStats`，就會改到 ABI，所以第一版只寫 log。

### 4.5 測試

新增 `render_clock_test`（kernel_tests，用 `require`，不要用 `assert`）：

1. **串流中斷：** 最新 snapshot 停在 tick 40，畫面時間請求一路推到超過它。
   投擲 prop 應持續前進到上限才停，不能停在 24.0 m。之後補上 tick 44 的
   snapshot，相鄰兩幀的位移要小於「速度 × 幀時間 × 1.2」，不能一次跳 3.2 m。
2. **偏移往回修正：** 注入一個讓 `target` 倒退 50 ms 的偏移樣本，畫面時間
   必須單調遞增，並以 0.9 倍速收斂。
3. **長時間中斷：** 超過上限後停住，恢復後在有限幀數內收斂，過程中不倒退。
4. **順序：** 超前期間，瓶子的延後刪除和爆炸出現仍在同一時刻發生
   （可沿用 `world_timeline_endings_test` 的 harness）。
5. **咬合檢查：** 把 overrun 改回 0，確認第 1 項測試會失敗。

### 4.6 驗收

- Unity 中用網路模擬（例如延遲 100 ms ± 50 ms、1% 掉包），
  「整個畫面一起停住」的次數和最大跳動距離都要明顯下降。
- `tick cadence` 正常、無掉包的本機測試不能有任何退化。

### 4.7 風險與未決問題

- **超前期間的過衝：** 超前時若有落地或刪除紀錄遲到，投擲 prop 會沿曲線
  多飛一段。以 15 m/s 飛 0.25 s 計，最多約 3.75 m，可能穿進牆或地面。
  **緩解方案：** 超前期間用 client 的 prediction physics world 檢查曲線是否
  撞到地形或靜態障礙，撞到就停在撞擊點。predicted projectile 已有相同做法。
- 若 Unity 動畫有自己依時間推進的部分（例如用 `Time.time` 驅動），
  放慢或加速時可能與 kernel 的畫面時間不同步。需要到 Unity 端確認。

---

## 5. W2 — 戰鬥事件依畫面時間釋放（對應討論中的第 8 項）

### 5.1 現況

- server：`broadcast_combat_events` → `broadcast_reliable_event` →
  `send_reliable_event`。送出的類型有 `FireConfirmed`、`HitConfirmed`、
  `DamageApplied`、`Explosion`（見 `is_authoritative_combat_event`）。
- client 其實已經支援依畫面時間釋放：`handle_client_reliable_event` 看到
  `presentation_time_us != 0`，就會先放進 `pending_presentation_events_`，
  等 `current_render_time_us_` 到了才交給 Unity。
- **但 server 從來沒有設定 `presentation_time_us`**：kernel.cc 裡只有讀取
  這個欄位的地方。所以所有戰鬥事件抵達時都是 0，client 收到就立刻處理。
- 結果：遠端 agent 的揮擊動作是畫在過去（落後約 133 ms），但受傷事件
  立刻出現。玩家會先看到自己受傷，再看到敵人揮刀，也就是最初舉的
  「憑空失去 HP」的例子。

### 5.2 設計

server 逐個 session 送出時設定：

```text
presentation_time_us = tick_time_us(event.tick)
例外（維持 0，立即處理）：
    事件的發起者就是接收端自己的玩家（自己的 FireConfirmed、HitConfirmed），
    因為自己的動作要立即回饋。
```

### 5.3 改動範圍

- `broadcast_reliable_event` 要知道每個接收的 session，才能判斷「發起者是否
  是自己」。
- client 端不需要改。

### 5.4 未決問題（實作前必須先查）

- **自己的 HP 顯示從哪裡來？** 自己的 HP 來自自己的 snapshot record，
  而且是立即套用，不經過畫面時間。只延後事件，HP 條仍可能比揮擊早掉。
  需要確認 Unity 的 HP 條是讀 render state、`HealthChanged` 事件，還是其他
  來源，再決定是否要讓自己的 HP 顯示也依畫面時間生效。
- Unity 是否有邏輯依賴戰鬥事件「立即」抵達，例如擊殺訊息或任務進度？
  這類邏輯應該改成依賴權威狀態，而不是呈現事件。

### 5.5 測試

- 兩個 session 的端對端測試，可參考 `actor_impulse_end_to_end_test` 的
  harness：非發起者收到的 `HitConfirmed` 要帶有 tick 時間，而且要等到畫面
  時間才出現在 `events_`；發起者收到的是 0，並且立即出現。

### 5.6 版本影響

無，欄位已存在於 reliable event 封包中。

---

## 6. W3 — 戰鬥事件依 relevance 過濾（對應第 9 項）

### 6.1 現況

`broadcast_reliable_event` 會送給每一個 welcomed 的 session，完全沒有
relevance 過濾。流量是「玩家數 × 事件數」，而且走 reliable 通道。
一顆 frag 打中 24 隻時，每位玩家都會收到全部事件。

### 6.2 設計

- 只有在事件的主體（`net_id`）在該 session 的 `relevant_entities` 裡，
  或事件牽涉到該 session 的玩家時，才送出。
- `Explosion` 依位置與 relevance 半徑判斷。
- 跟 W2 改的是同一個函式，建議一起做。

### 6.3 未決問題

- 每種事件的 `net_id`、`code` 各代表誰（發起者或目標），實作時要逐一確認。
- Unity 端是否有「全地圖」性質的功能依賴這些事件（例如擊殺訊息）。
  若有，改用另外的通道或依賴權威狀態。

### 6.4 測試

兩個 session 相距超過 relevance 半徑，靠近 A 發生的事件不應送給 B；
B 自己的玩家被打中時，則一定要送。

---

## 7. W4 — 受擊呈現事件的預算（對應第 10 項）

### 7.1 現況

remote presentation 通道（`HitReaction`、狀態效果等）的預設值：

| 參數 | 值 |
|---|---|
| client 預算 | 8 KiB/s（`kDefaultRemotePresentationClientBudgetBytesPerSecond`） |
| 每筆大小 | 32 B |
| 過期時間 | 250 ms |

換算下來，所有事件類型共用每秒約 256 筆。超過預算或過期的會被丟棄，
而且不重送。

### 7.2 做法

**先量測，不先改。** 在 G0 開啟 Detailed stats，觀察 40 / 60 / 80 隻時的
`remote_presentation_budget_dropped` 與 `remote_presentation_stale_dropped`。
若一般戰鬥中持續大於 0，可以考慮：

- 提高預算：預算本來就是 `KernelNetworkStatsConfig` 的欄位，已經可以設定；
- 縮小每筆大小：會改到封包格式；
- 調整各事件類型的優先順序。

---

## 8. W5 — client 預測 projectile 碰撞時納入 actor（對應第 7 項）

### 8.1 現況

`advance_predicted_projectiles` 的碰撞 filter 只有 `kTerrain | kStaticObstacle`。
所以打中 agent 的 projectile，在 client 上會穿過 agent 繼續飛，
直到 server 的刪除事件到達。

### 8.2 設計方向（只影響呈現）

- 把 actor 圖層加進這個只用於呈現的碰撞查詢。可以用 client 的
  prediction proxy（`prediction_proxy_collider_ids_`）。
- 預測命中時先隱藏 projectile，但繼續在背景模擬。如果在一段時間內
  沒有收到 server 的刪除，就讓它重新出現。這是用來處理預測錯誤。
- 傷害仍然只由 server 決定。

### 8.3 困難點

- 自己射出的 projectile 畫在「現在」，遠端 actor 卻是畫在過去（約 133 ms 前）。
  兩者在同一個查詢裡比對，會有系統性誤差，跟 homing 的限制是同一類。
- 誤判命中會讓 projectile 消失後又重新出現，反而更顯眼。

### 8.4 做法

G0 時先觀察「穿過 agent」在實際遊玩中是否明顯，再決定要不要做。

---

## 8a. W5a — 預測 projectile 壽命到期時在本地結束

### 8a.1 問題

自己射出的 projectile 到達射程或壽命終點時不會消失，要等 server 的
despawn 到了才消失，畫面上會多飛一段。

### 8a.2 跟 W1 不是同一類問題

| | 世界時間軸（W1 的對象） | 自己預測的 projectile |
|---|---|---|
| 畫在哪個時間 | 過去（比 server 晚約 133 ms） | 現在（比 server 超前） |
| 結束時的問題 | 結束紀錄比畫面早到，要延後套用（a 已處理） | 結束紀錄比畫面晚到，會多飛一段 |
| 讀的時鐘 | `render_server_time_us_` | `local_prediction_server_tick` |

W1 不會改善這個問題。`handle_client_despawn` 本來就把自己預測的 projectile
排除在延後刪除之外（`!has_predicted_projectile_net_id`），despawn 一到就刪除，
但那時已經晚了：server 在 tick T 結束，client 畫的位置早已超過 T，
despawn 還要再半個 RTT 才會到。多飛的距離約等於「速度 × (RTT + input buffer)」。

### 8a.3 現況：三種終點

`advance_predicted_projectiles`：

| 終點 | client 現在的行為 | 會延遲消失嗎 |
|---|---|---|
| 撞到地形或靜態障礙 | 本地立刻 `locally_terminated`，隱藏 | 不會 |
| 撞到 actor | filter 裡沒有 actor 圖層，直接穿過 | 會，屬於 W5 |
| 壽命或射程到期 | 有 `age_ticks >= max_lifetime_ticks` 檢查，**但綁定後實際上失效** | 會，屬於本項 |

失效的原因：`reconcile_predicted_projectiles` 每收到一個 snapshot，就把
projectile 的基準改成那個 snapshot：

```cpp
predicted->spawn_position = entity.position;     // snapshot 當下的位置，不是發射點
predicted->age_ticks = authoritative_age_ticks;  // local_tick - snapshot tick
```

所以綁定之後，`age_ticks` 代表的是「距離上一個 snapshot 幾個 tick」，
每收到一個 snapshot 就歸零，幾乎不可能達到 `max_lifetime_ticks`。
已由 `predicted_projectile_lifetime_test` 證實：修正前，projectile 在最後一個
snapshot 之後，又從那一刻重新飛了一整段壽命。

### 8a.4 設計

- 軌跡基準（`spawn_position`、`age_ticks`）維持現狀，讓 reconcile 的修正行為不變。
- 另外保存「發射時的 tick」：綁定前用預測時的 `spawn_tick`，綁定後用
  `entity.spawn_tick`。壽命檢查改成比較 `local_tick - spawn_tick` 與
  `max_lifetime_ticks`。
- 到期時設 `locally_terminated`（隱藏），和撞到地形時一樣，不直接刪除。
  物件本身仍由 server 的 despawn 或 action result 移除。
- 原則與 W6 相同：**自己預測的東西，結束也用自己的時間軸。**

### 8a.5 需要先確認

- server 端 projectile 的壽命是否就是 `spawn_tick + lifetime_ticks`，是否差一個 tick。
- homing 會把外推限制在 `kMaxHomingVisualExtrapolationSeconds` 以內，
  壽命到期時要不要也受這個限制。
- area effect 碰撞後會停住並把 `age_ticks` 歸零。它的壽命由
  `area_effect.lifetime_ticks` 決定，要確認新的判斷方式也適用。
- 如果 server 延長了壽命（目前應該不存在這種情況），本地結束會造成誤判。

### 8a.6 測試

- 單一 client 的測試：預測 projectile 綁定後持續收到 snapshot，到達壽命的那個
  tick 時，render states 裡就不能再有它，不必等 despawn。
- 咬合檢查：把壽命檢查改回使用 `age_ticks`，確認測試會失敗。
- 回歸：撞到地形的 projectile，以及停住的 area effect，行為都不能改變。

### 8a.7 版本影響

無，只改 client。

### 8a.8 實作結果（`claude/predicted-projectile-lifetime`）

- server 在同一個 tick 裡先開火、再模擬 projectile，所以 projectile 在 spawn
  tick 當下年齡已經是 1，到 `spawn + lifetime - 1` 那個 tick 結束。client 綁定時，
  用 `local_tick - spawn_tick + 1` 還原同樣的計數。
- 只有 standard projectile 會依壽命在本地結束。area effect 的結束時間由
  `expire_tick = spawn + lifetime` 決定，比 standard 晚一個 tick；beam 由 weapon
  持續刷新。這兩種維持原本的行為。
- 到期時先隱藏，保留 1 秒（`kPredictedProjectileEndedRetentionSeconds`）才刪除。
  這段時間讓 despawn 還找得到它：如果先刪掉，despawn 會被當成世界時間軸上的
  物件而延後處理。另外，離開 relevance 的 deterministic projectile 在 client 上
  會被保留、繼續飛，它只能靠這個逾時來清掉。
- 保留時間的計數在隱藏之後仍會繼續。撞牆而隱藏的 standard projectile，
  也改成在「壽命 + 1 秒」後刪除；在這之前，它只能等 despawn 才會被清掉。
- `client_mode_test` 的 `predicted_projectile_lifetime_cleanup_removes_batch_projectile`
  改成驗證「先隱藏，保留期過後才刪除」。

---

## 9. W6 — 自己投擲的預測（對應第 6 項）

### 9.1 現況

- 投擲沒有做 client 預測。自己丟出的 prop 也畫在世界時間軸上，所以按下後
  要經過「一次來回延遲 + 133 ms」才看到它飛出去。
- 起點是 server 當時的手部位置；自己如果正在移動，瓶子會從身後飛出。
- 投擲有兩種模式（`KernelItemThrowMode`）：
  - `IdentityPreserving`：同一個 entity 被丟出去；
  - `ConsumeAndSpawn`：消耗道具、生成一個新的 prop，fungible bottle 屬於這種。

### 9.2 設計方向

- 參考自己射出的 projectile 的做法：client 立刻在「現在」的時間軸上生成
  一個預測的 prop 外觀，用同一個軌跡公式計算，並帶一個 client action id。
- server 的 spawn 或 prop-state 帶回同一個 id，client 用它把預測物和權威物
  對應起來。這需要升 packet 格式。
- 對應成功後，自己擁有的 prop 要留在「現在」時間軸（跟自己的 projectile
  一樣做快轉），它的結束和爆炸也要用「現在」時間軸。這跟 a 的規則不同，
  必須明確寫成例外：**自己預測的東西，結束也用自己的時間軸。**

### 9.3 範圍

大：會動到投擲請求流程、封包格式、client 的預測與對應邏輯，以及 a 的規則。
建議獨立一單。

---

## 10. G1 / G2 — AI 意圖同步之前的量測與便宜方案

### 10.1 G1：誤差 bench

擴充 `snapshot_bandwidth_benchmark`，或新增一個 bench：

- server 端模擬會移動、會轉彎的 agent（巡邏、繞圈、急停）。
- 用真實的 `build_snapshot_send_set` 產生 send set，加上固定延遲後餵給
  client engine。
- 在每個畫面時間點，比較 `build_interpolated_snapshot_for_server_time`
  畫出的位置，和 server 在同一時間的真實位置。
- 依距離帶，在 40 / 80 / 200 隻時輸出誤差的 p50 / p90 / 最大值，
  以及每秒跳動次數。

### 10.2 G2：便宜方案（依序嘗試）

1. **Hermite 內插：** 在 `bridge_remote_actor_samples` 裡，跨缺口時用前後
   兩個樣本的速度做三次內插，修正轉彎時直線抄近路的誤差。只改 client。
2. **提高 snapshot 預算：** 每位玩家的 snapshot 成本已經可以由 server 設定
   （netcode preset）。預算加倍、各距離帶的間隔大約減半，代價是頻寬
   （目前上限 144 kbit/s）。

**過關條件：** 在目標人數下，G1 的近、中距離帶誤差仍明顯，才進入 W7。

---

## 11. W7 — AI 移動意圖同步

### 11.1 現況

- kernel 沒有「意圖」的概念。路徑在 `game_server`（`PatrolNavigation`、
  `PatrolDirector`、`AgentChaserController`），kernel 每個 tick 只收到每隻
  agent 的一筆移動輸入。
- 巡邏的路徑本來就是轉角點清單（`PatrolNavigation` 會輸出 waypoints）。
  整隊人跟著一個沿路線前進的游標走（`advance_speed_meters_per_second`，
  預設 1.25 m/s），每個成員有自己的隊形位置，另外還有追趕加速。

### 11.2 設計（第一階段：patrol）

以**隊伍**為單位送意圖，不是每隻 agent 各送一筆：

```text
PatrolRouteRecord（reliable，路線或速度改變時才送）：
    group_id, start_tick, waypoints[≤N], cursor_distance_at_start, advance_speed

MemberSlotRecord（成員加入、離開隊形時才送）：
    agent net_id, group_id, slot_offset（相對於游標與行進方向）
    detached 旗標：成員被戰鬥拉走、離開隊形時設定
```

client 端：

```text
成員位置 = cursor_position(t) + 依行進方向旋轉後的 slot_offset
收到 snapshot 樣本時，用擊退 anchor 那套「朝後續樣本修正」的方式吸收誤差
detached 的成員，或沒有意圖的 agent → 回到步驟 1 的 bridging
```

client 只需要沿轉角點走，**不需要 navmesh 查詢**。

### 11.3 改動範圍

| 層 | 內容 |
|---|---|
| C API | 讓 game_server 把路線交給 kernel，例如 `Kernel_ServerSetPatrolRoute`、`Kernel_ServerSetAgentFormationSlot`。**ABI 升版**；macOS（`BUILD.bazel` 內的清單）和 Windows（`.def`）兩份 export 清單都要加；Unity 的 managed ABI 驗證也要跟著升 |
| game_server | `PatrolDirector` / `PatrolGroupRuntime` 在路線、速度、成員變動時呼叫新 API |
| kernel server | 保存路線，依 relevance 送出新的 reliable 封包，**packet schema 升版** |
| kernel client | 保存路線、計算成員位置，並接進 `bridge_remote_actor_samples` |

### 11.4 後續階段

- **sentry：** 幾乎不動，意圖就是「原地、朝向」，價值低，可以最後再看。
- **chaser：** 意圖是「目標 net_id + 速度」，client 朝目標在畫面上的位置追。
  它有跟 homing 相同的時間軸問題（見 NETCODE_SYNC_POLICY § Homing），
  而且意圖變化頻繁，放在最後。

### 11.5 取捨（已寫入 NETCODE_SYNC_POLICY）

- **沒有 W1 的時鐘超前，意圖同步在串流遲到時一樣會停住。**
- 遲到期間若發生轉彎、受擊、死亡，事件會晚到，變成修正而不是停住。
- 意圖走 reliable 通道，掉包時會有隊頭阻塞，修正可能比 snapshot 更晚。

### 11.6 測試

- 端對端：路線紀錄從 server 送到 client，client 算出的成員位置和 server
  在同一 tick 的位置相比，直線段誤差要小於 1 cm；轉角處的誤差要記錄下來。
- detached 成員要回到 bridging；路線被取消時要回到 snapshot。
- G1 的 bench 在 W7 前後各跑一次，比較誤差是否下降。

---

## 12. 版本影響總表

| 項目 | ABI | packet schema | snapshot schema | 需要同時重 build server 和 client | 需要更新 `bundle.bytes` |
|---|---|---|---|---|---|
| W1 | 否 | 否 | 否 | 否（只改 client） | 否 |
| W2 / W3 | 否 | 否 | 否 | 否（只改 server） | 否 |
| W4 | 視方案 | 視方案 | 否 | 視方案 | 否 |
| W5 | 否 | 否 | 否 | 否（只改 client） | 否 |
| W5a | 否 | 否 | 否 | 否（只改 client） | 否 |
| W6 | 可能 | 是 | 否 | 是 | 否 |
| W7 | **是** | **是** | 否 | 是 | 否 |

---

## 13. 共通實作規則

- **測試：** 一律用 `require`，不用 `assert`（`-c opt` 會把 `assert` 連同裡面
  的呼叫一起拿掉）。每個新測試都要把對應的產品程式碼改壞跑一次，確認會
  失敗。
- **回歸：** 以失敗「名單」為準，不以「全綠」為準；需要時用 stash 確認
  是既有的紅燈。
- **分支：** 每一項在 `claude/*` 分支上完成。開分支時明確指定起點
  （`git switch -c <new> <base>`），交接前用 `git log main..HEAD` 列出分支
  帶了哪些 commit。
- **交接內容：** 分支名稱與 commit、有沒有 ABI 或 schema 變動、
  是否需要更新 `bundle.bytes`。
