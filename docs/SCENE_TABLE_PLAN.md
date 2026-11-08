# Scene table 實作計劃書

狀態：**計劃已定案（2026-11-08，§11 Q1–Q9 全部採用建議），可以開工。** 尚未實作。
分支：`claude/scene-table-plan`（只有文件），從 main `6bd6f524` 開出。
日期：2026-11-08。

前身：`docs/MAP_SYSTEM_EXPANSION_PLAN.md`（分支 `claude/map-system-expansion-plan`）的方案 B。
本文件定案 D1（採用 scene table），並把它落實成可以開工的步驟。

---

## 0. 摘要

- 一個 kernel、一個 world。scene 放在一個 **slot array** 裡，**slot 0 是 camp**，常駐不卸載。
  關卡放在 slot 1 以後，各自在固定的 offset，執行中載入和卸載。
- 玩家換 scene 就是一次 teleport（沿用傳送門）。net id、背包、snapshot 都不中斷。
- host（listen server）和 dedicated server 走同一套程式：流程放在 game_server，這兩種模式本來就共用 game_server。
- 第一版只有一支隊伍、只用 slot 0 和 slot 1，但資料結構一開始就以 **multiverse**（多個關卡 slot 同時存在）為前提。

### 存檔系統不是前置條件

scene table 不依賴存檔系統，可以先做：

- 換 scene 發生在同一個 world、同一個 session 裡。玩家的 net id、背包、loadout 都留在記憶體中，**沒有任何資料需要離開這個 world**。
- 存檔系統要處理的「角色資料跨 world 傳遞」（加入朋友的世界、斷線重連），是 scene table 之上的另一層。
- 本計劃只留下兩個介面點給存檔系統，見 §4.6。

---

## 1. 目標與範圍

**要做：**

1. kernel 支援多個 static scene，各有 offset，可以在執行中新增和移除（server、listen host、client 三種模式）。
2. catalog 能定義多個 **scene template**，camp 和每個關卡各一個。
3. game_server 用 slot array 管理 scene，每個 slot 有自己的 navmesh、director、budget、出生點。
4. 載入同步協定：server 通知載入，client 回報 ready，全員 ready 才開放進入。
5. 卸載時清乾淨：反覆載入、卸載同一個關卡後，entity 數、budget、director 數都回到原值。
6. 端到端測試：listen host 加一個 client，camp → 關卡 1 → camp → 關卡 2 → camp。dedicated 模式再測一次。

**不做（之後另外規劃）：**

- 存檔系統、角色資料上傳下載、玩家身份、斷線重連。決定記錄在 memory 的 multiplayer-session-decisions。
- Steam transport。
- 依 recipe 每局隨機產生地圖（第一版用 build 時就烤好的固定關卡）。
- tiled navmesh、區域串流（`MAP_SYSTEM_EXPANSION_PLAN.md` 的 P2）。
- Unity 端的實作、C# mirror、package 打包。這些由使用者處理。

---

## 2. 已定案的決定

| # | 決定 | 來源 |
|---|---|---|
| D1 | 採用 scene table，不用「兩個 server instance」，也不用「每關重開 server」 | 2026-11-08，比較時以「host 和 dedicated 都要支援」為前提 |
| S1 | scene 用 array 管理，index 0 = camp | 使用者 |
| S2 | 之後要能做 multiverse：硬體夠時，多個關卡 slot 同時存在 | 使用者 |
| S3 | 第一版的關卡固定，打包在 bundle 裡；換關時只傳 template id | 本文件的建議（bundle 只在握手時傳，上限 2 MiB） |

參考資料：兩個 server 的方案有 smoke test（`claude/two-server-switch-smoke` 305fd955），保留作為比較基準。

---

## 3. 現況（main `6bd6f524`，已讀過程式碼）

### 3.1 kernel

