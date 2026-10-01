# 帳篷建築（Tent）實作計劃書

狀態：**設計已定案。K1、K2 與 game_server 的進出流程已實作（見 §3.10）；K3–K6 未做。**
分支：`claude/tent-building`，2026-10-01 rebase 到 `claude/apply-pull`（a7da594）之上，
所以必須在 apply-pull 之後 merge。ABI 接著 apply-pull 的 95 升到 96。
最後更新：2026-10-01（第五輪 review：進出沿用既有流程、視野隱藏做成 YAML 選項、進入前提、住客上限 4）。

本文件整理 2026-09-30 的設計討論與實測結果，以及 2026-10-01 第二到四輪 review 的決定。
第 9 節列出第一輪的弱點分析，第三、四輪已全部回覆。

---

## 1. 目標與範圍

設計目標（使用者原話整理）：

> 建築物 prop，提供可讓玩家 interact 的介面，但從生成開始有一個規範好的
> 存在時間，避免數量重複過多。例如：休息 UI 可供玩家整備、商店 UI 可讓玩家交易。

拆成兩個 task：

| Task | 內容 | 狀態 |
|---|---|---|
| **A：帳篷 prop** | 丟擲與生成、存在時間、數量上限、進出帳篷流程（移動遮罩、無敵、入住鎖）、所有退出路徑 | catalog + probe 已做；kernel / game_server 未做 |
| **B：帳篷內互動（鍋爐）** | 投擲物品進鍋爐，依組合配方給全隊 buff | 使用者另行設計，未開始；**第一版不做** |

不在範圍內：
- **中型箭塔**：已移到另一個對話（allied_beam_sentry 系的 actor，與帳篷的 prop 路線不同）。
- **Unity package build / stage / bump**：由使用者處理。

---

## 2. 決策

### 2.1 已定案

| # | 決策 | 備註 |
|---|---|---|
| D1 | 帳篷是建築 prop，有 HP，HP 歸 0 時被破壞（同蟲巢），住客被迫退出 | 不照抄蟲巢 `on_health_depleted` 反傷攻擊者 |
| D2 | 丟擲流程比照 ice block：瓶子大小的 kit 飛行，落地後在接觸點展開成帳篷 | 見 §3.1 |
| D3 | 落地推開單位：沿用靜態障礙物的物理推開，不另外掛 AoE action graph | 實測有效，見 §4 |
| D4 | 落在 prop 上懸空、或撞到 prop 側面半嵌入：都視為「玩家失誤」，只能等帳篷自然消失 | **硬性要求：退出時出口點必須先確認是空的** |
| D5 | **存在時間只在生成時設定，之後不變**（比照 ice block）。L = 5400 ticks（3 分鐘，開發用）/ 9000 ticks（5 分鐘，正式用）。玩家進出完全不影響存在時間 | 第二輪改定，取代「首次進入縮為 N」。見 §5 |
| D6 | UI 必須顯示帳篷剩餘秒數 | 由 `spawn_tick + L` 算出，不需要 status（見 §3.4） |
| D7 | **建築物與 temporary_deployable 分成不同的 population group**；建築物 group 上限 8，滿了擠掉最舊的。全場配置 = 整隊持有 1 + 地圖生成，合計 ≤ 8 | 第二輪改定（原為 4）。最終目標仍是每人上限 |
| D8 | 進入建築物的共用規則：玩家移到建築內部 + 無敵 + 入住鎖；內部 UI 依建築種類各自設計 | 休息帳篷與商店共用 |
| D9 | 進入帳篷不再單純回血；回血等 buff 改由 Task B 的鍋爐決定 | |
| D10 | Task B：配方是組合；丟歪不消耗道具；每個帳篷有投放次數上限 | 細節未定；第一版不做 |
| D11 | **建築物種類只由綁定的 action graph 決定。** `on_activated` 綁定的 graph 用新 action `open_ui {ui_id}` 宣告這是哪一種建築；所有 population group 的機制一致 | 見 §3.3 K1 |
| D12 | **清除類的消失（Expired、CapacityEvicted）也觸發 `on_destroy_entity`。** 用途：建築物消失時對住客施加 impact，讓住客飛走 | 見 §3.5；需要改 kernel（K4） |
| D13 | 退出：住客在帳篷內再按一次互動即退出。第一版只做進出，不做建築內的互動操作 | 入住鎖只放行對該建築的 Activate |
| D14 | 互動距離維持 2.0；碰不到（例如帳篷架在冰塊頂上）視為玩家失誤，不處理 | |
| D15 | 死亡的玩家不能進入建築物 | 見 §3.2、§8 |
| D16 | 住客不受 stagger 影響 | 第三輪。由入住鎖（K2）處理 |
| D17 | **所有建築物共用一個 population group**（例如帳篷 + 商店 + …… 合計 ≤ 8），以 group 篩選 | 第三輪。取代「每種建築各一個 group」的選項 |
| D18 | 開發 / 正式兩組 L 做成兩份 YAML | 第三輪。見 §3.1 |
| D19 | kit 投擲距離比照 ice block，讓投擲距離的設定一致 | 第三輪。**已經一致**：`tent_kit` 與 ice block 的 `stateful_magic_bottle` 都用 `grenade_shell_projectile`，不需要改 |
| D20 | **建築物不主動承受或吸引威脅，只保留被意外破壞的可能**。不加入敵人的視野候選；HP 設高值 | 第三輪。回覆 §9-1 |
| D21 | 友軍傷害不保護：玩家自己的 AoE 炸掉自己的建築，屬於玩家自主行為。原則是建築不吸引仇恨，只需讓玩家觀察到它可能被破壞 | 第三輪。回覆 §9-8；client 需要顯示建築受損（例如 HP） |
| D22 | 兩份 L 的 YAML：`entity_templates/tent.yaml`（id 216，正式，9000）與 `entity_templates/tent_dev.yaml`（id 217，開發，5400）。由 `tent_kit_prop.yaml` 的 `on_collision` 參數 `template` 決定生成哪一個 | 第四輪。對齊現有機制：catalog 只有一個 entry、依目錄載入所有 template，沒有 overlay 或 dev / prod 變體機制，所以用兩個獨立的 template |
| D23 | 建築 HP 預設 5000 | 第四輪。`Health.hp` 是 uint16（上限 65535），放得下 |
| D24 | 建築共用上限時擠掉地圖生成的建築，可以接受，屬於玩家的「意外行為」 | 第四輪 |
| D25 | 數量限制只靠道具的 `max_stack`（`tent_kit` 為 1）與 group 上限；建築太多而互相擠掉，定位為 gameplay 設計問題（邏輯正確但設計錯誤），不另外加系統限制（不做每人上限） | 第四輪。回覆 §9-7 |
| D26 | 不指定陣營的投射物（例如 `spammer`）會穿過建築，定位為設定問題，不應出現在正式版資料。catalog 載入時印 log warning | 第四輪。回覆 §9-9；見 §3.9 |
| D27 | 入住狀態只存在玩家身上（`Sheltered{shelter_net_id}`），建築的住客以查詢取得，不在建築上存名單 | 第四輪。見 §3.6 |
| D28 | 入住狀態同步給 client（只給擁有者），client 預測時跟著切換遮罩、移動歸 0 | 第四輪。見 §3.7；需要改 snapshot schema |
| D29 | 生成建築前做安全落點校正（不保證 100%），伺服器端做；client 由 kit 的最後位置與建築的生成位置自行推導反彈演出 | 第四輪。見 §3.8 |
| D30 | **進出沿用既有流程**：`open_ui` 發事件 → game_server → 經 command queue 呼叫 K2。不為新系統客製化既有流程；command queue 造成的一個 tick 延遲可以接受 | 第五輪。回覆 G1 |
| D31 | 「住客不出現在敵人的視野候選中」做成建築 template 的 YAML 選項，方便實際遊玩時測試效果 | 第五輪。回覆 G3；見 §3.6 |
| D32 | 進入的前提條件：未死亡、不在任何建築內、不在擊退中；搬著的 prop 先放下；進行中的動作取消 | 第五輪。回覆 G4 |
| D33 | 每個建築的住客上限 4 | 第五輪。回覆 G5；目前程式裡沒有玩家上限常數可以共用，見 §3.6 |
| D34 | K5（入住狀態同步給擁有者，snapshot schema 變動）放進第一版 | 第五輪。client 與伺服器必須同一版 |

