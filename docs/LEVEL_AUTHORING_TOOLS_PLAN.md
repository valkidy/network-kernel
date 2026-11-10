# 關卡編輯工具：scene 與 stamp 的編輯、匯出與打包

狀態：計畫草稿（2026-11-10），以 main 26736476 為準。Q1–Q6 已全部決定，可以開始實作。

## 0. 目標

1. 在 unity-terrain-example 分別提供 **scene 編輯工具**和 **stamp 編輯工具**。
2. 匯出的檔案放進這個 repo、重 build bundle 之後就能直接使用，不需要再手改任何 YAML 或 BUILD 規則。
3. 這一版保留**手動複製**：工具輸出到 unity-terrain-example 的 `Export/`，由使用者複製到
   `game_server/gameplay_catalog/`，避免直接覆蓋 kernel repo 裡的檔案。
4. 新增**空氣牆**：只擋移動，看不見，可以指定擋誰（第一個用途：只擋玩家，不擋 NPC）。

不在這次範圍：見第 10 節。

## 1. 現況（26736476 實際查到的）

| 項目 | 現況 | 出處 |
|---|---|---|
| scene template 的地面 | 二選一：`terrain:`（recipe 合成）或 `static_collision_scene.entry_path` + `navigation_mesh`（mesh）。loader 拒絕兩者並存 | `gameplay_config.cc`（`terrain` 不可有 `entry_path`、不可有 `navigation_mesh`） |
| mesh 模式的打包 | `mesh_assets/raw/*.obj` 由 glob 收進來，依 `mesh_bake.yaml` 自動烘成 `.joltmesh` 和 `.navmesh` | `mesh_assets/BUILD.bazel` |
| scene / stamp 檔的打包 | `scene_templates/*.yaml`、`stamp_templates/*.yaml|*.hf`、`terrain_assets/*.nkgl|*.nkgm` 都是 glob | `game_server/BUILD.bazel` `gameplay_catalog_bundle_sources` |
| 固定 seed 的 terrain recipe | **每個 template 要手寫一條 genrule**（`terrain_recipe`、`terrain_recipe_level_terrain_seed2`） | `gameplay_catalog_bundle/BUILD.bazel` |
| terrain 的 stamp 擺放 | 只能由 layout 依 seed 產生；`terrain:` 沒有手動指定擺放的 key | `terrain_catalog_from_yaml` 的 key 清單 |
| `scene_props` | 只有 `entity_template` 和 `position`；建立時旋轉固定是 identity | `ScenePropConfig`、`GameServer::place_scene_props` |
| stamp yaml | kernel 只接受 `id/name/heights/nest_sites`；`nest_sites` 只有 x/z，目前沒有任何 stamp 標過 | `gameplay_config.cc` |
| Unity `StampExporter` | 輸出的 yaml 帶有 `format`、`size_m`、`annotations` 等 key，kernel 會拒絕，所以 repo 裡的 stamp yaml 是手動刪減過的；annotation 只存 yaw | unity-terrain-example `claude/stamp-export` |
| 靜態物理層 | 靜態物件只有 `kTerrain` 和 `kStaticObstacle` 兩層；子彈、視線、hitscan 都會查這兩層 | `physics/public/collision_types.h`、`physics_world.cc` |
| 移動 layer | `terrain`、`static_obstacle`、`actor`、`limb`；預設 mask = terrain + static_obstacle + actor | `kernel_types.h` `KERNEL_MOVEMENT_LAYER_*` |
| 魔毯 | `apply_hover_movement` 保持離地 15 m、每秒爬升 8 m | `action_hover_apply.yaml` |
| Unity prefab 對照 | `NetworkPrefabCatalog.asset` 以數字 id 對 prefab；`propPrefabs` 混用 item id 和 entity template id；沒有對照的 prop 會顯示 `propFallback` | unity-network-example |
| unity-terrain-example 的依賴 | 沒有依賴 network package，也沒有依賴 unity-network-example | `Packages/manifest.json` |

## 2. 決定與待確認

### 2.1 已決定（前幾輪討論）

