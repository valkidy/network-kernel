# Scene table 後續計劃：可遊玩的關卡循環

狀態：**計劃已定案（2026-11-09），可以開工。** 尚未實作。
分支：`claude/scene-table-s4`（`docs/SCENE_TABLE_PLAN.md` 的 S1–S4 已在這條分支上）。
日期：2026-11-09。

**目標：** 玩家開啟遊戲後，透過遊戲內機制完成一輪循環：進入 camp → 開啟關卡 → 走進傳送門 → 完成或失敗 → 回到 camp → 可以再開下一關。

S1–S4 已完成 kernel 的 slot、catalog 的 scene template、server 的關卡載入和卸載、隊伍流程，但目前只有測試能跑。本計劃補上其餘部分。

---

## 1. 已定案的決定（2026-11-09）

| # | 決定 |
|---|---|
| F1 | 每個關卡一份 scene template 檔案，有自己的關卡資訊、game rule、敵人配置和場景 |
| F2 | 場景兩種開啟方式：**用 id 開啟**（啟動時的 camp、快速測試）、**用 recipe 開啟**（從 camp 動態開關卡） |
| F3 | recipe 用 **game message 分段**傳輸，重組放在 native；不改 kernel、不 bump schema |
| F4 | `bundle.bytes` 定義為 global/template 資料 |
| F5 | 動態關卡的敵人只有已標記的蟲巢和巡邏隊 |
| F6 | 出生點預留欄位，預設在地圖正中央 |
| F7 | 回程傳送門在出生點往 +x 3 m |
| F8 | 預設成功條件：擊敗 20 個敵人，成功時出現回程門 |
| F9 | 失敗條件維持 S4：**隊伍重生次數用完且關卡內全員死亡**，直接回 camp |
| F10 | 重生次數預設 = **小隊人數 × 5**（2 人小隊 = `team_revive_times: 10`），中途加入者再加 5 次，**一場關卡累計發放上限 20** |
| F11 | 先做 camp 的開關卡 prop（美術由使用者準備） |
| F12 | 保留之後用 item 開啟特殊關卡（客製 recipe，例如活動關卡）的路 |
| F13 | seed 開放成欄位，用一個約定的數字代表由系統隨機產生 |
| F14 | **關卡權限屬於房主**：listen 模式是 host 自己的玩家，dedicated 模式是目前 peer id 最小的玩家（取代 S4 的「只有 host」）。`--level` 只是開發用參數 |
| — | LAN discovery（使用者的提案是 client-only action graph）**保留，另開一輪討論，不在本計劃內** |

---

## 2. 名詞

- **scene template id**：關卡在 catalog 裡的穩定編號，寫在檔案裡，也是 wire 上的 id。
- `static_collision_scene.scene_id`：碰撞物件的識別碼，所有關卡都一樣，**不是**上面的 id。
- **固定場景**：資料在 bundle 裡，用 id 開啟。
- **動態場景**：server 產生 recipe，傳給 client 組出來。

(a) 和 (b) 是**同一條載入流程**，只差 recipe 從哪裡來（bundle 或 server 當場產生）。

---

## 3. 現況（`claude/scene-table-s4` `3bde293c`，讀過程式碼）

| 事實 | 位置 |
|---|---|
| **正式啟動路徑沒有啟用 scene table**，只有測試呼叫 `enable_scene_table` | `GameServer_CreateWithGameplayCatalogFromMemory`、`dedicated_server_app.cc` |
| camp（slot 0）會跑 root 的任務和 `patrols: ids [1, 3]` | `gameplay_catalog.yaml` |
| 關卡可覆蓋的 key 不含 `patrols`、`patrol_budget`、`agent_budget`、`reinforce_budget` | `kMapOverlayKeys` |
| `offset_scene_config` 會清掉關卡的 patrols | `scene_table.cc` |
| 任務寫在全域的 director entity template（101、103），靠 `preload_directors` 選 | `entity_templates/` |
| game rule 的條件只有 `group_eliminated`、`player_count_at_least` | `game_rule_director.h` |
| stamp YAML 只允許 `id`、`name`、`heights`，**沒有蟲巢標記** | `gameplay_config.cc` 的 stamp 解析 |
| 版面產生沒有保留區域的參數 | `terrain_layout.h` 的 `LayoutParams` |
| `GameServer_ClientLoadSceneSlot` 在呼叫的 thread 上同步組地形，而且只能讀 bundle 裡的 recipe | `game_server_api.cc`、`scene_table.cc` |
| recipe 大小 = `96 + 24 × placement 數`；recipe 自帶 library hash 和 heights hash | `terrain_recipe.h` |
| `send_scene_state` 不檢查送出結果 | `scene_flow.cc` |
| 只有 CAMP 階段能開關卡；傳送門開了卻沒人進、或關卡沒有 game rule，都**回不到 CAMP** | `scene_flow.cc` |
| `ShelterDirector` 把每個 `UiOpened` 都當成進出建築 | `shelter_director.cc` |
| client 沒有 API 能列出有哪些關卡 | `game_server_api.h` |
| bundle 的 genrule 把 variant 名稱寫死，來源清單也要手動加目錄 | `gameplay_catalog_bundle/BUILD.bazel`、`game_server/BUILD.bazel` |