### 2.2 第二輪撤回的項目

| 原項目 | 撤回原因 |
|---|---|
| N（首次進入時剩餘時間縮為 min(剩餘, N)） | D5：存在時間只在生成時設定。理由：遠方的玩家來不及共用帳篷；降低設計複雜度 |
| K2 `Kernel_ServerClampPropLifetime` | 同上，不再需要在執行中修改剩餘時間 |
| K4（舊）`Kernel_ServerApplyStatusEffect` / `RemoveStatusEffect`、`tent_rest` status | 只用來顯示倒數；L 固定後 client 用 `spawn_tick + L` 即可。Task B 若需要 buff 再另外評估 |
| K3 `Kernel_ServerSetEntityDamageImmunity` 作為獨立 export | 無敵併入入住鎖（K2 新編號），見 §3.3 |

---

## 3. 架構

### 3.1 Catalog

已 commit 的部分：

```
tent_kit (item 3011)              背包裡的道具：pickupable, throwable
  └─ throw: identity_preserving → tent_kit_prop
tent_kit_prop (entity 215)        飛行中的瓶子，collider = collision_damage_prop_hitbox (1 m 方塊)
  └─ on_collision terrain|static_obstacle
       → action_spawn_ice_and_damage_self_at_collision
         spawn tent @ event.position，自毀
tent (entity 216)                 一般 prop（非 item-backed）
  ├─ lifecycle: 5400 ticks, population_group: tent
  ├─ health 600, impulse_resistance 15
  ├─ collider: tent_hitbox (33)  oriented_box, 半寬 1.2, 高 2.4, layer damageable
  ├─ interaction: [interactable], range 2.0
  └─ on_activated → action_open_rest_ui（open_ui, ui_id 1）
prop_population_rules: tent (id 2, max_alive 4)
```

第二輪要改的部分（未做）：

| 項目 | 改成 |
|---|---|
| population group | 從 `tent` 改為所有建築物共用的 group（例如 `building`），`max_alive: 8`（D17）；temporary_deployable 另開 group |
| `lifecycle.lifetime_ticks` | `tent.yaml` 9000（正式）/ `tent_dev.yaml` 5400（開發）（D18、D22） |
| `health` | 5000 / 5000（D23） |
| `tent_kit_prop.yaml` | `on_collision` 參數 `template: tent_dev`（開發期間）；正式版改回 `tent` |
| `on_activated` | ~~從 `action_noop` 改為含 `open_ui` 的 graph~~ 已做：`action_open_rest_ui` |
| `on_destroy_entity` | 新增：在帳篷位置產生範圍擊退（見 §3.5） |
| group 規則 | 新增 opt-in：清除類消失也觸發 on_destroy（K4） |

**永久存在的建築物**（例如地圖生成、只受數量上限限制）：不寫 `lifetime_ticks`、只寫 `population_group`
即可，不需要改 kernel。parser 要求兩者至少有一個（`gameplay_config.cc:4539`），
剩餘時間 0 代表永不過期（`systems.cc:2290`）。`lifetime_ticks: -1` 或 `0` 都會被 parser 拒絕。

為什麼要拆成 kit 和 tent：**item-backed prop 不能宣告 lifecycle 或 population**
（`gameplay_config.cc:8721`），帳篷兩者都需要。拆開後的附帶好處：帳篷不是世界道具，
無法被撿回去（實測 pickup 被拒，`InvalidContext`）。

碰撞盒尺寸的三個限制：
- 必須完整包住 `player_hit_aabb`（半寬 0.35、高 1.8）並留餘量，住客才不會露出可被直接命中的部位。
- 半寬不能太大，否則近戰 chaser 被牆擋住、到不了攻擊距離。
- 高度 ≥ 半寬，避免被推開的單位從屋頂出去。

### 3.2 進出流程（未實作）

