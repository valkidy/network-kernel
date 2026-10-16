# Action graph 鎖定打擊（`lock_strike`）實作計劃書

狀態：P0–P4 完成（2026-10-16）。D1–D14 全部已定（D13 在 P4 補充，見下）。
前置：`main`（`8fa5250f`，ABI 106、packet schema 33、snapshot schema 30）。
關聯：`docs/LOCK_ON_WEAPONS_PLAN.md`（鎖定武器 18/19，本計劃沿用它的選目標、立即傷害、飛彈發射與 client 演出）。

---

## 1. 目標

讓 action graph 也能做「鎖定打擊」，用來做兩個投擲道具：

- `fungible_chain_lightning_bottle`：落地處每 10 ticks 鎖定一次最近的 M 個敵人，立即傷害，共 5 次（50 ticks）。
- `fungible_magic_missile_bottle`：同樣的節奏，每次對鎖到的目標各射一顆必中飛彈。

現況（2026-10-16 評估）：graph 沒有任何動作能呼叫鎖定邏輯，鎖定只存在武器開火路徑（`weapon_system.cc`）。現有組件只能做近似版：area effect 會打全部目標、沒有 M 上限、不擋牆；多發追蹤飛彈則通常全部追同一個目標。

本計劃新增三件事：

1. graph 動作 `lock_strike`，模式分 `instant`（立即傷害）和 `missile`（排定傷害加飛彈演出）。
2. 鎖定範圍的 collider 除了 cone，也支援 sphere。
3. `lock_strike` 內建 repeat：`interval_ticks` × `count`，由 server 排程，每次重新鎖定 M 個。

結果一律由 server 決定，client 只做演出，和武器 18/19 相同。

---

## 2. 決策

| # | 決策 | 狀態 |
|---|---|---|
| D1 | 新增一個 graph 動作 `lock_strike`（`KernelEntityTriggerActionType_LockStrike = 16`），用 `mode: instant \| missile` 區分兩種行為，不拆成兩個動作 | 已定 |
| D2 | 鎖定範圍 collider 支援 **sphere 和 cone**。sphere 只看半徑、不看角度，也不需要方向。cone 沿用武器的 3D 角度規則（lock-on D1）。排序（距離、net id）、擋牆規則、M 的來源（projectile `max_hit_count`）、傷害的來源（projectile `damage`）都和武器相同 | 已定 |
| D3 | repeat 用**做法 A**：`lock_strike` 自帶 `repeat: {interval_ticks, count}`，server 保留一筆排程（剩餘次數、下次 tick）。第一次在觸發當下執行，之後每 `interval_ticks` 一次，例如 10 × 5 = 第 0、10、20、30、40 tick。不用 status 當計時器（做法 B） | 已定 |
| D4 | 每次 repeat **獨立重新鎖定**：位置和方向固定在第一次觸發的值；同一個目標可以連續被選中；已死亡的目標不再被選 | 已定（2026-10-16） |
| D5 | 起點抬高：鎖定和擋牆射線的起點 = `position` + `origin_lift`（向上，可在 yaml 設定，預設 1.0 m），避免貼地的射線被地形起伏擋住 | 已定（2026-10-16） |
| D6 | 打擊紀錄走 **snapshot 新區段**（`kLockStrike = 8`），和飛彈紀錄一樣只在有紀錄時才寫。不另開 reliable 事件 | 已定（2026-10-16） |
| D7 | missile 模式一次打擊對**每個**鎖到的目標各射一顆，最多 M 顆同時發射；不採用武器那種「一個 commit 一顆、輪流打」的做法（graph 沒有 commit） | 已定（2026-10-16） |
| D8 | 排程**不依附**觸發它的實體：瓶子撞擊後自毀，剩下的次數照常執行。排程只在 server 重啟或關閉時清掉 | 已定（2026-10-16） |
| D9 | 來源與歸屬：紀錄的來源是 graph provenance 的 `instigator`，沒有就是 0。kill credit 維持現狀，瓶子沒有射手，不算給丟瓶子的人 | 已定（2026-10-16） |
| D10 | 上限：`interval_ticks` 1..255、`count` 1..16、`interval_ticks × (count − 1)` ≤ 255；整台 server 同時存在的排程上限 256 筆，超過就拒絕新的觸發並記 warning | 已定（2026-10-16） |
| D11 | 飛彈紀錄必須改格式（P0 前評估已確認，見 §3.5）：<br>• 帶發射點座標<br>• 允許來源為 0<br>• client 去重的 key 加上目標 | 已定 |
| D12 | 同一次打擊不足 M 個目標時，instant 模式照武器規則補假電弧，終點由 client 用同一個 hash 算出，不傳 | 已定 |
| D13 | 紀錄送給誰（P0-4 新增）：現在飛彈紀錄只送給「看得到射手」的 client，來源為 0 時沒有人收得到。graph 紀錄（飛彈和打擊）改為：看得到任一目標，**或**玩家距起點在 relevance 距離（40 m）內，就送。武器紀錄維持原規則。（P4 修訂：原本只有沒鎖到目標的打擊才看起點，見 §9b P4。） | 已定（2026-10-16） |
| D14 | snapshot 大小（P0-5 新增）：可選區段現在是在切包**之後**才附加到第一包，會超過 1200 B 的預算。改成先算區段大小，切包時第一包預留這段空間；單一 snapshot 的區段上限 400 B，超過的紀錄留到下一個 snapshot（`launch_tick_delta` 最多 255 ticks，足夠） | 已定（2026-10-16） |

