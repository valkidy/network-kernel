# Action Graph 重構實作計劃

狀態：待使用者確認。日期：2026-10-12。基準：`main` ee8531ec（ABI 104）。
預定執行：2026-10-13 07:01 起，分支 `claude/action-graph-refactor`（從 `main` 開，不從 HEAD）。

本計劃依「Action Graph 重構需求」（2026-10-12）擬定，並以 2026-10-12 對 `main`
的靜態核對結果為事實基礎（§1）。merge 進 `feat-unity-plugin`／`dev-*`、Unity
package 打包與 `bundle.bytes` 更新、C# ABI mirror 都由使用者處理，不在本計劃範圍。

## 0. 決策紀錄

| # | 決策 | 結論（2026-10-13） |
|---|---|---|
| D1 | graph 路徑的傷害來源 | **採建議。** area effect／standard impact 把已解析傷害（含 falloff）放進事件，新增 event 來源 `event.damage`；graph `amount` 綁 `event.damage`；projectile `damage` 是唯一數值來源。ABI bump。 |
| D2 | 取代 `apply_impulse.collision_mask` | **使用者確認的路徑：** projectile `collision_mask` 決定誰被測試 → action 的 strength 對目標的 resistance 決定誰被推／拉／施加 status。graph 不再有 mask。執行器保留**硬性能力限制**（非 authoring）：actor 可推；prop 只在 world mode 且非 carrying 時可推；其他 kind 不推。`impulse_resistance` 改成通用名稱（見 D2a），因為 apply_impulse、apply_pull、apply_status 的 `status_strength` 三者都比它。 |
| D2a | resistance 新名稱 | **`force_resistance`**（component `ForceResistance`）。舊 YAML 鍵 `impulse_resistance` 載入時報錯並提示新名稱，不默默接受。 |
| D3 | tornado 飛行中重拉 | **接受飛行中重拉（預期效果）**，數值不動，只修正註解。 |
| D4 | entity hitbox 幾何重複、weapon_id 反寫 projectile | **整個 D4 另開一批，本批不碰。** D4a 方向已定：大型身形各建 collider template，entity 只引用 collider、移除 `hitbox:`，兩者並存時 loader 報錯。D4b 方向：weapon_id 由發射當下的事件來源帶入，不寫回 template。 |
| D5 | ABI bump 次數 | **採建議。** 整批一次 104 → 105；開工先檢查其他 `claude/*` 分支是否已佔用 105。 |
| D6 | 直接子 graph 呼叫 | **不做。** |

## 1. 已核對事實（實作依據）

- catalog：31 projectile、44 collider、32 graph、53 處非註解 graph 引用。
- 完全相同（去掉 id／註解）：
  - `action_apply_damage_at_{activated,destroy_entity,health_depleted}`、`action_status_damage_tick`
  - `action_spawn_projectile_at_{impact,expired,destroy_entity}`
- `action_apply_status_effect` 無引用。`action_apply_damage_at_collision` 是兩筆傷害（self＋target），**不屬於**重複組。
- **engine 也寫死 graph ID**：[action_graph.cc:816-860](../engine/src/simulation/src/action_graph.cc) 的 `compile_spawn_projectile_binding`／`compile_apply_damage_binding` 依事件合成 `action_spawn_projectile_at_*`、`action_apply_damage_at_*` 名稱。改名必須同步這裡。
- loader 用**非遞迴** `directory_iterator`（[gameplay_config.cc:1241](../game_server/src/gameplay_config.cc)），已排序。
- `game_server/BUILD.bazel` 至少 10 處 `action_graph_templates/*.yaml` 非遞迴 glob。
- 舊 ID 出現在測試：`gameplay_config_test.cc`、`weapon_template_loader_test.cc`、`activation_gameplay_config_test.cc`、`portal_test.cc`。
- binding 覆寫 strength 時一律 scalar＋RADIAL，vertical 歸零（[gameplay_config.cc:7150](../game_server/src/gameplay_config.cc)）。
- pull 的 distance／airtime_ticks／max_speed／strength、impulse 的 lockout_ticks、damage 的 stagger、`ui_id`、`placement` 都是 action 上的字面值，binding 無法供應。
- area effect 的 graph 分支計算了 falloff 後的 `damage` 但沒傳出（[area_effect_system.cc:196](../engine/src/simulation/src/area_effect_system.cc)）。
- 傷害多處維護：rocket_explosion 45×3、grunt_slam_hit 40×3、shockwave 20×2、pull_blast 10×2、frag 1000×2；tent_collapse_blast／tornado 寫 `damage: 1` 佔位。
- rocket（projectile 3）`damage: 45` 且 impact 綁 `action_spawn_projectile_at_impact`；依 grunt_slam_hit 註解「綁了 impact graph 就由 graph 出傷害」，**直擊 45 可能目前根本沒有造成傷害**。未經 runtime 驗證，列入 P0。
- `KernelActionDefinition::impulse_collision_mask` 在 ABI struct 內（[kernel_types.h:1104](../engine/src/kernel/public/kernel_types.h)）。