| # | 決定 |
|---|---|
| D1 | 編輯對象是 `stamp_templates`（輸出 `<id>_stamp_<name>.hf` + `.yaml`）和 `scene_templates`（輸出 `<id>_scene_<name>.yaml`）。 |
| D2 | 這一版保留手動複製。 |
| D3 | 空氣牆只擋移動：不擋子彈、不擋視線、不擋 hitscan，也不進 navmesh。 |
| D4 | 空氣牆要能指定擋誰；第一個用途是只擋玩家、不擋 NPC（包含玩家方的 NPC）。 |
| D5 | 空氣牆用於 initial camp 的邊界和一般地圖的邊界。魔毯飛越空氣牆不做任何阻止，掉出地圖就照常死亡並重生。 |
| D6 | 事件解鎖的區域用一般的 static obstacle，而且必須是 **entity prop**（解鎖時移除，晚加入的玩家從 snapshot 得知）。 |
| D7 | 會變的東西不是地形（kernel terrain 計畫 §8 的規則）。 |
| D8 | registry 與 prefab 對照用**名稱**存，生成時以同一個 catalog 版本的數字 id 對起來（id 在合併時會重新編號）。 |
| D9 | scene 的空氣牆（`player_barriers:`）是**永久**的：scene 載入時放好，執行中不移除，所以不需要開關或同步。用途：地圖邊界、永遠不開放的區域。 |
| D10 | 之後會開放、但不想讓玩家看見的牆，用 **entity 版 `player_barrier`**（P1b）：collider purpose `player_barrier` 的 prop，刪掉 entity 就開放，晚加入的玩家從 snapshot 得知。看得見、也擋 NPC 的解鎖牆照 D6。 |

### 2.2 待確認（附建議）

| # | 問題 | 建議 | 狀態 |
|---|---|---|---|
| Q1 | scene 編輯工具第一版要支援哪些地面模式？ | **mesh 和 terrain 都支援，但分兩階段**：先做 mesh（camp 就是 mesh），terrain 放第二階段。標記和匯出共用同一層，只有地面的匯出不同。 | 已決定（2026-11-10） |
| Q2 | terrain 模式的固定場景要不要能手動擺 stamp？ | **要**。新增 `terrain.placements:`；有它就跳過 layout，但照樣跑 `accept` 檢查。否則固定場景只能挑 seed，無法照設計擺。 | 已決定（2026-11-10） |
| Q3 | registry 與 preview 從哪裡開始？ | **先只用 collider 外框預覽**（不需要 prefab）。registry 由 build bundle 時一併產生的 json 提供；prefab 預覽之後再評估是搬到 unity-network-example，還是抽成共用 package。 | 已決定（2026-11-10） |
| Q4 | 空氣牆的高度預設值？ | **4 m**：高於玩家的跳躍頂點，遠低於魔毯的 15 m。實作時先量跳躍頂點再定。 | 已決定（2026-11-10） |
| Q5 | 一般地圖的邊界要手擺還是自動？ | 兩者都提供：scene 可手擺；另加 `map_edge_barrier: {height}`，由 game_server 沿地圖矩形自動放四面牆。 | 已決定（2026-11-10） |
| Q6 | 空氣牆的 mask 在牆上設定，還是在 actor 上設定？層叫什麼？ | **在 actor 上**，和現有的移動 mask 一致：牆屬於新的 `player_barrier` 層，玩家的 template 把 `player_barrier` 加進自己的移動 mask，NPC 的預設 mask 不含它。牆本身只標「我是 player_barrier」。 | 已決定（2026-11-10），名稱選 `player_barrier` |

## 3. 資料格式

### 3.1 scene template（catalog v21）

```yaml
id: 0
name: initial_camp
# 地面：二選一（不變）
static_collision_scene: {entry_path: mesh_assets/jolt/<name>_collision.joltmesh}
navigation_mesh: {entry_path: mesh_assets/recast/<name>_collision.navmesh}
# 或 terrain:（第二階段可加 placements:，見 Q2）

scene_props:
  - entity_template: initial_camp          # 名稱，不用 id
    position: {x: 100.8, y: 20.6, z: 72.0}
    rotation: {x: 0, y: 0, z: 0, w: 1}      # 新增，選填，預設 identity
    grounded: true                          # 新增，選填，預設看 template（registry）
  - entity_template: level_gate
    position: {x: 106.8, y: 30.0, z: 82.0}
    rotation: {x: 0.7071, y: 0, z: 0, w: 0.7071}
    grounded: false                         # 例：空中、朝下的門

player_barriers:                             # 新增：空氣牆
  - center: {x: 100.8, y: 22.6, z: 66.0}
    half_extents: {x: 6.0, y: 2.0, z: 0.25}
    yaw_degrees: 0
map_edge_barrier: {height: 4.0}                 # 新增，選填：沿地圖矩形自動放四面牆

player:
  spawn_position: {x: 106.8, y: 23.0, z: 77.0}
```

