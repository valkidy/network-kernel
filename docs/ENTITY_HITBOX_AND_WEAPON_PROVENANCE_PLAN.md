# D4：entity 命中體積單一來源 + weapon_id 改走發射來源 — 實作計畫

狀態：D4a-1、D4a-2 已實作（`claude/d4a-entity-hit-collider`）；D4b 未實作。2026-10-13。
來源：`docs/ACTION_GRAPH_REFACTOR_PLAN.md` §0 D4（從 action graph refactor 拆出）。
ABI：action graph refactor（`claude/action-graph-refactor`）用 105；本批 D4a-2 與 D4b 共用 **106**，合併時再核對一次。

---

## 0. 已定方向（不重開）

- **D4a**：大型身形各自建 collider template；entity 只引用 `collider_template:`，移除 `hitbox:`；兩者同時出現時 loader 報錯。
- **D4a-2（Q2，2026-10-13 定）**：有 rig 的大型身形改兩層判定——collider template 是 bounds（broad phase），`rig.yaml` 的骨骼 collider 判定實際傷害（narrow phase）。
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

### 1.4 Rig collider 已經存在，但被大盒子遮住

5 個大型身形全部設了 `skeleton.collision_flags: {purpose: limb | hit, layer: hostile_side}`（只有這 5 個 entity 有），每個 tick 由 `skeleton_assets/raw/*.rig.yaml` 的 `GEO_*` 骨骼產生逐骨骼 collider：

| rig | collider 數 | 涵蓋 | `hit_zone` |
|---|---|---|---|
| quadruped（21） | 9 | 4 腿 × 2 段 + Body | 腿 0.5、Body 1.5 |
| tripod（22、211） | 7 | 3 腿 × 2 段 + Body（sphere） | 未設（1.0） |
| biped（23） | 12 | 腿、手臂、Hip、Torso、Body、Head | 未設 |
| gingerbread_giant（39） | 8 | 腿、Hip、Torso、Body、Head | 未設 |

它們已經在伺服器 Jolt 世界（`kActorLimb`）、rewind 歷史（`history_buffer.cc:143-175`，`is_limb`）與 client 預測代理體裡。但現在它們和大盒子是**並列目標**，不是兩層：

- **Rewind**：`raycast_history_frame` 取最近的 volume。大盒子包住所有骨骼，射線一定先進大盒子，所以 limb 的 `hit_zone` 在 rewind 路徑上幾乎不生效（由程式推導，未實測）。
- **即時路徑**：只有 mask 寫了 `limb` 的才看得到骨骼（`KERNEL_COLLISION_LAYER_LIMB` 刻意不在任何 MASK_ 聚合裡）。目前只有 `rifle_shot`、`rocket`、`water_bubble_spammer_bullet`，以及打 player_side 的 `grunt_claw_hit`／`grunt_slam_hit`。shotgun、grenade、frag、meteor、tornado、beam、homing 等打大型怪時只碰得到腳邊的小盒子。

### 1.5 已定方向（Q2，2026-10-13）：大型身形改為兩層判定

- **大盒子（entity 的 `collider_template:`）只當 bounds**：relevance、AI 瞄準點、homing／lock 目標中心、rewind 的 broad-phase 篩選、debug。**不再是傷害體積**。
- **Rig collider 判定實際傷害**（narrow phase），`hit_zone` 由命中的骨骼決定。
- **沒有 rig 的 actor**（grunt、gingerbread、玩家……）：collider 本身就是傷害體積，行為不變。

行為變化（相對於今天）：
- 17 個沒有 rig 的 template：零變化。
- 5 個大型身形：rewind 不再打中大盒子的空處（腿與腿之間、身體旁邊），只算骨骼；即時路徑的 area effect、melee、沒寫 `limb` 的武器從「只碰得到腳邊小盒子」變成「打得到整個身體的骨骼」；quadruped 的腿 0.5／Body 1.5 開始真正生效。
- Client 預測與 Unity debug 已經有 limb，命中位置與伺服器一致。

