# 連射子彈的本機預測（commit / burst 編號）實作計劃書

狀態：P0–P3 全部完成（2026-10-09）。
前置：`claude/held-fire-keeps-bullets`（`e75cd17`，放開按鍵不再刪掉飛行中的子彈），尚未 merge 到 `main`。
本計劃以那個修正為基礎，並在 P2 把它的 `net_id == 0` 規則換成照編號判斷。

---

## 1. 問題

hold 類型的射擊（水球法杖、Projectile Spammer）一次按住會 commit 很多次，**每一發都用同一個 action instance id**。
client 和 server 之間對子彈的配對只靠 `(owner_peer, action_instance_id)`，因此：

| # | 現象 | 位置 |
|---|---|---|
| Q1 | 本機只預測第一發。第 2 發之後要等 server 的 spawn record 到了才出現，大約晚半個到一個 RTT | [kernel.cc:10494](../engine/src/kernel/src/kernel.cc) `predict_local_projectile` 開頭：這個 instance 已經有預測子彈就 return |
| Q2 | burst 武器（spammer `burst_count: 3`）本機只預測正前方一顆；server 第一個生的是 -15° 那顆，可能會綁到本機的 0° 子彈上。結果是 0° 有兩顆重疊，-15° 看不見 | `predict_local_projectile` 只 push 一顆；server 照 index 0..N-1 生（[weapon_system.cc:110](../engine/src/simulation/src/weapon_system.cc)） |
| Q3 | snapshot 修正只會套到這個 instance 的第一顆子彈 | [kernel.cc:10223](../engine/src/kernel/src/kernel.cc) 用 `find_predicted_projectile(peer, instance)` |
| Q4 | 長時間按住時，第一顆子彈消失後，下一個 spawn 可能會綁到前一個從 spawn 建出來的子彈上，那顆子彈會跳位 | `find_predicted_projectile` 回傳第一筆符合的，[kernel.cc:8206](../engine/src/kernel/src/kernel.cc) 只要 `!bound` 就綁 |
| Q5 | 收到 terminal 結果時，分不出哪些預測子彈屬於 server 已經確認的前 N 發 | `e75cd17` 先用「沒有 net id 就刪」頂著 |

使用者在 `4100c9a` 把水球法杖調成彈匣 12、`commit_interval_ticks: 15`、子彈 lifetime 120。
一次按住最多 8 顆子彈同時在飛（120 / 15），Q1、Q3、Q4 都會比之前明顯。

**根本原因只有一個**：spawn record 沒有「第幾發」。

> **P3 開工前的修正（2026-10-09）：純 Client 不會在本機預測子彈。**
> `predict_local_projectile` 要從 `world_` 取得本機玩家的武器（`entity_weapon_mechanics`），但只有 listen-server 的 host 和 server 端的 peer 握手會把玩家放進 `world_`。
> 一般連線進來的 Unity client（純 Client 模式）拿不到武器資料，會直接 return（memory：client-has-no-local-weapon-state，2026-09-30 實測）。所以在純 Client 上：
>
> | # | 純 Client 上 |
> |---|---|
> | Q1 | **不適用**：每一發（包括第一發）本來就都要等 server 的 spawn。要改善需要 client 端的武器狀態（`docs/ITEM_WEAPON_SYSTEM_PLAN.md` §8.1 選項 2 / 3），不在本計劃範圍 |
> | Q2 | **不會發生**：沒有本機預測可以綁錯 |
> | Q3 | **有效**，但只限會進 snapshot 的子彈（`hybrid_deterministic_then_snapshot`）。spammer 和水球法杖是 `local_predicted_deterministic`，不會進 snapshot（P3 實測） |
> | Q4 | **有效，而且比原本寫的嚴重**：從 spawn 建出來的子彈都是 `bound = 0`，舊的綁定每次都找這個 instance 的第一筆，所以**同一次按住的每個 spawn 都會綁到第一顆子彈上**，不是只有長按才會發生（P3 的 mutation MC） |
> | Q5 | **有效**：使用者看到的「放開按鍵子彈消失」就是這個（`e75cd17`） |
>
> P1、P2 的本機預測部分只會在 listen-server 的 host 上執行；P3 照使用者的決定，驗證純 Client + dedicated server。

