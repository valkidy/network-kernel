# D4：entity 命中體積單一來源 + weapon_id 改走發射來源 — 實作計畫

狀態：計畫（尚未實作）。2026-10-13。
來源：`docs/ACTION_GRAPH_REFACTOR_PLAN.md` §0 D4（從 action graph refactor 拆出）。
ABI：action graph refactor（`claude/action-graph-refactor`）用 105；本批若需 bump 用 **106**，合併時再核對一次。

---

## 0. 已定方向（不重開）

- **D4a**：大型身形各自建 collider template；entity 只引用 `collider_template:`，移除 `hitbox:`；兩者同時出現時 loader 報錯。
- **D4b**：projectile 的 `weapon_id` 從發射當下的事件來源（provenance）帶入，不寫回 projectile template。

---

## 1. D4a 現況核對（靜態閱讀）

### 1.1 Runtime 實際有兩份幾何，各自被不同系統使用

| 消費端 | 用哪份幾何 | 位置 |
|---|---|---|
| 伺服器 Jolt 即時碰撞世界（非 rewind 的 projectile sweep、area effect overlap、melee/lock 錐形、碰撞 trigger） | **collider template**（`ColliderInstance`，`materialize_entity_collider` 建立） | `engine/src/kernel/src/kernel.cc:4789`、`push_collider_into_physics` `:5072` |
| Rewind 命中歷史（hitscan `raycast_history_frame`、projectile `sweep_history_frame`） | **`hitbox:`**（`Hitbox` component） | `engine/src/sync/src/history_buffer.cc:115-125` |
| Standalone collision world（沒有 Jolt 時的 fallback） | **`hitbox:`** | `engine/src/world/src/world.cc:279-299` |
| Relevance／距離分帶的 `entity_bounding_radius` | **`hitbox:`** | `kernel.cc:14734` |
| AI 瞄準點 `server_get_entity_aim_point`、homing／lock 目標中心、rewind 槍口偏移 | **`hitbox.center`** | `kernel.cc:7211`、`projectile_system.cc:232/278/321`、`weapon_system.cc:195/438` |
| Client 預測用的其他 actor 代理體、本地玩家 hitbox | **collider template**（以複製下來的 `collider_template_id` 查表） | `kernel.cc:5600-5640`（`sync_client_render_colliders`）、`kernel.cc:12389`（`sync_prediction_local_hitbox`） |
| Unity `NetworkDebugView` | **collider template**（畫的是 kernel 回傳的 collider shape view） | `unity-network-example/Assets/Scripts/Rendering/NetworkDebugView.cs:274/498`（只讀，不改） |

`hitbox:` 的資料路徑：YAML → `ActorTemplateConfig::hitbox_*`（`gameplay_config.cc:4258-4272`）→ `make_combat_state_from_actor_template`（`:11620`）→ `KernelCombatStateDefinition::hitbox_*`（ABI，`kernel_types.h:2803`）→ `Kernel_SetEntityCombatState` 寫進 `Hitbox`（`kernel.cc:6693-6696`）。
Prop 已經是單一來源：它不寫 `hitbox:`，`materialize_entity_collider` 在 extents 為 0 時從 collider 補上（`kernel.cc:4816-4825`，條件是 `kind.type != kActor`）。

### 1.2 不一致的 5 個 template（其餘 17 個 `hitbox:` 與 collider 完全相同）

| entity | collider（現在） | `hitbox:` center / half_extents |
|---|---|---|
| 21 quadruped_actor | sentry_grunt_hit_aabb（0.8 / 0.4,0.8,0.4） | y 12 / 12, 14, 12 |
| 22 tripod_actor | 同上 | y 12 / 10, 14, 10 |
| 211 tripod_chaser | 同上 | y 12 / 10, 14, 10 |
| 23 biped_actor | 同上 | y 20 / 12, 22, 8 |
| 39 gingerbread_giant | 同上 | y 9 / 8, 10.7, 3 |

（`27_allied_beam_sentry.yaml` 也有 `hitbox:`，與 `allied_sentry_hit_aabb` 相同，共 22 個。）

### 1.3 實際後果（今天就存在的 bug）

對這 5 個大型身形：
- **rewind 命中**（玩家 hitscan、前段 projectile）打的是 24 m 寬的大盒子；
- **即時命中**（area effect、melee、lock、rewind 窗口之後的 projectile）打的是腳邊 0.8 m 的小盒子；
- client 的預測代理體和 Unity debug 畫的都是小盒子，所以 debug 畫面看不到 rewind 實際打的範圍。
同一顆子彈在 rewind 窗口內外會打到不同大小的目標。