- `grounded: true`：放置時用地面高度覆蓋 y，並檢查腳下有地面；找不到地面就載入失敗並說明原因（現在沒有任何生成路徑檢查地面）。
- `grounded: false`：完全照寫的 position 和 rotation。
- 空氣牆一律是 box，用 `yaw_degrees`（牆是直立的，不需要完整旋轉）。
- 編輯工具只管 `scene_props`、`player_barriers`、`map_edge_barrier`、`player.spawn_position`、`agent_routes`，以及 mesh 模式的地面路徑。`patrols`、`preload_directors`、`level`、`objective`、`nests` 這些手寫欄位，工具讀入後原樣保留（見 6.3）。

### 3.2 stamp template（catalog v21）

把 `nest_sites` 換成通用的 `sites:`。`nest_sites` 沒有任何人用過，直接移除，不留相容層。

```yaml
id: 1
name: fortress_0001
heights: 1_stamp_fortress_0001.hf
sites:
  - name: courtyard                 # 選填，給報告和錯誤訊息用
    options:                        # 加權抽一個（2(g)-1：同位置、不同目標）
      - {weight: 10}                # 沒有 entity_template = 這裡什麼都不放
      - {weight: 20, entity_template: gingerbread_nest}
      - {weight: 70, entity_template: watch_tower}
    position: {x: 12.0, y: 0.0, z: 12.0}   # stamp 本地座標，公尺；y 相對 stamp 邊界
    yaw_degrees: 0
    grounded: true
  - name: gate
    options:                        # 2(g)-2：同目標、不同位置；每個 option 自帶位置
      - {weight: 1, entity_template: level_gate, position: {x: 2, y: 6, z: 12}, rotation: {...}, grounded: false}
      - {weight: 1, entity_template: level_gate, position: {x: 22, y: 6, z: 12}, rotation: {...}, grounded: false}
per_map_limits:                     # 同一張地圖的總數上限 / 下限（依 entity_template）
  level_gate: {min: 0, max: 1}
player_barriers: []                  # 同 3.1，stamp 本地座標
```

- 一個 site 抽出一個 option；option 可以覆寫 site 的位置和旋轉，所以 (g)-1 和 (g)-2 用同一個結構。
- 抽選只在 server 端進行，種子 = terrain seed + placement index + site index，`seed_override` 能重現同一張地圖。抽出的東西都是 entity，透過 snapshot 同步，不改 recipe。
- `per_map_limits` 解決「stamp 放三次就出現三個門」的問題。抽選依**洗過的 placement 順序**進行，避免現在 `max_count` 永遠由前面的 placement 先填滿。
- 位置、旋轉要跟著 placement 的 `turns` 和 `mirror` 一起轉；鏡像時旋轉也要鏡像。
- 載入時檢查：site 在 stamp 範圍內、不在 2 m 外緣裡、引用的 template 存在。
- 現有的 level `nests:`（entity_template + max_count）改成讀 `sites` 裡的 nest；行為維持不變（沒有 site 時用 stamp 中心）。

### 3.3 registry（build bundle 時一併產生）

`placeable_registry.json`，和 `bundle.bytes` 同一次 build 產生，所以版本一定一致。內容：

```json
{"catalog_version": 21, "bundle_hash": "...",
 "entities": [{"name": "gingerbread_nest", "id": 208, "entity_type": "prop",
               "default_grounded": true,
               "bounds": {"center": [0,1.5,0], "half_extents": [1.9,1.5,1.9]}}]}
```

- 來源是 entity templates，加上其 collider template 的尺寸。
- 預設 grounded 先依 entity_type 決定（prop = true），之後 template 可加一個選填的 `placement: {grounded: false}` 覆寫。
- Unity 端的下拉選單只列 registry 裡的名稱，所以不會打錯字。

## 4. kernel（engine）：`player_barrier` 層

