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
| a/b 修正 | a、b 只看 entity template 的投擲軌跡，但 catalog 把軌跡放在 item template（沒有任何 entity template 有 `throw:`），所以在遊戲中從未生效：瓶子留在 snapshot 裡，停住再跳 4–11 m（G0 實測）。改成先查 item（IdentityPreserving）再查 entity template；server 另外比對 `ThrownPropMotion` 與軌跡 template 一致才排除 | `78a8f23`（`claude/item-thrown-prop-anchor`） | 無 |

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

### 3.1 量測結果（2026-09-26 至 09-27）

**量測工具：** Unity 端的 `RemotePresentationProbe`（unity-network-example `a742b2c`）
每秒輸出一行 `[G0]`，後面接著 `[G0 frozen]`（停住最久的物件）和 `[G0 jump]`（跳動
明細）。只統計遠端物件。判斷「應該在動」時只看水平速度，原因見下面的 template 28。

四次測試都是本機連線：rtt 固定 33.3 ms（等於一個 tick 的量測精度）、jitter 0、
沒有掉包。**所以 W1 要處理的「串流整體遲到」一次都沒有出現過，W1 還沒有被評估。**

| 次序 | 版本 | 長度 / 遠端物件 | 主要結果 |
|---|---|---|---|
| 1 | package `9111ac9`（修正前的基準） | 98 秒 / 最多 69 個 | 除了剛連上時，沒有整體停住。瓶子每次投擲都會跳 7–14 m。個別物件停住的數字很大，但主要是探測程式碼被垂直速度誤導 |
| 2 | 同上 | 240 個時間窗 | 出現一次 3.6 秒的整體停住（client 幀率正常，是串流中斷；當時沒有 server log，原因不明，之後沒再出現）。發現 template 28 的速度問題 |
| 3 | 同上，有 server log | 126 秒 / 最多 69 個 | server 30 Hz、`late=0`、最慢 9.1 ms。用 net_id 對照 server log，確認跳動的都是玩家自己丟的瓶子 → 找到 a/b 從未生效的原因 |
| 4 | server `4fcc491`（包含 `6e4431e`） | 60 秒 / 最多 64 個 | 瓶子跳動 0 次、整體停住 0 次；個別物件停住平均每幀 0.1 個（上一次是 0.85）；大跳動 1 次（template 28） |

**找到並已修正的問題：**

- **a/b 在遊戲中從未生效**：a/b 讀的是 entity template 的投擲軌跡，但 catalog 把軌跡
  放在 item template 上。修正於 `78a8f23`；另外修正飛行途中才進入 relevance 範圍的
  prop 拿不到 anchor 的問題（`4e89f2c`）。見 §1。
- **projectile 撞到東西後還停留約 1.5 秒**：其實是 `rocket_explosion`（area effect，
  壽命 45 tick）在 Unity 裡借用了 projectile 的 placeholder 外觀，kernel 的行為正確。
  Unity 端暫時改用 `Projectile_FireFloor`（unity-network-example `3568216`），正式的
  爆炸外觀還沒做。

**找到、但還沒處理的問題：**

- **template 28 的 agent**：站在地面上（有 Grounded 旗標、y = 0），垂直速度卻持續
  累加重力，數值剛好是 −9.81 × 停住的秒數，最多到 −128 m/s；好幾隻會疊在同一個
  座標上。這是 server 模擬或 AI 的問題。步驟 1 的外推只用水平分量，所以畫面上看
  不出來，但 **G2 的 Hermite 內插會用到這個速度，要在 G2 之前修掉。**
  **已修正（`claude/gingerbread-stuck`）：** nest 讓一整波 unit 以只碰地形的遮罩走出門，
  所以它們會停在同一個出口點（實測最多 7 隻疊在一起）。放行後 actor 之間恢復互相
  阻擋，疊在一起的膠囊會把彼此回報成「地面」，但法線是水平的。移動求解器看到不可
  站立的地面，就在原本的垂直速度上繼續加重力，而 Jolt 在角色被擋住、沒有移動的
  情況下，仍然把原速度傳回來，所以速度一直累加（server 端量到 −327 m/s，client 看到
  的 −128 是量化上限）。修法是求解後只保留實際移動所對應的垂直速度。回歸測試是
  `//game_server:gingerbread_door_stack_test`（完整跑 60 秒遊戲，開啟 actor-blocking）。
  unit 疊在出口本身、以及出門途中被擊飛會穿過 nest，這兩個問題還沒處理。
- **template 32 的 agent**：回報的水平速度大於 0.5 m/s，卻最長停了 8 秒。很可能也是
  在 server 端卡住，可以和 template 28 一起查。
- **擊退落地時的修正**：速度約 15 m/s 的擊退飛行，落地時會被拉回 0.4–0.6 m。量不大，
  需要更多樣本。
- **Stale 的 actor**：每隔約 8 秒出現一次 8–16 個，每次只持續一兩幀。時間點和 nest
  每 8 秒生成一批 agent 對得上，是新 agent 的第一個樣本還沒到，屬於正常現象。

**W4：** 四次測試中 `presBudgetDropped` 都是 0，預算夠用。`presStaleDropped` 平均每秒
約 2.7 筆，偶爾一次爆量約 25 筆（像是一次範圍攻擊打中很多隻）。本機沒有網路延遲
也會過期，比較像 server 端排隊太久，需要再查。

**W5：** 沒有觀察到「穿過 agent」的報告，還無法判斷是否值得做。