---

## 2. 決策（待使用者確認）

| # | 決策 | 建議 | 替代方案 |
|---|---|---|---|
| D1 | spawn record 加 `commit_index`（u16）和 `burst_index`（u8），補 1 byte 對齊，每筆 +4 B | 採用 | 由 client 從 `spawn_tick` 和速度方向推算：rewind 會改 `spawn_tick`，方向要做浮點數比對，不可靠 |
| D2 | 所有 peer 都送同一種格式 | 採用。只有擁有者用得到，但只給擁有者送要多一個 flag 和兩種格式，每顆子彈只省 4 B | 只給擁有者送 |
| D3 | `commit_index` 從 0 開始，等於 server `ActionCommit::commit_count - 1`（`commit_count` 在 fire 時已經加過 1） | 採用 | — |
| D4 | 子彈綁定後，snapshot 修正改用 `net_id` 找子彈，不改 snapshot 格式 | 採用：snapshot schema 維持 29 | snapshot 也加編號：多一次 schema bump，沒有必要 |
| D5 | client 每次 commit 都照 burst 方向預測所有子彈，所以 `projectile_burst_directions` 要從 `weapon_system.cc` 的內部函式移到共用 header | 採用：server 和 client 用同一個函式，方向才會一模一樣 | client 自己寫一份：兩份會分歧 |
| D6 | 舊的按鈕射擊路徑（`legacy_button_commit`，計劃書 WATER_BUBBLE R8）照樣填 `commit_count - 1` | 採用，不另外處理 | — |

---

## 3. 改動

### 3.1 Server（simulation）

- `ProjectileState`（[components.h:610](../engine/src/world/public/components.h)）加 `std::uint16_t commit_index`、`std::uint8_t burst_index`。
- `weapon_system.cc` 的 projectile 分支：`fire_projectile` 多傳 `commit.commit_count - 1` 和 burst 迴圈的 index。
  其他呼叫 `fire_projectile` 的地方（action graph 的 `spawn_projectile`、derived chain）填 0。
- `projectile_burst_directions` 移到 `simulation/public/`，`weapon_system.cc` 改呼叫共用版本。

### 3.2 Protocol

- `ProjectileSpawnRecord`（[network_packets.h:51](../engine/src/protocol/public/network_packets.h)）加 `commit_index`、`burst_index`。
- encoder / decoder：`kProjectileSpawnRecordPayloadSize` 從 40 改成 44（u16 + u8 + 1 byte padding）。
- `send_projectile_spawn_batch`（[kernel.cc:14877](../engine/src/kernel/src/kernel.cc)）從 `ProjectileState` 帶這兩個值。
- **`kPacketSchemaVersion` 從 30 改成 31**（[packet_header.h:12](../engine/src/protocol/public/packet_header.h)）。
  握手測試都用常數，沒有寫死的版本號（已用 grep 確認）。

**P1 實作紀錄**：
- `ProjectileState` 加 `commit_index`（u16）、`burst_index`（u8）。
  `weapon_system.cc` 的 projectile 分支傳 `commit.commit_count - 1` 和 burst 迴圈的 index。
  舊的按鈕射擊路徑 `commit_count` 是 1，所以 index 是 0（D6）。
- `fire_projectile` 只有一個呼叫端，沒有其他地方要補 0。action graph 的 `spawn_projectile` 和 derived chain 不經過它，維持預設值 0。
- `projectile_burst_directions` 從 `weapon_system.cc` 的 anonymous namespace 搬出來，宣告放在 `simulation/public/simulation.h`。內容沒有改。
- `ProjectileSpawnRecord` 的兩個新欄位放在 **struct 最後面**，這樣測試裡現有的 aggregate 初始化不受影響。
  線上的順序是接在 `action_instance_id` 後面：u16 + u8 + 1 byte reserved（寫 0，讀到後忽略）。record 從 40 B 變成 44 B。
