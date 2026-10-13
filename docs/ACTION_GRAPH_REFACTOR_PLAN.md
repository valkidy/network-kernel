# Action Graph 重構實作計劃

狀態：P0–P6 已實作於 `claude/action-graph-refactor`（2026-10-13）。計劃建立日期：2026-10-12。基準：`main` ee8531ec（ABI 104）。
預定執行：2026-10-13 07:01 起，分支 `claude/action-graph-refactor`（從 `main` 開，不從 HEAD）。

本計劃依「Action Graph 重構需求」（2026-10-12）擬定，並以 2026-10-12 對 `main`
的靜態核對結果為事實基礎（§1）。merge 進 `feat-unity-plugin`／`dev-*`、Unity
package 打包與 `bundle.bytes` 更新、C# ABI mirror 都由使用者處理，不在本計劃範圍。

## 0. 決策紀錄

| # | 決策 | 結論（2026-10-13） |
|---|---|---|
| D1 | 傷害與 graph 的契約 | **修訂（2026-10-13，P0 後）：一律都算。** 命中的普通傷害（含 falloff）一律由 projectile `damage` 經傷害管線提交，不論有沒有綁 graph；graph 只負責附加效果（推、拉、狀態、生成）。stagger 移到 projectile 的選填欄位 `stagger`（未寫＝依傷害衍生，0＝不 stagger），經 `DamageRequest::stagger` 帶入。不新增 `event.damage`。原 D1（event.damage）作廢。 |
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

## 1a. P0 結果（導致 D1 修訂）（2026-10-13，基準 main 656f90bd）

**現況的傷害／graph 契約依命中路徑而不同**（推翻 §1 的「rocket 直擊可能無傷害」假設）：

| 路徑 | 綁了 impact graph 時 | 位置 |
|---|---|---|
| standard projectile，`direct_hit` | **附加**：照常提交 `damage`，graph 也執行。rocket 直擊 45＋爆炸 | [projectile_system.cc:989](../engine/src/simulation/src/projectile_system.cc) |
| standard projectile，`piercing_segment` | actor 命中只提交傷害；graph 只在打到非 actor 時觸發 | projectile_system.cc:922 |
| melee（instant shot） | **替代**：不提交傷害，交給 graph | [weapon_system.cc:712](../engine/src/simulation/src/weapon_system.cc) |
| area effect | **替代**：不提交傷害；falloff 算了但沒傳出 | area_effect_system.cc:190 |

tent_collapse：area effect 本身只在 mask 含 PROP 位元時才選 prop（並排除非 world mode／carrying），`player_side` 不含 PROP，所以 graph 上的 `collision_mask: actor` 是多餘的，移除不改變行為。

**基準失敗集合**（這些測試在第一個失敗就中止，後面的檢查被遮住）：

| 目標 | 失敗點 |
|---|---|
| `//game_server:gameplay_config_test` | 崩潰：`unknown status effect: water_bubble` |
| `//game_server:activation_gameplay_config_test` | line 86 `collision_trigger_mask == KERNEL_COLLISION_MASK_ACTOR` |
| `//game_server:weapon_template_loader_test` | line 1038 `area.radius == 3.0f`（ee8531ec 把 tornado 改成 6 後過時） |
| `//engine/src/tests/simulation_tests:combat_test` | 編譯失敗，combat_test.cc:1525 參數個數 |

其餘 11 個目標通過：client_mode、entity_trigger_action_parity、status_effect_catalog、apply_pull、area_effect、impulse_lockout、impulse_strength、melee、projectile_interaction、status_strength、portal。

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
2. 把下列 action 欄位改為可接受 `params.<name>`：impulse `lockout_ticks`；pull `distance`／`airtime_ticks`／`max_speed`／`strength`；open_ui `ui_id`；spawn `placement`。
3. 型別、有限值與範圍檢查沿用各欄位現有的 validator（例如 pull 的 range check、lockout ≤ `KERNEL_MAX_IMPULSE_LOCKOUT_TICKS`）。
5. graph `parameters` 的 default 僅作為持有者未提供時的值；持有者有寫就以持有者為準（不新增 preset 檔）。
5. 測試：同一 graph 兩個持有者給不同數值；pair 覆寫保留 vertical；非法值拒絕。