**補充事實：** 只有玩家會丟瓶子，AI agent 和 local agent 都不會。ConsumeAndSpawn 生成
的 prop 在 server log 裡記為 `peer=0`，那是因為新生成的 prop 屬於 server，不代表是誰丟的。

**優先順序的調整：** a/b 的修正和 W2 / W3 已經完成。W1 必須在有網路延遲的條件下
重新評估。template 28 的問題排在 G2 之前。

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
| `catch_up_threshold` | ~~1 個 tick~~（已移除，見 §4.9） | 改成每一步直接瞄準目標，不會衝過頭，也就不需要門檻 |
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

### 4.8 實作結果（第一階段，`claude/render-clock`）

- `RenderClock` 只在 client 有 clock sync 時啟用。listen server 的 loopback
  不會遲到；沒有 clock sync 時，目標就是「最新 snapshot − 延遲」，時鐘本來就
  無法超前，所以這兩種情況維持舊的行為，既有測試不受影響。
- **只由 host 的繪製呼叫推進時鐘**，也就是 `get_render_states_at_time` 和
  skeleton presentation。封包處理函式觸發的內部重建用的是 `client_local_time_us_`，
  但 Unity 傳進來的是 `NetworkPresentationClock` 自己累加的時間，兩者不保證相同。
  如果兩種時間基準都去推進同一個時鐘，時鐘會停住或突然跳動。所以內部重建只
  讀時鐘目前的值。
- hard reset 可以往前或往後跳。因為門檻是 1 秒，只有重連或長時間暫停才會
  觸發往後跳。串流中斷期間，時鐘會停在上限，這時不算 reset，也不寫 log。
- log：時鐘被上限擋住的期間結束時，寫一行 `render clock held N ms past the newest snapshot`；
  發生 hard reset 時寫一行。都是 info 等級，每次事件只寫一次。
- `current_render_time_us_` 改成由時鐘推導，仍取整到 tick。依畫面時間釋放的
  事件，在超前期間也會照常釋放。
- 測試：`render_clock_test` 共 5 項，包含計畫的 1–4 項，另加「依畫面時間釋放的
  事件在超前期間照常釋放」。咬合檢查做了四種：overrun 上限設為 0、停用時鐘、
  不放慢、`current_render_time_us_` 改回用 header tick，四種都會讓測試失敗。
- 動態內插延遲（第二階段）還沒做，等 G0 的 `jitter_us` 量測。

**§4.2 的表格有一處與實際不符：** 遠端腿部重建（`update_follower_locomotion`）
推進到的是**最新 snapshot 的 tick**，不讀畫面時鐘。所以超前期間，root 會繼續
外推，腿的姿勢卻停在最後一個 tick，可能看到滑步。這要在 Unity 確認：如果明顯，
再考慮讓腿部重建也跟著外推，或在超前期間暫停步伐動畫。

### 4.9 評估：`render_clock_bench`（2026-09-27）

G0 的四次實測都沒有網路延遲，所以改用 kernel 內的 bench 評估。設定如下：
- 一個遠端 actor 以 3 m/s 繞半徑 10 m 的圓移動；snapshot 每秒 15 個；
- 單程延遲在 [延遲 − jitter, 延遲 + jitter] 之間均勻分布，另有掉包；
- 畫面以 60 fps 繪製，clock sync 精確；
- 每種網路條件的 60 秒輸入完全相同，分別用舊的時鐘（每幀重算，再限制在最新
  snapshot 以內）和新的畫面時鐘各跑一次。

指令：`bazel run -c opt //engine/src/tests/kernel_tests:render_clock_bench`

| 網路 | 時鐘 | 整體停住 | 最長停住 | 最大單步 | 跳動 | 平均 lag |
|---|---|---|---|---|---|---|
| 本機 / LAN 20 ± 5 ms | 舊 | 0% | 0 | 0.05 m | 0 | 133 ms |
| | 新 | 0% | 0 | 0.05 m | 0 | 133 ms |
| 50 ± 25 ms、1% 掉包 | 舊 | 0.7% | 100 ms | 0.35 m | 5 | 134 ms |
| | 新 | 0% | 0 | 0.05 m | 0 | 133 ms |
| 100 ± 50 ms、1% 掉包 | 舊 | **39.2%** | 150 ms | 0.50 m | **289** | 148 ms |
| | 新 | 0% | 0 | 0.05 m | 0 | 133 ms |
| 150 ± 75 ms、2% 掉包 | 舊 | **68.5%** | 233 ms | 0.80 m | **565** | 184 ms |
| | 新 | 0.03% | 17 ms | 0.06 m | 0 | 133 ms |
| 50 ± 25 ms，每 10 秒中斷 400 ms | 舊 | 3.2% | 367 ms | 1.15 m | 5 | 139 ms |
| | 新 | 1.0% | 117 ms | 0.08 m | 0 | 140 ms |
| 50 ± 25 ms，每 10 秒中斷 1.5 秒 | 舊 | 13.0% | 1483 ms | 4.46 m | 5 | 230 ms |
| | 新 | 10.8% | 1233 ms | 3.73 m | 5 | 200 ms |

一步約 5 cm（3 m/s × 1/60 秒）；「跳動」指單步超過正常的三倍。兩種時鐘都沒有
出現畫面時間倒退。

**判讀：**

- **§4.6 的驗收條件在 bench 上達成：** 在 100 ± 50 ms、1% 掉包下，整體停住從
  39% 降到 0，最大跳動從 0.50 m 降到 0.05 m。沒有延遲時兩者相同，沒有退化。