---

## 3. 改動

### 3.1 選目標：sphere 支援與起點抬高（D2、D5）

`engine/src/simulation/public/lock_target_selection.h` / `src/lock_target_selection.cc`：

- `lock_targets::Cone` 加 `bool omnidirectional`（或改名為 `LockVolume`，P1 決定）。為 true 時 `select` 跳過角度檢查。
- `unfilled_arc_end` 加 sphere 版本：在球內取點（方向由 hash 決定，距離取半徑的 40–100%），cone 版本不變。
- 起點抬高由呼叫端處理，選目標函式不知道 lift。

新增 `resolve_lock_volume(collider_templates, collider_id, out)`：

- 接受 shape 為 `Sphere`（讀 radius）或 `Cone`（讀 range、fov），且必須帶 `KernelColliderPurpose_Damage`。
- 武器的 `resolve_weapon_cone` 不動。武器要不要也支援 sphere 不在本計劃範圍。

### 3.2 Server：`lock_strike` 動作（D1、D7、D9）

- `engine/src/simulation/public/action_graph.h` / `src/action_graph.cc`：新增 `ActionLockStrikeDefinition`，參數為：
  - `collider`：鎖定範圍
  - `projectile`：提供 M、傷害、collision_mask，missile 模式另提供最大飛行 tick
  - `mode`
  - `position`、`direction`（sphere 可省略 direction）
  - `origin_lift`
  - `repeat`

  準備階段產生 `ActionLockStrikeCommand`。
- `systems.cc` 執行指令：
  - 第一次立即執行，有 repeat 時另外排程（§3.3）。
  - 執行一次 = 建候選、`lock_targets::select`、依模式呼叫：
    - instant：沿用 `apply_lock_instant_damage`
    - missile：沿用 `launch_lock_missile`

    這兩個函式現在是 `weapon_system.cc` 內部的，要搬到共用位置，或開一個共用介面。
- 候選規則和武器相同：活著的 actor、陣營符合 projectile 的 collision_mask、不在建築內。另外排除 `instigator` 自己和它的 peer 控制的單位。
- 傷害的 `DamageRequest`：
  - `source_weapon_id` 填 provenance 的值（瓶子通常是 0）
  - `apply_tick`：instant 為當下，missile 為到達 tick，沿用 lock-on P2 的排定傷害，閃避和死亡在生效當下判斷

### 3.3 Server：repeat 排程（D3、D4、D8、D10）

- `KernelEngine` 新增 `pending_lock_strikes_`。每筆保存：
  - 完整參數，包括固定的 position 和 direction
  - provenance
  - `next_tick`、`remaining`
  - `strike_seq`（每次打擊遞增，給紀錄和去重用）
- `simulate_tick` 在傷害 drain 之前跑一次，時間到的就執行，然後 `remaining--`。
- **只在 server 模式執行**。pure client 的佇列本來就是空的（graph 只由 server 執行），仍然加 guard。
- 不依附來源實體（D8）：排程只需要資料，不查實體是否還在。

### 3.4 ABI（107）

`engine/src/kernel/public/kernel_types.h`：

- `KernelEntityTriggerActionType_LockStrike = 16`。
- `KernelActionDefinition` 尾端加入：
  - `lock_collider_template_id`
  - `lock_mode`（u8）+ 保留位
  - `lock_origin_lift`
  - `lock_repeat_interval_ticks`
  - `lock_repeat_count`

  projectile 沿用現有的 projectile 欄位。這個 struct 嵌在每個 trigger 定義裡，**所有 C# mirror 都會位移**，C# 端由使用者更新。
- `KernelLockMissileView` 尾端加 `origin`（`KernelVec3`）和 `flags`（bit 0：有發射點、不跟射手）。
- `KernelLockStrikeEvent` 用原本的 `reserved` 位元組當 `flags`（bit 0：graph 打擊、沒有 FireCommit），layout 不變。
- 不新增 export：
  - graph 的立即打擊進 `Kernel_PollLockStrikeEvents`，Unity 沿用遠端連鎖閃電的路徑。
  - graph 的飛彈進 `Kernel_GetLockMissiles`。

### 3.5 Protocol（snapshot schema 31）與 client（D6、D11、D12）