### 1.6 計算負擔（評估）

- **伺服器 Jolt**：limb 現在就每 tick 同步並進物理世界，這部分成本已在付。Jolt 自己有 broad phase（以 body AABB 建 BVH），所以**不需要也不應該**在 Jolt 查詢前用大盒子篩；bounds 根本不進 Jolt 的傷害查詢即可。新增成本 ≈ 0。
- **Rewind**：現在 `include_limbs` 的射線對每個 volume 都做 OBB 測試；改成「先測 bounds，中了才測該 entity 的骨骼」後，沒瞄準的射線每隻少測 7–12 個 OBB。成本持平或下降。
- **Client 預測**：limb 代理體已存在。2026-08 量過（client 端）每個 body 約 0.06 µs，64 隻 quadruped（576 body）約 34 µs，30 Hz tick 預算 33,333 µs 的約 0.1%。本次未重跑。
- **Area effect**：一顆球對 N 個骨骼盒而非 1 個盒子，可以忽略。
- 真正的成本在語意（mask、area effect 去重、`hit_zone` 規則），不在 CPU。

---

## 2. D4a-1 實作步驟：單一來源（不動 ABI）

不動 ABI：`KernelCombatStateDefinition::hitbox_*` 保留，改由 game_server 從 collider template 推導後填入。kernel 端不改。

1. **新增 4 個 collider template**（`game_server/gameplay_catalog/collider_templates/`，命名照 `$id_collider_$name`）——D4a-1 先用 `purpose: hit`，D4a-2 改為 `purpose: bounds` 並改名 `*_bounds_aabb`
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
   - 有 inline `hitbox:` 的測試 fixture 要改：`game_server/tests/{hover_controller,tent_shelter,gameplay_config,tent_feasibility_probe}_test.cc`、`engine/src/tests/kernel_tests/{client_mode,relevance}_test.cc`（實作時逐一確認是 YAML 還是註解）。

**D4a-1 結論：ABI 不變、snapshot／packet schema 不變；catalog hash 會變（collider 新增），`bundle.bytes` 需要重新產生（由使用者處理）。中間狀態不單獨交付，見 §2b。**

## 2b. D4a-2 實作步驟：大型身形兩層判定（ABI 106，與 D4b 共用）

D4a-1 之後，5 個大型身形的 collider 是 `purpose: hit` 的大盒子，即時路徑會開始拿大盒子算傷害。**這個中間狀態不交付**：D4a-1 可以先 commit，但要和 D4a-2 一起交給使用者打包。

1. **新增 collider purpose `bounds`**
   - `KernelColliderPurpose_Bounds = 1u << 6`（`kernel_types.h:2015-2024`，目前用到 bit 5 `_Limb`）。語意：entity 的外框，只給 relevance、瞄準點、rewind broad phase 與 debug 用，任何傷害查詢都看不到它。
   - YAML token `bounds`；46-49 四個 collider 改為 `purpose: bounds`（名稱改成 `*_bounds_aabb`）。
   - 新增 purpose bit 加上語意變化 → **ABI 106**（與 D4b 同一次 bump）。
2. **Loader 規則**（`gameplay_config.cc`，actor 解析與 `actor_collider_template_id_from_yaml`）
   - `skeleton.collision_flags.purpose` 含 `hit` ⇒ `collider_template:` 必須是 `bounds` + `aabb`。
   - 沒有 rig hit ⇒ 必須是 `hit` + `aabb`（D4a-1 的規則）。
   - 不符就報錯，訊息寫出是哪一種情況。