### P4 傷害契約與 mask 移除（ABI 105，依 D1／D2）

1. 依修訂後 D1：移除 area effect（area_effect_system.cc:190 起）與 melee（weapon_system.cc:712）的「綁 graph 就不提交傷害」分支；兩者都照常提交管線傷害（area effect 帶 falloff），graph 另外執行。standard direct_hit 已是此行為；piercing 維持現狀（graph 只在非 actor 命中觸發）。
2. projectile 新增選填 `stagger`（ABI：projectile 定義新增欄位＋「是否指定」），一路帶到 `DamageRequest::stagger`；melee 從 shot template 取得。未寫＝`kStaggerDerivedFromDamage`，0＝不 stagger。
3. 允許 `damage: 0` 表示無傷害（管線不提交）；tent_collapse_blast／tornado 改 0。
4. 確認同 tick 內「傷害致死」與「推／拉」的先後與改前一致（改前兩者同在一個 graph batch）；不一致就停下回報。
5. 移除 graph `collision_mask`（D2）：authoring 寫了就**報錯**，錯誤訊息指出改用 projectile mask＋目標 resistance；`KernelActionDefinition::impulse_collision_mask` 改 reserved（版面不變）；執行器（[systems.cc:652](../engine/src/simulation/src/systems.cc)）改成只看 kind 能力：actor，或 world mode 且非 carrying 的 prop。P0 已確認 tent_collapse 不會選到 prop，直接移除。
6. resistance 改名（D2a）：`KernelEntityTemplateDefinition::impulse_resistance`、`ImpulseResistance` component、loader、12 個 catalog YAML、相關註解；版面不變，名稱改動併入 105。
7. 同步三個寫死 ABI／schema 版本的測試（見 memory：schema-bump-touch-points），以及直接設 `impulse_collision_mask` 的 kernel／simulation 測試（約 6 處）。
8. 測試（放新 target）：rocket_explosion 綁 graph 時遠近兩點傷害不同（falloff 生效）且仍被推；melee 綁 graph 時傷害 40＋stagger 120；`damage: 0` 不扣血；stagger 未寫 vs 0；舊 mask 與舊 resistance 鍵被拒；carrying 中的 prop 不被推。

### P5 參數化合併（依 P3／P4）

| 合併 | 目標 | 每個持有者 binding 提供 |
|---|---|---|
| rocket_explosion／shockwave／melee_impact／tent_collapse | `common/action_apply_impulse` | strength pair、lockout；傷害與 stagger 改由 projectile（shockwave 150、grunt_slam 120） |
| tornado_pull／pull_at_target | `common/action_apply_pull` | pull 四數值；pull_blast 的 10 傷害與 stagger 150 改由 projectile |
| frag | 不再需要 graph | projectile `damage: 1000`、`stagger: 0`，falloff 維持 none |
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
- ABI：P4 起 104 → 105（`impulse_collision_mask` 退為 reserved、projectile 新增 `stagger`、`impulse_resistance` 改名）；snapshot schema 不變。client 與 server 必須同版，C# mirror 由使用者更新。
- `bundle.bytes`：需要更新（catalog 內容與 hash 都變了），由使用者執行。
- 完成回報：讀取檔案、修改檔案、搜尋次數、build／test 命令。

## 4. 成本與停止條件

- smoke-test-first；只跑受影響的目標測試，不跑完整 build／test。
- 以下任一情況就停下回報，不自行擴大範圍：
  - 實作中量到的行為與本計劃假設不同。
  - ABI 105 已被其他分支佔用。
  - 某合併會改變觸發次數或順序。
  - 需要碰 Unity 專案或 package。