| 步驟 | 內容 |
|---|---|
| K1 | `physics::CollisionLayer::kPlayerBarrier`（新 bit）、`CollisionObjectKind::kPlayerBarrier`（附加在最後）、新的 Jolt object layer，放在 static world broad phase。 |
| K2 | `kCollisionMaskAll` **不含** `kPlayerBarrier`，所以子彈、hitscan、視線、area effect 都查不到。`kMovementCollisionMask`（預設）也不含，所以沒設定的 actor 不會被擋。 |
| K3 | 公開 ABI：`KERNEL_MOVEMENT_LAYER_PLAYER_BARRIER`；移動 mask 的 YAML token `player_barrier`；加進 `KERNEL_MOVEMENT_MASK_SUPPORTED`，**不加進** `KERNEL_MOVEMENT_MASK_DEFAULT`。 |
| K4 | 新 export：`Kernel_SetStaticScenePlayerBarriers(kernel, slot, const KernelPlayerBarrierBox* boxes, uint32_t count)`，每次呼叫取代該 slot 的全部牆；`Kernel_RemoveStaticSceneSlot` 時一併移除。server world 和 client prediction world 都要加。放在新的 capability bit 後面（additive，不升 ABI 版本；挑 bit 時對照 `capability-bits-not-contiguous` 的教訓）。 |
| K5 | 確認 character controller、hover、knockback 飛行、leap 這幾條移動路徑都用 actor 自己的移動 mask 查碰撞；任何一條如果寫死預設 mask，牆就會對它失效。 |
| K6 | Windows `.def` 和 macOS `BUILD.bazel` 的 export 清單都要加（兩份清單）。 |
| K7（P1b，選做） | collider purpose `player_barrier`：prop collider 的物理層現在寫死為 static obstacle（server 端 `push_collider_into_physics`、client 端 prediction proxy 各一處），各加一個分支讓這種 collider 進 `kPlayerBarrier`。不接受用在 actor 上。 |

## 5. game_server

| 步驟 | 內容 |
|---|---|
| G1 | `scene_props` 讀 `rotation`、`grounded`；`grounded` 時以地面高度覆蓋 y，找不到地面就載入失敗。 |
| G2 | `player_barriers`、`map_edge_barrier` 讀入後，在 slot 載入時呼叫 `Kernel_SetStaticScenePlayerBarriers`。server 和 client 都經過 `prepare_scene`，所以兩端走同一條路。slot 0（camp）也要套用。 |
| G3 | player template 的移動 mask 加上 `player_barrier`。 |
| G4 | stamp `sites`、`per_map_limits`、stamp 內的 `player_barriers`；取代 `nest_sites`。 |
| G5（第二階段） | `terrain.placements:`：手動指定 stamp 擺放，跳過 layout，照跑 `accept`。 |
| G6（第二階段） | 固定 seed 的 terrain recipe 改成**對所有 terrain scene template 自動 cook**（一條 genrule 掃過 bundle 裡的 template），不再每個 template 手寫一條。 |
| G7 | build bundle 時輸出 `placeable_registry.json`（3.3）。 |

## 6. Unity（unity-terrain-example）

### 6.1 標記

- `SceneMarker` component：掛在空的 GameObject 上，不帶 renderer 或 collider。欄位：
  - template 名稱（從 registry 下拉選）
  - grounded（預設取 registry）
  - stamp 用：options（加權清單）、site 名稱
- gizmo：依 registry 的 bounds 畫線框和朝向箭頭。
- `AirWall` component：BoxCollider 只用來在場景裡調整尺寸。gizmo 用半透明色顯示，匯出時轉成 `player_barriers`。
- **`MeshStampSampler` 的 Skip 規則要排除 `AirWall` 和 `SceneMarker`**，否則它們的 collider 會被當成地形取樣。
- 取代現有的名稱前綴（`bld_`、`prop_`…）。前綴打錯字會靜默失效，這正是要避免的。

### 6.2 檢查（匯出前）

- template 名稱在 registry 裡存在。
- grounded 標記的腳下有地面，且坡度在 template 允許內（nest 至少 6 m 平地）。
- stamp 的 site 不在 2 m 外緣、不超出 stamp 範圍。
- scene 檔名符合 `<id>_scene_<name>.yaml`，name 不超過 32 bytes，id 0 只能是 initial camp。
- 空氣牆的高度大於跳躍頂點、小於 15 m。

