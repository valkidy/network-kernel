# 以玩家為目標的巡邏隊 實作計劃書

需求：巡邏隊刻意出現在玩家附近，以穩定的頻率製造交戰機會。多名玩家時，先照顧人多且聚在一起的群，再照顧落單的玩家，而且落單的玩家不能因為預算被別人用完而沒有威脅。

狀態：**計劃，尚未實作。** 設計決定於 2026-11-03 與使用者確認（§2）。數字是草案（§9），實作前要確認。P0 已完成（§10），生成環帶由 25–45 m 修訂為 50–60 m。基準：main 40f1ad57。

本計劃只改 `game_server`（PatrolDirector、catalog 解析與驗證），**不動 kernel ABI，不動 snapshot / packet schema**，`bundle.bytes` 只有在 catalog 內容變更時才需要重新產生。

---

## 1. 現況（main 40f1ad57）

- 巡邏的生成位置用的是世界座標。`1_patrol_gingerbread_warband.yaml` 的 `area` 是以原點為中心的 ±40 m 方框，`draw_route`（`patrol_director.cc`）每次在方框內抽起點和終點，最多試 `route_attempts` 次。
- `docs/AI_PATROL_SYSTEM.md` 的前提是「巡邏是世界自己的事件，玩家可以避開」。WHERE 只有 `rect` / `circle` 兩種，「relative to players」列為還沒做。
- 預算：每個定義有 `max_live_groups`；全服有 `patrol_budget.max_live_agents`（48）和 `agent_budget`（256）。檢查時一律用 `count_max` 計算，也就是「這個定義最多可能抽到幾隻」。
- 已量測到的問題：換成 composed 216 m 地圖後（navmesh 的 x,z 範圍是 0..216，不在原點附近），±40 m 的方框只有四分之一落在 navmesh 上，出現 `patrol found no route patrol=gingerbread_warband attempts=8`。以玩家為中心生成，可以避開「方框必須跟著地圖原點擺放」這個問題，但地圖邊界的處理仍然需要（§6）。

可以直接利用的現有條件：

- `sample_area(area, origin, state)` 本來就有 `origin` 參數，`draw_route` 目前傳入的是 `{0,0,0}`。
- `PatrolNavigation::find_route` 會檢查起點和終點是否連通，也會檢查繞路比例。可達性檢查不需要新的 API。
- `retire_finished_patrols` 已經依玩家距離讓隊伍退場（`despawn_distance_meters`），而且正在戰鬥的隊伍不會退場。

---

## 2. 已確認的設計決定

| # | 決定 |
|---|---|
| D1 | 巡邏隊**刻意**衝著玩家來。這推翻 `AI_PATROL_SYSTEM.md` 的「可避開」前提，文件要一起改。 |
| D2 | 路線採用 **A：穿越**。起點在目標周圍的一圈環帶上，終點在對側，路線經過目標在生成當下的位置附近。**不採用**攔截（也就是終點不是玩家）。路線在生成時固定，之後不追蹤玩家，維持一次性的設計。 |
| D3 | 玩家依距離分群。人多的群優先，落單的玩家排在後面。 |
| D4 | **隊伍大小依群的人數調整**（§5）。 |
| D5 | **落單玩家有保底頻率**（§4.3）。 |
| D6 | 生成距離有**最小值**，讓玩家可以先從遠處看到隊伍，再決定要繞開還是開打。 |
| D7 | 抽點範圍 = 環帶 ∩ 地圖的可生成範圍 ∩ navmesh。玩家在地圖邊上時剩約 180°，在角落剩約 90°（§6）。 |

---

## 3. 新的 WHERE：`around_players`

在定義裡新增一種生成位置的模式，跟現有的 `area` 二選一：

```yaml
where:
  mode: around_players
  ring: {min_meters: 50.0, max_meters: 60.0}
  # 路線離目標生成當下的位置多近才算「穿越」
  pass_within_meters: 15.0
  # 終點在對側方位上加多少隨機偏移
  exit_spread_degrees: 30.0
  # §4 的分群與分配參數，都只對 around_players 有意義，所以放在 where 底下
  group_radius_meters: 18.0
  groups_per_member: 1.0
  groups_per_player: 1.5
  floor_interval_ticks: 1800 # 0 = 不保底
```