- kernel 只持有**一個** static scene：`kernel.h:1639-1642`（`static_collision_scene_` 和三個 id）。
- 用到它的地方共 14 處，**已全部讀過**：
  - `commit_static_collision_scene`（`kernel.cc:2404`）
  - `prepare_prediction_physics`（`:2413`）：client 和 listen host 的 prediction world
  - `prepare_server_physics`（`:2446`）：server world
  - `start_listen_server`（`:2559`）：有 scene 就準備 prediction world
  - `apply_welcome`（`:9659`）：client 收到 Welcome 時建立 prediction world
  - `has_static_collision_scene()`（`kernel.h:145`）：唯一的外部使用者是 `item_gameplay_system.cc:259`，用來決定擺放時要不要往下找地面
- **註冊時機有限制**（`kernel.cc:2350`）：server 只能在 start 之前，client 只能在 Welcome 之前。
- **地形判斷看的是 kind 和 layer，不看 collider id。** 例如 `CollisionObjectKind::kTerrain` 和 `CollisionLayer::kTerrain`（`player_movement.cc:427`、`collision_filter.h:105`、`item_gameplay_system.cc:270`）。所以多個地形 body 可以直接共存，沒有程式假設地形在原點。

### 3.2 物理層

- `PhysicsWorld` 以 collider_id 管理 body，同一個 world 可以放多個 static body。
- `remove_object(collider_id)` 可以移除，`optimize_broad_phase()` 每 tick 都會跑。
- `load_static_scene` 把 body 放在固定的 `glm::vec3(0)`（`physics_world.cc`），**只缺一個 offset 參數**。

### 3.3 game_server

- **地圖相關設定已經集中在 `map_variants`。** 每個 variant 是一層覆蓋，包含 `terrain` 或 `navigation_mesh` 加 `static_collision_scene`、`preload_directors`、`scene_props`、`player.spawn_position`、`agent_routes`。這幾乎就是 scene template 需要的欄位。
- **static scene 的註冊在 server 和 client 都走 game_server**（`game_server_api.cc:290-320`）：準備地形、建立 `TerrainView`、呼叫 `Kernel_LoadGameplayCatalog` 時一起註冊 scene。client 是透過 bridge 呼叫 `Kernel_LoadGameplayCatalogFromMemory`。
- **以下目前都是單一值：**
  - navmesh：`AgentRuntimeManager::patrol_navigation_`
  - mission：game rule director
  - `RespawnScheduler`（`team_revive_times` 整個玩家陣營共用一份）
  - `agent_budget`
  - `TerrainView`：以 `KernelHandle*` 為 key，一個 kernel 一份（`game_server_api.cc:117`）
  - `fall_death_height`：全域值，`Kernel_ServerSetFallDeathHeight` 只收一個高度
- **地形 recipe 只烤一份**：只從 `kernel_terrain` 這個 variant 烤成 `terrain/gameplay_catalog.nktr`。
- **game message** 已經有了（512 B，可靠傳輸，雙向，支援 listen host 自己的 peer）。`GAME_SERVER_MESSAGE_*` 目前用到 3。
- **清理用的 API**：`Kernel_ServerQueryEntities` 依 entity type 回傳全部的 entity 和位置，由 game_server 自己依 slot 範圍過濾。

---

## 4. 架構

### 4.1 Slot array

```
slots[N]                         N 在 server 啟動時決定（建議預設 8）
  slot 0 : camp                  原點，常駐
  slot i : 關卡                  offset = (i × kSlotSpacing, 0, 0)
```

| 欄位 | 說明 |
|---|---|
| `state` | 見 §4.2 |
| `template_id` | 這個 slot 現在放的是哪一個 scene template |
| `generation` | slot 每重用一次就加 1。所有訊息都帶 `(slot, generation)`，避免把舊關卡的訊息套到新關卡 |
| `party_id` | 使用這個 slot 的隊伍；slot 0 是共用的 |
| `collider_id` | `kSceneColliderBase + slot`，kernel 用它管理物理 body |
| `bounds` | template 的範圍加上 offset，用於清理和「玩家在哪個 slot」的判斷 |

**間距 `kSlotSpacing`：** 必須大於「最大地圖邊長 + relevance 距離（44 m）+ 安全邊界」。最大地圖目前是 1440 m，建議 **4096 m**。8 個 slot 最遠到 28.7 km，該處 float 精度約 2 mm，夠用。