### 6.3 匯出

| 模式 | 輸出 | 複製到 |
|---|---|---|
| stamp | `<id>_stamp_<name>.hf` + `.yaml`（只寫 kernel 接受的 key） | `stamp_templates/` |
| scene（mesh） | `<id>_scene_<name>.yaml` + `<name>_collision.obj` | `scene_templates/`、`mesh_assets/raw/` |
| scene（terrain，第二階段） | `<id>_scene_<name>.yaml`（含 `terrain.placements`） | `scene_templates/` |

- scene 匯出會先讀入現有的 yaml（如果有），只改工具負責的欄位，其他欄位原樣保留（round-trip）。
- 碰撞 OBJ 只包含地面和會擋人的靜態幾何；空氣牆不進 OBJ（它是 `player_barrier` 層，不是 terrain）。
- `StampExporter` 現在輸出的 metadata（`size_m`、`rim_m`…）改成寫在另一份報告檔，不放進 stamp yaml。

### 6.4 registry 匯入

- 選單 `Tools > Level Authoring > Load Registry`，指定 `placeable_registry.json`。registry 不在 repo 裡；路徑記在 EditorPrefs。

## 7. 打包後「馬上能用」的檢查

| 項目 | mesh 模式 | terrain 模式 |
|---|---|---|
| scene / stamp yaml 進 bundle | 已經是 glob | 已經是 glob |
| 碰撞與 navmesh | OBJ 已經是 glob 自動烘 | 由 recipe 合成；固定 seed 要 G6 才不用手改 BUILD |
| client 畫得出地面 | **待確認**：client 有 per-slot mesh query，但 Unity 是否拿它來畫，或需要另一份 render 資產 | client 依 recipe 自己合成 |
| 新 template 的 prefab | 沒有對照的 prop 會顯示 `propFallback`；要加進 `NetworkPrefabCatalog`（Unity 端，使用者負責） | 同左 |

## 8. 步驟