- `kPacketSchemaVersion` 30 → 31。`dynamic_abi_smoke_test` 和 `kernel_api_test` 寫死了 30，改成 31。
  §3.2「沒有寫死的版本號」是錯的：握手測試用常數，但這兩個 build info 的檢查是寫死的。C# 只讀這個值，沒有寫死。
- 測試：
  - 新增 `//engine/src/tests/protocol_tests:projectile_spawn_index_roundtrip_test`：編號 roundtrip（含 u16 / u8 的最大值），每筆 record 剛好多 44 B。
  - `combat_test` 新增 `held_burst_shots_carry_their_commit_and_burst_index`：按住、burst 3，三次 commit 共 9 顆，每顆的 `(commit, burst)` 都不重複，方向等於共用函式的第 `burst_index` 個。
  - mutation：把 burst index 寫死 0、把 encoder 的 commit index 寫死 0，這兩個測試都會失敗。
- 既有的測試：`client_mode_test` 和 7 個 projectile 相關的 kernel 測試都通過。`network_packets_test`（752）、`session_packets_test`（174）、`kernel_api_test`（288）失敗的位置跟 `main` 上原本壞掉的一樣。

### 3.3 Client（kernel）

- `PredictedProjectile` 加 `commit_index`、`burst_index`。
- `predict_local_projectile`：
  - 拿掉「這個 instance 已經有預測子彈就 return」。
  - 改成「這個 `(instance, commit_index)` 已經預測過就 return」，防止同一個 commit 重複生。
  - `commit_index = predicted_local_entity_.action_commit_count - 1`（`predict_local_action` 已經加過）。
  - 用共用的 `projectile_burst_directions` 一次 push 所有子彈，每顆帶自己的 `burst_index`。
- `find_predicted_projectile` 多兩個參數：spawn 綁定時用 `(peer, instance, commit_index, burst_index)` 找。
  `predict_local_projectile` 的重複檢查只看 `(peer, instance, commit_index)`。
- snapshot 修正（[kernel.cc:10223](../engine/src/kernel/src/kernel.cc)）：先用 `net_id` 找已經綁定的子彈，找不到才退回用 instance 找。
- terminal 結果（[kernel.cc:7841](../engine/src/kernel/src/kernel.cc)）：刪掉 `net_id == 0 && commit_index >= confirmed_commit_count` 的子彈。
  `e75cd17` 的「沒有 net id 就刪」改成這個精確的規則。
- 動作逾時（[kernel.cc:2583](../engine/src/kernel/src/kernel.cc)）：沒有 server 的確認數，維持 `e75cd17` 的「沒有 net id 就刪」。
- 從 spawn 建出來的子彈（[kernel.cc:8232](../engine/src/kernel/src/kernel.cc)）也記下這兩個編號，並標成 `bound = true`，因為它本來就是 server 的子彈。
  這樣 Q4 不會再發生。

**P2 實作紀錄**：
- `PredictedProjectile` 結尾加 `commit_index`、`burst_index`，放在最後面，既有的 aggregate 初始化不受影響。
- `find_predicted_projectile(peer, instance)` 移除，改成 4 個用途明確的函式：
  - `find_unspawned_prediction(peer, instance, commit, burst)`：spawn 綁定用，只找 `net_id == 0` 的純本機預測。
  - `find_sole_unspawned_prediction(peer, instance)`：snapshot 的備用找法，只有在這個動作**剛好只有一顆**還沒命名的預測時才回傳。
  - `find_predicted_projectile_by_net_id`：snapshot 修正優先用這個。
  - `has_projectiles_of_commit(peer, instance, commit)`：本機預測的重複檢查。
- `predict_local_projectile`：拿掉「每個 instance 只預測一次」，改成「每個 commit 只預測一次」。
  `commit_index = action_commit_count - 1`，用 `projectile_burst_directions` 一次預測整個 burst，每顆帶自己的 `burst_index`。
  這個 commit 如果已經有從 spawn 建出來的子彈，就不再預測。