---

## 4. Catalog：scene template 檔案

新增 `scene_template_dir: scene_templates`，每個關卡一個 `<id>_scene_<name>.yaml`：

```yaml
id: 1
name: forest_patrol
level:                        # 新增：給選關介面用
  display_name: ...
  description: ...
terrain:
  seed: -1                    # -1 = 由系統隨機
  ...
# 或 static_collision_scene + navigation_mesh（固定場景）
player:
  # spawn_position 省略時為地圖正中央
  respawn: {revives_per_player: 5}
objective: {defeat_enemies: 20}
game_rule: {...}              # 可選，直接寫在關卡檔
nest_sites: {...}             # 第 7 節
patrols: [...]
patrol_budget: {...}
agent_budget: {...}
reinforce_budget: {...}
```

**留在全域的：** 武器、道具、projectile、collider、action、status、entity template、stamp 和 ground map。

**camp：** 新增一個 map variant `camp`（沒有任務、沒有巡邏，有開關卡的 prop）。Unity host 預設用 `gameplay_catalog.camp.yaml`。現有的 `composed_mesh` 和 root 不變，約 70 個依賴它們的測試不受影響。

**bundle.bytes** 的定義：global/template 資料，加上用 id 開啟的固定場景（camp 和測試場景）。stamp 和 ground map 是組地形的素材，要留在 bundle。動態關卡的 recipe 不進 bundle，所以每局不同的地圖不會改變 bundle 的 hash。

---

## 5. 步驟

每一步都有自己的 `claude/scene-table-*` 分支，從前一步疊上去。交付時說明 ABI、schema、catalog 格式和 `bundle.bytes` 的變動。

### L0 接上正式啟動路徑（已完成，`claude/scene-table-l0`）

已驗證：`scene_flow_startup_test`（走 Unity host 用的 C API，listen 和 dedicated）；實際執行 dedicated app 加 `--level`，玩家連上後關卡開啟、離線後關卡結束。`Kernel_ServerSendGameMessage` 送出失敗後的重試沒有直接測到（無法在測試中讓 GNS 送出失敗），定期補送有測到。`SCENE_STATE` 多了 `owner_net_id`，header 變成 29 B。

| # | 工作 |
|---|---|
| L0.1 | `GameServer_CreateWithGameplayCatalogFromMemory` 在 catalog 有 scene template 時啟用 scene table（沒有的話行為不變） |
| L0.2 | `dedicated_server_app` 同樣啟用。開發用參數 `--level=<name>`：隊伍在 camp 且至少有一位玩家時自動開啟該關卡，載入失敗就停止，不重試 |
| L0.3 | `send_scene_state` 檢查送出結果，失敗就重試，並定期補送 |
| L0.4 | 卡死的出口：房主可以取消關卡（新訊息 `SCENE_ABORT`）；傳送門開啟後 5 分鐘沒人進就自動取消；**沒有任何玩家連線時直接結束關卡、回到 camp**（dedicated 才會發生） |
| L0.5 | 關卡權限：`host_player_net_id` 改成「房主」，listen 是 host 自己的玩家，dedicated 是目前 peer id 最小的玩家（peer id 只增不減，所以就是最早連線還在線上的人，房主離開時自動換人，不用另外記狀態）。`SCENE_SELECT` 依此檢查；`SCENE_STATE` 帶房主的 net id |

測試：改用 C API 的建立路徑，驗證 `SCENE_SELECT` 確實會開啟關卡；取消關卡後能再開下一關；dedicated 上第一位玩家能選關、其他人不行，房主離線後換人；全員離線時關卡結束。

### L1 Scene template 檔案（已完成，`claude/scene-table-l1`）

