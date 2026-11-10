# 起始場景：用 scene template id 啟動任何場景

## 1. 目標

啟動流程統一成一件事：**選一個 scene template id 當 slot 0**。沒有指定時用 id 0，也就是 initial camp。

- initial camp 不再是特例。它只是一個沒有隨機 seed、帶有 game flow prop（`initial_camp`、`level_terminal`）的場景，寫在 `0_scene_initial_camp.yaml`。
- 指定別的 id 就能直接從那個場景開始，方便測試地圖、AI 和內容。
- map variant（`map_variant`／`map_variants`）和 scene template 合併成一套，只留 scene template。

背景：[SCENE_TABLE_PLAN.md](SCENE_TABLE_PLAN.md)、[SCENE_TABLE_PLAYABLE_LOOP_PLAN.md](SCENE_TABLE_PLAYABLE_LOOP_PLAN.md)。

## 2. 現況

| 項目 | 現在 |
|---|---|
| slot 0 的內容 | 入口檔的 `map_variant` 選一個 `map_variants` 疊在 root 上。預設 `composed_mesh`，也就是 mesh 地圖加上任務和巡邏 |
| 進 camp | 用另一個入口 `gameplay_catalog.camp.yaml`，由 bundle 用 sed 產生 |
| 關卡 | `scene_templates/<id>_scene_<name>.yaml`，id 必須是 1..65535，載入方式是「root 去掉 variant 後疊上 template」 |
| client 怎麼知道 slot 0 是什麼 | 跟著 server manifest 裡的 entry path 載入 catalog（Unity：`syncResult.Manifest.entry_path`） |
| 地形 recipe | `kernel_terrain` variant 烘焙成 `terrain/gameplay_catalog.nktr`；固定 seed 的 template 各自烘焙 |

template 本來就是一個完整的場景描述，所以 slot 0 也改用 template 時，載入器幾乎不用改。

## 3. 決定

使用者在 2026-11-10 選擇依建議撰寫，以下 Q1–Q5 照建議定案。Q6、Q7 是寫計劃時新發現的小問題，先給建議。

| # | 問題 | 決定 |
|---|---|---|
| Q1 | 用關卡 template 當 slot 0 時，要跑多少關卡流程？ | 只跑地圖、game_rule、巡邏和 prop。擊敗目標、`nests:`、回程門、重生次數池屬於「從 camp 開出去的關卡」，slot 0 不啟用 |
| Q2 | `--level` 要保留嗎？ | 保留（開發用：從 camp 開始，自動開那一關）。隨機關卡只能這樣快速測 |
| Q3 | `map_variants` 怎麼處理？ | 移除，不留相容層。catalog v20 起寫了就拒絕 |
| Q4 | 依賴目前預設地圖的測試怎麼改？ | 把現在的 `composed_mesh` variant 原樣做成一個 template，這些測試明確指定它 |
| Q5 | template id 0 保留給誰？ | 固定保留給 initial camp，不能是可選的關卡 |
| Q6 | 由 variant 改成的 template 叫什麼？（建議） | `5_scene_composed_mesh_test`、`6_scene_kernel_terrain_test`。測試用，沒有 `level:` 區塊，所以不會出現在關卡清單 |
| Q7 | `--scene` 和 `--gameplay-catalog-entry` 同時指定時？（建議） | 啟動失敗並說明。兩者只能擇一 |

## 4. 設計

### 4.1 catalog