- 現有的 `area` 定義行為完全不變。
- `where` 和 `area` 不能同時出現；`where` 也不能和 `max_live_groups` 同時出現（由 `groups_per_player` 取代），catalog 都會拒絕載入。
- 驗證規則（P1 實作）：`0 < ring.min < ring.max`；`0 < pass_within < ring.min`；`exit_spread` 在 0..90；`group_radius`、`groups_per_member`、`groups_per_player` 都必須大於 0。

---

## 4. 選目標與分配預算

每次輪到某個定義可以生成時（現在的 `ticks_until_spawn` 歸零），依序執行：

### 4.1 分群

- 從 `ActorStateView` 取出所有存活的玩家，用**貪婪半徑分群**：距離在 `group_radius_meters`（草案 18 m）以內的玩家併成同一群，以連通分量計算。
- 4 名玩家的規模，直接兩兩比較就夠了，不需要 k-means 或空間索引。
- 群的「中心」取成員位置的重心，再 `snap` 到 navmesh。重心可能落在兩人之間的懸崖上，所以要 snap。

### 4.2 已有威脅的判斷：看當下距離

- 一群「已有威脅」的條件：當下有任何巡邏隊的 cursor 落在這群任一成員的 `ring.max_meters` 以內。
- **不看那隊當初是替誰生的。** 群每次都重新計算，沒有固定的身分。落單的玩家走進三人組隊伍的範圍時，也算已有威脅。

### 4.3 冷卻與保底記在玩家身上

- 每名玩家記一個 `last_threatened_tick`：只要這名玩家依 §4.2 有威脅，就更新為當下的 tick。
- 一群玩家的「飢餓時間」= 現在減去群內成員中**最晚**的 `last_threatened_tick`，也就是以群裡最近一次有威脅的人為準。這樣三人組拆散時，不會因為變成「新的群」而立刻重新生成。
- **保底**：群的飢餓時間超過 `floor_interval_ticks`（草案 1800 = 60 s）時，這群進入保底佇列。

### 4.4 分配順序

1. **保底佇列**：飢餓時間最長的先處理。保底不受每群上限以外的組數限制，只要全服的隻數預算還有空間。
2. **還沒有威脅的群**：人數多的先處理。人數相同時，飢餓時間長的先處理。
3. **已有威脅、但還沒到每群上限的群**：同樣是人數多的先處理。

每群上限 = `ceil(人數 × groups_per_member)`（草案 k = 1）：落單的玩家最多 1 組，三人組最多 3 組。

全服的組數上限 = `ceil(玩家數 × groups_per_player)`（草案 1.5），取代現有的固定 `max_live_groups`。4 名玩家就是最多 6 組。

**每次最多只生一組。** 與現在的行為一樣，分配完一組就重新開始 `interval_ticks` 倒數。保底佇列不用等倒數，每個 tick 都會檢查。

### 4.5 例子（4 名玩家：3 人一群、1 人落單，全服上限 6 組）

- 三人組最多 3 組，落單的最多 1 組，總共 4 組，不會超過上限。
- 即使三人組已經有 3 組，落單的玩家仍會經由 §4.4 第 2 項拿到他的那 1 組。60 s 內都沒有威脅時，他會被排進保底佇列，優先順序最高。

---

## 5. 隊伍大小：依群的人數分層（D4）

```yaml
squad_by_group_size:
  - members: {min: 1, max: 1}
    count: {min: 4, max: 6}
    composition:
      - {entity_template: gingerbread_warrior, min: 0, max: 1}
      - {entity_template: gingerbread_mage,    min: 0, max: 1}
      - {entity_template: gingerbread,         min: 3, max: 6}
  - members: {min: 2, max: 2}
    count: {min: 6, max: 9}
    composition:
      - {entity_template: gingerbread_warrior, min: 1, max: 3}
      - {entity_template: gingerbread_mage,    min: 0, max: 2}
      - {entity_template: gingerbread,         min: 4, max: 8}
  - members: {min: 3}
    count: {min: 8, max: 12}
    composition: # 沿用現在 warband 的配比
      - {entity_template: gingerbread_warrior, min: 1, max: 5}
      - {entity_template: gingerbread_mage,    min: 1, max: 3}
      - {entity_template: gingerbread,         min: 4, max: 8}
```