3. **Kernel：bounds 不進傷害世界**
   - `materialize_entity_collider`（`kernel.cc:4789`）：bounds 照樣建 `ColliderInstance`（debug／`Kernel_QueryColliderShapes` 看得到），並在 `Hitbox` 標記 `bounds_only = true`（`components.h:609` 新增欄位）。
   - `push_collider_into_physics`（`:5072`）：purpose 只有 bounds 的不推進 Jolt。
   - Standalone collision world（`world.cc:279`）：跳過 `bounds_only` 的 `Hitbox`，改推該 entity 的 limb（或在 standalone 模式下直接不支援 rig 傷害——實作時看哪個測試依賴它再決定）。
   - Client 的 `sync_client_render_colliders`／`sync_prediction_local_hitbox` 只收 `_Hit`，bounds 自動排除；limb 代理體已存在，不用改。
4. **Rewind：先篩後判**（`history_buffer.cc`）
   - `HitVolumeSnapshot` 加 `is_bounds`（由 `Hitbox.bounds_only` 帶入）。
   - `raycast_history_frame`／`sweep_history_frame`：bounds volume 永遠不是命中結果；射線沒碰到某 entity 的 bounds，就跳過該 entity 的所有 limb；碰到了才測它的 limb，取最近的。
   - **前提：bounds 必須包住所有 limb。** 腿在跨步、地形起伏時可能伸出 bounds，伸出的部分會被篩掉而打不中。實作時加一個 debug 檢查（limb 的 world AABB 超出 bounds 就記 log），並在 locomotion 測試裡斷言走一段路、跨越高低差時沒有超出；若有，放大 46-49 的尺寸，而不是取消篩選。
   - 有 rig 的 entity，limb 一律參與（不看 `include_limbs`）；`include_limbs` 只剩對「沒有 bounds 的 limb」有意義——目前 catalog 沒有這種 entity，見 Q3。
5. **即時路徑的 mask 語意**（`kernel.cc:1879/1994/3667/12643` 一帶的 mask→filter 轉換、`physics_world.cc:140-160`、`weapon_system.cc:112`、`projectile_system.cc:1852`）
   - 對有 bounds 的 rigged actor：任何會打到該 actor 的 mask（`actor`／`damageable`／該 actor 的 side）都要打到它的 limb，category 仍以 limb 的 `layer`（`collision_flags.layer`）比對。
   - 這推翻了「limb 必須明寫才打得到」的舊規則，但只對 rigged actor；沒有 rig 的世界不受影響。
   - 實作重點：不能把 limb 塞進 `actor_hit_*_mask()` 共用 helper（舊坑：會把 limb 發給所有沒要的查詢），而是在 filter 建立時針對 `kActorLimb` kind 另開條件。
6. **Area effect／melee 去重與 `hit_zone`**（Q4 已定）
   - Area effect：已以 net_id 去重（`area_effect_system.cc:154-161`），不用改；damage request 改為明確傳 `KERNEL_HIT_ZONE_UNSCALED`。
   - Melee：實作前先讀 melee 路徑，確認以 net_id 去重（若以 collider，一次揮擊碰到 3 根骨骼會扣 3 次血，必須改）；同一 entity 被碰到的骨骼取最大 `hit_zone`。
   - 測試補一條：quadruped 被 area effect 同時碰到腿（0.5）與 Body（1.5）→ 傷害 × 1.0；melee 同時碰到 → × 1.5。
7. **測試**
   - Rewind：射線穿過 quadruped 腿與腿之間（在 bounds 內、不碰骨骼）→ miss；修改前同一條射線 → hit（先確認修改前確實 hit）。
   - Rewind：射中 quadruped 腿 → 傷害 × 0.5；射中 Body → × 1.5。
   - 即時：grenade（mask 沒寫 `limb`）在 giant 腰部爆炸 → 命中一次，只扣一次血。
   - 即時：同一把武器打一個沒有 rig 的 grunt → 行為與修改前完全相同（regression guard）。
   - Loader：rigged actor 用 `hit` collider → 報錯；非 rigged 用 `bounds` → 報錯。
   - 既有 `//game_server:limb_collider_test`、`//engine/src/tests/sync_tests:history_buffer_test` 必須仍綠；若其斷言依賴「limb 必須明寫」，逐一確認是預期改變再改。
   - 注意 memory「test static scene needs catalog ids」：沒有地面時 limb 不會被發佈，limb 相關斷言會兩邊都空而假綠；每個 limb 測試先斷言 limb 數量 > 0。

