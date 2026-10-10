# 鎖定類武器（連鎖閃電、魔法飛彈）實作計劃書

狀態：規格討論完成（2026-10-10），尚未開工。§2 標「待確認」的項目要先定。
前置：`main`（`fc4e964`，ABI 102、packet schema 31、snapshot schema 29）。

---

## 1. 目標

兩把新武器，操作方式都像 Rifle（hold 連射），開火時先在錐形範圍內掃描目標：

| | 連鎖閃電（A） | 魔法飛彈（B） |
|---|---|---|
| 每次 commit | 打最近的 min(N, M) 個目標，**瞬間**固定傷害 | 發 1 顆，打第 `commit_index % min(N, M)` 近的目標（發法乙） |
| 傷害時機 | commit 當下 | 到達 tick（排定傷害） |
| 扣彈 | 固定 M，N = 0 也扣 | 每顆 1；N = 0 時延後 commit，不扣（D9） |
| client | 自己跑同一個掃描畫電弧，不足 M 條補假電弧 | 照 server 給的 (目標, 到達 tick) 播飛行與爆炸，純演出 |
| 新封包 | 無 | 有（packet schema 32） |

N：錐形內偵測到的目標數。M：單次最大鎖定數。T：一顆飛彈的飛行 tick 數。T2：最大飛行 tick 數。

兩者都只由 server 決定結果，client 只做演出，所以**都不需要純 client 有本機武器狀態**（`ITEM_WEAPON_SYSTEM_PLAN.md` §8.1 的方案 1 維持不變）。

---

## 2. 決策

| # | 決策 | 狀態 |
|---|---|---|
| D1 | 錐形用 **3D 角度**：aim 和「目標中心 − 射手 launch 位置」的夾角 ≤ 錐角 / 2，且距離 ≤ 錐形 range | 已定 |
| D2 | 依**距離由近到遠**取前 M 個，同距離比 net id。判斷點用目標 hitbox **中心**，不用碰撞表面（client 只有中心） | 已定 |
| D3 | 遮擋：排序後依序檢查，射手 → 目標中心的射線碰到地形或靜態障礙物就跳過，由下一個近的補上 | 已定 |
| D4 | server 選目標用回溯畫面（`context.rewind_frame`），射手位置用現在的 `projectile_launch_position`，和 hitscan 相同 | 已定 |
| D5 | A：取消「沒目標不扣彈」，每次 commit 都正常 Committed、扣 M。因此不會走 `Corrected`（client 視為 terminal，[kernel.cc:7801](../engine/src/kernel/src/kernel.cc)） | 已定 |
| D6 | A 的傷害用鎖 actor：直接對選中的 net id 送 `DamageRequest`，位置只用於演出 | 已定 |
| D7 | A 的 client 演出用「各 client 自己算」：不加封包。假電弧終點用 `(射手 net id, action_instance_id, commit_index)` 當種子 | 已定 |
| D8 | B 用發法乙：每次 commit 1 顆，目標是 `commit_index % min(N, M)` 近的那個。要重現發法甲時，`commit_interval_ticks` 乘上 M 即可維持同樣的每秒飛彈數 | 已定 |
| D9 | B 在 N = 0 時**延後 commit**：這一 tick 視為沒有 commit。不扣彈、不進冷卻、不送任何結果給 client、`commit_count` 不增加，下一 tick 再掃描。所以 A 每個彈匣打完的時間固定，B 的時間取決於有目標的時間。不能用拒絕路徑（`Corrected`），也不能用「Committed 但扣 0」，原因見 §3.3 | **待確認**（2026-10-10 提出） |
| D10 | B 用排定傷害：server 在 commit 時就算好到達 tick，server 上不生飛彈 entity | 已定 |
| D11 | 直接設定 T2，不設定速度：每 tick 速度 = range / T2，T = ceil(d / 每 tick 速度)，落在 [1, T2] | 已定 |
| D12 | 延遲傷害用 tick：`DamageRequest` 加 `apply_tick`（0 = 不延後）。「時間用 tick」是全專案慣例，`DamagePipeline` 的微秒時間窗是延遲補償的例外，維持不動 | 已定 |
| D13 | 到達 tick 時目標若已死亡、已消失、在無敵時間內、在帳篷內、或身上有無法被選取的 status，傷害取消。除了「已死」，其他都是 `apply_damage_applications` 現有的檢查 | 已定 |
| D14 | 目標死亡時，取消所有打它的待生效傷害（`PendingDamage::canceled`）。這樣不用靠「重生間隔 > T2」的數值限制；限制本身留作設計準則 | 已定 |
| D15 | 閃避和格擋會作用在 B 的傷害上：命中時間設成到達 tick 對應的時間，閃避窗對準到達那一刻。有 `apply_tick` 的傷害不再額外加 100 ms grace。目前玩家沒有閃避動作，所以實際上幾乎必中 | 已定 |
| D16 | 未來的閃避動作做成「狀態」（無敵的那幾個 tick 寫入 `DamageImmunity` 或 status），不靠按鍵時間窗。不在本計劃範圍 | 已定（之後做） |
| D17 | `weapon_type` 名稱。建議用機制命名：`lock_instant`（A）、`lock_missile`（B） | 已定（2026-10-10） |
| D18 | 錐形沿用 melee 的做法：武器模板指定一個 cone collider 模板（range、fov），M 用 shot 模板的 `max_hit_count`，B 的 T2 用 shot 模板的 `lifetime_ticks`。這樣 `KernelWeaponMechanicsDefinition` 不用加欄位，只把 `melee_collider_template_id` 的用途擴大到這兩種模式 | 已定（2026-10-10） |
| D19 | A 的 `ammo_cost_per_commit` 必須等於 M，且 `magazine_size % M == 0`，否則是載入錯誤 | 已定 |
| D20 | B 的記錄用 unreliable 送：純演出，掉了只少一顆飛彈畫面，傷害照算。到達 tick 已過才收到的記錄只播爆炸，或直接略過 | 已定（2026-10-10） |
| D21 | 純 client 上射手自己的 A 演出：建議 Unity 按下時就呼叫 client 查詢 API（§3.4）立即畫，不等 snapshot；旁觀者照 snapshot 的 commit 數畫 | 已定（2026-10-10） |