- **中斷時間在 0.25 s 以內會被完全遮住；** 超過時，時鐘停在上限，恢復後用 1.1 倍速
  追回，不會跳。中斷 400 ms 時最長只停 117 ms，最大單步 0.08 m。
- **超過 1 秒的中斷兩者都會跳**（hard reset）。以 1.1 倍速追回 1.2 秒要花 12 秒，
  直接跳過去比較合理。這種情況要從串流本身處理，不是畫面時鐘的範圍。
- 誤差欄位（畫出的位置與同一時刻真實位置的差距）在所有情況都在 3 cm 以內，所以
  沒有列出：路徑是平滑的圓，外推和內插都很準。bench 量的是停住和跳動，不是轉彎時的
  誤差，後者屬於 G1 的範圍。

**bench 找到並修正的問題：** `advance_render_clock` 原本拿推進前的時鐘，和推進後的
目標比較，所以串流穩定時時鐘會一直停在目標前面一幀，實際的內插延遲變成 117 ms
（少了一幀）。另外，「落後超過 1 tick 才加速」的門檻形成一個 0–33 ms 的死區，時鐘
會停在死區裡的任何位置。改成每一步直接瞄準目標，步距限制在真實時間的
0.9–1.1 倍之間，門檻因此移除。`render_clock_test` 新增了一項測試：串流穩定時，
畫面時間必須在目標的 1 ms 以內。舊寫法會讓這項測試失敗。

**還沒驗證的部分：** 腿部滑步（§4.8），以及 `render clock held` 的 log 會不會出現在
Unity Console。這兩項都要在 Unity 裡開網路延遲才能看。GameNetworkingSockets transport
原本沒有開放模擬延遲的設定，後來以環境變數加上（`93eeb7c`），結果見 §4.10。

### 4.10 Unity 實測：模擬網路延遲（2026-09-27）

**設定：** 在 dedicated server 設定 `NETWORK_KERNEL_FAKE_LAG_MS=100`、
`NETWORK_KERNEL_FAKE_JITTER_MS=25`、`NETWORK_KERNEL_FAKE_LOSS_PCT=1`，只作用在
server → client 方向。client 用 Unity package `cf90dc9`，包含 `a2121e3` 和之前所有的修正。

server log 的 `git_commit` 欄位仍然顯示 `4fcc491`，但 server 印出了模擬網路的設定行，
代表執行的是新版程式碼，只是這個標記沒有更新（`bazel run` 沒有重新產生它）。
**判斷 server 版本時不能依賴這個欄位。**

**結果（約 121 秒，遠端物件最多 46 個）：**

| 項目 | 結果 |
|---|---|
| 網路（`[G0]`） | rtt 中位數 167 ms、最高 367 ms；jitter 中位數 33 ms、最高 233 ms；掉包最高 3.4% |
| **整體停住** | **0 次** |
| 個別物件停住 | 每幀平均 0.01 個 |
| 大跳動 | 1 次 |
| server | 30 Hz、`late=0`，最慢 8 ms |
| `presStaleDropped` / `presBudgetDropped` | 平均每秒約 1.7 筆 / 0 |

bench（§4.9）在相近的條件（100 ± 50 ms、1% 掉包）下，舊時鐘有 39% 的幀是整體停住；
這次實測是 0，**§4.6 的驗收條件在遊戲中也達成了。**

**剩下的現象，都不屬於 W1：**

- **唯一一次跳動：** 一隻 template 32 的 agent 一次移動 4.24 m，當時速度 2.25 m/s，
  等於約 1.9 秒沒收到樣本。牠的位置很可能在 relevance 範圍邊緣（約 40 m）；遠處的
  agent 分到的 snapshot 次數少，樣本斷掉超過 0.25 秒的外推上限就會停住，下一個樣本到
  時再跳過去。這是 snapshot 預算的問題（G2），不是時鐘的問題。
- **個別物件停住：** 最長約 1.2–1.3 秒（template 32 和 28），中位數 50–70 ms。原因應該
  和上一項相同，或是 §3.1 記錄的 template 28 卡住問題。

**還沒解決：**

- **kernel 的 log 送不到 Unity：** Editor.log 裡完全沒有 kernel 的 spdlog 輸出，kernel
  也沒有提供 log 回呼的 API，所以 client 端的 `render clock held` / `render clock reset`
  在 Unity 裡看不到。要讓 Unity 看得到，需要在 `KernelNetworkStats` 加計數，或新增 log
  回呼 API，兩者都會改動 ABI。既然整體停住已經是 0，這件事不急。
- **腿部滑步（§4.8）：** 從 log 無法判斷，還需要目測。

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

**查證結果（2026-09-26，unity-network-example `main`，package `6fa82940403c`）：**

- **Unity 目前沒有 HP 顯示。** `Assets/Scripts` 裡沒有任何地方讀 `hp`、
  `max_hp` 或 `HealthChanged`。所以「HP 條比揮擊早掉」目前不會發生。
  如果之後加了 HP 條，要讓它依畫面時間生效。