### 1.4 改成單一來源後的行為變化（要讓使用者知道）

以「大盒子」為準（數值沿用現在的 `hitbox:`）：
- 17 個一致的 template：**零行為變化**。
- 5 個大型身形：rewind／relevance／瞄準點不變；**即時碰撞世界的命中體積從小盒子變大盒子**，area effect、melee、lock、client 預測與 Unity debug 都會開始打到／畫出大盒子。這是修正，但會改變手感（例如 grenade 炸在 giant 腰部現在會中）。
- 需要確認的風險：大盒子是 `kDamageable` layer，確認視線（vision LOS）與移動查詢不會被它擋住（`push_collider_into_physics` 把它歸為 `kActorHitbox`，只有 damage 查詢會看到；實作時用 smoke test 驗證，不靠推論）。

---

## 2. D4a 實作步驟

不動 ABI：`KernelCombatStateDefinition::hitbox_*` 保留，改由 game_server 從 collider template 推導後填入。kernel 端不改。

1. **新增 4 個 collider template**（`game_server/gameplay_catalog/collider_templates/`，命名照 `$id_collider_$name`）
   - `46_collider_quadruped_hit_aabb.yaml`：center y 12、half 12/14/12、`purpose: hit`、`layer: hostile_side`
   - `47_collider_tripod_hit_aabb.yaml`：center y 12、half 10/14/10（22 與 211 共用）
   - `48_collider_biped_hit_aabb.yaml`：center y 20、half 12/22/8
   - `49_collider_gingerbread_giant_hit_aabb.yaml`：center y 9、half 8/10.7/3
   - id 46-49：目前 main 與本機所有 `claude/*` 分支最大為 45。**合併前再檢查一次**（見 memory「parallel branches collide on numbers」）。
2. **5 個 entity 改引用新 collider**：21→quadruped、22/211→tripod、23→biped、39→gingerbread_giant。
3. **Loader（`game_server/src/gameplay_config.cc`）**
   - `parse actor template`（`:4258`）：`hitbox:` 存在就 throw：`"actor template sets hitbox; its hit volume is collider_template: <name>"`。從 allowed keys（`:4046`）移除 `hitbox` 也行，但明確訊息比 unknown key 好，兩者擇一（建議保留在 allowed keys，然後用明確訊息擋）。
   - `actor_collider_template_id_from_yaml`（`:3574`）：額外要求 collider 為 `purpose: hit` 且 `shape: aabb`（`Hitbox` 只描述 AABB；現在沒有檢查）。
   - 新增 helper：從 collider template 填 `actor_template.hitbox_center/half_extents`，在 collider 解析之後呼叫。
   - 內建預設 actor（`:3269`、`:3292`）：改從其 collider 推導，或保留常數並加 assert 與 collider 相同（建議推導）。
   - `hash_actor_template`（`:345`）：保留（值由 collider 推導，仍會反映在 hash）。
4. **22 個 entity YAML 移除 `hitbox:` 區塊**。
5. **文件**：`docs/DATA_DRIVEN_TEMPLATE_DESIGN.md` 的 `hitbox:` 範例改掉；在 `ACTION_GRAPH_AUTHORING_AND_RUNTIME_GUIDE.md` 已知問題那行（refactor 批次加的）指向本文件。
6. **測試**
   - 新 loader test：同時寫 `hitbox:` 與 `collider_template:` → 報錯；collider 不是 hit/aabb → 報錯。
   - 黃金比對：以本批修改前的 22 組 `hitbox:` 數值為表，斷言推導出的 `hitbox_center/half_extents` 逐一相等（17 個沒換 collider 的、5 個換了的都要）。這是本批最重要的 regression guard。
   - 一個 kernel smoke test：spawn 一個 giant，在其 y≈9 處放 area effect，斷言即時路徑有命中（修改前是 miss）——要先確認修改前確實 fail，避免 vacuous assert。
   - 有 inline `hitbox:` 的測試 fixture 要改：`game_server/tests/{hover_controller,tent_shelter,gameplay_config,tent_feasibility_probe}_test.cc`、`engine/src/tests/kernel_tests/{client_mode,relevance}_test.cc`（實作時逐一確認是 YAML 還是註解）。