- terminal 結果：刪掉 `net_id == 0 && commit_index >= confirmed_commit_count` 的子彈。`e75cd17` 的測試跟著改：沒 spawn 的那顆子彈改成第 2 發。
- 動作逾時：維持 `net_id == 0` 就刪，跟計劃一樣。
- **跟計劃不同的地方**：
  1. **從 spawn 建出來的子彈沒有標成 `bound = true`**（§3.3 原本要這樣做）。這些子彈也包括其他玩家的子彈，而 `has_predicted_projectile_net_id` 依賴 `bound`，用來決定遠端子彈的 despawn 要不要延到 world timeline（[kernel.cc](../engine/src/kernel/src/kernel.cc) `handle_client_despawn`），以及 snapshot entity 要不要另外畫。標成 bound 會改到遠端子彈的行為。
     Q4 改用另一種修法：spawn 只綁 `net_id == 0` 的純本機預測，從 spawn 建出來的子彈本來就有 net id，不會再被綁走。
  2. **Q3 比原本寫的嚴重**：舊的 snapshot 修正每次找到這個 instance 的第一顆子彈，**還會把它的 `net_id` 改成 snapshot 那顆的**。按住連射時，同一顆預測子彈會被輪流拉到每顆子彈的位置。
     現在先用 net id 找；找不到時，只有這個動作剛好一顆還沒命名的預測才會被綁定。有好幾顆的話，就等 spawn 送到。
- R1 的確認：沒有 spawn 對上的預測子彈，會走既有的 lifetime 清理（lifetime 到了先隱藏，再留 1 秒給 despawn，然後刪掉，見 `predicted_projectile_lifetime_cleanup_removes_batch_projectile`），不會一直留著。
- 測試（`client_mode_test`）：
  - `a_held_burst_predicts_every_pellet_of_every_commit`：2 次 commit × 3 顆，同一個 commit 重複呼叫不會多生，方向等於共用函式算出來的。
  - `each_spawn_binds_the_shot_it_names`：6 個 spawn 亂序送到，每顆都綁到正確的 net id，沒有重複、沒有遺漏。
  - `a_commit_the_client_did_not_predict_is_built_from_its_spawns`：client 沒預測到的 commit 會從 spawn 建出來，之後也不會再預測一次。
  - `a_release_drops_only_the_commits_past_the_confirmed`：確認 2 發時，只刪第 2 發的預測；第 0 發（已 spawn）和第 1 發（spawn 還在路上）都保留。
  - `a_snapshot_corrects_the_shot_it_names`：snapshot 只修正 net id 對應的那顆；有好幾顆還沒命名的預測時，不會綁到任何一顆。
  - mutation：重複檢查只看 instance、綁定時忽略 burst、snapshot 只用 instance 找、terminal 時忽略 commit，各自都會讓對應的測試失敗。
- 既有的測試：8 個 projectile 相關的 kernel 測試、`combat_test`、`projectile_spawn_index_roundtrip_test`、`dynamic_abi_smoke_test` 都通過。

**P3 實作紀錄**：
- 新增 `//engine/src/tests/kernel_tests:held_fire_end_to_end_test`，兩個 engine 透過 loopback 互傳：
  - dedicated server 加上純 Client（client 的 `world_` 裡沒有自己的玩家，測試開頭會檢查這點）。
  - input 編碼好之後從 `kInput` channel 注入 server。server 的封包（spawn、snapshot、action result）送回 client。
  - 場景是 spammer 的形狀：hold、burst 3、interval 2。按住打出 3 次 commit 共 9 顆，然後放開，再飛 30 tick。
- 檢查項目：
  - server 上有 9 顆子彈。
  - client 收到放開的 terminal 結果（`Corrected` / `Cancelled`，`confirmed_commit_count = 3`）。
  - client 每顆子彈都有 net id（沒有本機預測），而且 commit、burst、初速都跟 server 同一個 net id 的子彈一樣。每個 net id 剛好一顆，沒有重複也沒有遺漏。
  - 放開後的 30 tick 內每 tick 都檢查一次。
- 兩種 sync mode 各跑一次：
  - `local_predicted_deterministic`（spammer、水球法杖用的）：snapshot 裡**一顆都沒有**，子彈只靠 spawn 建出來。
  - `hybrid_deterministic_then_snapshot`：snapshot 裡最多 9 顆，這輪才會走到 snapshot 修正（Q3）。