- Unity 讀取戰鬥事件的只有兩處：
  1. `NetworkRenderStateApplier.ApplyKernelEvents`：本地玩家收到自己的
     `DamageApplied` 時，播放受擊動作。W2 之後，受擊者不是發起者，所以這個
     事件會延到畫面時間才出現，受擊動作剛好會和敵人的揮擊對齊。這正是
     W2 想要的效果。
  2. `LocalAgentPerception.CountDamage`：local agent 用 `DamageApplied` 更新
     `LastDamagedTime`，這是 AI 的決策輸入。W2 之後，agent 對受傷的反應會
     晚約 133 ms。這屬於「依賴事件立即抵達」的邏輯；要嘛接受這個延遲，
     要嘛改成讀權威狀態。
- `FireConfirmed`、`HitConfirmed`、`Explosion` 在 Unity 裡目前沒有使用者。

### 5.5 測試

- 兩個 session 的端對端測試，可參考 `actor_impulse_end_to_end_test` 的
  harness：非發起者收到的 `HitConfirmed` 要帶有 tick 時間，而且要等到畫面
  時間才出現在 `events_`；發起者收到的是 0，並且立即出現。

### 5.6 版本影響

無，欄位已存在於 reliable event 封包中。

---

### 5.7 實作結果（`claude/combat-event-timing`，與 W3 一起）

**§5.1 的前提有誤。** `HitConfirmed` 和 `DamageApplied` 其實有設定
`presentation_time_us`（在 simulation 層的 `damage_system.cc` 設為 `hit_time_us`；
先前只搜尋了 kernel.cc）。多數傷害來源的 `hit_time_us` 是命中當下的 server 時間
（projectile、範圍攻擊、beam、action graph，包含 AI 近戰），hitscan 則是射擊者
開火時看到的時間。所以受害者端的 hit 和 damage 本來就會延到畫面時間，local agent
的 `LastDamagedTime` 也早已是延後的。

用端對端測試（`combat_event_delivery_end_to_end_test`，一個 server 加上射擊者、
受害者、遠處旁觀者三個 client）量到的實際現況：

| 事件 | 射擊者（自己） | 受害者 | 遠處旁觀者 |
|---|---|---|---|
| `FireConfirmed` | 立即 | **立即**（比畫面上的動作早約 100 ms） | **有收到** |
| `HitConfirmed` / `DamageApplied` | **延到畫面時間**（命中提示晚約 133 ms） | 延到畫面時間 | **有收到** |

**改動：** `broadcast_combat_events` 改成逐一 session 判斷。

- 送給誰（W3）：事件的主體（`net_id`）在這個 session 的 relevance 範圍內、主體是
  這個 session 的玩家，或這個 session 就是發起者（`peer_id`），才會送出。
- 什麼時候出現（W2）：發起者自己的事件，`presentation_time_us` 設為 0，立即出現；
  其他人收到的事件如果原本是 0，就填入事件 tick 的時間。已經有值的保留原值
  （hitscan 的回溯時間）。
- `Explosion` 列在廣播清單中，但沒有任何地方會產生這種事件，未處理。

**Unity 端影響：** 用到這些事件的只有兩處，都是 `DamageApplied`：本地玩家的受擊
動作，以及 local agent 的 `LastDamagedTime`。兩者對受害者來說原本就已經延後，
所以行為不變。已確認接受這個延遲：它讓 local agent 和一般玩家在同一個時間點
得知自己被打。

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
（2026-09-27：實測回報火箭打中 agent 後延遲消失，決定實作。）

### 8.5 實作結果（`claude/w5-predicted-actor-hit`）

- 碰撞體來源：`sync_client_render_colliders` 把其他 actor 的 hit collider，
  以「畫面時刻」的位置放進預測物理世界，kind / layer 與 server 的
  `push_collider_into_physics` 相同（`kActorHitbox` / `kDamageable`）。
  沿用 prop 障礙物的 `prediction_obstacle_collider_ids_`，清理路徑共用。
  不放自己、不放帶 dead flag 的 actor。移動查詢不含 `kDamageable`，
  所以不影響本地移動預測。
- 沒有用 `prediction_proxy_collider_ids_`：proxy 是「最新 snapshot + 最多
  3 tick 外插」的位置，不是玩家瞄準時看到的位置。
- 判定：只限自己射出、`Standard`、`hit_response = Destroy` 的 projectile。
  filter 用 server 同一個 `collision_filter_from_mask`（取 template mask 的
  actor / limb 部分），忽略自己。actor 比地形或障礙物近才算。
- 命中後隱藏，但繼續在背景飛。等待 despawn 的時間是
  RTT（沒有量測值時用 100 ms）+ 150 ms，上限 600 ms。
  逾時沒收到 despawn，就在當時位置重新顯示，而且這顆不再預測 actor 命中，
  避免一路閃爍。
- 8.3 的誤差比原先估計小，但不是零：server 回溯上限是 100 ms
  （`kMaxCompensationWindowUs`），client 畫面落後約 RTT/2 + 133 ms。
  以目標移動量計算，誤差約為「RTT + 33 ms」內 actor 走的距離：
  站著不動的目標沒有誤差；4.6 m/s 行走、RTT 100 ms 時約 0.6 m。
  這種誤判由上面的重新顯示處理。
- 未涵蓋：AI 的 deterministic projectile 打中本地玩家（例如 mage 的榴彈），
  以及其他玩家的 projectile。（已由 8.6 補上打中本地玩家的部分。）
- 測試：`predicted_projectile_actor_hit_test`。

### 8.6 延伸：別人的 projectile 打中本地玩家（`claude/w5-incoming-projectile-hit`）