---

## 3. 改動

### 3.1 共用：目標選擇函式

- 新增一個純函式，放 `simulation/public/`，server 和 client 都呼叫：
  - 輸入：候選清單（net id、中心、是否存活）、射手 launch 位置、aim、range、fov、M、遮擋判斷的 callback。
  - 輸出：依 D1–D3 選出的 net id 和中心，最多 M 個。
- server 的候選來自回溯畫面的 `HitVolumeSnapshot`（`alive`、不是 limb、陣營照武器的 mask 過濾）；沒有回溯畫面時用 live world 的 hitbox。
- client 的候選來自畫面上內插後的遠端 actor。
- 遮擋 callback：server 用 collision world 的 ray cast，只看地形和靜態障礙物；client 用自己的靜態世界（沒有動態道具，見 R1）。

### 3.2 Server：連鎖閃電（`lock_instant`）

- `WeaponFireMode` 加一個值，`KernelWeaponFireMode` 加 5，`to_weapon_fire_mode`（[kernel.cc:1614](../engine/src/kernel/src/kernel.cc)）跟著加。
- `weapon_system.cc` 的開火分支（[weapon_system.cc:993](../engine/src/simulation/src/weapon_system.cc) 附近）：
  - 扣彈、冷卻、FireConfirmed 照現有流程，不另外拒絕（D5）。
  - 呼叫 §3.1 選目標，對每個目標 `submit_damage_request`，`hit_time_us` 用 `context.action_time_us`。
- `gameplay_config.cc`：解析新的 `weapon_type`、要求 cone collider（D18），檢查 D19。

### 3.3 Server：延遲傷害與魔法飛彈（`lock_missile`）