為什麼用分層表，不用公式：

- **每一層的 composition 必須不同。** 現在 warband 的保底是 1 + 1 + 4 = 6 隻，放不進落單的 4 人小隊。公式只能調整總數，配比還是需要一張表。
- 每一層各自套用現有的 `validate_patrol_definition` 規則：保底要放得進 `count.min`，上限加總要填得滿 `count.max`。
- 新增的驗證規則：
  - `members` 必須從 1 開始，連續不中斷。
  - 只有最後一層可以沒有 `max`。
  - `squad_by_group_size` 與頂層的 `count` / `composition` 不能同時出現。
- **預算檢查改用目標那一層的 `count_max`**，不再用整個定義的最大值。否則落單的玩家也要等到空出 12 隻的額度才能生成，保底就失效了。
- 群的人數以生成當下的 snapshot 為準。隊伍生成之後，群的人數再怎麼變，都不會改變這隊的大小。
- 決定性：隨機狀態仍然由 `seed` 和 `spawn_ordinal` 推導，同一個場景重跑會得到同樣的結果。

---

## 6. 生成點與路線（D2、D6、D7）

針對選定的目標群（中心 C、成員集合 M），最多試 `route_attempts` 次：

1. **起點**：在環帶 [r_min, r_max] 內，以面積均勻抽一個方位 θ 和半徑 r，得到點 S。
   - S 必須在 `playable_bounds` 以內；做不到就換一次抽樣。
   - S 必須能 `snap` 到 navmesh（水平 1 m）。
   - S 與**所有玩家**的距離都必須 ≥ r_min，不只目標那一群。否則替落單玩家生成的隊伍，可能剛好出現在三人組眼前。
2. **終點**：沿 θ + 180° ± `exit_spread_degrees` 的方向，從 C 往外延伸，取 [r_min, r_max] 之間的一點 E。同樣要在 `playable_bounds` 以內，而且能 snap 到 navmesh。
3. **路線**：`find_route(S → E)`，沿用 `max_detour_ratio`。
4. **穿越檢查**：路線的折線上至少要有一點離 C 不超過 `pass_within_meters`。不符合就重抽。這個檢查也順便確認了起點能走到目標，相當於 `AI_PATROL_SYSTEM.md` 延後處理的「屋頂孤島」問題第 3 步。
5. 全部嘗試都失敗：這一輪不生成，記一次 `route_failure`，**不消耗冷卻**，下一個 tick 再試。這是幾何上的失敗，提高預算解決不了。

邊界處理：第 1、2 步先用解析方式把環帶裁成 `playable_bounds` 內的扇形，再在扇形裡抽樣。這樣玩家在邊上時不會浪費一半的嘗試次數。如果解析裁切太複雜，先用拒絕抽樣，再量測失敗率。

### 6.1 地圖系統需要的欄位

catalog 目前沒有「可生成範圍」這個欄位。navmesh 的 bmin / bmax 包含 Recast 的邊緣內縮，也包含之後可能加的 margin，不能直接用。新增：

```yaml
map:
  playable_bounds: {min: {x: -108, z: -108}, max: {x: 108, z: 108}}
```

- 沒有寫這個欄位時，退回使用 navmesh 的 bmin / bmax，並在 log 出一次 warning。
- 使用者正在考慮在 216 m 的可玩範圍外，再加 12 m 的 margin（mesh 總寬 240 m）。margin 的定義：
  - 玩家不能進入 margin。這要靠地圖本身的碰撞牆或推回機制，不在本計劃範圍內。
  - 巡邏隊的起點和終點只能在 `playable_bounds` 以內。AI 不應該站在玩家到不了的地方，否則遠程單位會站在 margin 裡讓玩家打不到。
  - margin 的用途：讓 navmesh 和碰撞不會在可玩邊界上斷掉；擊退或拉扯把單位推出邊界時有地面接住，不會直接觸發 `fall_death_height`；以及視覺上的延伸。