| 步驟 | 內容 | 測試 |
|---|---|---|
| P0 | 量玩家跳躍頂點，定 Q4 的預設值 | **完成（2026-11-10）**：遊戲沒有跳躍（engine 和 game_server 都沒有 jump 輸入或跳躍速度），玩家往上只有 0.4 m 的 `step_height`，其餘都來自擊飛、龍捲風和魔毯。4 m 維持不變：遠高於跨階和 capsule，遠低於魔毯的 15 m |
| P1 | K1–K6：`player_barrier` 層與 export | **完成（`claude/player-barrier-p1`）**。`player_barrier_test` 驗證了：牆只對帶該 layer 的查詢可見，gameplay 的 catch-all mask 和移動預設 mask 都看不到；server、listen host 的兩個 world、client 在 Welcome 前後都有牆；取代、清除、隨 slot 移除；各種錯誤輸入不改變現狀；玩家帶 layer 時停在接觸點（5.13 m，理論 5.15 m），不帶時走完 15 m。**沒有驗證**：真的 NPC agent（用預設 mask 的玩家代替）、端到端的子彈和視線（只在查詢 filter 層驗證）、hover 從 15 m 越過，以及兩個 process 的 client prediction |
| P2 | G1–G3：`scene_props` 旋轉/grounded、`player_barriers`、`map_edge_barrier`；camp 加四面牆 | **完成（`claude/player-barrier-p2`，catalog v21）**。`finish_scene_placement`（`game_server/src/scene_placement.*`）在每個準備碰撞的地方執行：兩個 app、bundle 載入、level slot 的 `prepare_scene`，server 和 client 都一樣。邊界牆每段預設 6 m，高度取該段邊緣 3 m 內最高的地面再加 `height`，底部比最低地面再低 1 m；地圖太大時自動加長每段，維持在 256 個以內。`grounded` 預設關閉（registry 的預設值留給 P4）。`scene_placement_test`：shipping camp 在 listen host 上，玩家停在 x = 214.63（牆內面 215 減 capsule 半徑），拿掉 layer 的對照組走到 227.68 並掉出地圖；`scene_template_catalog_test` 驗證格式、hash 和錯誤。**沒有驗證**：dedicated server 的端到端（只 build 過 app）、純 client 的 prediction |
| P3 | G4：stamp `sites`、`per_map_limits` | **完成（`claude/stamp-sites-p3`，仍是 catalog v21）**。stamp yaml 接受 `sites`、`per_map_limits`、`player_barriers`，移除 `nest_sites`（寫了就拒絕）。`place_stamp_contents`（`scene_table.cc`）：亂數用 splitmix，以 recipe seed 為種子；placement 順序先洗牌；每個 site 加權抽一個 option，達到上限的 template 不再抽；最低數量不足時，從能放該 template 的 site 改抽，仍不足就拒絕這張地圖。巢穴照舊進 `level_nests`（沿用 `nests.max_count`），其他進 `scene_props`。位置、四元數、空氣牆都經過 `stamp_site_on_map` 量出的剛體轉換，鏡像時旋轉也鏡像。完全沒寫 `sites` 的 stamp 保留舊的「中心放巢穴」。`stamp_sites_test`：20 個 seed、160 個朝向（117 個鏡像 placement），上下限、可重現、貼地、空氣牆位置全部成立；`scene_template_catalog_test` 驗證 yaml 格式和 7 種錯誤；`scene_level_content_test` 維持 120/120 巢穴可達。**注意**：shipping 的 stamp 還沒標任何 site（要等 P5 的 Unity 工具） |
| P4 | G7：registry json | **完成（`claude/placeable-registry-p4`）**。`//game_server/gameplay_catalog_bundle:placeable_registry` 從最終的 bundle 產生 `placeable_registry.json`，放在 bundle 旁邊、不打包進去。內容：`format`、`catalog_version`、`catalog_hash`、`bundle_hash`（64-bit 用 hex 字串），以及每個 prop template 的 `name`、`id`、`server_only`、`default_grounded`（目前一律 true）、`bounds`（collider 的中心和半邊長；segment、cone 或沒有 collider 時為 null）。依名稱排序、固定小數位，同一個 bundle 每次產生的檔案都一樣。`placeable_registry_test`：檔案與 bundle 逐 byte 一致、每個 prop 都在而非 prop 都不在、巢穴外框等於它的 hitbox。目前 31 個 prop，包含掉落物 bottle 和測試用 prop；要不要在編輯器過濾，留給 P5 |
| P5 | Unity：`SceneMarker`、`AirWall`、registry 下拉、檢查、stamp 匯出修正 | **完成（unity-terrain-example `claude/level-authoring-p5` fbf21a4，從 `claude/stamp-export`）**。runtime 元件 `SceneMarker`（加權 option、`emptyWeight`、共用 `siteName` 表示「同目標、不同位置」、grounding）、`AirWall`（只有尺寸和 yaw，不帶 collider 也不帶 renderer，所以不需要 Skip 規則也不會被取樣；與 6.1 原本「用 BoxCollider 調尺寸」不同，改用 BoxBoundsHandle）、`StampDefinition`（kernel id、名稱、`per_map_limits`）。Editor：`Tools > Level Authoring > Load Placeable Registry...`、下拉選單只列 registry 的名稱。`StampExporter` 輸出 `<id>_stamp_<name>.hf`/`.yaml`，量測值改成註解，內容不合格就拒絕匯出；名稱前綴的 annotation 已移除。`MeshStampSampler` 跳過 marker 與它底下的物件。batchmode 自我測試 `LevelAuthoring.LevelAuthoringSelfTest.RunBatch` 通過；匯出的 `3_stamp_l_concave_0001` 在 kernel 端換上後 `scene_template_catalog_test`、`scene_level_content_test` 都通過，`.hf` 與 repo 逐 byte 相同 |
| P6 | Unity：scene 匯出（mesh 模式，round-trip） | **完成（unity-terrain-example `claude/level-authoring-p6` dc7b49b，接在 P5 上）**。`SceneDefinition`（id、kernel 名稱、kernel 現有 yaml 的路徑、是否匯出地面、`map_edge_barrier`）和 `SpawnPoint`；`Tools > Level Authoring > Export Selected Scene` 輸出 `<id>_scene_<name>.yaml` 與 `<ground>.obj`。yaml 依頂層 key 做文字層級的 round-trip：只換 `id/name/scene_props/player_barriers/map_edge_barrier/player.spawn_position`（匯出地面時加上兩個地面路徑），其他 key、順序和註解原樣保留。OBJ：Terrain 依 stride 切成朝上的格狀三角形，其他幾何沿用 `MeshStampSampler`，scene root 座標、不做軸轉換。scene 的 marker 必須只有一個 template。batchmode `LevelAuthoring.SceneExportSelfTest.RunBatch`：用 marker 重做的 camp 與 kernel 的 `0_scene_initial_camp.yaml` 逐 byte 相同、移動一個 prop 只改那一行、新 scene 的 OBJ 法線朝上且 box 屋頂在 14 m、6 種錯誤都拒絕。kernel 端：匯出的 `test_yard` 放進 P4 分支後 bundle 烘出 joltmesh/navmesh，dedicated server 以 `gameplay_catalog.scene_7.yaml` 啟動並開始監聽。**限制**：地形是規則格點、沒有簡化（RTIN），檔案比原本的 216 m OBJ 大；`agent_routes` 仍要手寫；terrain 模式的 scene 匯出屬 P7 |
| P1b（選做） | K7 加上 catalog 的 `player_barrier_wall` entity template（只有這個 collider，沒有 HP、不能互動）；Unity 端在 `NetworkPrefabCatalog` 綁一個空 prefab（使用者負責） | 延伸 `player_barrier_test`：prop 版擋玩家、不擋 NPC 和子彈；刪掉 entity 後玩家能通過；client prediction 一致 |
| P7（第二階段） | G5、G6：terrain 手動擺放、recipe 自動 cook | 手擺的 terrain scene 不改 BUILD 就能 build 進 bundle 並載入 |