- mutation（每一個都讓這個測試失敗）：
  - MA，放開時刪掉這個 instance 的所有子彈（`e75cd17` 以前）：子彈數量對不上，重現了使用者看到的現象。
  - MB，snapshot 只用 instance 找（P2 以前）：只有 hybrid 那輪失敗。
  - MC，spawn 綁定這個 instance 第一筆 `!bound` 的子彈（P2 以前）：burst index 對不上。
- 建測試時踩到一個坑：`World::spawn_player` 生出的玩家血量是 0，server 會用 `Dead` 拒絕動作。測試要自己設 `Health`，跟其他測試一樣。

### 3.4 不用改的

- **C ABI**：沒有新的 export，struct 也沒變，Unity 的 C# 不用改。
- **Snapshot schema**：維持 29（D4）。
- **遠端 client**：收到編號但用不到，行為不變。
- **game_server**：不用改 code。

---

## 4. 流量影響

每顆子彈對每個看得到它的 peer 各送一個 reliable 封包。現在大約是 entity spawn 105 B 加上 projectile spawn batch 100 B，約 205 B。

| 情境 | 每秒子彈數 | 現在（每個收到的 peer） | 增加 |
|---|---|---|---|
| Spammer 按住（30 commit/s × 3） | 90 | 約 18.5 KB/s | +360 B/s（約 2%） |
| 水球法杖按住（`4100c9a` 之後 2 commit/s） | 2 | 約 410 B/s | +8 B/s |

---

## 5. 版本與交付

- **Packet schema 30 → 31**：client 和 server 一定要用同一版 native library。
- **ABI 不變**（102）、**snapshot schema 不變**（29）。
- **`bundle.bytes`**：這個計劃本身不需要更新。但 `4100c9a` 改了 catalog 的參數，那次要更新 `bundle.bytes`，由使用者處理。
- 照規則在 `claude/*` 分支上 commit 並測好後交給使用者，不碰 `feat-unity-plugin` 和 package。

---

## 6. 實作階段

| 階段 | 內容 | 驗證 |
|---|---|---|
| P0 | 不改 protocol，先用 client 測試重現 Q2（burst 綁錯子彈），確認推斷正確。同時確認 `water_bubble_staff_test` 在 `4100c9a` 之後的狀態（見 R4） | 一個新的 client 測試；`water_bubble_staff_test` |
| P1 | Server 和 protocol：`ProjectileState`、`fire_projectile`、spawn record 的兩個欄位、encoder / decoder、schema 31、共用的 burst 函式 | protocol roundtrip 測試；`weapon_system` 的 burst 方向跟搬移前完全一樣 |
| P2 | Client：預測每個 commit 的每顆子彈、照編號綁定、照編號刪除、snapshot 修正改用 net id、從 spawn 建出的子彈標成 bound | `client_mode_test` 新增案例 |
| P3 | 兩個 engine 的 loopback e2e：spammer 按住 3 次 commit（9 顆）、放開，每顆都綁到正確的 net id，放開後繼續飛，沒有重複 | 新的 kernel e2e 測試 |

---

## 7. 測試策略

- **protocol**：`ProjectileSpawnRecord` 的編號 roundtrip；舊版 40 B 的 record 會被拒絕（schema 不同）。
- **client 單元測試**（`client_mode_test`）：
  - 同一個 instance 的 3 次 commit 都會在本機預測。
  - burst 3 顆各帶自己的 `burst_index`，spawn 依編號綁定，順序打亂也一樣。
  - terminal 結果 `confirmed_commit_count = 2` 時，只刪 `commit_index >= 2` 而且還沒綁定的子彈。
  - snapshot 修正套到 net id 對應的那顆，不是第一顆。
- **e2e**：P3 的 loopback 測試，要有一個「拿掉修正會紅」的對照，避免測試是空的（參考 memory「Test assertions can be vacuous」）。
- **既有的測試**：`client_mode_test`、`predicted_projectile_actor_hit_test`、`ground_follow_prediction_test`、`network_packets_test`（main 上本來就有紅的，用失敗清單比對，不看「全綠」）、`water_bubble_staff_test`。

---

## 8. 風險