- **為什麼可以準：** AI 和其他玩家的 deterministic projectile 在 client 上也放在
  `predicted_projectiles_`，`reconcile_predicted_projectiles` 把它們推到
  `local_prediction_server_tick`，也就是本地玩家所在的預測時間軸。兩者外推到同一個
  tick，server 在那個 tick 用玩家當時的位置判定（AI 的 projectile 不做回溯），
  所以沒有 8.3 那種時間軸錯開的誤差，只剩本地移動預測本身的誤差。
- **碰撞體：** `sync_prediction_local_hitbox` 每個預測 tick 把本地玩家的 hit
  collider 放在 `predicted_local_entity_` 的位置，kind / layer 與其他 actor 相同
  （`kActorHitbox` / `kDamageable`）。本地玩家死亡、沒有預測實體或 template 不是
  hit 用途時移除。會碰到它的其他查詢都已排除本地玩家：移動不查 `kDamageable`，
  自己的 projectile 與投擲的落地掃描都設了 `ignored_entity_net_id`。
- **判定：** 不是自己射出的 `Standard` + `Destroy` projectile，查詢結果只保留本地
  玩家；畫在過去的其他 actor 一律不算。隱藏、重新顯示的規則與 8.5 相同。
- **已知限制：** 還沒被 snapshot bind 的 projectile（生成後第一個 snapshot 之前），
  它的 despawn 會被當成世界時間軸物件延後處理；它已經隱藏，所以畫面上沒有差別。
- 測試：`predicted_projectile_actor_hit_test` 新增 4 個情境（命中位置、跟著預測位置
  移動、死亡與擋在前面的其他 actor、自己的 projectile 不受本地 hitbox 影響）。
  咬合檢查：拿掉「只保留本地玩家」的過濾，或不放本地 hitbox，都會失敗。

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

### 9.4 確認結果（2026-09-27，讀程式碼）

**對應預測物和權威 prop：不用改封包格式。**
- 玩家實際在丟的瓶子（shockwave、frag、glyph、magic 等）全部是
  `identity_preserving`，只有 `grenade_consumable` 是 `consume_and_spawn`。
  9.1 說 fungible bottle 屬於 ConsumeAndSpawn 是錯的。
- `identity_preserving` 的投擲把 prop 的 net_id 填進回覆的 `prop_entity_id`
  （`item_gameplay_system.cc`），回覆以 reliable 送回發出請求的 client，帶著
  `request_id`。`request_id` 由呼叫端決定，Unity 的 `ItemPropRequestSender`
  從 1 遞增、不為 0。
- `consume_and_spawn` 的 prop 由 action graph 事後生成，回覆沒有 net_id；
  W6 先不涵蓋。

**從身後飛出：會，主因是畫面時間軸，不是 server 的起點。**
- server 在收到請求的 poll 立刻處理，起點是當時玩家位置 + 1 m；移動輸入在
  下一個 tick 才套用，沒有輸入緩衝，所以起點只比 client 按下時晚約 1 tick
  （5 m/s 時約 0.17 m）。
- 丟出的 prop 沒有「自己的」特例，跟別人的一樣用畫面時間（落後約 133 ms）
  從錨點推算；自己的玩家則畫在預測的「現在」。瓶子出現時玩家已經往前走了
  約「速度 ×（RTT + 133 ms）」：本機約 0.7 m、50 ms RTT 約 0.9 m、100 ms 約 1.2 m。

### 9.5 實作（`claude/w6-local-throw-prediction`）

client（`kernel.cc`，`PredictedThrow`）：
- `submit_gameplay_request` 送出背包的 identity-preserving 投擲時，立刻在
  預測時間軸（`prediction_timeline_now_us`：最新預測 tick + 本幀外插，
  最多一個 tick，與本地玩家相同）上從「預測的玩家位置 + 1 m」開始飛，
  用同一個軌跡 template 的速度、模型和重力。
- 回覆到達：被拒絕就移除；成功就記下 prop 的 net_id，並改用
  `entity_id_for_net_id(prop)`，也就是 prop 之後自己會用的 view key。
- 權威錨點到達：改用錨點的起點、初速和 tick，原本畫的位置差成為修正量，
  以 50 ms 半衰期衰減。
- 在權威的飛行結束到達前，每個固定 tick 用預測物理世界（地形、障礙物、
  畫面上的其他 actor）檢查軌跡，撞到就停在撞擊點。
- 配對到的 prop 在預測期間不由世界時間軸畫。prop 的 despawn 立刻套用
  （不延後），預測一起移除；Unity 依 net_id 找到的就是預測的 view，碎裂
  發生在畫面上的落點。飛行結束後、世界時間軸也到達結束 tick 時交還給 prop。
- 沒有回覆 2 秒逾時；存活超過 10 秒也移除。

server（`systems.cc`）：
- 飛行中的 prop 碰撞時，事件的 `owner_peer` 用投擲者的 peer，所以爆炸
  屬於投擲者，client 立刻畫（`before_its_spawn` 對自己的 projectile 不延後）。
  instigator 仍是 0，範圍效果不依 owner 過濾，所以投擲者和同 peer 的旁人
  照樣會被炸到（`thrown_bottle_self_hit_test` 的旁人檢查仍通過）。
- 影響：projectile 之間的互動規則會跳過同 owner 的 projectile，爆炸現在
  和投擲者自己的 projectile 屬於同一個 owner。