`DamagePipeline`（[simulation.h:278](../engine/src/simulation/public/simulation.h)、[damage_pipeline.cc:162](../engine/src/simulation/src/damage_pipeline.cc)）：
- `DamageRequest`、`PendingDamage` 加 `apply_tick`。
- `drain_ready_damage` 多一個 `current_tick` 參數（`confirm_ready` 已經有）：
  - 有 `apply_tick` 的：`current_tick < apply_tick` 就留著。
  - 沒有的：照舊用 `confirm_time_us`。
  - 有 `apply_tick` 的不加 grace（D15）。
- 加 `cancel_pending_for_target(net_id)`，在目標死亡的地方呼叫（D14）。死亡的入口在 P0 確認。
- `apply_damage_applications`（[damage_system.cc:164](../engine/src/simulation/src/damage_system.cc)）補「血量已是 0 就跳過」。

`weapon_system.cc` 的 `lock_missile` 分支：
- N = 0 時延後 commit（D9）：把 action 的 `commit_count` 減回去、`next_commit_tick` 設成下一 tick、phase 還原，不 push 任何 outcome。
  不能用另外兩種做法：
  - 拒絕（`Corrected`）：client 視為 terminal、Unity 停掉開火演出（[kernel.cc:7801](../engine/src/kernel/src/kernel.cc)）；server 的 session 會放掉這個 action 的 claim，但 simulation 的 hold 還在跑（[kernel.cc:16556](../engine/src/kernel/src/kernel.cc)）；interval 1 時每秒最多 30 個結果封包。
  - Committed 但扣 0：server 會發 `FireCommit` 的遠端演出事件，旁觀者看到施法卻沒有飛彈；listen host 預測會先扣彈再被 snapshot 拉回。
  P0 要確認：`update_visual_flags` 在這一 tick 設的 firing 旗標會不會讓旁觀者看到閃一下；`cancel_after_first_commit` 和 `completes_action` 在回滾時的處理。
  listen host 的本機預測要跑同一個掃描，才不會預測出 server 沒有的 commit。
- 扣 1 發，選目標，取第 `commit_index % n` 個（D8）。
- T = ceil(d ÷ (range ÷ T2))，d 用回溯畫面的距離。
- 送一筆 `DamageRequest`：`apply_tick = current_tick + T`，`hit_time_us` = 到達 tick 對應的時間。
- 把 (射手, instance, commit, 目標, 發射 tick, T) 放進這個 tick 的飛彈記錄清單，交給 kernel 送出。

### 3.4 Protocol 與 kernel

**魔法飛彈記錄（新封包，packet schema 31 → 32）：**
- 同一個封包裡，同一個 `(射手, instance)` 合成一組：
  - 組頭：射手 net id u32、instance u32、基準 tick u32、第一個 commit u16、筆數 u8，共 15 B。
  - 每顆：tick 差 u8、commit 差 u8、burst u8、目標 net id u32、飛行 tick 數 u8，共 8 B。
- 送給「看得到射手」的 session（和 projectile spawn 相同），加上射手本人。
- reliability 照 D20。

**client（kernel）：**
- 收到記錄後放進一個演出清單，項目是 (射手, 目標, 發射 tick, 到達 tick, instance, commit, burst)，到達後多留一小段讓 Unity 播爆炸，然後移除。用 `(射手, instance, commit, burst)` 去重。
- 連鎖閃電：client 從 snapshot 看到射手的 `action_commit_count` 增加，且射手拿著 `lock_instant` 武器時，用 §3.1 和射手的 `aim_direction` 算出目標，產生一筆演出事件（射手、commit、目標 net id、假電弧終點）。兩個 snapshot 之間跨了好幾次 commit 時，每一次都產生一筆。
- 新 export（照 memory「Additive ABI needs no bump」用 capability flag，macOS 和 Windows 兩份 export 清單都要加）：
  - 讀取兩種演出清單。
  - D21 的查詢 API：給 weapon id、位置、aim、seed，回傳目標和假電弧終點。`lock_missile` 也用它：Unity 按住時用它畫鎖定標記，沒有目標時顯示「搜尋中」，不要照 §8.1 方案 1 在按下時就播開火。