**進入**（`open_ui` 事件，見 K1；玩家不在任何建築內，且玩家未死亡）：
1. 設玩家移動遮罩為 `terrain`。
2. 把玩家移到建築中心。**順序必須是先設遮罩再移動**，否則下一個 tick 會被推出去。
3. 上入住鎖（K2）：移動歸 0、無敵、拒絕動作與 domain action（只放行對這個建築的 Activate）、擋下 impulse、pull 與 stagger。

**退出**（任一觸發）：
1. 在建築周圍找空的出口點（物理 overlap 查詢；嵌在 prop 或牆裡時要往外找）。
2. 把玩家移到出口點。
3. **之後**才恢復遮罩（0 = 引擎預設）。順序顛倒會被推往最短軸方向。
4. 解除入住鎖（一次清除移動鎖、無敵、動作白名單，以及 impulse / pull / stagger 的阻擋）。

**退出觸發一覽：**

| 觸發 | 偵測方式 | 備註 |
|---|---|---|
| 住客再按一次互動 | Activate（玩家已在這個建築內） | 入住鎖唯一放行的請求 |
| 建築 HP 歸 0 | 建築 despawn | 之後執行 on_destroy 擊退（§3.5） |
| 建築存在時間到 | 建築 despawn（reason Expired） | 同上；需要 K4 才會觸發 on_destroy |
| 被數量上限擠掉 | 建築 despawn（reason CapacityEvicted） | 同上；需要 K4 才會觸發 on_destroy |
| 玩家斷線 | PlayerLeft | |

玩家死亡**不是**退出觸發：住客無敵，死亡唯一的路徑是傷害管線，而傷害管線會先檢查
`DamageImmunity`（`damage_system.cc:162`、`kernel.cc:12055`）。見 §8 的死亡與復活分析。

#### 入住期間的玩家輸入

程式裡沒有 `ActorInput` component。玩家輸入是 `KernelPlayerInput` → `QueuedInput`，
伺服器每個 tick 在 `player_movement.cc:328-358` 決定水平速度：死亡 → 0；擊退中 → 沿用目前速度；
stagger → 0；其他 → 依輸入移動。

- **入住**：新增一條「入住中 → 水平速度歸 0」，寫法同死亡 / stagger。輸入照收，`input_seq`
  照常回報已處理（`acknowledge_simulated_movement_inputs`），client 的 reconciliation 不會卡住。
- 武器與動作輸入（`action_intent` / `action_input`）在動作路徑拒絕；domain action 只放行對該建築的 Activate。
- **退出**：移除入住狀態後，同一條判斷自然回到依輸入移動。
- **Client**：不知道伺服器上的入住狀態。UI 開著時若仍送移動輸入，預測會被伺服器拉回 → client 在 UI
  開著時送零輸入（或把入住狀態同步給 client，需改 schema）。這是 T3 的風險。

### 3.3 Kernel 需要新增的部分（未實作）

| # | 項目 | 用途 | ABI 影響（預期） |
|---|---|---|---|
| K1 | action graph 新 action `open_ui {ui_id}` + 事件（目標 = 建築，發起者 = 玩家，`ui_id`） | 建築物種類只由 graph 決定（D11）。game_server 目前**完全無從得知**遠端玩家啟動了什麼：action graph 的 action 只有 damage / health / impulse / speed_modifier / status / spawn，沒有任何通知 client 或 game_server 的出口；activation 結果只回給發出請求的 peer | 新 action 與事件；catalog schema 增加 `open_ui`；需確認舊 client 會忽略未知事件 |
| K2 | 入住鎖 `Kernel_ServerSetEntityShelter(net_id, shelter_net_id)`（0 = 解除） | 一次設定與清除：移動歸 0、無敵（旗標，不是 until_tick）、動作白名單、擋 impulse / pull / stagger。進入時在 kernel 內驗證 D32 的前提條件與 D33 的上限（D30） | 新 export |
| K3 | 建築消失時 kernel 先釋放住客，再執行 on_destroy graph。釋放必須放在 `destroy_entity_with_context` 呼叫 `world_.destroy` 之前 | §3.5 的順序要求 | 無 |
| K4 | population group 的 opt-in 規則：Expired / CapacityEvicted 也觸發 `on_destroy_entity` | D12 | catalog schema 欄位 |
| K5 | 擁有者的權威移動狀態加 `shelter_net_id`；client 預測依此切換遮罩、移動歸 0 | D28、§3.7 | **snapshot schema 變動**（client 與伺服器必須同一版） |
| K6 | `spawn_entity` 的安全落點選項（例如 `placement: clear_box`） | D29、§3.8 | catalog schema 欄位 |
| — | 不指定陣營的投射物載入警告 | D26、§3.9 | 只改 game_server 的 catalog 載入，無 |

新 export 依慣例放在 capability flag 後面，不升 ABI（見 memory `additive-abi-no-bump`）。
macOS 的 export 清單在 BUILD.bazel、Windows 在 .def，兩邊都要加。

無敵改成旗標的原因：L 固定時用 `spawn_tick + L` 當 until_tick 雖然可行，但永久存在的建築沒有 L。

### 3.4 Client（Unity，使用者負責）

- 收到 `open_ui` 時依 `ui_id` 開對應的 UI（休息 / 商店）。
- 倒數：`spawn_tick + L`。永久存在的建築不顯示倒數。
- UI 開著時不送移動輸入（見 §3.2、§7 T3）。
- 顯示建築受損（例如 HP），讓玩家觀察到建築可能被破壞（D21）。是否顯示、怎麼顯示，留到做 UI 時決定。

**建築 HP 與生成時間已經可以從 API 取得，不需要改 schema 或封包：**
- prop 的 HP 走 prop state change 封包（`kPropStateChangeHealth`，`kernel.cc:12998`），
  client 寫進 `RenderEntityState.hp` / `max_hp`（`kernel.cc:13951`）。
- `RenderEntityState.spawn_tick` 也在同一個結構裡，倒數用得到。
- Unity 透過現有讀取 `RenderEntityState` 的 API 即可取得，只需要確認 C# 端有讀這兩個欄位。
- 建築 despawn 或退出時關閉 UI。

### 3.5 建築消失時擊退住客（D12）

目的：建築物以任何原因消失時，住客被彈飛，而不是被安靜地放到出口點。