已驗證：`scene_template_catalog_test`（檔案規則、hash、camp variant、patrol offset）、`scene_slot_runtime_test`（關卡自己的 game rule 生出蟲巢，warband 巡邏隊在 slot 內）、`scene_flow_startup_test`（client 列出關卡清單，wire 上用 id）。

和計劃的差異：關卡內的 `game_rule` 由 loader 轉成一個內部的 director template（id 60000），沿用既有解析，只有該關卡會 preload。inline 的 `scene_templates:` 已移除。`GAME_SERVER_MESSAGE_SCENE_*` 的 template 欄位改成 `template_id`。

| # | 工作 |
|---|---|
| L1.1 | `scene_template_dir` 掃描、檔案內明確的 `id`（重複就拒絕）；`SCENE_STATE` 和 `SCENE_SELECT` 用這個 id |
| L1.2 | 關卡可覆蓋 patrols 和各種 budget；`offset_scene_config` 補上 patrol 區域的 offset，不再清掉 |
| L1.3 | `level:` 資訊；`GameServer` 新增列出關卡清單的 API |
| L1.4 | 關卡檔內的 `game_rule`，沿用現有 director 的解析 |
| L1.5 | `camp` variant；bundle 規則加入新目錄和新 variant |

版本：catalog_version 19，package 和 `bundle.bytes` 都要更新。

### L2 關卡目標

| # | 工作 |
|---|---|
| L2.1 | `objective.defeat_enemies`：用 `EntityDied` 計算該 slot 內敵方 agent 的死亡數，達標就開回程門 |
| L2.2 | 重生次數：`revives_per_player`（預設 5）、`max_team_revives`（預設 20）。第一個人進入關卡時發放 min(連線玩家數 × 5, 20)；之後每位中途加入者再發 5 次。上限算的是**這場關卡累計發放的總數**，不是目前剩餘數，否則反覆進出仍能把次數補滿。有寫 `team_revive_times` 時以它為準，不再追加 |
| L2.3 | `SCENE_STATE` 加上目前擊敗數和目標數，給 HUD 用 |

完成條件：有 `objective` 用 objective，否則用 game rule 全部完成。

### L3 用 recipe 開啟

| # | 工作 |
|---|---|
| L3.1 | `seed: -1` 由 server 隨機，實際 seed 寫進 log；烤 recipe 的工具遇到 `-1` 要報錯 |
| L3.2 | `SCENE_STATE` 的 slot 項目加上來源（id 或 recipe）和 recipe hash |
| L3.3 | 分段傳輸層：`(slot, generation, transfer_id)`、native 重組、收齊時限、逾時重傳整份、任一段送出失敗就整份重來；這層之後存檔系統也會用 |
| L3.4 | server 保留 recipe，給中途加入和重連的玩家 |
| L3.5 | client 載入 API 接受傳入的 recipe |
| L3.6 | client 載入拆成兩段：背景組地形，主執行緒提交給 kernel |
| L3.7 | client 組地形失敗或 hash 不符時回報 server（新訊息 `SCENE_LOAD_FAILED`），不用等 60 秒 |

測試：listen 和 dedicated 各跑一次動態關卡，含中途加入；在 100 ms、20% 遺失下再跑一次。

### L4 動態地圖的內容擺放

| # | 工作 |
|---|---|
| L4.1 | `LayoutParams` 加上中央保留區，讓出生點不會落在 stamp 或懸崖上 |
| L4.2 | 出生點預設地圖正中央，高度取自 heightfield；回程門同樣取高度 |
| L4.3 | stamp YAML 新增 `nest_sites`（本地座標），隨 stamp 的旋轉和鏡像換算到世界座標 |
| L4.4 | 關卡用 `nest_sites` 生成蟲巢；巡邏隊用既有的 `around_players` |

測試：多個 seed 下，出生點和傳送門都在地面上、蟲巢都在地面上、20 個敵人確實打得到。

### L5 開關卡 prop

| # | 工作 |
|---|---|
| L5.1 | camp 新增開關卡 prop：`on_activated` 加 `open_ui`，用新的 ui_id；美術由使用者準備 |
| L5.2 | `UiOpened` 分流：只有 shelter 建築交給 `ShelterDirector` |
| L5.3 | game_server 檢查是否為房主（L0.5），用新的 game message 通知該玩家開介面 |

Unity 顯示關卡清單，選定後送 `SCENE_SELECT`。不動 kernel。

### L6 端到端驗證