**打擊紀錄（新區段 `kLockStrike = 8`，只有 instant 模式需要）**：
- 欄位：
  - 起點：3 × float，抬高後的值
  - 方向：oct 編碼，4 B；sphere 時忽略
  - collider id：u16
  - `instigator`：u32，可為 0
  - `strike_seq`：u16
  - `launch_tick_delta`：u8
  - `arc_count`：u8，也就是 M
  - `target_count`：u8
  - 目標 net id：u32 × n
- 大小約 30 B + 4 B × n。
- client 收到後，用 collider 模板和 hash 補足假電弧，組成 `KernelLockStrikeEvent`（flags bit 0）。依 `launch_tick` 在 render time 到達時釋放，和遠端 FireCommit 的時機一致。

**飛彈紀錄（`kLockMissile = 7` 改格式）**：
- 加入：
  - 發射點：3 × float
  - flags：u8
- shooter 允許為 0：**現在解碼器會拒絕 0，被拒的是整個 snapshot**。這一點必須改，並加測試。
- 每筆從 12 B 變成約 25 B。武器的飛彈不帶發射點，可以用 flags 省掉座標，P4 決定。

**client 去重（`handle_client_lock_missiles`）**：
- key 從 (shooter, launch_tick, commit) 改為 (shooter, launch_tick, commit, target)。否則同一次打擊的 M 顆只會留下第一顆，同一位玩家的兩個瓶子在同一 tick 觸發也會撞 key。
- graph 飛彈的 `commit_index` 填 `strike_seq`。

**其他**：
- packet schema 不變（33）。
- `derive_lock_strikes`、`ClientQueryLockTargets`、投擲預測都**不受影響**（2026-10-16 已確認）：
  - 前兩者只對武器的 FireCommit 或本機開火反應。
  - 投擲預測只管飛行和落點。
- 唯一的小影響：`report_own_throw_blast` 的延遲 log 對不到這兩種瓶子，因為沒有生成爆炸投射物。只影響除錯紀錄。

### 3.6 game_server：yaml

`gameplay_config.cc`：

- 解析 `type: lock_strike`，參數：
  - `collider`、`projectile`、`mode`
  - `position`、`direction`
  - `origin_lift`（預設 1.0）
  - `repeat: {interval_ticks, count}`
- 載入時檢查：
  - collider 是 sphere 或 cone，且帶 damage purpose
  - cone 必須有 direction
  - projectile 的 `max_hit_count` ≤ `KERNEL_MAX_LOCK_ARCS`（16）
  - missile 模式的 projectile lifetime 在 1..255
  - repeat 符合 D10

新增共用 graph `action_graph_templates/common/action_lock_strike.yaml`，也可以再加一個附帶自我傷害的版本，給瓶子用（同 `action_spawn_projectile_and_damage_self`）。

### 3.7 Content（P5）

新 id 先暫定，開工時再確認是否已被其他分支使用（見 R3）：

| | 連鎖閃電瓶 | 魔法飛彈瓶 |
|---|---|---|
| item | 3033 `fungible_chain_lightning_bottle` | 3034 `fungible_magic_missile_bottle` |
| 世界 prop | 228 `chain_lightning_bottle` | 229 `magic_missile_bottle` |
| 鎖定範圍 collider | 54，sphere，半徑暫定 8 m | 55，sphere，半徑暫定 10 m |
| projectile | 35，dmg 暫定 10，M 暫定 4，`hostile_side` | 36，dmg 暫定 6，M 暫定 3，lifetime 20 |
| repeat | 10 × 5 | 10 × 5 |

- 兩個瓶子都照 `tornado_bottle` 的寫法：`on_collision` 觸發 lock_strike 並自我傷害。
- 要不要加進 camp loadout 的 `options`，P5 再決定。
- `docs/WEAPON_AUTHORING_GUIDE.md`（或 action graph 撰寫文件）加 `lock_strike` 一節。

### 3.8 不用改的

- 武器 18/19 的開火流程和 `resolve_weapon_cone`。
- status effect 系統（不採用做法 B）。
- 投擲預測、`PredictedThrow`。
- packet schema。

---

## 4. 流量影響（估計，P4 實測）

- 一次 instant 打擊（M = 4）：一筆打擊紀錄約 46 B，只出現在一個 snapshot，每個看得到的 client 各一次。5 次 repeat 共約 230 B。
- 一次 missile 打擊（3 顆）：約 75 B。
- 傷害仍然走現有的戰鬥事件。每命中一次就有 HitConfirmed + DamageApplied 兩個 reliable 封包，這才是主要流量。合併方式見 `docs/COMBAT_EVENT_BATCHING_PLAN.md`。
- 以 4 人同時各丟一個瓶子估計，戰鬥事件的峰值大約和 4 把 Chain Lightning 同時開火相近（約 7 KB/s / client），但只持續 50 ticks。

---

## 5. 版本與交付