**判斷玩家在哪個 slot：** 用 x 座標除以間距就能算出來，不用另外記錄。

### 4.2 Slot 狀態機

```
Empty ──load──▶ Loading ──server 準備好 + 全隊 ready──▶ Ready ──第一位玩家進入──▶ Active
  ▲                                                                                │
  └── Unloading ◀──清理完成── Draining ◀──隊伍要求離開 / 任務結束 / 失敗────────────┘
```

- **Loading：** server 在背景 thread 組地形、建 navmesh。完成後，在 tick thread 上把 scene 加進 kernel，再廣播 `SceneLoad`。
- **Ready：** 全隊都回報 `SceneReady` 才進入這個狀態，之後才生成傳送門。
- **Draining：** 等到 slot 範圍內沒有活著的玩家。還留在裡面的玩家會被送回 slot 0。
- **Unloading：** 刪除範圍內的 entity（despawn 原因用 `Retired`），停掉這個 slot 的 director，釋放 navmesh，從 kernel 移除 scene，廣播 `SceneUnload`，最後 `generation += 1`。

### 4.3 隊伍（為 multiverse 預留）

- 隊伍 = 一組玩家 + 最多一個關卡 slot + 一個流程狀態機（Camp → Loading → PortalOpen → InLevel → Returning）。
- **第一版只有一支隊伍**，所有玩家都在裡面。資料結構還是寫成「隊伍的 array」，之後要開多個關卡不用重寫。
- **重生次數改成每支隊伍一份。**

### 4.4 分工

| 層 | 負責 |
|---|---|
| kernel | 只認得 slot → static scene（offset、collider_id）。不知道 template、隊伍或流程 |
| game_server | slot array、狀態機、隊伍、per-slot navmesh/director/budget/出生點、載入協定、清理、client 端的地形組裝 |
| Unity | 收訊息、呼叫 client 端 API、依 slot 顯示地形、載入畫面（使用者負責） |

### 4.5 座標

- scene template 裡的座標都是**這個 scene 自己的本地座標**。
- game_server 把 template 放進 slot 時加上 offset：擺設、出生點、mission 生怪點、巡邏路徑、傳送門目的地都一樣。
- 已有的 `map_variants` 座標就是本地座標（camp 在 slot 0，offset 為 0），**現有地圖的數值不用改**。

### 4.6 留給存檔系統的介面

1. **camp 選項：** `LoadoutDirector::send_offers` 已經是每位玩家各一份。存檔系統之後只要把選項改成「camp 選項 ∩ 玩家已解鎖」。scene table 不動它。
2. **隊伍名單：** §4.3 的隊伍以 player net id 管理成員。之後加入玩家身份時，改用身份作為 key。

---

## 5. 載入協定（走 game message，不用新 packet）

| 訊息 | 方向 | 內容 | 時機 |
|---|---|---|---|
| `SCENE_TABLE` | S→C | 目前非 Empty 的 slot：`(slot, generation, template_id, state)` | Welcome 之後馬上送；中途加入的玩家靠這個補載入 |
| `SCENE_LOAD` | S→C | `(slot, generation, template_id)` | slot 進入 Loading，server 端準備好時 |
| `SCENE_READY` | C→S | `(slot, generation)` | client 組好地形、加進 prediction world 之後 |
| `SCENE_UNLOAD` | S→C | `(slot, generation)` | slot 進入 Unloading |

- 每則都遠小於 512 B。
- message id 接在 `GAME_SERVER_MESSAGE_LOADOUT_RESULT`（3）之後。
- listen host 自己也要回 `SCENE_READY`，走 peer 1 的本機路徑。

**等待和 timeout：**
- 全隊 ready 之前，傳送門不會出現，所以不需要把玩家凍結在關卡裡。
- 某個 client 在 `kSceneLoadTimeout` 內沒有回 ready（第一版建議 60 s），就把他踢掉，這是 `MAP_SYSTEM_EXPANSION_PLAN.md` D3 的第一版做法。