**順序是硬性要求：**
1. kernel 先釋放所有住客（找出口點、移出、恢復遮罩、解除入住鎖）。
2. 再執行 `on_destroy_entity` graph，在建築位置產生範圍擊退。

若順序顛倒，擊退打到的住客仍在入住鎖內，會被 impulse 阻擋擋掉；就算沒擋，住客的遮罩只有 terrain，
會被打穿牆飛出去，身上還帶著無敵。game_server 收到 `EntityDestroyed` 事件時 graph 已經跑完，
無法在中間插入釋放，**所以釋放必須在 kernel 內完成**（K3）。這也是 §9-2 傾向「kernel 內單一 shelter 概念」的理由。

**三種消失路徑目前的行為**（第三輪讀程式碼確認，`systems.cc`）：

| 消失原因 | 路徑 | 目前會觸發 on_destroy？ |
|---|---|---|
| HP 歸 0（Destroyed） | `destroy_dead_entities`（2465）→ `destroy_entity_with_context`，`execute_destroy_graph` 用預設值 true | **會**。prop 沒有 `DeathBehavior` 時不會進入休眠，直接銷毀 |
| 存在時間到（Expired） | `update_prop_lifetimes`（2284），明確傳 false | 不會 → K4 |
| 被數量上限擠掉（CapacityEvicted） | `enforce_prop_population_limit`（2313），明確傳 false | 不會 → K4 |

所以 HP 歸 0 這條是現狀，不需要討論。`destroy_entity_with_context` 內的順序是：排入 on_destroy
→ `world_.destroy` → 發出 `EntityDestroyed` → 執行 graph。graph 在實體移除之後才執行，
事件位置取自消失前的 Transform，所以擊退仍以建築位置為中心。K3 的住客釋放要插在 `world_.destroy` 之前；
那時建築的碰撞盒還在，出口點查詢要排除建築自己的碰撞盒。

**清除類消失觸發 on_destroy 的風險：** kernel 原本刻意跳過，是為了避免連鎖生成
（`systems.cc:2302`、`2350`）。若 on_destroy graph 生成同一個 group 的實體，滿額時會擠掉另一個、
再觸發 on_destroy、再生成，形成無限迴圈。對策：K4 只開放給有 opt-in 的 group，並由 catalog 驗證
該 group 的 on_destroy graph 不生成任何帶 population group 的實體。

擊退的做法：graph 不能以「所有住客」為目標（§6），所以用範圍擊退（在建築位置生成 area effect）。
住客在步驟 1 已被放到建築周圍，會被範圍涵蓋。未實測（見 T8）。

### 3.6 入住狀態完備性檢查（第四輪）

逐項檢查「kernel 內單一 shelter」的設計，找出的缺口與處理方式：

| # | 缺口 | 處理 |
|---|---|---|
| G1 | **誰觸發進入。** `open_ui` 發事件 → game_server 呼叫 K2，會多一個 tick 的延遲 | **定案（D30）**：維持既有流程，不讓 kernel 在 `open_ui` 裡直接切換。進入與再按一次的退出都由 game_server 呼叫 K2。因為事件到 K2 之間隔了一個 tick，玩家狀態可能已經改變（例如死亡、被擊退），**K2 必須在 kernel 內重新驗證 D32 的前提條件與 D33 的上限**，不依賴 game_server 收到事件時的判斷。建築消失時的住客釋放（K3）仍在 kernel 的銷毀路徑內，這是順序上的硬性要求，不是另開流程 |
| G2 | **斷線。** 斷線時伺服器直接 `world_.destroy` 玩家實體（`kernel.cc:6443`），**不經過** `destroy_entity_with_context`，任何 lifecycle 掛勾都不會跑 | D27：入住狀態只放在玩家身上，建築的住客用查詢取得。玩家實體消失時狀態跟著消失，不會在建築上留下過期的名單。重新連線是新實體，沒有入住狀態 |
| G3 | **住客仍是 AI 的目標。** 住客的位置在建築中心，敵人照樣追過來、照樣打建築，和 D20「建築不吸引威脅」矛盾 | **定案（D31）**：做成 YAML 選項，見下方「G3 的可行性」 |
| G4 | **進入的前提條件** | **定案（D32）**：未死亡、不在任何建築內、不在擊退中；搬著的 prop 先放下；進行中的動作取消 |
| G5 | **每個建築的住客上限** | **定案（D33）**：4。見下方「G5 的常數」 |
| G6 | **找不到空的出口點。** 帳篷四周都被擋住時怎麼辦 | 建議：進入時記錄入口位置，找不到出口時用入口位置當備案 |
| G7 | **持續性 status。** 傷害類的 status 走傷害管線，會被無敵擋下；減速等效果照常跑，不影響入住 | 不需處理 |
| G8 | **伺服器把玩家換 template（`set_actor_template`，復活或換角色時用）** | K2 強制退出後再換 |

**G3 的可行性（讀程式碼確認）：可以做成 YAML 選項。**
- 敵人選目標只經過 kernel 的視野候選迴圈（`update_vision_states`，`kernel.cc:12423`）。game_server 的 AI 透過
  `ai_perception_adapter` 讀這個結果，沒有其他選目標的來源。
- 這個迴圈已經有同類的前例：死亡的候選者會被跳過（`kernel.cc:12512` 附近，「dormant corpse ... nothing to chase」）。
  「入住中、且所在建築開啟了這個選項」的候選者用同樣的方式跳過即可，改動很小。
- YAML 放在建築的 template 上，和住客上限放在一起：

  ```yaml
  shelter:
    capacity: 4                  # D33
    hide_occupants_from_vision: true   # D31，測試時切換
  ```

- 這是 catalog schema 的新欄位，`bundle.bytes` 要更新（使用者處理）。
- 實際遊玩時要觀察的行為：視野狀態保留「最後看到的目標」與「最後已知位置」（`last_seen_target`、
  `last_known_target_position`），住客消失後，敵人可能仍會走到帳篷附近搜索一段時間，而不是立刻離開。