用正式建立路徑跑完整循環：camp → 啟動 prop → 選關 → 動態關卡 → 擊敗 20 個敵人 → 回程門 → camp → 再開一關 → 全滅失敗 → 回 camp。

**Windows：** 動態關卡要求不同平台組出完全相同的地形，所以 Windows 的 golden hash 必須驗過（使用者在 Windows 上跑）。

### 之後：item 開啟特殊關卡（F12，不在這一輪）

`open_ui` 無法帶參數，所以 item 需要一個新的 action（暫名 `request_level_start`，參數是 template 和 recipe 來源）。這會動到 kernel 的 action enum 和 ABI。L1 和 L3 的設計要讓「指定 template 和 recipe」這個入口可以直接接上。

---

## 6. Unity（使用者負責）

- C# 對應：S1–S4 和本計劃新增的 export 與訊息格式。
- 單一遊戲入口：開遊戲就用 `gameplay_catalog.camp.yaml` 啟動 listen host。
- 依 `SCENE_STATE` 載入和卸載 slot、回報 ready（pure client 設 `REGISTER_COLLISION`，listen host 不設）。
- 依 slot 畫地形、畫傳送門、載入畫面、擊敗數 HUD、選關介面、開關卡 prop 的美術。
- 單人模式在 port 被占用時無法啟動的問題，一起處理。

---

## 7. 版本影響總表

| 步驟 | kernel ABI | snapshot / packet schema | catalog 格式 | `bundle.bytes` | package |
|---|---|---|---|---|---|
| L0 | 不變 | 不變 | 不變 | 不變 | 要重打（S1–S4 累積） |
| L1 | 不變 | 不變 | **19** | **要更新** | 要重打 |
| L2 | 不變 | 不變 | 新 key | 要更新 | 要重打 |
| L3 | 不變 | 不變（走 game message，`SCENE_STATE` 格式變更） | 不變 | 動態關卡的 recipe 移出 | 要重打 |
| L4 | 不變 | 不變 | stamp 新 key | 要更新 | 要重打 |
| L5 | 不變 | 不變 | 新 prop | 要更新 | 要重打 |

GameServer API 的新增都用 capability 判斷，不 bump。

---

## 8. 風險

| 風險 | 對策 |
|---|---|
| 動態地圖上的東西生在半空或地底（沒有機制會擋） | L4 的測試跑多個 seed，全部檢查是否在地面上 |
| 中央保留區讓版面更難通過驗收 | L4.1 量測各 seed 的通過率 |
| 跨平台組地形不一致 | hash 不符時回報；L6 驗 Windows |
| 巡邏隊打不滿 20 個敵人，關卡完成不了 | L4 測試確認；L0.4 的取消是最後的出口 |
| 拆檔弄壞大量測試 | camp 另開 variant，root 和 `composed_mesh` 不變 |

---

## 9. 問題與決定（2026-11-09）

| # | 問題 | 決定 |
|---|---|---|
| Q1 | 隨機 seed 的約定數字 | `-1`，和 `team_revive_times: -1` 慣例一致 |
| Q2 | bundle 是否包含固定場景（camp、測試） | 包含 |
| Q3 | 蟲巢標記怎麼來 | stamp 檔加 `nest_sites`；沒有資料的 stamp 用中心點 |
| Q4 | 「擊敗敵人」算什麼 | 只算敵方 agent 的死亡，蟲巢和被系統清掉的不算 |
| Q5 | 小隊人數怎麼算 | 第一個人進入關卡時的連線玩家數；中途加入者再加 5 次；一場關卡累計發放上限 20 |
| Q6 | 傳送門開啟後無人進入多久自動取消 | 5 分鐘 |
| Q7 | dedicated 模式的開關卡方式 | 第一位連線的玩家當房主（F14、L0.5）；`--level` 只用於開發測試 |

**已接受的限制：**
- 房主斷線重連後會拿到新的 peer id，因此失去房主身份；等存檔和重連那一輪有了玩家身份再處理。
- 第一個連上的人就有權限；公開伺服器需要別的機制，不在這一輪。
- 房主中途離開不影響進行中的關卡，由下一位接手選關和取消的權限。

---

## 10. Token 與成本控管

- 每一步先跑該步的 smoke 測試，碰到 AI 或流程核心時才跑全部 game_server 測試。
- build 一律用 `-c opt`。
- 只讀和該步直接相關的檔案，第 3 節已列出已知的修改點。
- 範圍明顯超出本文件時，先停下來說明原因。
- 每一步結束時回報：讀過的檔案、修改的檔案、搜尋次數、build 和 test 指令。