**client 不需要地形的 render 資料也能 ready：** ready 的條件是**碰撞已就緒**（地形已加進 prediction world）。Unity 的美術資源可以晚一點到，由 Unity 決定要不要延後送 ready。

---

## 6. Catalog：scene template

### 6.1 格式（S2 已實作）

```yaml
catalog_version: 18
map_variant: composed_mesh       # slot 0（camp）照舊由 map_variant 決定（Q6）
map_variants: {...}
scene_templates:                 # 關卡用的地圖；在這個 mapping 裡的順序就是 wire 上的 id
  level_terrain_seed2:
    terrain: {...}               # 或 static_collision_scene + navigation_mesh
    stamp_template_dir: stamp_templates
    preload_directors: []        # 必填，[] 代表沒有任務
    scene_props: []
    agent_routes: []
    player: {spawn_position: ...}
```

- **欄位和 `map_variants` 相同**（同一份允許清單 `kMapOverlayKeys`）。template 疊在 catalog 的 **root** 上，不疊在 camp 的 variant 上。
- **每個 template 必須寫明自己的地圖**（`terrain:` 或 `static_collision_scene:`）**和 `preload_directors:`**，避免 root 的 mission 漏進關卡。
- **不設 `camp_scene`：** Q6 定案 slot 0 由 `map_variant` 決定，所以原本建議的 `camp_scene` 不需要。
- **延遲解析：** 載入 catalog 時只檢查 template 的格式，並保留名稱和原始 YAML 文字（`SceneTemplateRef`）；要用時才呼叫 `load_scene_template_config_from_bundle_memory` 或 `load_scene_template_config_from_catalog_file`，得到一份完整的 config。所以約 70 個直接載入原始 catalog 的測試不需要 template 的地形檔。
- **catalog hash** 納入每個 template 的名稱和原始 YAML 文字，順序依檔案順序。
- **地形 recipe 每個 terrain template 各烤一份**，放在 template 的 `terrain.recipe` 指定的路徑。`terrain_recipe_cook` 接受 `scene_template:<name>`；`gameplay_catalog_bundle/BUILD.bazel` 每個 template 一個 genrule。
- **之後要補的欄位**（S4）：關卡的傳送門出口。

### 6.2 版本影響

- **新的 catalog key，舊 package 會無法解析**（`InvalidBundle unknown field`）。client 和 server 要用同一份 game_server build，**package 要重新打包**。
- `bundle.bytes` 要重新產生。
- bundle 上限 2 MiB。S2 實測：加入一個 recipe 地形關卡後 bundle 為 1,039,680 B；該關卡的 recipe 只有 384 B（組好的碰撞資料 1.05 MB 是 client 和 server 各自組出來的，不在 bundle 裡）。用 mesh 的地圖每張約 0.4 MB，數量受限，優先用 recipe 地形。

---

## 7. 步驟

每一步都在自己的 `claude/scene-table-*` 分支上做，從前一步的分支疊上去。每一步交付時說明：ABI 變動、snapshot schema、packet schema、catalog 格式、`bundle.bytes`。

### S1 kernel：多個 static scene 和執行中增減

| # | 工作 |
|---|---|
| S1.1 | `PhysicsWorld::load_static_scene` 加上 `position` 參數，預設 0 |
| S1.2 | `KernelEngine` 的單一 scene 改成 `std::map<slot, StaticSceneSlot>`（bytes、scene_id、collider_id、layer、offset）。`Kernel_SetStaticCollisionScene` 保留，等同 slot 0 |
| S1.3 | `prepare_server_physics` 和 `prepare_prediction_physics` 載入所有 slot。`apply_welcome` 重建 prediction world 時也是 |
| S1.4 | 新 export：`Kernel_AddStaticSceneSlot(kernel, const KernelStaticSceneSlotConfig*)` 和 `Kernel_RemoveStaticSceneSlot(kernel, slot)`。依模式更新對應的 world：dedicated 只有 server world，listen host 兩個都要，client 只有 prediction world（Welcome 之後才能呼叫）。slot 0 不能移除 |
| S1.5 | `has_static_collision_scene()` 改成「任一 slot 存在」 |
| S1.6 | capability flag。bit 不連號，要先 grep 所有 capability 再選（memory：capability-bits-not-contiguous）。macOS 的 export 清單在 `BUILD.bazel`，Windows 在 `.def`，兩份都要加 |