| 項目 | 變動 |
|---|---|
| ABI | 106 → **107**（新動作 enum、`KernelActionDefinition` 尾端欄位、`KernelLockMissileView` 尾端欄位） |
| snapshot schema | 30 → **31**（新區段 8、飛彈紀錄格式） |
| packet schema | 不變（33） |
| `bundle.bytes` | 要更新（新 collider、projectile、prop、item） |
| C# mirror | `KernelActionDefinition`、所有內嵌它的 trigger 定義、`KernelLockMissileView`、`KernelLockStrikeEvent.flags`，由使用者更新 |
| Unity | 讀 `KernelLockStrikeEvent.flags` 和 `KernelLockMissileView.origin`/`flags`，其餘沿用 lock-on 的演出路徑 |

測試裡寫死版本號的地方（`kernel_c_header_test.c`、`dynamic_abi_smoke_test`、`kernel_api_test`、`snapshot_lock_missile_roundtrip_test`）要一起更新。

---

## 6. 實作階段

| 階段 | 內容 | 驗收 |
|---|---|---|
| P0 | 只讀確認（§9） | 回報結果，必要時修正計劃 |
| P1 | 選目標：sphere、假電弧 sphere 版、`resolve_lock_volume` | `lock_target_selection_test` 新增 sphere 案例 |
| P2 | `lock_strike` 單次（instant、missile），ABI 107，yaml 解析 | server 端單元測試、`lock_strike_catalog_test` |
| P3 | repeat 排程 | `lock_strike_repeat_test` |
| P4 | 同步：打擊紀錄區段、飛彈紀錄改格式、client 去重和事件，snapshot schema 31 | roundtrip 測試、e2e（dedicated + 2 pure client）、listen host |
| P5 | Content：兩個瓶子、撰寫文件 | 載入真實 catalog 的測試、mutation 檢查 |

每個階段一個 commit，在 `claude/*` 分支上，不 merge。

---

## 7. 測試策略

- **P1**：
  - sphere 不看角度，背後的目標也會選到
  - 擋牆跳過、排序和 cone 一致
  - 假電弧終點落在球內 40–100% 半徑
- **P2**：
  - instant：M 個最近、傷害等於 projectile 的值、不打 instigator 和同 peer 單位、牆後跳過
  - missile：到達 tick 才扣血，目標中途死亡就取消
  - 起點抬高後，原本被地面起伏擋住的目標可以選到（對照組不抬高時選不到）
- **P3**：
  - 10 × 5 恰好在 t、t+10、…、t+40 各執行一次
  - 第 3 次之前殺掉一個目標，之後不再選它
  - 觸發實體在第 1 次之後被摧毀，剩下 4 次照常
  - 超過上限的觸發被拒絕
- **P4**：
  - 打擊紀錄 roundtrip
  - shooter 為 0 的飛彈紀錄可以解碼（舊規則下整個 snapshot 被拒，作為對照）
  - 同一 tick 的 M 顆飛彈在 `GetLockMissiles` 全部存在
  - 遠端 client 從 `PollLockStrikeEvents` 拿到 graph 打擊（flags bit 0）
  - listen host 自己也收到
- **P5**：真實 catalog 載入兩個瓶子，從丟出到 5 次打擊的端到端流程。
- 每個新測試都做 mutation（例如拿掉 repeat 遞減、去重 key 拿掉 target），確認測試會變紅。
- 只以失敗集合判斷：和 `main` 相同的那組紅燈不算回歸。

---

## 8. 風險

| # | 風險 | 處理 |
|---|---|---|
| R1 | snapshot 新區段不在 split 預算內：飛彈區段已有同樣問題（lock-on 的已知待辦，P0-5 確認） | D14 |
| R2 | 排程佇列無上限會被大量投擲撐爆 | D10 的上限加 warning |
| R3 | 平行分支撞號：ABI 107、snapshot 31、catalog id（3033/3034、228/229、35/36、54/55）。`claude/combat-event-batching-plan` 也會動 packet schema | 開工時和 merge 時都檢查一次（參考 lock-on 與 portal 的 ABI 重編號經驗） |
| R4 | client 用 hash 補假電弧時，collider 模板若在 client 和 server 不同步會畫錯 | 和武器相同：catalog bundle 一致就一致，不另做處理 |
| R5 | 起點抬高後可能穿過低矮天花板 | 抬高前先做一次向上射線，碰到就停在碰撞點下方 |
| R6 | C# mirror 位移漏改會讓 Unity 讀錯所有 trigger 定義 | 交付時明確列出受影響的 struct；ABI 不同時 package 會直接拒絕 |

---

## 9. P0 驗證結果

2026-10-16，在 `main`（`8fa5250f`）上只讀，沒有改程式。