**D4a-2 結論：ABI 106（新 purpose bit + mask 語意），與 D4b 同一次 bump；snapshot／packet schema 不變（client 本地推導 limb）；catalog hash 變。**

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
- **Q2（D4a）— 已定 2026-10-13：兩層判定。** 大盒子只當 bounds，rig collider 判定傷害（§1.5、§2b）。原本「大盒子直接當傷害體積」的方案不採用：它比身體大很多，空處也會算命中。
- **Q3（D4a-2）— 已定 2026-10-13：採 (a)。** `limb` token 保留，但只作用於「沒有 bounds 的 limb」（目前 catalog 沒有這種 entity）；從 `rifle_shot`、`rocket`、`water_bubble_spammer_bullet`、`grunt_claw_hit`、`grunt_slam_hit` 移除它；loader 不報錯。
  - 不採 (b)：直接移除 token、loader 拒絕。
- **Q4（D4a-2）— 已定 2026-10-13：採 (a)。** area effect 不套 `hit_zone`（爆炸不分部位）；melee 取被碰到骨骼中最大的 `hit_zone`。
  - 核對結果（靜態）：area effect **其實有**傳 `hit_zone`——`area_effect_system.cc:154-161` 以 net_id 去重、保留查詢結果裡該 entity 的**第一個** hit，`damage_request_from_hit` 帶上它的 `hit.identity.hit_zone`（`damage_pipeline.cc:125`）。但整個 catalog 只有 `simplified_quadruped.rig.yaml` 設了 `hit_zone`，而 area effect 的 mask 都沒寫 `limb`，所以今天實際上永遠是 1.0。
  - 因此實作方式是：area effect 建 damage request 時明確傳 `KERNEL_HIT_ZONE_UNSCALED`。今天零行為變化，也避免兩層判定後「哪根骨骼先回傳就用誰的倍率」這種依查詢順序決定的結果。
  - 去重已經是以 net_id 為準，§2b 第 6 步的 area effect 部分不用改；melee 路徑仍要在實作前確認。
  - 不採 (b) 一律取最大、(c) 取最近。

---

## 6. 版本與交付

| 項目 | D4a-1 | D4a-2 | D4b |
|---|---|---|---|
| ABI | 不變 | **106**（`KernelColliderPurpose_Bounds`、rigged actor 的 mask 語意） | **106**（`KernelProjectileTemplateDefinition.weapon_id` → reserved、驗證為 0） |
| Snapshot schema（30） | 不變 | 不變 | 不變 |
| Packet schema（32） | 不變 | 不變 | 不變（Q1 採 (a)） |
| catalog hash／`bundle.bytes` | 變，需重產 | 變，需重產 | 變，需重產 |
| C# mirror | 不需 | 新 purpose 常數（使用者處理） | 欄位改名（使用者處理） |

ABI 106 只 bump 一次，D4a-2 與 D4b 共用，接在 refactor 的 105 之後。

建議順序：D4a-1 → D4a-2 → D4b，三個 commit 放在同一個 `claude/*` 分支，**一起交付**（D4a-1 單獨交付會出現「大盒子當傷害體積」的中間狀態）。Q1-Q4 皆已定，沒有待決事項。分支從 `main` 開（等 `claude/action-graph-refactor` 進 main 之後再開，避免 ABI 105/106 與 `hash_projectile_template` 衝突）。不 merge 到 `feat-unity-plugin`／`dev-*`，不碰 Unity package。

驗證只跑 targeted tests：loader test、gameplay_config_test、§2b 與 §4 新增的 kernel smoke test、`limb_collider_test`、`history_buffer_test`、`projectile_interaction_test`、ABI 版本 3 個測試。不跑 full build/test。