## 2. 分階段實作

每個 phase 一個 commit（必要時拆小），先寫失敗的 smoke test，再改程式。
每個 commit 前檢查目前分支（checkout 可能被切走）。

### P0 基準與量測（不改行為）

1. 開分支；記錄受影響測試的**現有失敗集合**（main 有紅色基準，以集合比對，不以「全綠」為準）。
2. 量測三個現況，寫成測試或記錄：
   - rocket 直擊在綁 spawn graph 時是否造成 45 傷害（決定 P4 的「替代／附加」契約如何保持現有行為）。
   - tent_collapse_blast 的 `player_side` mask 會不會選到 prop（決定 D2 是否需要 `applies_to`）。
   - rocket_explosion 在 graph 路徑上的實際傷害（預期：不衰減的 45）。
3. 輸出：一份 fact 記錄附在 commit message。

預算：讀 ≤6 檔，搜尋 ≤5 次，跑 3 個目標測試。

### P1 目錄、命名、全域 ID（無 ABI 變動）

1. loader：graph 目錄改遞迴掃描，排序以相對路徑為準（確定性）；其他 template 目錄維持非遞迴。
2. 驗證：`id` 必須等於檔名（去副檔名）；必須以 `action_` 開頭、小寫 snake_case；跨子目錄重複 ID → 錯誤訊息列出兩個路徑；未知 graph 引用 → 錯誤帶引用者路徑。
3. BUILD.bazel：所有 graph glob 改 `action_graph_templates/**/*.yaml`。
4. 盤點打包／工具腳本是否寫死 graph 路徑（只 grep 該資料夾名稱）。
5. 測試：子目錄載入、重複 ID 拒絕、id≠檔名拒絕、未知 ID 診斷。

預算：讀 2 檔局部，修改 loader＋BUILD＋1 個測試檔。

### P2 合併兩組完全重複（無 ABI 變動）

1. 新增 `common/action_apply_damage.yaml`、`common/action_spawn_projectile.yaml`。
2. 遷移引用並刪除 7 份舊檔；同步 engine 合成名稱（apply_damage 系列 → `action_apply_damage`，`at_collision` 改為 derived 或 `action_apply_damage_to_source_and_target`，交由 P5 決定，P2 先保留原名）。
3. 刪除 `action_apply_status_effect`（再 grep 一次全 repo，含測試與工具）。
4. 更新 4 個測試檔的舊 ID。
5. 測試：每個原持有者仍載入、event expression 仍合法、catalog hash 確定（同輸入兩次一樣）。

graph 數：32 → 26。

### P3 typed binding（loader 端解析，預期無 ABI 變動）

沿用 `compile_impulse_strength` 的模式：binding 在載入時解析成 `KernelActionDefinition` 的字面值，kernel 不需新欄位。

1. impulse strength：binding 可寫 scalar 或 `[horizontal, vertical]`；pair 覆寫 → SPLIT，不再默默改成 RADIAL。
2. 把下列 action 欄位改為可接受 `params.<name>`：impulse `lockout_ticks`；damage `stagger`；pull `distance`／`airtime_ticks`／`max_speed`／`strength`；open_ui `ui_id`；spawn `placement`。
3. 型別、有限值與範圍檢查沿用各欄位現有的 validator（例如 pull 的 range check、lockout ≤ `KERNEL_MAX_IMPULSE_LOCKOUT_TICKS`）。
4. stagger：「未指定」＝依傷害衍生，「0」＝明確不 stagger，兩者在 binding 與 default 都要能區分。
5. graph `parameters` 的 default 僅作為持有者未提供時的值；持有者有寫就以持有者為準（不新增 preset 檔）。
6. 測試：同一 graph 兩個持有者給不同數值；pair 覆寫保留 vertical；stagger 未指定 vs 0；非法值拒絕。

### P4 傷害契約與 mask 移除（ABI 105，依 D1／D2）