- 不受影響的地方：patrol director 的 `nearest_player_distance`（`patrol_director.cc:161`）只用來判斷巡邏隊離玩家太遠時是否要 despawn，不是選目標。住客仍算「在附近的玩家」，附近的巡邏隊不會因此被回收。

**G5 的常數：** 程式裡目前沒有玩家上限的常數可以共用（kernel、game_server、app 都沒有）。
建議在 kernel 的公開標頭新增一個小隊人數常數（值為 4），作為 `shelter.capacity` 的預設值；之後如果加入玩家上限，也用同一個常數。
YAML 沒寫 `capacity` 時用這個預設值。

### 3.7 入住狀態同步給 client（第四輪，D28）

**有必要同步。** 讀程式碼確認的事實：
- client 預測本地玩家時，遮罩用的是 template 寫的遮罩（`build_local_character_movement_config`，`kernel.cc:8584`），**看不到伺服器用 `Kernel_ServerSetEntityMovementCollisionMask` 設定的遮罩**。
- prop 的碰撞盒會以 `kStaticObstacle` 放進 client 的預測物理世界（`sync_client_render_colliders`，`kernel.cc:5025`），和伺服器一樣。
- 實測（§4）：預設遮罩下，就算沒有輸入，住客也會被推出帳篷。

所以住客在 client 端每一步預測都會被推出帳篷，再被伺服器的 snapshot 拉回來。**只靠「UI 開著時送零輸入」無法消除拉扯**，因為推開來自物理，不是來自輸入。T3 依程式碼判斷會失敗，仍需實測確認。

做法：在擁有者專用的權威移動狀態（`has_authoritative_movement_state` 那組欄位）加一個 `shelter_net_id`。
client 重新對齊時讀到非 0，就把預測的遮罩切成 `terrain`、移動歸 0，和伺服器一致。

**風險與檢查：**

| 風險 | 結論 |
|---|---|
| 入住後斷線 | 伺服器直接移除玩家實體，入住狀態跟著消失（G2）。同步的欄位只是權威狀態的一部分，不會在 client 或伺服器上殘留。重新連線是新實體，欄位為 0 |
| 進入 / 退出的時間差 | 傳送與入住狀態在同一個伺服器 tick 設定，放在同一筆權威移動狀態裡，client 會在同一次重新對齊中同時拿到，不會有「位置已傳送、遮罩還沒換」的空窗。**不可以**放到另一個 reliable 管道（例如 prop state），否則兩者會分開到達 |
| 掉封包 | snapshot 是最新狀態，下一個 snapshot 會修正 |
| 版本相容 | 改 snapshot schema，client 與伺服器必須同一版；依 memory `schema-bump-touch-points`，有三個測試寫死 schema 版本，要一起更新 |
| 作弊 | client 知道自己在帳篷裡沒有安全問題，伺服器仍是權威 |

其他玩家的顯示（住客模型要隱藏）：不需要這個欄位，只給擁有者即可。其他玩家的 client 可以由「這個 actor 的位置在某個建築碰撞盒內」推導，或之後再評估用 `visual_flags` 的保留位元（不改 schema）。

### 3.8 安全落點校正（第四輪，D29）

**伺服器端可行。** kernel 已經有 `PhysicsWorld::overlap_all`（`physics_world.h:83`），蟲巢出兵也用碰撞盒驗證出口點。做法：
1. 從接觸點出發，沿丟擲方向的反方向退後「建築半寬 + 餘量」。
2. 向下打射線找地面。
3. 用建築的碰撞盒做 overlap 查詢（只看 terrain 與 static_obstacle）。
4. 不空就再往後退一步，最多試幾次（例如最遠 3 m）；都失敗就用最後一個候選。不保證 100% 安全。

放在 action graph 的 `spawn_entity` 上當成選項（例如 `placement: clear_box`），冰塊瓶子也能用，順便消除 §4 的 0.6 m 偏差。需要改 catalog schema（新欄位）。

**能不能雙端共用：** 技術上可以。client 的預測物理世界裡有 terrain、static obstacle 和 prop 碰撞盒，同一個 kernel 函式在 client 也能跑。但**第一版不需要**：
- client 看到的是 kit 在某個 tick 消失、帳篷在同一個 tick 出現在位置 P。兩者都在 snapshot 裡。
- Unity 由「kit 的最後位置 → P」自行推算一條拋物線，播放撞牆反彈的動畫即可，不需要真實物理，也不需要 solver。動畫期間先隱藏帳篷，落地後再顯示。
- kit 和帳篷的對應：同一個 tick、同一個 `owner_peer`。一個玩家同時只會有一個 kit 在飛（`max_stack: 1`），這個對應不會錯。
- 只有在想讓「自己丟的 kit」在伺服器結果回來前就開始反彈時，才需要 client 端執行 solver（新增 client 查詢 API）。留到之後。

### 3.9 不指定陣營的投射物警告（第四輪，D26）

`spammer_projectile` 的 `collision_mask` 是 `terrain | static_obstacle`，沒有任何陣營位元。
`ice_block_hitbox.yaml` 已記錄這個缺口：這種投射物的 `gameplay_category_mask` 是空的，會穿過所有
`layer: damageable` 的碰撞盒（冰塊、帳篷都是）。

警告條件：投射物 `damage > 0`，且 `collision_mask` 不含任何陣營位元。在 catalog 載入時印 warning，
指出 template 名稱，不擋載入。

### 3.10 K1、K2 實作紀錄（2026-10-01）

**rebase 與 id 調整。** apply-pull 先佔了 ABI 95、action enum 9，以及 catalog id
collider 32、entity 214、item 3010。帳篷改用下一個空號：`tent_hitbox` 33、`tent` 216、
`tent_kit` 3011（`tent_kit_prop` 維持 215）。D22 的 `tent_dev` 順延為 217。

**K1：`open_ui`。**
- YAML：`type: open_ui`、`target`（綁 `event.instigator`）、`ui_id`（字面值，非 0）。
  只能用在 entity 的 `on_activated`；catalog loader 與 kernel validator 都會擋。
- 一種 UI 一個 graph：`action_open_rest_ui`（ui_id 1）。`action_noop` 已刪除。
- 執行時只發 `KernelEventType_UiOpened`（`net_id` = 建築、`related_net_id` = 啟動者、
  `peer_id` = 啟動者的 peer、`code` = ui_id），不做其他事（D30）。