Unity（尚未改，`NetworkRenderStateApplier.ShouldRender`）：
- 非 projectile 的 render state 需要 `net_id != 0` 才會畫。預測的瓶子在
  回覆到達前 net_id 為 0，所以要加上「`status == Predicted` 且
  `entity_id != 0` 也畫」。沒有這行時，瓶子在回覆到達（約一個 RTT）後才
  出現，但之後仍畫在自己的時間軸上。

版本：沒有 ABI、封包或 snapshot schema 變動；但 server 的爆炸歸屬和
client 的畫法要一起更新（舊 client 遇到新 server，會在瓶子落地前就畫出爆炸）。

測試：`own_throw_prediction_test`（送出即畫、接上錨點後與 server 同一時刻
位置一致且只畫一次、本地落地判定、拒絕、逾時、despawn 立刻套用），
`thrown_bottle_self_hit_test` 新增爆炸歸屬檢查。


### 9.6 延伸評估與量測（`claude/w6-blast-gap-stats`）

W6 留下三項延伸，讀程式碼後（2026-09-28）：

| 項目 | 目前遊戲會發生嗎 | 原因 |
|---|---|---|
| 丟出手上拿著的 prop | 不會 | Unity 只送 Use / Throw / Pickup，沒有 Carry；撿起來直接進背包 |
| `consume_and_spawn` 投擲 | 不會 | 只有 `grenade_consumable`，不在玩家初始背包，也沒有其他地方引用 |
| 爆炸預測 | 會 | 瓶子在預測落點停住，等 server 的 despawn 和爆炸（約 RTT + 輸入延遲）才炸 |

爆炸預測需要：從 item → prop template → 碰撞 trigger 的 action graph 參數找出爆炸
template、在落點本地生成、用「自己的 + 同 template + 未配對 + 靠近落點」配對
server 的爆炸（它的 action_instance_id 無法預測）、落點誤判時的處理，以及 Unity
碎裂效果的同步。先量停頓再決定要不要做。

量測（client，network stats 開啟時，每次投擲寫 log）：

```
own throw 214: despawn arrived 183 ms after its predicted landing
own throw 214: blast 801 (projectile template 7) arrived 183 ms after its predicted landing, 0.50 m from it
own throw 215: despawn arrived with no predicted landing, 466 ms after release
```

- 時間從預測落地「在畫面上發生」的 client 時間算起（偵測時往回推到落點所在時刻）。
- 爆炸比 despawn 早到或晚到都能配對；同一次落地只配對一個爆炸。
- 「with no predicted landing」表示 client 沒掃到撞擊（例如撞到畫面上沒有的東西），
  這種情況爆炸預測也幫不上忙。
- 距離是 server 爆炸生成點到預測落點的距離，用來估計落點誤判的程度。
- 已知誤差：玩家自己射出、會在撞擊時生成爆炸的 projectile（例如火箭），如果在
  投擲落地後 1 秒內炸開，可能被算到這次投擲上；log 裡的 template id 可以分辨。

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
2. **提高 snapshot 預算：** 預算加倍、各距離帶的間隔大約減半，代價是頻寬
   （目前上限 144 kbit/s）。**更正（G1）：** 預算不能設定，是寫死的常數
   `kSnapshotSendBudgetBytes = 1200`（`network_packets.h`），約等於一個 MTU。

**過關條件：** 在目標人數下，G1 的近、中距離帶誤差仍明顯，才進入 W7。

### 10.3 G1 結果（`remote_actor_error_bench`，2026-09-27）

指令：`bazel run -c opt --config=macos //engine/src/tests/kernel_tests:remote_actor_error_bench`

設定：
- server 端用真實的 `build_relevant_snapshot` / `build_snapshot_send_set`，
  send set 經過 encode / decode；
- agent 以 5 m/s 照腳本移動（6 m 折返、4 m 方形直角轉彎、半徑 2.5 m 繞圈、
  走 2 秒停 1 秒），所以任何時刻的真實位置都已知；
- 單程延遲 50 ± 25 ms、1% 掉包；client 用真實的畫面時鐘與內插，60 fps；
- 每組 30 秒。

每一幀依 client 當時手上的資料分成三類：
- 正常：前後樣本相隔一個間隔；
- 缺口：前後都有，但相隔更久，中間用直線橋接；
- 外推：還沒有更新的樣本，只能用速度往前推，下一個樣本到時再修正。

「跳動」指單幀移動超過 0.25 m（正常一步的三倍）；
「停住」指 agent 實際在動、畫面卻沒動的幀。

**中距離帶（10–25 m）：誤差 p90 / 跳動（次/agent/分）/ 停住**

| 方案 | 80 隻，動作中 | 200 隻，閒置 | 200 隻，動作中 |
|---|---|---|---|
| 現行 1200 B、15 Hz | 0.07 m / 37 / 1.3% | 0.25 m / 76 / 4.9% | 1.33 m / 95 / 42% |
| 預算 2400 B | 0.02 m / 5.7 / 0.3% | 0.03 m / 19 / 0.5% | 0.11 m / 52 / 2.3% |
| 30 Hz × 1200 B（延遲維持 133 ms） | 0.01 m / 3.1 / 0% | 0.03 m / 24 / 0.3% | 0.17 m / 66 / 3.2% |
| 現行 + 延遲多 133 ms | 0.09 m / 6.8 / 0% | 0.20 m / 30 / 1.9% | 0.77 m / 77 / 20% |
| 無預算上限（下限） | 0.01 m / 0.6 / 0% | 0.00 m / 0.4 / 0% | 0.00 m / 0.4 / 0% |