| # | 風險 | 處理 |
|---|---|---|
| R1 | （只影響 listen-server 的 host，純 Client 沒有本機預測）client 和 server 的 commit 次數可能不同。例如 input 掉包，server 因為 `hold_input_timeout_ticks` 提早結束；或是 client 預測的開始 tick 跟 server 差一格。這樣預測的第 k 發會對不到 server 的第 k 發 | terminal 結果會刪掉超過確認數的子彈；中途對不上的子彈，綁定時會因為編號不同而不綁。P2 要確認沒綁定的預測子彈最後會被清掉（lifetime 到了，或動作逾時那條路徑） |
| R2 | 一次按住的預測子彈變多，client 每 tick 的預測成本變高 | spammer 最多 3 × 60 = 180 顆、水球法杖最多 8 顆。P3 量一下 `predicted_projectiles_` 的大小和每 tick 時間 |
| R3 | 搬移 `projectile_burst_directions` 時改到了數學 | 搬移前後比對 server 的方向，結果要完全一樣 |
| R4 | `water_bubble_staff_test` 寫死彈匣 3（`require(full == 3u)`），還用「按住 4 tick 打空」。`4100c9a` 改成 12 發、interval 15 之後應該會紅 | P0 先跑一次確認；測試要跟著新參數改，或改成從 catalog 讀值。要不要改由使用者決定 |
| R5 | 跟其他還沒 merge 的分支撞到 packet schema 版本 | 開工時和 merge 時各檢查一次（參考 memory「Parallel branches collide on numbers」） |

---

## 9. P0 驗證結果（2026-10-09）

在 `main`（`26668fd`）上的暫時 worktree 跑，臨時測試沒有 commit（diff 留在 session scratchpad，P2 改寫成正式測試）。

### 9-1. Q2：burst 綁錯子彈（已重現）

本機放一顆往正前方飛的預測子彈（instance 1234），再照 server 的順序送 3 個 spawn record：index 0（偏 +z）、正中、index 2（偏 -z），同一個 instance id。結果：

| client 的子彈 | net id | bound | 速度 | 實際對應 server 的 |
|---|---|---|---|---|
| 本機預測的那顆 | 101 | 1 | (5, 0, 0)，正前方 | **index 0，偏 +z** |
| 從 spawn 建出來 | 102 | 0 | (5, 0, 0)，正前方 | 正中 |
| 從 spawn 建出來 | 103 | 0 | (4.83, 0, -1.29) | index 2 |

- 正前方畫了兩顆（101、102 重疊），偏 +z 那顆畫面上沒有：它的 net id 掛在正前方的預測子彈上，綁定時只換 net id，沒有改速度。
- 從 spawn 建出來的子彈是 `bound = 0`，下一個 spawn 可能綁到它們身上（Q4），跟 §3.3 的推斷一致。
- 沒實測：server 的 101 撞到東西消失時，client 刪掉的會是正前方那顆（從 despawn 用 net id 刪子彈推斷）。

### 9-2. R4：`water_bubble_staff_test` 失敗（已確認）

`require failed at line 218: full == 3u`。`4100c9a` 把彈匣改成 12，第一個斷言就失敗。後面的「按住 4 tick 打空」也是照舊參數寫的（interval 現在是 15、lifetime 120），修好這行之後還會繼續失敗。
測試要改成符合新參數，還是改成從 catalog 讀值，由使用者決定；不在 P1 範圍內。

### 9-3. 其他

- 檢查了所有本機分支，packet schema 最高是 30，沒有分支用到 31（R5）。
- P0 結果跟計劃一致，P1 照 §3.1、§3.2 進行。

---

## 10. Token / 成本限制

- 先跑 smoke test：每個階段只跑直接相關的測試，不跑全部。
- 只讀直接相關的檔案：`kernel.cc` 只讀第 3 節列出的段落，`weapon_system.cc` 只讀 projectile 分支。
- 不做大範圍搜尋，不做 full build。
- 範圍如果擴大（例如需要改 snapshot 或 C ABI），先停下來說明。
- 每個階段完成時回報讀了哪些檔、改了哪些檔、搜尋了幾次、跑了哪些 build 和測試。
