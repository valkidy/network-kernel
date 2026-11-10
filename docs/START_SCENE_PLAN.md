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

原本的 P1–P8 中，P2（移除 variant）、P4（bundle 入口）、P7（測試遷移）互相依賴，任何一個單獨做都會讓分支壞掉：bundle 的 sed 檢查會失敗、地形 recipe 烘焙找不到 variant、約 24 個測試的地圖被換掉。使用者在 2026-11-10 同意改成下面的順序，每個 commit 都能通過測試。

| # | 工作 | 驗證 | 狀態 |
|---|---|---|---|
| P1 | 載入器：`start_scene`、id 0、`level:` 規則、slot 0 不能是 `seed: -1`、v20 拒絕 `map_variant(s)` | `scene_template_catalog_test` | 完成（`claude/start-scene-p1`） |
| A | v19 下新增 template 0、5、6，和 variant 並存；載入器加「指定起始場景 id」（`load_start_scene_config_from_*`）；關卡清單和 `SCENE_SELECT` 只認 `level:`（原 P5 提前） | 三個 template 和對應 variant 的 catalog hash 相同；清單只列 4 個關卡；選 0 或 5 不會開關卡 | 完成（`claude/start-scene-a`） |
| B | 地形 recipe 改從 template 6 烘焙（原 P3） | recipe 和改動前逐 byte 相同（heights_hash `e03e9e899ec5c966`）；改 template 6 的 seed 時 recipe 跟著變 | 完成（`claude/start-scene-b`） |
| C | 一次切換（原 P2 + P4 + P7）：root 升 v20、`start_scene: 0`、刪除 variant、bundle 產生 `gameplay_catalog.scene_<id>.yaml`、依賴任務地圖的測試改指定 template 5、variant 入口的引用改名 | 完整回歸只允許原本的 6 個失敗 | 完成（`claude/start-scene-c`） |
| D | app 的 `--scene` 和 Q7 的衝突檢查（原 P6）；確認關卡 template 當 slot 0 時不跑關卡流程（原 P5 的 Q1 部分）；文件（原 P8） | 實際啟動 dedicated：`--scene=0`、`--scene=5`、`--scene=4`（應失敗）、`--scene` 加 `--gameplay-catalog-entry`（應失敗） | 完成（`claude/start-scene-d`） |

**最終驗收（使用者 2026-11-10 指定）：** 全部做完後，用下面這條預設指令啟動 dedicated server，玩家連上後必須在 initial camp（出生點在 camp、看得到 `initial_camp` 和 `level_terminal`、沒有敵人）：

```text
--mode=dedicated_server
--port=7801
--gameplay-catalog-bundle=game_server/gameplay_catalog_bundle/bundle.zip
--gameplay-catalog-entry=gameplay_catalog.yaml
```

**驗收結果（2026-11-10，`claude/start-scene-d`）：通過。** 用上面這條指令實際啟動 dedicated server，再用暫時的 client（未 commit）連上：server 給的入口是 `gameplay_catalog.yaml`，玩家出生在 (106.8, 20.4, 77.0)，看得到 `initial_camp`（219）和 `level_terminal`（231），8 秒內沒有敵人，`SCENE_STATE` 是 camp 狀態而且玩家是房主。對照組 `--scene=5` 看不到 terminal，判定為不在 camp。

**D 的實測：** `--scene=0`、`--scene=5` 正常啟動；`--scene=4` 以「bundle 沒有 `gameplay_catalog.scene_4.yaml`」失敗；`--scene` 加 `--gameplay-catalog-entry`、client 加 `--scene` 都在參數檢查時失敗。file 模式（`--gameplay-catalog=`）在原始碼目錄下不論有沒有 `--scene` 都會失敗，因為產生的 skeleton 檔不在原始碼目錄，這是原本就有的限制；`--scene` 在 file 模式用的載入函式已由單元測試涵蓋。Q1 由 `scene_flow_startup_test` 的 `level_as_start_scene` 驗證：template 3 當起始場景時 game_rule 照常出兵，殺 25 個敵人後仍沒有目標、回程門或關卡 slot。

每一步先跑相關的單一測試（smoke-first）；C 和最後的驗收跑完整回歸。

### C 的結果

- **測試 data**：62 個測試的 data glob 原本沒有 `scene_templates/`。v19 時找不到 template 只是沒有關卡，v20 起 template 0 是必要的，所以全部補上。
- **改用 template 5 的測試**（依賴舊預設任務地圖）：`ai_agent_command`、`ai_controller_spec`、`alert_spawner`、`gameplay_config`、`gingerbread_door_stack`、`pass_giant_route`、`patrol_catalog`、`shipping_catalog_flow`、`game_server_api`（敵人那段改在獨立 kernel 上）、`dynamic_abi_smoke`（同上）。新增 `default_game_server_gameplay_config(start_scene)`。
- **入口改名**：`mission_on_map`、`static_scene_slot_grounding`、`terrain_catalog`、`scene_flow_startup`、`scene_playable_loop`。
- **改寫**：`scene_template_catalog_test` 改測 v20；`terrain_catalog_test` 的地形規則改成修改 template 6。
- **規則調整**：「必須宣告 director」只要求有 `level:` 的關卡。這條規則是為了防止 root 的任務漏進關卡；template 5 不是關卡，可以像舊 `composed_mesh` variant 一樣沿用 root 的任務。
- **建立順序**：用 catalog 建立 game server（`GameServer_CreateWithGameplayCatalogFromMemory`）必須在 kernel 啟動之前，否則 kernel 會拒絕註冊靜態場景。

### C 的測試遷移範圍

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