- ABI 96：`KernelEntityTriggerActionType_OpenUi = 10`、`KernelActionDefinition.ui_id`
  （append）、`KernelEvent.related_net_id`（填進原本的尾端 padding，sizeof 不變，但 mirror
  仍要補欄位）。

**K2：入住。**
- `Kernel_ServerEnqueueEntityShelter(kernel, source, net_id, shelter_net_id)`，走 command
  queue，下一個 tick 生效（D30）。放在 `kernel_api_internal.h`：和其他 `Enqueue*` 一樣是
  game_server 用的內部介面，不是 managed export，所以不需要 capability flag，也不用改兩份
  export 清單。
- 狀態只在玩家身上：`Sheltered{shelter_net_id, entry_position, previous_movement_collision_mask}`（D27）。
- 進入：驗證 D32（未死亡、不在任何建築內、不在擊退中）、建築是 prop、住客 <
  `KERNEL_SHELTER_CAPACITY`（= `KERNEL_SQUAD_SIZE` = 4，D33）；放下搬著的 prop；先設遮罩
  terrain，再傳送到建築原點；速度歸 0、清除 stagger。發 `KernelEventType_ShelterChanged`
  （`code` = 建築）。
- 入住期間：
  - 移動歸 0，輸入照常 ack；
  - 傷害整筆丟棄（也就沒有 stagger）；
  - impulse 和 pull 跳過；
  - 不能開始新動作（`KernelLocalActionResultReason_Sheltered`），進行中的動作被中斷；
  - gameplay request 只放行「Activate 自己所在的建築」，其他回
    `KernelGameplayRequestRejection_InstigatorSheltered`。
- 退出（`shelter_net_id` 0）：在建築周圍找空位（建築 footprint 半對角線 + 膠囊半徑 +
  0.25 m，8 個方向，從入口那一側開始，再往外一圈），找不到就回入口位置（G6）；先移出，再恢復
  原本的遮罩。發 `ShelterChanged`（`code` 0、`related_net_id` = 建築）。建築已不存在時也能退出（直接用入口位置）。

**測試：`//game_server:tent_shelter_test`（全過）。**
- K1：啟動帳篷會發一個 UiOpened，欄位都正確，而且不會讓人進去。
- 進入：位置在中心。之後推 30 tick 輸入，位置不動。
- 入住中：Activate 別的建築被拒（InstigatorSheltered），Activate 自己的帳篷成功。
- 退出：落在 (2.30, 0, 0)，也就是入口那一側、footprint 外、地面上。之後往帳篷走，停在
  x = 1.57（牆邊），證明遮罩已經恢復。
- 拒絕：5 人只進 4 人；已在 A 帳篷內不能進 B；死亡玩家不能進；不在帳篷內時退出不會有事件。
- 無敵：法師榴彈對照組（手動放進帳篷）命中 1 次、HP 1000 → 955；入住後命中 0 次、HP 不變，
  帳篷仍被打中 2 次。

**未涵蓋：**
- 放下搬著的 prop、impulse/pull 阻擋（T10）、進行中動作被中斷，這三項有實作但沒有測試。
- G6 找不到空位時回入口位置，沒有測試（T16）。
- 斷線（T14）沒有測試。

**game_server：`ShelterDirector`（`game_server/src/shelter_director.{h,cc}`）。**
- 只看事件，不需要 tick。住客 → 建築的對照表只依 kernel 回報的 `ShelterChanged` 更新，
  不記錄「已送出的請求」，所以 kernel 拒絕的請求不會留下錯誤狀態。
- `UiOpened`：玩家已在這個建築內 → 送退出；不在任何建築內 → 送進入；在別的建築內 → 不處理
  （kernel 的請求閘門本來就擋掉了）。不看 `ui_id`：所有建築共用進出規則（D8）。
- `EntityDestroyed`：
  - 被移除的是住客（斷線）→ 從表中刪除。
  - 被移除的是建築 → 對每位住客送退出，晚一個 tick 生效；建築已不在，所以回到入口位置。
  - 這是 K3 完成前的替代做法。
- `PlayerLeft`：從表中刪除。
- 測試：`tent_shelter_test` 的 `game_server_runs_the_door` 用真的 `GameServer` 處理事件：
  - 啟動一次進入、再啟動一次退出；
  - 帳篷在有人時被摧毀，住客晚一個 tick 回到入口位置 (1.80, 0, 0)，之後能進另一頂帳篷。

**實作中發現：退出點在互動距離之外。** 退出點離中心 2.30 m，互動距離是 2.0（D14），所以出來後
要往帳篷走幾步才能再進去。是否接受，或把退出圈縮小、或把互動距離放大，待決定。

**還沒做：**
- K3（建築消失時先釋放住客）。目前由 `ShelterDirector` 晚一個 tick 放人，見上。
- K4–K6、G3 的 YAML 選項，以及 `shelter.capacity` 的 YAML 欄位（目前固定是常數）。

---

## 4. 實測事實

來源：`//game_server:tent_feasibility_probe_test`（`game_server/tests/tent_feasibility_probe_test.cc`），2026-09-30。

| 項目 | 結果 |
|---|---|
| 平地落點 | 帳篷生成在 kit 停下位置前方約 0.6 m（`event.position` 是接觸點，不是停止位置） |
| 平平地丟向冰塊 | kit 撞到側面，帳篷生在側面上，半嵌入冰塊（重疊約 3.4 m³） |
| 遮罩只有 terrain（冰塊瓶子的設定） | kit 穿過冰塊，帳篷完全在冰塊內（約 9.2 m³） |
| 帳篷生成時的旋轉 | 依 `event.direction` 只轉 yaw（實測 90°），不傾斜 |
| 互動距離 | 3D 距離。地面距中心 1.8 m 可互動；2.5 m 被拒（OutOfRange）。**可互動的範圍只有 1.6–2.0 m 的窄環** |
| 撿起帳篷 | 被拒（帳篷不是世界道具） |
| 落點下的單位 | 帳篷出現後，所有單位（含玩家）被推出最近的一面，全部在帳篷外 |
| 四位住客、遮罩只有 terrain | 4 人都留在中心不動。`terrain|actor` 也一樣。預設遮罩的對照組：4 人都被推出去 |
| beam 射擊住客 | 帳篷吃下全部 43 發，住客 0 發 |
| 法師榴彈 | 帳篷被打中，**住客仍被範圍傷害打到**（需要無敵） |
| spammer | 連對照組都沒打中任何人，無結論 |
| kit 投擲距離 | 沿用 `grenade_shell_projectile`，飛了約 40 m |