| # | 問題 | 結果 | 對計劃的影響 |
|---|---|---|---|
| P0-1 | 瓶子 `on_collision` 的 graph provenance | `instigator` 固定為 0；`owner_peer` 是丟瓶子玩家的 peer（[systems.cc:2473](../engine/src/simulation/src/systems.cc)，從 `ThrownPropMotion.thrower_net_id` 反查）。丟瓶子者的 net id 沒有放進 provenance | 候選排除用 `owner_peer`：丟瓶子的玩家和他控制的單位不會被鎖定。紀錄來源為 0（D9 不變） |
| P0-2 | 鎖定函式對射手的依賴 | `select_lock_targets`、`apply_lock_instant_damage`、`launch_lock_missile`（[weapon_system.cc:466–640](../engine/src/simulation/src/weapon_system.cc)）從 `WeaponMechanicsDefinition` 讀 `melee_range`、`melee_fov_degrees`、`projectile_template_id`、`collision_mask`、`id`、`damage`；射手只用到 net id（排除自己、射線忽略自己）和 peer。來源為 0 都能正常運作。`launch_lock_missile` 每次只射 `commit_index % locked` 那一個目標 | P2：抽出一個不依賴武器的參數 struct（範圍、角度或全方向、projectile、mask、weapon id、傷害），武器從 definition 組出它。飛彈拆成「對單一目標發射」，武器和 graph（D7，每個目標各一顆）共用 |
| P0-3 | 來源為 0 時的排定傷害、取消、擊殺 | 都正常。`submit_damage_request` 不看來源；`damage_source_may_damage` 只看目標陣營；取消只依目標（`cancel_pending_for_target`）。`owner_peer` 是丟瓶子的玩家，所以對玩家沒有防禦寬限延遲。擊殺者為 0，和現有瓶子相同 | 無 |
| P0-4 | listen host 收不收得到 | **收得到**：listen host 的 `publish_snapshot` 也替本機 session 附上飛彈（[kernel.cc:16475](../engine/src/kernel/src/kernel.cc)），經本機 transport 回到 `handle_client_snapshot`。目前沒有測試。<br>**新問題**：`lock_missiles_for` 只把紀錄送給「看得到射手」的 session（[kernel.cc:15301](../engine/src/kernel/src/kernel.cc)），來源為 0 的紀錄**不會送給任何人** | 新增 D13。P4 加 listen host 測試 |
| P0-5 | snapshot 大小 | 切包上限是 `kSnapshotSendBudgetBytes = 1200`。飛彈區段在 `split_snapshot_for_packets` **之後**才放進第一包，所以第一包可能超過上限，lock-on 的已知待辦（R1）就是這個 | 新增 D14，P4 一起修，武器的飛彈也受益 |
| P0-6 | 平行分支撞號 | 沒有撞到。未 merge 的分支 ABI 最高 106（`feat-unity-plugin`），snapshot schema 最高 30。計劃用的 id（item 3033/3034、entity 228/229、projectile 35/36、collider 54/55）在任何分支都沒有被用到 | merge 前再查一次 |

**其他確認**：damage pipeline 不檢查 `sequence_id` 重複，只拿它排序，所以同一 tick 多個瓶子各自從 0 編號不會互相吃掉。

---

## 9b. 實作紀錄

### P1：選目標支援 sphere（2026-10-16）

- `lock_targets::Cone` 加 `omnidirectional`。為 true 時，`select` 不看 aim 和 fov，只看半徑；排序、上限、擋牆規則不變。`unfilled_arc_end` 的 sphere 版本在整個球面上均勻取方向，距離仍是半徑的 40–100%，同一發的結果固定、和 aim 無關。cone 版本的計算順序和數值都沒有變。
- `kernel.cc` 新增 `resolve_lock_volume(templates, id, sphere_allowed, out)`：cone 讀 range/fov，sphere 讀 `shape_params.x` 當半徑，兩者都要有 damage purpose。`resolve_weapon_cone` 改成呼叫它並傳 `sphere_allowed = false`，武器仍然只接受 cone。graph 的 `lock_strike`（P2）會傳 true。
- 測試：`lock_target_selection_test` 新增兩個案例（sphere 各方向都鎖得到、aim/fov 被忽略、擋牆；sphere 假電弧四周都有、距離範圍、與 aim 無關）。
- Mutation：
  - 拿掉 sphere 假電弧分支：測試變紅。
  - 武器允許 sphere：`catalog_weapon_definition_test` 變紅。
  - 拿掉 `select` 裡 sphere 跳過角度檢查的條件：**測試仍綠，這是等價變異**。sphere 的 aim 設為 0、門檻設為 0，`0 < 0` 永遠不成立，角度檢查本來就不會擋任何目標。保留這個條件，是為了讓意圖一眼看得出來。
- 順手修好兩個在 `main` 上就編不過的測試：`catalog_weapon_definition_test`、`lock_weapons_end_to_end_test` 還在設定 projectile 的 `weapon_id`，這個欄位在 ABI 106 改名為 `reserved_weapon_id` 且必須為 0。刪掉那一行即可，兩個測試都恢復通過。
- 通過：`lock_target_selection_test`、`lock_instant_test`、`lock_missile_test`、`catalog_weapon_definition_test`、`lock_weapons_end_to_end_test`、`//game_server:lock_weapon_catalog_test`。`kernel_api_test` 失敗，屬於既有紅燈名單。