- root 新增 `start_scene: <id>`，取代 `map_variant` 和 `map_variants`。只有 catalog 有 `scene_template_dir` 時才能寫；沒有 `scene_template_dir` 的舊 catalog（例如 `legged_locomotion`）完全不受影響。
- 沒寫 `start_scene` 時預設 0。
- 載入時，若呼叫端沒有指定 template，就套用 id = `start_scene` 的 template。呼叫端明確指定的 template（level slot 走的路徑）優先。
- template id 範圍改成 0..65535。id 0 必須存在，而且不能有 `level:` 區塊。
- **`level:` 區塊代表「可以從 terminal 選」**：關卡清單（`GameServer_GetSceneTemplate`）只列有 `level:` 的 template，server 收到選沒有 `level:` 的 template 的 `SCENE_SELECT` 時拒絕。
- **能當 slot 0 的 template 必須是 mesh 或固定 seed（recipe 已烘焙進 bundle）。** client 只能從 bundle 拿到 slot 0。`start_scene` 指向 `seed: -1` 的 template 時載入失敗並說明，要測隨機關卡請用 `--level`。
- catalog_version 升到 20。

### 4.2 bundle 入口

- bundle 為每個可當 slot 0 的 template 產生 `gameplay_catalog.scene_<id>.yaml`：就是 `gameplay_catalog.yaml`，只把 `start_scene` 改成那個 id，做法和現在產生 variant 入口一樣（sed）。
- `gameplay_catalog.yaml` 本身就是 `start_scene: 0`，所以預設入口就是 camp。
- 原本的 `gameplay_catalog.{kernel_terrain,composed_mesh,camp}.yaml` 移除。
- client 不需要任何改動：它照舊跟著 manifest 的 entry path 走。

### 4.3 啟動參數

| 啟動方式 | 怎麼選起始場景 |
|---|---|
| dedicated（bundle） | `--scene=<id>` 等於 `--gameplay-catalog-entry=gameplay_catalog.scene_<id>.yaml`。不給就是 `gameplay_catalog.yaml`（id 0） |
| dedicated（原始檔，`--gameplay-catalog=<path>`） | `--scene=<id>` 直接傳給載入器，不經過產生的入口檔 |
| listen host（`host_server_app`） | 同 dedicated |
| Unity host | 入口傳 `gameplay_catalog.scene_<id>.yaml`，或維持 `gameplay_catalog.yaml` 進 camp（C# 和 Unity 端由使用者處理） |

`--level` 不變：從起始場景開始，有人連線且在 camp 時自動開那一關。

### 4.4 slot 0 是關卡 template 時（Q1）

- 會跑：地圖、navmesh、`scene_props`、`patrols`、`agent_routes`，以及 template 自己的 `game_rule`（載入器已經會把它加進 `preload_director_template_ids`）。
- 不會跑：`objective`、`nests:`、回程門、`revives_per_player`／`max_team_revives` 的關卡重生池。這些只在 scene flow 開出去的 slot 1 生效。`player.respawn` 的一般設定照常生效。
- 沒有 `level_terminal`，就沒辦法從這個場景開關卡。這是預期行為：要測完整循環請從 camp 開始。

## 5. 步驟

| # | 工作 | 驗證 |
|---|---|---|
| P1 | 載入器：`start_scene`、id 0、`level:` 規則、slot 0 不能是 `seed: -1`、拒絕 `map_variant(s)`、catalog v20 | `scene_template_catalog_test` 新增案例：預設套用 id 0、明確指定的 template 優先、各種錯誤都有拒絕 |
| P2 | 內容：新增 `0_scene_initial_camp`，`camp` variant 的內容搬進去；`composed_mesh`、`kernel_terrain` 原樣改成 template 5、6；刪除 `map_variants`；root 寫 `start_scene: 0` | template 0 和舊 `camp` variant 載入後的 config hash 相同；5、6 分別和舊 variant 相同（hash 不包含 scene template 名稱時才可比，否則逐欄比） |
| P3 | 地形 recipe：`terrain_recipe` 改成烘焙 template 6（`terrain_recipe_cook` 已支援 `scene_template:<name>`），輸出路徑由 template 的 `terrain.recipe` 決定 | 地形 golden hash 測試不變：seed 和參數原樣搬過去，hash 必須完全一樣 |
| P4 | bundle：產生 `gameplay_catalog.scene_<id>.yaml`，移除 variant 入口 | 檢查入口清單；client 經 catalog sync 用 `scene_5` 入口啟動，地圖正確 |
| P5 | 關卡清單和選關只認 `level:`；確認 slot 0 是關卡 template 時不會跑關卡流程（Q1） | `scene_flow_startup_test` 新增：選 template 0 或 5 被拒絕；用 template 2 當 slot 0 時 game_rule 有跑、沒有回程門也沒有蟲巢 |
| P6 | app：`--scene`、Q7 的衝突檢查、說明文字；`--level` 不變 | 實際啟動 dedicated：`--scene=0`、`--scene=5`、`--scene=4`（應失敗）、`--scene` 加 `--gameplay-catalog-entry`（應失敗） |
| P7 | 測試遷移：預設入口的測試改用 template 5（或它真正需要的場景）；引用 variant 入口的 14 處改名 | 完整回歸：只允許原本的 6 個已知失敗 |
| P8 | 文件：`gameplay_catalog.yaml` 的註解、[SCENE_TABLE_UNITY_HANDOFF.md](SCENE_TABLE_UNITY_HANDOFF.md) 第 2 節第 1 項改成「預設入口就是 camp，不用改」 | 人工檢查 |