- 演出的時間軸用 `client_render_server_time_us`，不用 snapshot header 的 tick（memory「Interpolated snapshot tick is floored」）。

### 3.5 Content

- 兩把武器各四個檔（shot、fire action、reload action、weapon）加上 cone collider 模板，照 `$id_<kind>_$name` 命名。不放進任何配裝。
- id 在 P5 開工時查目前最大值再定，merge 時再查一次（memory「Parallel branches collide on numbers」）。

### 3.6 不用改的

- `ITEM_WEAPON_SYSTEM_PLAN.md` §8.1：純 client 仍然不預測扣彈和冷卻，彈藥 UI 用 server 的值。
- Snapshot 格式：射手的 `aim_direction`、`action_instance_id`、`action_commit_count`、`held_weapon_id` 已經在 snapshot 裡（[snapshot.h:34](../engine/src/sync/public/snapshot.h)）。P0 要確認 AI 射手也有這些值。
- 閃避和格擋的按鍵時間窗邏輯。

---

## 4. 流量影響

只有魔法飛彈有新流量；連鎖閃電沒有。

- 每秒飛彈數 = (30 ÷ `commit_interval_ticks`) × 每次 commit 的顆數。
- 每顆約 8 B，每個送出的封包裡每個 `(射手, instance)` 再加 15 B 組頭。

| 情境 | 每秒飛彈數 | 每個收到的 session |
|---|---|---|
| 發法乙，interval 1，一直按住 | 30 | 約 240 B/s + 組頭 |
| 發法甲，M = 6，interval 6 | 30 | 同上，組頭比較少 |
| 彈匣 24、interval 1、reload 1 秒的平均 | 約 13 | 約 105 B/s + 組頭 |

- 再乘上看得到射手的 session 數，以及同時使用的射手數。
- 跟一般投射物比：一般子彈每顆約 205 B 的 reliable spawn（見 `HELD_FIRE_PROJECTILE_PREDICTION_PLAN.md` §4），還會出現在 snapshot 裡。這裡沒有 server entity，也不進 snapshot。

---

## 5. 版本與交付

- **Packet schema 31 → 32**：client 和 server 一定要用同一版 native library。`dynamic_abi_smoke_test`、`kernel_api_test` 寫死了版本號，要跟著改（memory「Schema bump touch points」）。
- **Snapshot schema 維持 29**（P0 確認後定案）。
- **ABI 預計維持 102**：新的 fire mode 只是新增 enum 值，新 export 用 capability flag。D18 如果不採用、要加 struct 欄位，就要 bump，P1 時再確認。
- **`bundle.bytes` 需要更新**：P5 加了新的 catalog 內容。由使用者處理。
- Unity 的 C# 鏡像和演出由使用者處理。
- 照規則在 `claude/*` 分支上 commit 並測好後交給使用者，不碰 `feat-unity-plugin` 和 package。

---

## 6. 實作階段

| 階段 | 內容 | 驗證 |
|---|---|---|
| P0 | 只讀不改，確認：(1) 目標死亡的單一入口在哪；(2) 回溯路徑怎麼套用陣營和 mask（`raycast_history_frame` 沒有收 mask）；(3) 純 client 能不能從 catalog 讀到武器的 cone 和 M；(4) client 怎麼知道遠端 actor 的陣營和 hitbox 中心；(5) client 的靜態碰撞能不能做遮擋射線；(6) AI 射手的 snapshot 有沒有 `aim_direction` 和 `action_commit_count` | 結果寫回 §9 |
| P1 | §3.1 共用選擇函式；§3.2 連鎖閃電的 server 端與載入檢查 | 選擇函式單元測試；`combat_test` 新案例 |
| P2 | §3.3 前半：`apply_tick`、已死跳過、死亡時取消 | `damage_pipeline_test` 新案例 |
| P3 | §3.3 後半：魔法飛彈 server 端；§3.4 的封包與 schema 32 | protocol roundtrip；`combat_test` 新案例 |
| P4 | §3.4 的 client 端：兩種演出清單、export、查詢 API | `client_mode_test` 新案例；dedicated server + 純 client 的 loopback e2e（以 `held_fire_end_to_end_test` 為範本） |
| P5 | §3.5 content | catalog 載入測試；用真的模板跑一次 e2e |