**D4a 結論：ABI 不變、snapshot／packet schema 不變；catalog hash 會變（collider 新增），所以 `bundle.bytes` 需要重新產生（由使用者處理）。**

---

## 3. D4b 現況核對

### 3.1 寫入點

`apply_weapon_template_references` 對**每一種** weapon type 都把 `weapon_id` 寫回 projectile template，不只 hitscan/melee：
`gameplay_config.cc:8240`（hitscan/shotgun）、`:8271`（melee）、`:8293`（lock_instant/lock_missile）、`:8362`（targeted_strike）、`:8389`（projectile/area_effect/beam）。

### 3.2 兩把武器共用一個 projectile template 會互相覆寫嗎？

- **會**：後載入的武器覆蓋前者，loader 不擋也不警告。
- **今天沒有發生**：20 把武器的 `projectile_template:` 全部不同（已逐一列出確認）。
- 但有**一個現存的錯誤歸屬**：`item_templates/3001_fungible_grenade_consumable.yaml` 用 action graph 生成 `grenade_shell`，而 `grenade_shell` 被 weapon 7（grenade launcher）蓋了章，所以**丟手榴彈道具的命中與擊殺被記成 weapon 7**。

### 3.3 讀取端

| 讀取端 | 現在讀什麼 | 位置 |
|---|---|---|
| `spawn_projectile_from_template`（action graph 生成路徑） | `template.weapon_id != 0` 就**蓋過** provenance 的 `source_weapon_id` | `projectile_system.cc:59-61` |
| 武器直接發射路徑 | 直接用 `definition.id`，**不讀 template** | `weapon_system.cc:888` |
| Targeted strike 落點 | 傳 `definition->id`（provenance） | `weapon_system.cc:1404` |
| `ProjectileImpactPayload.source_weapon_id`、area effect 觸發 provenance | `ProjectileState.weapon_id` | `area_effect_system.cc:223/242`、`projectile_system.cc:750/774` |
| 傷害請求 → `ConfirmedDamage.source_code` → 擊殺歸屬 → destroy graph provenance | `ProjectileState.weapon_id` | `projectile_system.cc:956/1002/1881`、`systems.cc:3186-3197/3334` |
| Projectile interaction rule（以 weapon_id 配對） | `ProjectileState.weapon_id` | `projectile_system.cc:1022-1034`；**正式 catalog 沒有任何規則**，只有測試用 |
| Benchmark stats 的 sync_mode 統計 | **反查**：找 `template.weapon_id == projectile.weapon_id` | `kernel.cc:4489`（已有 bug：graph 生成的子彈繼承了發射武器的 id，會查到發射武器那顆的 sync_mode） |
| Client 綁定 spawn record | `projectile_template->weapon_id` 寫進 `PredictedProjectile.weapon_id`（**之後沒人讀**） | `kernel.cc:8729/8763` |
| Client projectile debug record | `projectile_template->weapon_id` | `kernel.cc:8832` |
| Client `DerivedChainPresenter`（重播 targeted strike 衍生鏈） | `root_template->weapon_id` 當 `spawn_projectile_at` 的 weapon 參數 | `derived_chain_presenter.cc:91`；衍生選擇的 seed 不含 weapon_id，所以只影響影子世界的 provenance，不影響畫面 |
| Unity | 不讀 projectile 的 weapon_id | 已 grep `unity-network-example/Assets` |

`ProjectileSpawnRecord`（`network_packets.h:51`）**沒有** weapon_id；client 目前只能從 template 推。

### 3.4 Server 端 provenance 已經是完整的

武器發射、targeted strike、area effect、destroy graph 都已經把 weapon_id 一路帶在 `ProjectileState.weapon_id` / `ActionExecutionProvenance.source_weapon_id` 裡。template 上的值只扮演「覆蓋」與「反查」兩個角色，兩個都是錯的來源。

---

## 4. D4b 實作步驟