### P2：單次 `lock_strike`，ABI 107（2026-10-16）

**重構（`39c8d7b0`，行為不變）**
- 選目標、立即傷害、發射飛彈從 `weapon_system.cc` 搬到 `simulation/{public,src}/lock_strike.{h,cc}`。
- 參數改成 `LockStrikeSpec`，內容是範圍、shot、mask、weapon id、傷害，不再讀 `WeaponMechanicsDefinition`。
- `launch_lock_missile` 改成「對指定的一個目標發射」。「第 commit % N 近」的挑法留在武器端。
- 武器的四個 lock 測試都維持通過。

**ABI 107**
- `KernelEntityTriggerActionType_LockStrike = 16`。
- `KernelLockStrikeMode`（Instant 0、Missile 1）。
- `KERNEL_MAX_LOCK_ORIGIN_LIFT`（5 m）。
- `KernelActionDefinition` 尾端加 16 B：
  - `lock_collider_template_id`
  - `lock_mode`、保留位
  - `lock_repeat_count`
  - `lock_repeat_interval_ticks`
  - `lock_origin_lift`

  shot 沿用 `spawn_projectile_template_id`。`kernel_c_header_test` 已更新 offset 與 size。

**Kernel**
- `KernelEngine::lock_strike_is_runnable`：batch 的事前檢查。
- `KernelEngine::execute_lock_strike`：執行。
  - 起點抬高後，若頭上有地形或靜態障礙，停在其下方 0.1 m（R5）。
  - M、傷害、mask 從 shot 讀。
  - 用 provenance 的 `instigator` 和 `owner_peer` 排除丟瓶子者。
  - instant 立即傷害；missile 對每個鎖到的目標各發一顆（D7），`commit_index` 暫時都是 0（P3 改成打擊序號）。
- 載入 catalog 時分兩段檢查：
  1. 欄位本身：在 entity trigger 迴圈裡檢查。
  2. 引用的 collider 和 shot：在 collider、projectile 都驗證完之後才檢查。
     - collider 必須是有 damage purpose 的 cone 或 sphere。
     - cone 必須有 direction。
     - shot 的 `max_hit_count` ≤ 16。
     - missile 模式下，shot 的 lifetime 在 1..255。
- 只允許出現在 entity trigger。projectile、item、status trigger 都會拒絕。
- repeat 欄位已經在 ABI 裡，但 P3 之前，非 0 的 repeat 會被拒絕。

**game_server**
- graph 動作 `type: lock_strike`，欄位：
  - `collider`、`projectile_template`、`position`：params 參照
  - `direction`：params 參照，可省略
  - `mode`：字面值或 params
  - `origin_lift`：字面值或 params，預設 1.0
- 編譯時檢查的規則和 kernel 一樣，錯誤訊息會指出是哪個 graph。
- `compile_action_trigger_binding` 多一個參數 `colliders`，五個 entity trigger 的呼叫處都有傳入。
- lock_strike 用的 shot 沒有任何東西生成它，所以加進 unspawned shot 清單，逐個 binding 找出來。
- graph 動作的 key 白名單加入 `collider`、`mode`、`origin_lift`。
- catalog hash 加入新欄位。
- 新增共用 graph：`action_lock_strike`、`action_lock_strike_and_damage_self`。

**測試**
- `lock_strike_test`（kernel）：
  - sphere 鎖四周最近的 2 個、不鎖丟瓶子者、半徑外不鎖。
  - cone 只鎖前方。
  - 每顆飛彈在各自的到達 tick 扣血，不會提早。
  - 抬高 4 m 後飛行時間變長。
  - catalog 會拒絕的 10 種情況。
- `lock_strike_catalog_test`（game_server）：
  - yaml 編譯出正確的 `KernelActionDefinition`，kernel 能載入。
  - cone 加 lift。
  - 6 種錯誤都在 game_server 被擋下，而且錯誤原因正確。
- Mutation：kernel 6 個、game_server 3 個，全部讓測試變紅。

**尚未覆蓋**
- 物理世界裡的擋牆和天花板截斷。測試的 engine 沒有碰撞世界，留到 P5 的端到端測試。
- graph 飛彈的紀錄現在不會送給 client：`lock_missiles_for` 依射手可見度過濾，來源為 0 就送不出去（D13，P4 處理）。所以 P2 不會產生 shooter 為 0 的 snapshot 紀錄，也不會觸發解碼失敗。

**回歸**
- 相關的 45 個 kernel、simulation、game_server 測試裡，37 個通過。沒通過的都是 `main` 上就有的：
  - 已知紅燈：`gameplay_config`、`hitscan`、`damage_pipeline`、`kernel_api`、`kernel_c_header`。`kernel_c_header` 是 `RenderEntityState` 大小的斷言，`main` 上也有這行。
  - `main` 上就編不過：
    - `targeted_strike_test`、`status_action_block_test`：還在用 projectile 的 `weapon_id`。
    - `combat_test`：呼叫兩參數版的 `drain_ready_damage`。
  - game_server 另外 4 個測試也在用 `weapon_id`，同樣編不過：`actor_intent_executor`、`agent_chaser_controller`、`agent_sentry_controller`、`patrol_director`。