**近距離帶（≤ 10 m）** 在現行預算下，80 隻以內都沒問題（p90 ≤ 2 cm，跳動 ≤ 10）；
200 隻動作中時變成 0.33 m / 85 / 9.8%，預算加倍後回到 0.02 m / 15 / 0.1%。
40 隻動作中時，中距離帶 p90 2 cm、跳動 18 次/分，遠距離帶 26 次/分。

**判讀：**

- **內插本身不是問題。** 每個 agent 每次都送到時，誤差 p90 在 1 cm 以內，
  最大值來自掉包。所以 G2 方案一（Hermite）能改善的範圍很小：跨缺口橋接的幀，
  即使 200 隻動作中，p90 也只有 0.24 m（中）/ 0.34 m（遠）。**建議先不做。**
- **誤差和跳動主要來自「外推後修正」。** 畫面落後 133 ms，扣掉約 50 ms 延遲，
  只剩約 83 ms 的餘裕；agent 只要被 send set 跳過一次，缺口就是 133 ms，
  畫面時間會先超過它最新的樣本，只能沿速度往前推，下一個樣本到時再拉回。
  遇到折返或轉彎，拉回就是跳。現行預算 200 隻動作中時，中距離帶 87% 的幀
  都在外推。
- **頻寬加倍是最有效的便宜方案。** 80 隻以內近、中距離帶都乾淨；200 隻時
  中距離帶仍每分鐘跳 50–65 次。兩種加倍方式效果接近：
  - 預算 2400 B：一個 snapshot 會超過一個 MTU，被切成兩片，任一片掉了整個
    snapshot 就丟失；
  - 30 Hz × 1200 B：每包仍在一個 MTU 內。但 kernel 的內插延遲寫死為
    「兩個 snapshot 間隔」，30 Hz 時會自動降到 67 ms，扣掉延遲幾乎沒有餘裕。
    **必須先把延遲和 snapshot 頻率脫鉤**（bench 是補回 67 ms 才得到上面的數字）。
- **加大內插延遲** 對 80 隻有效，但每個遠端單位都會晚 133 ms 才看到，而且
  server 的回溯上限只有 100 ms（`kMaxCompensationWindowUs`），畫得越舊，
  玩家瞄準的位置和 server 判定的位置差越多。不建議當作主要方案。
- **W7 過關判斷：** 實測高峰約 60 隻 agent（2026-09-26）。頻寬加倍後，80 隻以內
  近、中距離帶的問題已經解決，**目前不需要 W7**。只有目標人數是「視野內約 200 隻
  同時戰鬥」時，中距離帶才仍然明顯，那時才值得做 W7。

**限制：** 腳本讓每隻 agent 都以 5 m/s 持續移動和轉彎，比實際遊戲嚴苛
（實際上很多 agent 會站著或走直線）。距離帶以 agent 的中心點分類。

### 10.4 評估：同樣 1200 B，縮小 agent 紀錄（方案 D）

1200 B 是單一封包（MTU）的限制，不是每個間隔的限制；但在不加頻寬的前提下，
能做的是讓每包塞更多 agent。現行 agent 紀錄（`network_packets.cc` 的
`kActorAgent` 區段）：

- 閒置 32 B：net_id 4、flags 1、**位置 12（三個 float）**、速度 6、朝向 2、
  瞄準 3、動畫狀態 2、視覺 flags 2；
- 動作中另加 20 B：template_id 4、instance_id 4、**start_tick 4、
  commit_count 4、phase 2、補齊 2**。

可縮的部分：

| 欄位 | 縮成 | 省 |
|---|---|---|
| 位置 | 3 × i16，相對於 snapshot 標頭帶的錨點（±80 m 時解析度 2.5 mm） | 6 |
| net_id | 區段內排序後送差值（varint，估計時以 2 B 計） | ~2 |
| template_id | u16 | 2 |
| start_tick | 相對 snapshot tick 的 i16（超出範圍時用旗標送完整值） | 2 |
| commit_count | u16（連射可能超過 255） | 2 |
| phase + 補齊 | u8 | 3 |

結果：閒置 32 → 約 24 B，動作中 52 → 約 35 B。每包 agent 可用約 1030 B，
格數從 32 / 19 變成約 42 / 29–30。

bench 以「等效預算」模擬（格數與縮小後相同），中距離帶：

| 方案 | 格數 | 80 隻閒置 | 80 隻動作中 | 200 隻動作中 |
|---|---|---|---|---|
| 現行 | 32 / 19 | 0.02 m / 11.5 / 0.3% | 0.07 m / 37 / 1.3% | 1.33 m / 95 / 42% |
| 只縮位置 | 39 / 22 | 0.02 m / 5.9 / 0.2% | 0.05 m / 28 / 0.9% | 0.69 m / 113 / 29% |
| 全部 | 42 / 30 | 0.02 m / 6.0 / 0.3% | 0.02 m / 14 / 0.2% | 0.27 m / 78 / 5.4% |
| 預算加倍（參考） | 69 / 43 | 0.01 m / 0.6 / 0% | 0.02 m / 5.7 / 0.3% | 0.11 m / 52 / 2.3% |

**判讀：**
- 「全部」大約拿到頻寬加倍一半的效果，而且頻寬完全不增加。
- 效果集中在戰鬥中：動作時間軸從 20 B 縮到約 11 B，動作中格數 +58%；
  只縮位置時動作中只多 3 格，不值得單獨做。