其他 probe 中確認的事：prop 的 entity state 回報的 template id 是 0；`owner_peer` 為 0 建立的 prop 屬於 hostile 陣營（實際丟出的帳篷繼承丟擲者的 peer，是玩家陣營）。

第二輪 review 以讀程式碼確認（未實測）：
- 目前沒有任何 action graph action 能通知 client 或 game_server（K1 的由來）。
- `activate_entity` 不檢查發起者是否死亡（`systems.cc:1866-1891`）。
- 復活會覆寫或移除 `DamageImmunity`（`systems.cc:2820-2824`）。
- impulse 與 pull 不經過 `DamageImmunity` 檢查。

---

## 5. 存在時間與數量

- **L 只在生成時設定**：開發 5400（3 分鐘）、正式 9000（5 分鐘）。懸空或半嵌入的帳篷也是等這段時間結束。
- 玩家進出不影響存在時間；限制單一類型建築的出現機率靠 L 與數量上限。
- 永久存在：不寫 `lifetime_ticks`，只受數量上限限制（§3.1）。
- 數量：建築物 group 上限 8，滿了擠掉最舊的（目前唯一的溢出規則）。配置為整隊持有 1 + 地圖生成，合計 ≤ 8。
  - 注意：玩家丟出的與地圖生成的建築在同一個 group，滿額時擠掉的可能是地圖生成的那個。
  - 最終目標是每人上限（依 `owner_peer` 分組，需改 kernel）。

---

## 6. Task B 摘要（鍋爐，使用者另行設計；第一版不做）

已定案：組合配方、丟歪不消耗、每個帳篷有投放次數上限。

目前的構想：
- 瞄準和投擲只是 client UI 的表演，伺服器不模擬軌跡。
- 命中時送 `KernelGameplayRequest`：`target_net_id` = 帳篷，新增 domain action `Offer`。
- 伺服器驗證後消耗道具，記錄到該帳篷的鍋爐狀態。
- 配方表放在 game_server catalog（帳篷 template 上），不放 action graph（graph 不能依道具分支，也不能以「所有玩家」為目標）。
- 建築內的互動由 `open_ui` 喚起的另一個模組決定（D11）。

已知限制：
- status 能執行的效果只有傷害、回血、移速修正。其他 buff 類型都要新增 kernel 的 status action。
- 鍋爐狀態要同步給帳篷內所有住客的 UI，目前沒有同步管道，可能要改 schema。
- 同一個 channel 的 status 會互相覆蓋。

Task A 為 B 預留的接口：
- 「建築 → 住客」的查詢；
- 建築消失的通知；
- 入住鎖的請求白名單（A 只放行 Activate，B 要加入 `Offer`）。

---

## 7. 測試計畫

| # | 項目 | 狀態 |
|---|---|---|
| T1 | 架在 prop 上的帳篷，站在地面碰不到 | 部分：嵌在側面的情況已測（碰不到）；落在頂上未測。依 D14 視為玩家失誤 |
| T2 | 敵方攻擊會被帳篷擋住 | 已測：beam 擋住、榴彈的範圍傷害擋不住（由無敵處理） |
| T3 | pure client 預測本地玩家時是否和伺服器互相拉扯 | **依程式碼判斷不同步就會拉扯**（§3.7）。改為驗證 D28：同步後不拉扯；需兩個 process |
| T4 | 落地推開單位 | 已測 |
| T5 | 每條退出路徑都安全，入住鎖完整清除；嵌入時出口點空著 | 未測，需要流程 |
| T6 | 帳篷不會重疊 prop | 已測：**會重疊**，已決定接受（D4） |
| T7 | 四人同住不互推 | 已測 |
| T8 | 建築以 HP 歸 0 / Expired / CapacityEvicted 消失時，住客先被釋放、再被擊退飛走，且身上沒有殘留入住鎖 | 未測，需要 K3、K4 |
| T9 | 死亡的玩家 Activate 建築被拒 | 未測 |
| T10 | 住客被範圍擊退 / pull 打到時不移動 | 未測，需要 K2 |
| T11 | 清除類 on_destroy 不會連鎖生成（catalog 驗證拒絕會生成 group 實體的 graph） | 未測，需要 K4 |
| T12 | 住客被會造成 stagger 的攻擊打到時不進入 stagger | 未測，需要 K2 |
| T13 | 不同種類的建築共用上限 8，第 9 個生成時擠掉全場最舊的 | 未測，需要第二種建築 |
| T14 | 入住後斷線：建築不殘留住客，其他住客不受影響 | 未測 |
| T15 | 朝牆面或冰塊側面丟 kit，帳篷生在牆前的空地；地形複雜時允許失敗 | 未測，需要 §3.8 |
| T16 | 四周被擋住時，退出回到入口位置 | 未測，需要 G6 |
| T17 | 載入含不指定陣營、會造成傷害的投射物時印 warning | 未測 |

---

## 8. 待決問題與已知待確認事項

已解決（第二輪）：
- ~~退出方式~~：再按一次互動（D13）。
- ~~kit 投擲距離~~：仍待定，但不阻擋 Task A。
- ~~互動距離~~：維持 2.0，碰不到為玩家失誤（D14）。
- ~~N 是否足夠~~：N 已撤回（D5）。

死亡與復活的分析：死亡唯一的路徑是傷害管線，無敵會擋下，所以入住期間不會死亡，死亡與復活流程本身
不衝突。但有以下漏洞，已併入設計：
- 死亡的玩家可以 Activate → D15，進入時拒絕。
- 復活會覆寫或移除 `DamageImmunity` → 擋住死亡玩家進入後就不會發生；入住的無敵改為旗標（K2），不和復活共用 until_tick。
- 擊退與 pull 不經過無敵 → 入住鎖擋下（K2）。