---

## 7. 測試策略

- **選擇函式**：3D 錐形邊界（正上方的目標在 fov 外就不選）、超過 range 不選、依距離排序、同距離比 net id、被遮擋的跳過並由下一個補上、最多 M 個、死掉的不選。
- **連鎖閃電**：N = 0 照樣扣 M 發、不會 `Corrected`、按住不中斷；N < M 只傷 N 個；每個目標各受一次傷害。
- **載入**：`ammo_cost_per_commit != M` 或 `magazine_size % M != 0` 會被拒絕。
- **延遲傷害**：到 `apply_tick` 前不生效、當下生效；目標已死、已消失、無敵、在帳篷內都取消；死亡後馬上復活也不會被打到（D14）；在到達時刻按閃避會取消。
- **魔法飛彈**：commit k 打第 `k % n` 近的目標；T 落在 [1, T2]；記錄 roundtrip，包括合成一組和分組的邊界。
- **e2e**：純 client 收到的飛彈記錄和 server 的一致；連鎖閃電在純 client 上選中的目標，和 server 在無延遲時選中的一樣。要附一個「拿掉修正就會紅」的對照（memory「Test assertions can be vacuous」）。
- **既有的測試**：`main` 本來就有紅的，比對失敗清單，不看「全綠」（memory「main carries a red baseline」）。

---

## 8. 風險

| # | 風險 | 處理 |
|---|---|---|
| R1 | client 畫的電弧目標和 server 實際打的不同：延遲造成位置差、client 沒有動態道具所以遮擋判斷不同 | 已接受（D7），只影響演出。實際玩過覺得明顯，再加 server 送命中名單 |
| R2 | 連鎖閃電的 interval 比 snapshot 間隔短時，旁觀者看到的是好幾次 commit 一起出現 | 照 commit 數的差補播；用最新的 aim，演出會略有偏差 |
| R3 | 魔法飛彈 interval 1，每個射手每秒掃描 30 次。開放世界約 1300 個單位時，回溯畫面每次掃一遍 | P3 量每 tick 成本；太高就先用 live world 的 broad phase 縮小候選，再到回溯畫面取位置 |
| R4 | `drain_ready_damage` 每次呼叫都對整個待生效清單做 `stable_sort`。每個射手最多約 30 × T2 筆待生效傷害（T2 = 30 時為 900） | P2 量成本；需要時改成依 `apply_tick` 分桶 |
| R5 | 目標不在旁觀者的視野內，client 找不到它的位置 | 在最後看到的位置爆炸；完全沒看過就只畫到射程終點 |
| R6 | 和其他還沒 merge 的分支撞到 packet schema 或 catalog id | 開工時和 merge 時各檢查一次 |
| R7 | D18 的欄位沿用讓 `melee_collider_template_id`、`lifetime_ticks` 的意義變多 | 在 ABI 註解和 `WEAPON_AUTHORING_GUIDE.md` 寫清楚各模式怎麼解讀 |

---

## 9. P0 驗證結果

（尚未開始）

---

## 10. Token / 成本限制

- 先跑 smoke test：每個階段只跑直接相關的測試，不跑全部。
- 只讀直接相關的檔案：`kernel.cc` 只讀 §3 列出的段落，`weapon_system.cc` 只讀開火分支和 melee 的錐形掃描。
- 不做大範圍搜尋，不做 full build。
- 範圍如果擴大（例如需要改 snapshot 或 ABI struct），先停下來說明。
- 每個階段完成時回報讀了哪些檔、改了哪些檔、搜尋了幾次、跑了哪些 build 和測試。