- 80 隻動作中時，中距離帶跳動從 37 降到 14 次/分，遠距離帶停住從 25% 降到 2.4%；
  但遠距離帶仍每分鐘跳 71 次，要更好就要再加上方案 B（每間隔多送獨立封包）。
- 改動範圍：只有 snapshot 編碼、解碼和大小估計，snapshot schema 升版；
  C ABI、client 的內插與 Unity 都不受影響。

### 10.5 方案 D 實作結果（`claude/compact-agent-record`，snapshot schema 24）

- agent 紀錄：net_id 改 varint（1–5 B，大小只看 net_id 本身，估計仍精確）；
  位置改 3 × i16，每格 1/256 m，相對於 agent 區段的錨點（邊界框中心），
  每軸誤差最多約 2 mm（合計 3.4 mm）；閒置時其餘 22 B。
- 區段前導：錨點 12 B + 模式 1 B，每包固定送（併入 base 大小）。跨度超過
  ±128 m 時整個區段退回 float，實際大小比估計多 6 B/agent；40 m relevance 下不會發生。
- 動作時間軸 11 B：template u16、instance u32、start_tick 低 16 位元（以 snapshot
  tick 還原）、commit u16、phase u8。template 或 commit 超出 u16 時送原本的 20 B
  格式；start_tick 早於 snapshot 超過 65535 tick 或晚於 snapshot 時，編碼端也改送
  20 B（這種情況大小比估計多 9 B）。
- 實際格數（bench 用真實編碼）：閒置 32 → **44**，動作中 19 → **30**。
  長時間遊玩後 net_id 超過 16383 時 varint 變 3 B，閒置約 41 格。
- 誤差（中距離帶，p90 / 跳動 / 停住）：80 隻動作中 0.02 m / 14.5 / 0.2%
  （原本 0.07 m / 37 / 1.3%）；遠距離帶停住 25% → 2.4%。與 §10.4 的模擬一致，
  §10.4 的模擬段落已從 bench 移除。

### 10.6 方案 B：每個間隔多個獨立封包（`claude/multi-packet-snapshot`，snapshot schema 25）

目標：200 隻同時戰鬥時，近、中距離帶跟不限預算一樣乾淨。

**做法：**
- server 每個間隔只產生一次 send set，預算是 `snapshot_send_set_budget(4)`
  （4 × 1200 B，扣掉後三包各自的標頭和前導），再用 `split_snapshot_for_packets`
  切成最多 4 個各自 ≤ 1200 B 的獨立 snapshot 封包。非 agent 的 entity
  （自己的玩家紀錄、projectile、prop）放第一包，agent 依序填後面。
  只送實際填到的包數，人少時仍然只有一包。
- 每包都是完整可解碼的 snapshot（不是分片），掉一包只少那一包的 agent；
  wire 格式沒有變。
- client：同一個 tick 的後續封包合併進 `latest_client_snapshot_` 和內插 buffer
  （`store_client_snapshot` 也改成合併，重複收到同一包結果不變）。
  vision 狀態用合併後的整個 tick 重建；本地武器、本地預測和預測 projectile 的
  對帳只看這一包，避免同一個 tick 處理兩次。只有一包時行為和原本完全相同。
- snapshot schema 24 → 25：舊 client 會用第二包取代第一包，必須兩端一起更新。

**GNS 檢查（本專案抓下來的版本）：**
- `SendRateMin` / `SendRateMax` 預設都是 256 KB/s；4 包約 73 KB/s。
- 單一 UDP 封包加密後酬載上限 1248 B，1200 B 放得下。
- `NoDelay` 在這個版本只有 FIXME，不會丟包；同時送出的 4 包會排隊，
  依 256 KB/s 送出，最後一包約晚 15–19 ms。

**結果（200 隻，中距離帶：誤差 p90 / 跳動 / 停住）：**

| 每間隔封包數 | 動作中 agent/間隔 | 頻寬 | 動作中 近 | 動作中 中 | 動作中 遠 | 閒置 中 |
|---|---|---|---|---|---|---|
| 1 | 29 | 142 kbit/s | 0.05 m / 40 / 0.7% | 0.28 m / 85 / 7.4% | 1.70 m / 80 / 51% | 0.10 m / 49 / 2.3% |
| 2 | 62 | 288 kbit/s | 0.02 m / 5.8 / 0.1% | 0.04 m / 25 / 0.6% | 0.20 m / 75 / 2.2% | 0.02 m / 11 / 0.3% |
| 3 | 95 | 433 kbit/s | 0.01 m / 0 / 0% | 0.02 m / 10 / 0.3% | 0.05 m / 28 / 0.6% | 0.01 m / 1.5 / 0% |
| **4（採用）** | 128 | 580 kbit/s | 0.01 m / 0.4 / 0% | **0.01 m / 1.4 / 0%** | 0.05 m / 26 / 0.5% | 0.01 m / 0.2 / 0% |
| 5 | 161 | 729 kbit/s | 0.01 m / 0.4 / 0% | 0.01 m / 0.2 / 0% | 0.02 m / 8 / 0.3% | 0.01 m / 0.3 / 0% |

4 包時近、中距離帶和不限預算相同；遠距離帶每分鐘仍跳約 26 次（p90 5 cm）。
頻寬只在人多時才上升：200 隻戰鬥時約 580 kbit/s（73 KB/s）下行。

**W7 判斷更新：** 方案 D + B 之後，200 隻同時戰鬥的近、中距離帶已經乾淨，
**不需要 W7**。遠距離帶若要再改善，再加第 5 包即可。

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