### P3：repeat 排程（2026-10-16）

**Kernel**
- 新增 `pending_lock_strikes_`，每筆存放：
  - 第一次觸發時的完整指令，位置和方向固定（D4）
  - `next_tick`
  - `remaining`
  - `strike_index`
- `execute_lock_strike` 立即打第一次。如果 count > 1，就排入佇列，`next_tick = 現在 + interval`。
- `run_due_lock_strikes` 只在 server 模式下執行：
  - 依排入的順序，執行到期的打擊，每次都重新鎖定。
  - 打完就把 `next_tick` 加上 interval，次數用完就移除。
  - 呼叫位置在 `simulate_tick` 裡：本 tick 所有 action graph batch 執行完之後、傷害 drain 之前。所以到期的 instant 打擊在當 tick 就會生效。
- 佇列和觸發它的實體無關（D8），server 重設時清空。
- 佇列上限 256（D10）：
  - 滿了的時候，帶 repeat 的打擊在 batch 事前檢查就被拒絕，並記 warning。整個 batch 不執行，包括瓶子的自我傷害，所以瓶子會留著，等有空位時再觸發。
  - 不帶 repeat 的打擊不受影響。
- 飛彈的 `commit_index` 改為 `strike_index`，從 0 起算，P4 用它區分同一輪的各次發射。
- 合法的 repeat：
  - 一次：count 0 或 1，且 interval 為 0。
  - 多次：count 2..16、interval ≥ 1、`interval × (count − 1)` ≤ 255。
  - 新增巨集 `KERNEL_MAX_LOCK_STRIKE_REPEAT`、`KERNEL_MAX_LOCK_STRIKE_SPAN_TICKS`，ABI 仍是 107。
  - kernel 載入、執行前檢查、game_server 編譯三處用同一套規則。

**game_server**
- `lock_strike` 支援 `repeat: {interval_ticks, count}`，兩者都可以是字面值或 params。
- 共用 graph 新增參數 `repeat_count`（預設 1）、`repeat_interval`（預設 0）。

**測試**
- `lock_strike_test` 新增：
  - 10 × 5 剛好在 t、t+10、…、t+40 各打一次，之後不再打，佇列清空。
  - 第一次打擊後殺掉一個目標，第二次改鎖下一個最近的。
  - repeat 飛彈的 `commit_index` 依序為 0、1、2，發射 tick 相差 interval。
  - 佇列滿時，repeat 打擊整批被拒，單次打擊照常執行。
  - catalog 的 repeat 合法與不合法案例。
- `lock_strike_catalog_test` 新增：
  - repeat 經由 binding 綁定後能編譯，kernel 能載入。
  - 三種不合法的 repeat。
- Mutation 7 個（kernel 5、game_server 2），全部讓測試變紅。

**尚未覆蓋**
- `simulate_tick` 裡呼叫 `run_due_lock_strikes` 的那一行和 server 模式的 guard。單元測試是直接呼叫函式，P5 的端到端測試會補上。

**回歸**
- 和 P2 同一組 45 個測試：37 通過。8 個沒通過的和 P2 完全相同，都是 `main` 上本來就有的。

### P4：同步給 client，snapshot schema 31（2026-10-16）

**Wire（snapshot schema 30 → 31）**
- 飛彈紀錄（區段 7）：
  - 多 1 B `flags`。
  - 帶 `kSnapshotLockRecordHasOrigin` 時再多 12 B 的發射點。武器飛彈 13 B，graph 飛彈 25 B。
  - 只有帶發射點的紀錄允許 shooter 為 0，未知的 flag 一律拒絕。
- 新區段 8 `kLockStrike`（instant 打擊）：
  - 欄位：起點、方向（每軸 16 bit）、collider id、instigator、strike index、launch tick delta、arc_count（1..16）、目標 id。
  - 大小 31 B + 每個目標 4 B。
  - 解碼時檢查 arc_count 範圍、目標數 ≤ arc_count、目標 id 非 0、collider 非 0。
- 新增 `snapshot_lock_record_bytes` 等大小函式，和編碼器逐位元組一致。
- packet schema 不變（33）。

**Server**
- `LockMissileLaunch` 加入 `from_origin`、`origin`。graph 飛彈從抬高後的起點發射。
- instant 打擊另外記一筆 `PendingLockStrikeRecord`。
- **D14**：`choose_lock_records_to_send` 在每次 snapshot 依舊到新，挑出不超過 400 B 的紀錄，其餘留到下一次。超過 255 tick 的紀錄直接丟掉。
  - 每個 session 都會：
    - 在第一包的切包預算和 send set 預算裡，扣掉自己這份紀錄的大小。
    - 把紀錄放進第一包。