P1 與 P3–P4 互不依賴，可以並行；P1b、P2 依賴 P1；P5–P6 依賴 P2–P4 的格式。

## 9. 版本影響

| 項目 | 影響 |
|---|---|
| ABI | **有新增**（additive）：`KERNEL_MOVEMENT_LAYER_PLAYER_BARRIER`、`Kernel_SetStaticScenePlayerBarriers`、新 capability bit。依現有規則不升 ABI 版本，但 package 的 native library 要重 build。 |
| snapshot schema | 不變（牆是 scene 資料，不是 entity）。 |
| catalog 格式 | **v21**：`scene_props.rotation/grounded`、`player_barriers`、`map_edge_barrier`、stamp `sites`/`per_map_limits`，移除 `nest_sites`；第二階段加 `terrain.placements`。Unity client 要等 package 重 build 才能讀新 catalog。 |
| `bundle.bytes` | 需要更新（新的 catalog 格式、新的 camp 牆）。package 的 build 與更新由使用者執行。 |
| C# | 如果 client 端只經過 native 的 `ClientLoadSceneSlot`，C# 不需要新 binding；P1 實作時確認，需要的話放在獨立的 `claude/*-csharp` 分支。 |

## 10. 不在這次範圍

- 可攀爬、可跨越的矮牆：資料上是「一般碰撞 + `traversal` 標記（範圍、類型、方向、起終點）」，執行面需要新的移動動作和 navmesh off-mesh connection，另開計畫。
- 事件解鎖區域的 navmesh 開關（§8.1 的 polygon flags）。這次 static obstacle prop 可以擋人，但 AI 不會繞開。
- 可破壞的橋。
- Decoration（地形渲染、植被、天氣）。
- prefab 預覽（Q3 第二步）。
- 自動複製到 kernel repo（D2）。

## 11. 風險

- **移動路徑寫死預設 mask**（K5）：只要有一條沒用 actor 的 mask，牆就會對那種移動失效，例如被擊飛時穿牆。P1 的測試要逐條覆蓋。
- **client prediction 沒有牆**：玩家會在 client 穿過去再被拉回。P1 必須在 client prediction world 驗證。
- **catalog 格式變更**：v21 和舊 package 不相容，要和 package 更新一起規劃。
- **round-trip 蓋掉手寫欄位**：P6 用現有的 camp 檔比對來驗證。

## 12. Token / 成本控制

- 每一步先跑該步自己的 smoke test，不跑全部測試；全部測試只在使用者要求時跑。
- 只讀直接相關的檔案（第 1 節的出處清單）；不做全 repo 搜尋。
- 範圍擴大時（例如 K5 發現某條移動路徑要重構）先停下來說明。
- 每一步完成時回報：讀了哪些檔案、改了哪些檔案、做了哪些搜尋、跑了哪些 build/test。