1. **Loader**：刪掉 5 處 `->definition.weapon_id = weapon_id`；`hash_projectile_template`（`:235`）移除 `weapon_id`。
2. **共用 template**：拆掉蓋章後，兩把武器共用同一個 projectile template 就不再衝突，loader 不需要擋。在 `WEAPON_AUTHORING_GUIDE.md` 寫明「projectile template 可被多把武器共用」。
3. **`spawn_projectile_from_template`**：直接用 `source_weapon_id`，刪掉 template 覆蓋。
4. **`RuntimeProjectileTemplate::weapon_id`**：移除（`kernel.cc:1542` 的轉換一併移除）。
5. **Benchmark sync_mode 統計（`kernel.cc:4480-4492`）**：改以 `projectile.projectile_template_id` 查 template。
6. **Client**
   - `PredictedProjectile.weapon_id`：write-only，直接刪。
   - `DerivedChainPresenter`：傳 0（或在 `DerivedChainRoot` 加 weapon_id 但沒有來源，不做）。
   - Projectile debug record 的 `weapon_id`（§5 Q1 已定 (a)）：client 端填 255（`KERNEL_HELD_WEAPON_NONE`，與 `KERNEL_DEBUG_WILDCARD_U8` 同值，任何武器都不得使用）表示「未知」，不改線上格式。**不能填 0**：0 是 rifle。副作用：client 上以特定 weapon_id 過濾 projectile debug record 不再命中任何紀錄，在 `KernelProjectileDebugInfo` 註解寫明；server 端 debug record 不受影響（仍讀 `ProjectileState.weapon_id`）。
7. **ABI 欄位 `KernelProjectileTemplateDefinition::weapon_id`（`kernel_types.h:2224`）**：改名為 `reserved_weapon_id`（保持 layout），kernel 驗證要求為 0。語意與驗證都變了，舊 `bundle.bytes` 會被拒 → **bump ABI 106**，`NETWORK_KERNEL_ABI.md` 加一段說明。C# mirror 的對應改名是使用者的事。
8. **測試**
   - 改寫目前斷言 template weapon_id 的測試（`weapon_template_loader_test`、`gameplay_config_test`、`lock_weapon_catalog_test`、`tornado_catalog_test`、`catalog_weapon_definition_test` 等；實作時逐一確認，只改真的讀 projectile template weapon_id 的斷言）。
   - 新增：道具手榴彈（item 3001）命中 → `source_code` 不再是 7（修改前先跑一次確認是 7）。
   - 新增：兩把測試武器共用一個 projectile template，各自發射後 `ProjectileState.weapon_id` 各為自己的 id。
   - 既有 `projectile_interaction_test` 必須仍綠（它直接設 `ProjectileState.weapon_id`，應不受影響）。
   - 依 memory「schema bump touch points」更新寫死 ABI 版本的 3 個測試。

**行為變化**：只有「action graph 生成、但 template 曾被某把武器蓋章」的子彈會改變 weapon_id。今天的 catalog 只有 `grenade_shell`（道具手榴彈）這一例；其他 graph 生成的 template（`rocket_explosion`、`meteor_*`、`frag_blast`、`tornado`……）本來就沒被蓋章，不變。

---

## 5. 需要使用者決定

- **Q1（D4b）— 已定 2026-10-13：採 (a)。** client debug record 的 projectile `weapon_id` 填「未知」（255），不改線上格式，packet schema 不動。目前沒有任何遊戲邏輯或 Unity 程式讀它。
  - 不採 (b)：在 `ProjectileSpawnRecord` 加 1 byte weapon_id 會讓 packet schema 32→33，每筆 spawn record 多 1 byte，只為了 debug。
- **Q2（D4a）**：大型身形改成大盒子後，即時命中（area effect／melee／lock）會開始打中原本打不到的位置。這是預期的修正，但會改手感；若要保留小盒子給某些系統，就違反「單一來源」，需要另外討論。

---

## 6. 版本與交付

| 項目 | D4a | D4b |
|---|---|---|
| ABI | 不變 | **106**（`KernelProjectileTemplateDefinition.weapon_id` → reserved、驗證為 0）；接在 refactor 的 105 之後 |
| Snapshot schema（30） | 不變 | 不變 |
| Packet schema（32） | 不變 | 不變（Q1 採 (a)） |
| catalog hash／`bundle.bytes` | 變，需重產 | 變，需重產 |
| C# mirror | 不需 | 欄位改名（使用者處理） |

建議順序：D4a、D4b 分兩個 commit 放在同一個 `claude/*` 分支；D4a 不碰 ABI，可以先單獨交。分支從 `main` 開（等 `claude/action-graph-refactor` 進 main 之後再開，避免 ABI 105/106 與 `hash_projectile_template` 衝突）。不 merge 到 `feat-unity-plugin`／`dev-*`，不碰 Unity package。

驗證只跑 targeted tests：loader test、gameplay_config_test、新增的兩個 kernel smoke test、projectile_interaction_test、ABI 版本 3 個測試。不跑 full build/test。