- **D13（補充）**：
  - 武器飛彈：看得到射手才送，規則不變。
  - graph 的飛彈和打擊：看得到任一目標，**或**玩家在起點的 relevance 距離內，就送。
  - 原本的 D13 只對一個目標都沒鎖到的打擊看起點。P4 實測發現一個問題：紀錄被 D14 延到下一次 snapshot 時，目標可能已經被同一批打擊打死、從 relevance 裡消失，紀錄就送不到任何人。所以起點規則改為一律適用。

**Client**
- 飛彈去重的 key 加入目標和發射點。同一次打擊的 M 顆都會保留，兩個瓶子同 tick 打同一目標也不會互吞。
- `KernelLockMissileView`：
  - 原本的 `reserved` 改為 `flags`（`KERNEL_LOCK_MISSILE_FLAG_FROM_ORIGIN`）。
  - 尾端加 `origin`，大小從 24 B 變成 36 B。
- 打擊紀錄會先暫存。等 render 時鐘到達 launch tick，才轉成 `KernelLockStrikeEvent` 交給 `Kernel_PollLockStrikeEvents`：
  - `flags = KERNEL_LOCK_STRIKE_FLAG_GRAPH`
  - shooter 是 instigator，可能為 0
  - `commit_index` 是 strike index
  - 鎖到的目標，弧畫到畫面上的 hitbox 中心；畫面上沒有的目標，當成假弧處理。
  - 假弧由 client 用 collider 模板和 hash 算出：seed 是 instigator、launch tick、strike index，所以每個 client 結果相同。弧碰到牆會截斷。
- 不新增 export。

**測試**
- `snapshot_lock_missile_roundtrip_test`：
  - 13 B 的武器飛彈。
  - graph 飛彈（shooter 0、帶發射點）。
  - 有目標和沒目標的打擊。
  - 四個可選區段同時出現。
  - 大小函式和編碼一致。
  - 5 種壞掉的打擊紀錄和 4 種壞掉的飛彈紀錄都會被拒絕。
- 新增 `lock_strike_end_to_end_test`（dedicated server + 2 個 pure client，loopback）：
  - repeat 3 × 6 由 server 自己的 tick 執行。這也補上 P3 沒覆蓋的 `simulate_tick` 呼叫。
    - 兩個 client 各收到 3 個打擊事件，編號 0、1、2。
    - 弧畫到兩個目標的中心。
    - 交出事件的時間點，render tick 在 launch tick 和 launch tick + 2 之間。
  - graph 飛彈：
    - 兩個 client 都列出 2 顆，shooter 0，從抬高後的起點發射。
    - server 在各自的到達 tick 扣血。
  - D13：
    - 遠處的打擊不送給任何人。
    - 沒鎖到目標、在附近的打擊兩人都收到，假弧在球內，且兩個 client 完全一致。
    - 沒鎖到目標、在遠處的打擊不送。
  - D14：
    - 場上 120 個實體把第一包塞滿，同一 tick 有 20 筆打擊。
    - 第一次 snapshot 只送 10 筆，其餘之後陸續送完。
    - 所有 snapshot 封包都不超過 1200 + 28 B。
    - 目標在中途被打死，其餘紀錄仍然送到。
  - listen host 送給自己的 snapshot 裡有這筆打擊。
- `kernel_c_header_test`：新 struct 的 layout 和 flag 數值。`dynamic_abi_smoke_test`、`kernel_api_test` 的 snapshot schema 改為 31。
- Mutation 8 個，全部讓測試變紅：
  - 解碼器接受沒有 shooter 也沒有發射點的飛彈
  - 去重 key 沒有目標
  - graph 飛彈改用射手判斷可見
  - 不看 400 B 預算
  - 切包不預留空間
  - 不等 render 時鐘就交出打擊
  - 拿掉起點規則
  - 不記錄打擊

  其中「不等 render 時鐘」第一次跑時沒抓到，補了 render tick 的檢查才抓到。

**回歸**
- 62 個相關測試，51 個通過。11 個沒通過的都是 `main` 上就有的：
  - P3 那 8 個。
  - `relevance`、`network_packets`、`session_packets`：已知紅燈。把 P4 的改動 stash 掉再跑一次，失敗的行號和訊息完全相同。
  - `listen_server_test`：同樣卡在 projectile 的 `weapon_id`，`main` 上就編不過。

---

## 10. Token / 成本限制

- 先跑 smoke test：每個階段只跑直接相關的測試，不跑全部。
- 只讀直接相關的檔案：
  - `weapon_system.cc` 只讀鎖定相關函式
  - `kernel.cc` 只讀 §3.5 列出的 client 段落和 `simulate_tick`
  - `action_graph.cc` 只讀動作準備和事件欄位檢查
- 不做大範圍搜尋，不做 full build。
- 範圍如果擴大（例如需要動 packet schema 或武器開火流程），先停下來說明。
- 每個階段完成時回報讀了哪些檔、改了哪些檔、搜尋了幾次、跑了哪些 build 和測試。