**狀態：** P1 已完成（`claude/start-scene-p1`）。載入器支援 v20 的 `start_scene`、id 0 和 `level:` 旗標（`SceneTemplateRef::is_level`）；v19 的 catalog 行為不變，所以出貨的 catalog 在 P2 之前照常可用。catalog hash 多了 `start_scene` 一欄，client 和 server 用同一版就會相符。

**最終驗收（使用者 2026-11-10 指定）：** 全部做完後，用下面這條預設指令啟動 dedicated server，玩家連上後必須在 initial camp（出生點在 camp、看得到 `initial_camp` 和 `level_terminal`、沒有敵人）：

```text
--mode=dedicated_server
--port=7801
--gameplay-catalog-bundle=game_server/gameplay_catalog_bundle/bundle.zip
--gameplay-catalog-entry=gameplay_catalog.yaml
```

P1–P6 每一步跑單一相關測試（smoke-first）。P7 是唯一需要完整回歸的步驟，也是成本最高的一步。

### P7 的範圍

47 個測試檔用預設入口 `gameplay_catalog.yaml`。粗估其中約 24 個提到 director、巡邏或座標（grep 只代表有提到，不一定依賴）。預設場景從任務地圖換成 camp 後，最直接的遷移是讓這些測試改用 template 5，行為和現在完全一樣。之後若要讓測試改用更貼切的場景，另外再做。

原始檔載入的測試（不經過 bundle）沒有產生的入口檔可用，改用現有的 `load_gameplay_config(path, scene_template)` 指定 template 名稱。

## 6. 版本影響

| 項目 | 影響 |
|---|---|
| kernel ABI | 不變 |
| snapshot／packet schema | 不變 |
| game message | 不變 |
| catalog 格式 | **v20**：新增 `start_scene`，移除 `map_variant(s)`，template id 允許 0 |
| `bundle.bytes` | **要更新**：入口檔改名、新增 template |
| package | 要重打。舊 package 讀不了 v20 catalog |
| Unity | 預設入口不變就會進 camp；`HostModeRunner` 的入口說明要改（列的是舊 variant 入口） |

## 7. 風險

- **測試遷移量大（P7）**：機械性但檔案多，要一次改完並跑完整回歸，中途不 merge。
- **地形 hash（P3）**：recipe 只是換來源位置，seed 和參數必須原樣搬過去。Windows hash 仍待使用者驗證，這次不影響它。
- **Unity 版本相依**：catalog v20 和舊 package 不相容，server、client、package 要同時更新。

## 8. 不在這次範圍

- 讓隨機關卡當 slot 0：需要在 `SCENE_STATE` 列出 slot 0，並讓 client 用 API 載入 slot 0，要動 kernel 和 capability。目前用 `--level` 代替。
- 存檔與重連、LAN discovery、用 item 開特殊關卡。