**ABI：** 新增 export 和 struct，掛在 capability 後面，照「additive ABI 不 bump」的慣例。snapshot 和 packet schema 不變。

**測試：** 新增 `static_scene_slot_test`：
- dedicated server 加 client：slot 1 放在 x = 4096，把玩家傳過去。server 和 client prediction 都要能站在地上，並和一個「沒有地形」的對照組比較，避免測試空轉（memory：test-static-scene-needs-catalog-ids、test-assertions-can-be-vacuous）。
- 移除 slot 1 之後，站在那裡的 actor 會往下掉。
- 同一個 slot 加入、移除 50 次，不會觸發 Jolt 的「QuadTree: Out of nodes」。
- listen host 版本：兩個物理 world 都要確認。

### S2 game_server：scene template catalog

| # | 工作 |
|---|---|
| S2.1 | `gameplay_config` 解析 `scene_templates`（延遲解析，見 §6.1）；沒有這個 key 的舊格式照常運作；catalog_version 18 |
| S2.2 | catalog hash 納入所有 template |
| S2.3 | `terrain_recipe_cook` 和 bundle genrule 改成每個 terrain template 各烤一份 |
| S2.4 | 出貨 catalog：camp 不變，加一個測試用關卡 template `level_terrain_seed2`（同一張地面、seed 2 的 stamp 配置，沒有任務） |
| S2.5 | 量 bundle 大小 |

**版本：** catalog 格式變動，package 和 `bundle.bytes` 都要更新。

### S3 game_server：slot array 與 per-slot 執行（已實作，`claude/scene-table-s3`）

**實作方式：每個關卡 slot 一個自己的 `AgentRuntimeManager`。** manager 本來就把一張地圖的 navmesh、director、budget、agent 包在一起，所以每個 slot 用它的 template config 建一個新的 manager，不另外拆分。camp 是 slot 0，沿用原本的 manager。

| # | 工作 | 狀態 |
|---|---|---|
| S3.1 | `GameServer` 的 slot array：Empty → Loading → Ready，generation，bounds（`scene_table.h`、`scene_bounds.h`） | 完成。Active、Draining 跟著 S4 的協定一起加 |
| S3.2 | navmesh 每個 slot 一份：每個 manager 有自己的 `PatrolNavigation`，`set_origin(slot offset)` 讓所有查詢都用世界座標 | 完成 |
| S3.3 | director 依 slot 執行：template 的 `preload_directors` 在 slot Ready 時隨 manager 啟動，卸載時隨 manager 移除；`offset_scene_config` 把出生點、擺設、巡邏路徑、playable bounds、mission 生怪點和世界座標路徑、guard anchor、world rule 生怪點都加上 offset | 完成 |
| S3.4 | 只看同一個 slot：manager 唯一的 entity 查詢、game rule 的玩家計數、spawner 的 carrier 掃描都依 slot 的 x 範圍過濾（`keep_in_scene`）。所以 `around_players`、hunter、`player_count_at_least` 都只看同一個 slot | 完成。關卡 slot 不跑 catalog 的 world patrols（`patrols` 會被清掉），只跑 template 自己的 director |
| S3.5 | budget：全域上限，再分給各個 slot | **延後**：目前每個 slot 用 template 自己的 budget。第一版流程只有一支隊伍（Q9），最多 camp 加一個關卡 |
| S3.6 | 重生：掉出地圖的玩家，在他掉下去的那個 slot 的出生點復活 | 完成。重生次數改成每支隊伍一份 → 移到 S4，跟隊伍一起做 |
| S3.7 | 背景載入：template config、地形組裝、navmesh 在 worker thread（`std::async`）；加進 kernel、建 manager、擺設在 tick thread | 完成 |
| S3.8 | 卸載清理：slot 範圍內的玩家送回 camp 出生點，其餘 actor、prop、projectile 全部刪除（`Retired`），移除 manager 和 kernel slot，generation + 1 | 完成 |
| S3.9 | `TerrainView` 以 `(kernel, slot)` 為 key | **移到 S4**：它是給 client 畫地形用的，和 S4.2 的 client 端載入 API 一起做 |