1. 依 D1 新增 `event.damage`：area effect 與 standard impact 在 graph 路徑把已解析傷害（含 falloff）放入事件；graph `amount` 綁 `event.damage`。
2. 明確契約（寫入 guide）：綁了 graph 的命中階段，普通傷害**不**提交，傷害由 graph 透過 `event.damage` 套用；P0 量到的 rocket 直擊行為以明確設定保留，不隱含切換。
3. 允許 `damage: 0` 表示無傷害；tent_collapse_blast／tornado 改 0。
4. 移除 graph `collision_mask`（D2）：authoring 寫了就**報錯**，錯誤訊息指出改用 projectile mask＋目標 resistance；`KernelActionDefinition::impulse_collision_mask` 改 reserved（版面不變）；執行器（[systems.cc:652](../engine/src/simulation/src/systems.cc)）改成只看 kind 能力：actor，或 world mode 且非 carrying 的 prop。若 P0 量到 tent_collapse 的 `player_side` 會選到 prop，就把該 prop 的 resistance 調高，不加回 mask。
5. resistance 改名（D2a）：`KernelEntityTemplateDefinition::impulse_resistance`、`ImpulseResistance` component、loader、12 個 catalog YAML、相關註解；版面不變，名稱改動併入 105。
6. 同步三個寫死 ABI／schema 版本的測試（見 memory：schema-bump-touch-points），以及直接設 `impulse_collision_mask` 的 kernel／simulation 測試（約 6 處）。
7. 測試：rocket_explosion 遠近兩點傷害不同（falloff 生效）；無傷害 push／pull 不扣血；舊 mask 與舊 resistance 鍵被拒；carrying 中的 prop 不被推。

### P5 參數化合併（依 P3／P4）

| 合併 | 目標 | 每個持有者 binding 提供 |
|---|---|---|
| rocket_explosion／shockwave／melee_impact | `common/action_apply_damage_and_impulse` | amount=`event.damage`、strength pair、stagger、lockout |
| tornado_pull | `common/action_apply_pull` | pull 四數值 |
| pull_at_target | `common/action_apply_damage_and_pull` | amount、stagger、pull 四數值 |
| tent_collapse | `common/action_apply_impulse` | strength pair、lockout |
| frag | `common/action_apply_damage` | amount、`stagger: 0` |
| 三個 open UI | `common/action_open_ui` | `ui_id` |
| spawn ice／building＋傷害自身 | `common/action_spawn_entity_and_damage_self`（名稱實作時依行為定） | placement 等 |

其餘真正專屬的流程（meteor_storm、portal、water_bubble、encase、heal_and_consume 等）移到 `derived/<owner_type>/action_<owner_name>_<event>.yaml`，ID 隨之改名。
每項合併前後各跑該持有者的代表性測試，確認觸發次數、順序、來源不變。

### P6 漂移修正與文件

1. tornado（D3）：數值不動，只改註解。collider 39 的 3 m → 6 m；projectile 29 與 graph 的「once a second／30 ticks」→ 15 ticks（0.5 s）；改寫「飛完才會被下一次拉」那句，明講 airtime 18 > interval 15、飛行中可被重拉是預期效果；更新「(3 - 1) / 0.6」的速度推算。
2. 更新 `ACTION_GRAPH_AUTHORING_AND_RUNTIME_GUIDE.md`：目錄規則、binding 能力、傷害契約、mask 移除；只寫已實作的格式。
3. D4 不在本批；只在 guide 的已知問題裡留一行指向後續批次。

## 3. 交付

- 分支 `claude/action-graph-refactor`，每個 phase 的 commit hash。
- ABI：P4 起 104 → 105（`impulse_collision_mask` 退為 reserved、新增 `event.damage` 來源、`impulse_resistance` 改名）；snapshot schema 不變。client 與 server 必須同版，C# mirror 由使用者更新。
- `bundle.bytes`：需要更新（catalog 內容與 hash 都變了），由使用者執行。
- 完成回報：讀取檔案、修改檔案、搜尋次數、build／test 命令。

## 4. 成本與停止條件

- smoke-test-first；只跑受影響的目標測試，不跑完整 build／test。
- 以下任一情況就停下回報，不自行擴大範圍：
  - P0 量到的行為與本計劃假設不同（例如 rocket 直擊確實有傷害）。
  - ABI 105 已被其他分支佔用。
  - 某合併會改變觸發次數或順序。
  - 需要碰 Unity 專案或 package。