- 建議地圖原點放在中心：playable [-108, 108]，mesh [-120, 120]。

---

## 7. 生成距離的上下界（D6，P0 修訂）

P0 的結論（§10）推翻了原本的草案（25–45 m）：

- **45 m 的生成點，client 根本收不到。** entity 的 relevance 距離是固定的 40 m（進入）/ 44 m（離開），超出範圍就不會被送到 client。
- **25 m 的生成點，會直接生在 gingerbread 的 vision（26 m）和 mage 的 vision（30 m）裡面。** 隊伍一出現就開打。
- **生在 40 m 以內，玩家會看到隊伍憑空冒出來。**

修訂後的規則：

| 界線 | 條件 | 修訂值 |
|---|---|---|
| r_min（生成） | 離**每一名玩家**都要超過 relevance 的離開距離 44 m，再加上 squad 縱深的一半（約 5 m）。這樣整隊都在 client 看不到的地方生成，不會憑空出現，隊伍會從 40 m 的邊緣走進視野 | 50 m |
| r_max（生成） | 越大，走到玩家那裡要越久。0.9 m/s 走 50 m 約 55 s，走 60 m 約 67 s，與保底的 60 s 同一個量級 | 60 m |
| 觀察窗 | 從進入 relevance（40 m）到隊伍裡 vision 最遠的成員看到玩家（mage 30 m）之間，只剩 **10 m**。玩家站著不動時，大約是 11 s | 由成員的 vision 決定 |
| `pass_within_meters` | 決定「預設會不會開打」：小於 20 m 時，warrior 和 gingerbread 也都會看到，玩家要主動閃開才不會開打；30–38 m 時，只看得到、不會開打，除非玩家主動靠近。依 D1 取前者 | 15 m |

隊伍的觀察窗落在 snapshot 的遠帶（25–40 m，權重 ×1）。在人多的時候，更新頻率會低到每秒一次的保底，遠處的隊伍會跳動。這是已知的遠帶問題，不是本計劃要處理的；要擴大觀察窗，需要改 kernel 的 relevance 距離（§10），不在本計劃範圍內。

---

## 8. 實作分段

| 階段 | 內容 | 測試 |
|---|---|---|
| P0 ✅ | 量測：relevance 距離、snapshot 優先順序分帶、隊伍成員的 vision。由常數直接決定，不需要 runtime probe（§10）。 | 量測記錄寫進本文件 §10 |
| P1 ✅ | catalog：`where.mode: around_players`、`squad_by_group_size`、`map.playable_bounds`、`groups_per_member` / `groups_per_player` / `floor_interval_ticks` 的解析與驗證 | `patrol_catalog_test`：每一條拒絕規則各一個 case；舊的 `area` 定義照常載入 |
| P2 | 分群（§4.1）、已有威脅的判斷（§4.2）、每名玩家的冷卻與飢餓時間（§4.3）。寫成不依賴 kernel 的純函式。 | 新的 `patrol_targeting_test`：3 + 1 分群、拆散後不會立刻重新生成、走進別群的範圍算已有威脅 |
| P3 | 分配順序（§4.4），預算檢查改用目標那一層的 `count_max` | `patrol_director_test`：§4.5 的例子；落單的玩家在三人組額度用完時仍拿得到隊伍；保底優先 |
| P4 | 生成點與路線（§6）：環帶抽樣、對所有玩家檢查 r_min、穿越檢查、邊界裁切 | 平面 navmesh：生成點的距離都在 [r_min, r_max] 內；玩家貼著邊時沒有點落在邊界外；穿越檢查失敗時不消耗冷卻 |
| P5 | 把 warband 改成 `around_players`；改寫 `AI_PATROL_SYSTEM.md` 的前提與 WHERE 表 | headless 跑一次 4 名玩家（3 + 1）、120 s，記錄每群的威脅覆蓋率和 `route_failure` 次數 |

每個階段都注意：
- 斷言必須包含一組對照情境，證明斷言真的有在檢查。
- 測試要在 `-c opt` 下仍然有效。
- 使用到 navmesh 的測試要有一組「確定有地面」的對照。

---

## 9. 草案數字（待確認）