已解決（第三輪）：
- ~~stagger 是否穿過無敵~~：住客不受 stagger 影響，由入住鎖處理（D16）。
- ~~HP 歸 0 是否觸發 on_destroy~~：已確認會觸發，是現狀（§3.5）。
- ~~共用 group 或各自 group~~：所有建築物共用一個 group（D17）。
- ~~兩組 L 的切換方式~~：兩份 YAML（D18）。
- ~~kit 投擲距離~~：比照 ice block，現狀已一致（D19）。

已解決（第四輪）：
- ~~兩份 YAML 的檔名與載入方式~~：D22。
- ~~HP 數值~~：5000（D23）。
- ~~擠掉地圖生成的建築~~：接受（D24）。

確認過與帳篷無關的事：
- **nest 不會被 temporary_deployable 的清除機制刪除。** `gingerbread_nest.yaml` 沒有 `lifecycle`，
  生成時不會掛 `PropLifecycle`（`systems.cc:1670`）。存在時間與數量上限都只看有 `PropLifecycle` 的實體。
  目前用 `temporary_deployable` 的只有 `ice_block` 與 `glyph_block`。

已解決（第五輪）：
- ~~G1~~：沿用既有流程（D30）。
- ~~G3~~：YAML 選項（D31）。
- ~~G4~~：依建議（D32）。
- ~~G5~~：4（D33）。

- ~~K5 是否放進第一版~~：放進第一版（D34）。

目前沒有待決定的設計問題。

---

## 9. 已知弱點與值得重新思考的地方

（第一輪列出的原始分析。回覆對照：1 → D20；2 → §3.6；3 → D28、§3.7；4、5 → D29、§3.8、§3.7；
6 → §3.4（HP 已可由 API 取得）；7 → D25；8 → D21；9 → D26、§3.9。）

1. **[已回覆，D20] 空帳篷不吸引攻擊。** 定案：這是設計方向，建築物不主動承受或吸引威脅，不做 kernel 的視野候選改動。以下為原始分析。
   **空帳篷不吸引攻擊。** 原始需求是「有 HP，會吸引 hostile_side 攻擊」。目前的設計靠「住客仍是目標、帳篷包住住客」讓帳篷挨打，所以**沒人住的帳篷幾乎不會被攻擊**，HP 只有在有人休息時才有意義。真正讓敵人主動攻擊 prop，需要把 prop 加入敵人的視野候選（kernel 改動）。chaser 的接近距離是以目標中心計算，也要一起改。
2. **入住狀態放在哪裡。** 第一輪的寫法是遮罩、無敵、鎖、status 分開設定，由 game_server 在每條退出路徑逐一清除。第二輪的 D12 要求「先釋放住客，再跑 on_destroy graph」，game_server 收到事件時 graph 已經跑完，無法插入 → 目前設計已改為 kernel 內單一的 shelter（K2、K3）。
3. **移動遮罩與入住狀態只存在伺服器。** 這是 T3 的風險來源。如果實測確實互相拉扯，要嘛把狀態同步給 client（改 schema），要嘛 client 在 UI 開著時送零輸入。
4. **撞到側面就半嵌入，是丟擲時很常見的情況。** 平平地丟幾乎都會撞到側面。雖然已接受為玩家失誤，但玩家的感受可能是「莫名其妙浪費一個帳篷」。如果之後想改，選項有：生成前檢查位置（`require_clear`，擋到就不生成並退還道具），或新增「停止位置」這個事件來源（只能消除 0.6 m 的偏差，解決不了側面問題）。
5. **住客全部疊在同一點。** 4 位住客在伺服器上位置完全重疊；client 端如果仍渲染住客的模型，需要隱藏或另外安排。
6. **入住期間看不到外面。** 休息 UI 開著時世界照常運行，住客對帳篷外的威脅（例如即將被打爆）只能靠帳篷 HP 判斷。UI 可能需要顯示帳篷 HP。
7. **Population 是全場共用。** 多人時，A 丟的新帳篷可能擠掉 B 正在休息的帳篷（上限改為 8 後較少發生）。最終需要每人上限。
8. **[已回覆，D21] 友軍傷害**：不保護，屬於玩家自主行為；HP 設高值，client 顯示受損。以下為原始分析。
   **帳篷是 player side，但敵人的 AoE 也會打到帳篷**（法師榴彈實測打中帳篷）；而玩家自己的 AoE（例如 frag 瓶，對所有陣營造成 1000）同樣會炸掉自己的帳篷。是否需要友軍保護，未討論。
9. **spammer 的攻擊對帳篷是否有效，沒有結論**（對照組就沒命中）。spammer 的投射物沒有指定陣營，依文件記載會穿過所有指定陣營的 collider，可能完全無視帳篷。

---

## 10. 相關檔案

- Catalog：`game_server/gameplay_catalog/entity_templates/{tent,tent_kit_prop}.yaml`、`collider_templates/tent_hitbox.yaml`、`item_templates/tent_kit.yaml`、`action_graph_templates/action_open_rest_ui.yaml`、`gameplay_catalog.yaml`（population rule）
- 測試：`game_server/tests/tent_feasibility_probe_test.cc`
- 相關 kernel 位置：
  - `kernel.cc` 視野候選迴圈（只看有 vision config 的實體）；`kernel.cc:12055` 進入死亡狀態
  - `systems.cc`：飛行中 prop 落地（`static_contact`）；`update_prop_lifetimes`（2284，Expired 跳過 on_destroy）；`enforce_prop_population_limit`（2313，CapacityEvicted 跳過 on_destroy）；`activate_entity`（1866）；`revive`（2771）
  - `player_movement.cc:328-358`：每 tick 決定水平速度的判斷鏈
  - `damage_system.cc:162`（`DamageImmunity` 檢查）
  - `gameplay_config.cc:4539`（prop lifecycle 解析）
  - `kernel_api.h`（`Kernel_ServerSetEntityMovementCollisionMask`）
- 參考前例：蟲巢的出兵流程（`spawner_director.cc`，先設遮罩再移動；出口點依碰撞盒驗證）