**其他限制：** 玩家的 agent command 目前只交給 camp 的 manager 處理，S4 再依 agent 所在的 slot 分派。

**測試（`scene_slot_runtime_test`，真實的 listen host 加出貨的 bundle）：**
- 關卡載入後，任務生出的東西全部在自己的 slot 裡，camp 的 AI 只管 camp。
- 從關卡掉下去的玩家，在關卡裡復活。
- 卸載後 slot 清空，camp 的 entity 一個都沒少；連續載入、卸載 20 次，每次都清乾淨。
- 兩個關卡同時存在：等待玩家的任務只在有玩家的 slot 開始。
- 地形關卡在背景組裝時，host 照常 tick。
- 拿掉 slot 範圍過濾後，測試會失敗（camp 的 agent 被關卡的 manager 收走）。

實測：mesh 關卡約 25 ms 就 Ready；216 m 的地形關卡約 100 ms，期間 host 跑了 50 個 frame。

### S4 協定與流程

| # | 工作 |
|---|---|
| S4.1 | §5 的四種訊息：server 端送出和處理 |
| S4.2 | client 端 API（game_server bridge，新 GameServer capability）：`GameServer_ClientLoadSceneSlot(kernel, bundle, template, slot, generation)` 組地形並呼叫 `Kernel_AddStaticSceneSlot`；對應的 unload 也一樣。Unity 收到訊息時呼叫這兩個 API |
| S4.3 | 隊伍流程狀態機（第一版）：隊長選關 → Loading → 全隊 ready → 在 camp 生成傳送門 → InLevel → 任務完成時在關卡生成回程傳送門 → 全員回到 camp → Draining/Unloading |
| S4.4 | 任務結果：讀取 `GameRuleStatus::kCompleted`，作為該 slot 的 mission 完成事件。**失敗** = 隊伍重生次數用完而且全員死亡 → 全員送回 camp 並復活 |
| S4.5 | 傳送門：每個關卡 template 一個入口傳送門 template，目的地是「slot offset + template 的本地座標」。由 game_server 在 slot 進入 Ready 時用 `Kernel_ServerCreateEntity` 生成。注意出口 net id 必須是入口 + 1（memory：portal-design） |
| S4.6 | 選關請求：第一版只有 host（listen 模式的 peer 1，或 dedicated 模式下由 RPC 或指令觸發）可以選 |

### S5 Unity（使用者負責）

- C# mirror：S1 的 export 和 struct、S3.9 和 S4.2 的 GameServer API、訊息格式。
- 地形顯示改成每個 slot 一份；載入畫面；收到 `SCENE_*` 時呼叫 S4.2 的 API。
- package 重新打包。

### S6 端到端驗證

- `scene_table_loop_test`（game_server）：listen host 加一個 client，跑 camp → L1 → camp → L2 → camp，檢查：
  - 兩人都能走進傳送門，換 scene 前後 net id 不變。
  - 每次卸載後，S3 的清理計數都回到原值。
  - 中途加入：L1 進行中第二位 client 才加入，他能收到 `SCENE_TABLE`、載入 slot 1，再被送到隊伍旁邊。
- dedicated 模式再跑一次。

---

## 8. 測試總覽

| 測試 | 步驟 | 重點 |
|---|---|---|
| `static_scene_slot_test` | S1 | 多個 slot、offset、執行中增減，三種模式 |
| catalog 解析測試（擴充現有的） | S2 | 新舊格式並存，hash 涵蓋所有 template |
| `scene_slot_cleanup_test` | S3 | 反覆載入卸載後計數歸零 |
| multiverse 最小情況 | S3 | 兩個關卡 slot 互不影響 |
| `scene_table_loop_test` | S6 | 完整循環、中途加入，listen 和 dedicated 各跑一次 |