| 參數 | 草案 | 依據 |
|---|---|---|
| `group_radius_meters` | 18 | gingerbread 的 squad leash |
| `ring` | 50–60 m（P0 修訂） | §7：在 relevance 44 m 外生成 |
| `pass_within_meters` | 15（P0 修訂） | §7：小於 warrior 的 vision（20 m），預設會開打 |
| `exit_spread_degrees` | 30 | 避免每次都直直撞向玩家 |
| `groups_per_member`（k） | 1 | 4 人時：三人組 3 組加落單 1 組 |
| `groups_per_player` | 1.5 | 4 人時共 6 組 |
| `floor_interval_ticks` | 1800（60 s） | 現在 `interval_ticks` 的 2 倍 |
| 各層隊伍大小 | 4–6 / 6–9 / 8–12 | §5 |

預算總量：最壞情況是 6 組 × 12 隻 = 72 隻巡邏隊，加上 on_alert 增援 64 隻，總共 136 隻，仍在 `agent_budget 256` 以內。`patrol_budget.max_live_agents` 要從 48 調高到大約 72。巡邏隊衝著玩家來之後，交戰會觸發 caller 叫增援，兩者會疊在同一群人身上，P5 要一起觀察。

---

## 10. 量測記錄

### P0：relevance 與 vision（2026-11-03，main 40f1ad57）

這些數字都是常數，可以直接從程式碼讀出來，不需要 runtime probe。遠帶在人多時的實際卡頓，之前已經量過：template 32 在 relevance 邊緣停頓 1–1.8 s，原因是 15 個 snapshot 的保底，加上 0.25 s 的外插上限，再加上封包遺失。

**Relevance**（`engine/src/kernel/src/kernel.cc`，`kDefaultEntity*`）：

| 項目 | 值 | 說明 |
|---|---|---|
| 進入距離 | 40 m | 距離 = 與玩家的**3D** 距離減去 entity 的水平包圍半徑 |
| 離開距離 | 44 m | 避免在邊界上反覆進出 |
| projectile | 80 m | 只適用於 projectile，與巡邏隊無關 |

- 這些值是固定常數，沒有 catalog 或 per-entity 的設定可以覆寫。要改只能改 kernel。
- 距離是 **3D**（`glm::length(entity.position - player.position)`），不是水平距離。3D 距離一定不小於水平距離，所以 §6 用水平距離檢查 r_min ≥ 50 m 就足以保證生成點在 relevance 外。代價是：在高低差大的地形上（composed 地圖 y 8–38 m、giantwood y −38–148 m），隊伍要走得更近才會進入視野，觀察窗會比平地更窄。

**Snapshot 優先順序分帶**（`kSnapshotPriority*`）：

| 帶 | 距離 | 權重 |
|---|---|---|
| 近 | ≤ 10 m | ×4 |
| 中 | ≤ 25 m | ×2 |
| 遠 | 25–40 m | ×1 |
| 保底 | — | 15 個 snapshot（1 s）沒被送過，就優先送 |

**隊伍成員的 vision**（`collider_templates` 的 `range`）：

| 成員 | vision | leash |
|---|---|---|
| gingerbread（28） | 26 m | 18 m |
| gingerbread_warrior（29） | 20 m | 20 m |
| gingerbread_mage（31） | 30 m | 30 m |

隊伍在任何一名成員開打時就會停下來（`PatrolGroup::holding`），所以整隊在什麼距離開打，取決於 vision 最遠的成員：有 mage 的隊伍是 30 m。

**結論：**
1. 原本的草案 25–45 m 兩頭都不對：45 m 時 client 收不到，25 m 時一生成就在 vision 裡。修訂為生成在 50–60 m（§7）。
2. 「從遠處觀察」的窗口只有 40 → 30 m，也就是 10 m。這個窗口由 kernel 的 relevance 常數與成員的 vision 共同決定。
3. 要擴大觀察窗，有兩個方向，都不在本計劃範圍內：
   - 把 entity 的 relevance 距離調大。這是 kernel 常數，會增加頻寬，而且會讓更多 entity 落在權重 ×1 的遠帶。
   - 縮小巡邏隊成員的 vision。這會影響這些 template 在巡邏以外的行為。