只跑每一步相關的 target，不跑全部測試（main 本來就有幾個已知會紅的測試，memory：main-carries-nine-red-tests）。

---

## 9. 版本影響總表

| 步驟 | kernel ABI | snapshot schema | packet schema | catalog 格式 | `bundle.bytes` | package |
|---|---|---|---|---|---|---|
| S1 | 新增 export（capability，不 bump） | 不變 | 不變 | 不變 | 不變 | 要重打（新 export） |
| S2 | 不變 | 不變 | 不變 | **變動** | **要更新** | 要重打 |
| S3 | 不變 | 不變 | 不變 | 可能（per-slot 設定） | 可能 | 要重打 |
| S4 | 不變 | 不變 | 不變（走 game message） | 可能（傳送門 template） | 可能 | 要重打 |

---

## 10. 風險

| 風險 | 對策 |
|---|---|
| 卸載沒清乾淨：entity、director、budget、navmesh 越積越多 | S3 先寫 `scene_slot_cleanup_test`，所有清理都以它為準 |
| 改 per-slot 時弄壞座標寫死的測試 | §6.1 舊格式相容，現有地圖當作 slot 0，座標不變 |
| 組地形卡住 tick（1440 m server 約 4.3 s） | S3.7 放到背景 thread，只有加進 kernel 在 tick thread 上 |
| listen host 有兩個物理 world，容易漏更新一個 | S1 測試對 listen 模式兩個 world 都檢查 |
| bundle 超過 2 MiB | S2.5 先量；mesh 地圖數量受限時，優先用 recipe 產生的地形 |
| `fall_death_height` 是全域值 | 所有 scene 用同一個高度基準（§11 Q2 已定案） |
| multiverse 的 tick 成本疊加 | 第一版流程只開一支隊伍（Q9）；multiverse 是否只在 dedicated 開放，之後再決定 |

---

## 11. 已定案的問題（2026-11-08 使用者全部採用建議）

| # | 問題 | 決定 |
|---|---|---|
| Q1 | slot 間距和數量 | 4096 m，沿 +x；預設 N = 8 |
| Q2 | kill floor 要每個 slot 一個，還是統一高度基準？ | 第一版統一基準（template 的地面都要高於 -10 m），kernel 不改 |
| Q3 | 誰可以選關？ | 第一版只有 host；dedicated 模式由 RPC 或指令觸發 |
| Q4 | 載入 timeout 的處理 | 第一版 60 s 未 ready 就踢除 |
| Q5 | 任務失敗的定義 | 隊伍重生次數用完而且全員死亡 → 全員回 camp 並復活 |
| Q6 | `map_variants` 要改名成 `scene_templates`，還是兩者並存？ | 並存：`map_variant` 只決定 slot 0，`scene_templates` 是新的 |
| Q7 | 有人沒走進傳送門怎麼辦？ | 留在 camp 也可以；slot 要等到裡面沒有玩家才卸載 |
| Q8 | client 端由誰觸發載入？ | Unity 收到訊息後呼叫 S4.2 的 API；不在 kernel 裡自動處理 |
| Q9 | 第一版要不要支援兩支隊伍同時在不同關卡？ | 資料結構支援，但只測最小情況（S3 測試），流程只開一支隊伍 |

---

## 12. Token 與成本控管

- 每一步先跑該步的 smoke 測試，不跑全部測試。
- build 一律用 `-c opt`，避免 debug build 拖慢測試（memory：server-perf-baseline-and-build-config）。
- 只讀和該步直接相關的檔案；本文件 §3 已經列出所有已知的修改點，開工時從這裡出發，不再大範圍搜尋。
- 如果某一步的範圍明顯超出本文件，先停下來說明原因再繼續。
- 每一步結束時回報：讀過的檔案、修改的檔案、搜尋次數、build 和 test 指令。
