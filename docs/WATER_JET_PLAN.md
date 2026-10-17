# 水柱衝擊魔法實作計劃書

狀態：**Q1–Q5、Q6 採預設值定案；P0 驗證完成（§5.2）；P1 kernel 完成（§5.3），ABI 108；P2 尚未開始。**
分支：`claude/water-jet`，從 `main`（dcc805da，ABI 107）開；計畫最早寫在
`claude/water-jet-plan`。
最後更新：2026-10-17。

本文件整理 2026-10-17 的可行性評估：`apply_impulse` 再擴充，做出「水柱打在哪個表面，
就沿那個表面的法線把人沖開」的魔法。第 2 節是決策，第 3 節是 kernel 改動，第 5.2 節是
P0 驗證結果，第 6 節是待定問題（已全部採預設值）。

---

## 1. 目標與範圍

設計目標（使用者原話整理）：

> 1. impulse 能設定單一方向性。
> 2. collision mask 只保留 terrain 和 static object，根據落點的表面 normal 決定
>    impulse 方向。落點在地上就是 (0, 1, 0)；打在牆上則由牆的表面 normal 決定，
>    例如 (1, 0, 0) 或 (0, 0, 1)。

串法：

```
水柱彈（standard projectile，mask = terrain | static_obstacle）
  └─ on_collision：spawn_projectile(position = event.position,
                                    direction = event.surface_normal)   ← 新 source
       └─ 水柱（area effect，cylinder，speed 很小但 > 0）
            └─ on_collision：apply_impulse(direction = event.subject_direction,
                                           RADIAL, strength = N)
```

在範圍內：
- kernel：觸發事件帶上表面法線、新的 vec3 source、`spawn_projectile` 接受這個 source。
- game_server：authoring 字串 `event.surface_normal`、loader 檢查、水柱的 catalog 內容。

不在範圍內：
- **水柱的外觀**：client presentation，Unity 端自己處理。
- **Unity package build / stage / bump、C# mirror、`bundle.bytes`**：由使用者處理。
- **merge 到 `feat-unity-plugin`**：由使用者處理。

---

## 2. 決策

| # | 決策 | 理由 |
|---|---|---|
| D1 | 推力沿用現有的 `apply_impulse` `RADIAL` 模式：`normalize(direction) * strength` | 單一方向性現在就有。`SubjectDirection` 是同一次範圍效果裡每個目標共用的同一個向量（`kernel_types.h` 的 `KernelEventVec3Source_SubjectDirection`） |
| D2 | 方向放在**水柱的 spawn 速度裡**：水柱 template 的 `speed` 設一個很小的正值，`initial_velocity = normal * speed` | 不需要在 `ProjectileState` 另存方向欄位，`area_effect_system.cc` 的 `subject_direction` 現在就讀 `initial_velocity`。第二段**完全不用改 code**。（P0 修正：原本寫的「client 預測讀 `initial_velocity`」不適用於水柱，見 §2.1） |
| D3 | 表面法線放在新的 `TriggerEvent::surface_normal`，用新的 `KernelEventVec3Source_SurfaceNormal = 5` 讀出來 | 現在 projectile 撞擊事件的 `direction` 填的是飛行方向，`CollisionHit::normal` 算出來了卻沒往下傳 |
| D4 | `surface_normal` **只在撞到非 actor 時填值**，其他情況一律是零向量 | 走 rewind 的歷史 hitbox 那條路沒有法線，actor 的法線對這個魔法也沒意義 |
| D5 | `surface_normal` 是零向量時，`spawn_projectile` **放棄這次 spawn**，不退回預設方向 | `spawn_projectile_from_template` 遇到零向量會用 `normalized_or(direction, {1,0,0})` 補一個方向，水柱就會朝 +X 推人 |
| D6 | 水柱彈的 collision mask 是 `TERRAIN \| STATIC_OBSTACLE` | 使用者指定。PROP 要不要包含見 Q2 |
| D7 | impulse 維持「**加**到目標原本的速度上」 | 現有語意。要不要改成取代見 Q3 |
| D8 | 法線**不吸附**到主軸，直接用物理算出來的值 | 牆是什麼角度就推往什麼方向。是否要吸附見 Q1 |
| D9 | 水柱彈的 collider 用 **segment** | P0-2：sphere 在地形 mesh 上的法線最多偏 37.5° |
| D10 | 水柱 template：`motion_collision_mask = 0`，`sync_mode` 不是 server-snapshot-only，不做成 derived chain | P0-1、P0-3 |

### 2.1 為什麼不在 `ProjectileState` 另存方向

另存方向也能讓原地不動的水柱保有方向，但要多一個欄位、多一條寫入路徑。D2 用現成的
`initial_velocity`，第二段零改動。

**P0 修正**：原本的理由是「client 預測 impulse 要讀方向，另存就得同步、要 bump snapshot
schema」。實際上 client 只在「會飛的 area effect 撞上 static world」那一刻預測 impulse
（`advance_predicted_projectiles` 裡 `if (!hits.empty())` 那段）。水柱是從表面往外漂，
永遠不會撞上，所以 **client 不會預測水柱的推力**。本機玩家要等 server 的
`ActorImpulseBatch`（packet 25），大約晚一個 round trip 才被推，跟現有原地爆開的瓶子一樣。
因此「另存方向就要 bump schema」這個理由不成立，但 D2 一樣比較省事，維持不變。
要讓本機玩家的推力也被預測，是另一個較大的題目，不在這次範圍。

代價是水柱會沿法線慢慢漂，距離等於 speed × lifetime，例如 0.05 m/s 存在 1 秒就是 5 cm。
地上會往上漂、牆邊會往外漂離牆面，這兩種都沒有危害。

---

## 3. Kernel 改動

### 3.1 K1：觸發事件帶上表面法線

- `engine/src/world/public/components.h`：`TriggerEvent` 在最後面加
  `glm::vec3 surface_normal{0.0f};`。放在最後，現有沒帶這個值的 producer 不用改。
- `engine/src/simulation/src/projectile_system.cc` 的 `process_projectile_hit_records`：
  - 一般路徑：`records.front()` 不是 actor 時，把 `records.front().hit.normal` 傳下去。
  - piercing segment 路徑：撞到非 actor 的那個分支，同樣傳 `record.hit.normal`。
  - `queue_projectile_trigger` 要多一個參數把法線帶進 `TriggerEvent`。
- rewind 的歷史 hitbox 路徑只會撞到 actor，維持零向量（D4）。

法線的精準度（P0-2 已量測，§5.2）：
- segment（ray）projectile：Jolt 的 `GetSurfaceNormal`，是真正的表面法線，最準。
- sphere 或 box projectile：shape cast 的 `-mPenetrationAxis`，是接觸法線。box 的面上精確，
  但在地形 mesh 上最多偏 37.5°。**水柱彈用 segment（D9）。**

### 3.2 K2：新的 vec3 source

- `kernel_types.h`：`KernelEventVec3Source_SurfaceNormal = 5`，附上說明；ABI 107 → 108，
  版本歷史加一條。
- `components.h` 的 `EventVec3Source` 加 `kSurfaceNormal`。
- `action_graph.cc`：`event_vec3_source_from_kernel` 和解析 `EventVec3Expression` 的地方
  加上 `kSurfaceNormal → event.surface_normal`。

### 3.3 K3：`spawn_projectile` 接受 `SurfaceNormal`

- `kernel.cc` 的 `validate_projectile_mechanics`（projectile 觸發的檢查）：只有 **standard
  projectile 的 impact 觸發**可以把 `spawn_projectile` 的 direction 設成 `SurfaceNormal`。
  area effect 的 impact 打到的都是 actor，`on_expired` 在半空中結束，兩者都沒有表面可回報，
  所以都拒絕。
- 計畫原本寫「兩張表都要改」，P1 實作時修正：另一張是 **entity 觸發**的表（約 3769 行），
  entity 觸發沒有任何 producer 會填表面法線，維持只接受 `Direction`，不改。
- action graph 端：`expression_available_for_event` 讓 `kSurfaceNormal` 只在
  `kProjectileImpact` 可用，`compile_action_trigger_definition` 會用它擋掉其他事件。
- runtime：`ActionSpawnProjectileDefinition` 新增 `drop_on_zero_direction`，direction source
  是 `SurfaceNormal` 時才設。拿到零向量就只跳過這一個 spawn，同一個 graph 的其他 action 照常
  執行。其他 source 的行為完全不變。

### 3.4 不需要改的部分

- `apply_impulse`：`SubjectDirection` + `RADIAL` 已經能用，loader 也已經接受。
- `area_effect_system.cc`：`subject_direction` 本來就是 `normalize(initial_velocity)`。
- spawn record：`initial_velocity` 以完整 float 傳送（`packet_codec.h` 的 `write_vec3`），
  0.05 m/s 的方向不會被量化掉。
- snapshot schema、wire packet：都不變。

---

## 4. game_server 改動

### 4.1 G1：authoring 字串

`game_server/src/gameplay_config.cc` 的 projectile 觸發編譯（`spawn_projectile` 那段，
目前要求 `position == "event.position" && direction == "event.direction"`）：
- direction 多接受 `"event.surface_normal"`，編成 `KernelEventVec3Source_SurfaceNormal`。
- 錯誤訊息一起更新。
- action hash（`hash_scalar(hash, action.direction_source)`）本來就有包含 source，不用改。

### 4.2 G2：loader 檢查

- `event.surface_normal` 只能用在 projectile 的撞擊觸發。
- 水柱的 `apply_impulse` 用 `event.subject_direction`，現有檢查要求 `speed > 0`。D2 的
  小速度剛好滿足這條。

### 4.3 G3：catalog 內容

水柱 weapon、水柱彈 projectile、水柱 area effect、cylinder collider、兩個 graph。
- 檔名照 `$id_<kind>_$name` 規則。
- **id 在 merge 時才決定**：平行分支在 git 上 merge 得乾淨，但 id 會在 runtime 撞號。

---

## 5. 階段

| 階段 | 內容 | 驗收 |
|---|---|---|
| P0 | 驗證（見 5.1） | 三個問題都有量測結果，寫回本文件 |
| P1 | K1 + K2 + K3 | simulation 測試：打在地上 `surface_normal ≈ (0,1,0)`；打在牆上是牆的法線；撞到 actor 是零向量；零法線不 spawn |
| P2 | G1 + G2 | `action_graph_catalog_test`：`event.surface_normal` 能編譯；用在非撞擊事件時被拒絕 |
| P3 | G3 catalog 內容 | catalog 能載入；play log 裡水柱的 spawn 方向等於表面法線 |
| P4 | 端到端測試 | kernel 測試：打在地上目標被往上推、打在牆上目標被沿牆的法線推開；listen server 和 pure client 的預測速度一致 |

每次改 ABI 都要更新三個寫死版本號的測試（ABI version 和 snapshot schema version）。

### 5.1 P0 驗證項目（原始問題）

1. **client 端的撞擊路徑**：client 有沒有預測水柱彈的撞擊並在本機 spawn 水柱？如果有，
   法線來自 client 自己的 collision world，要確認 client 上也有同一份 static collision，
   而且走的是同一個 `process_projectile_hit_records`。
2. **地形上的法線**：在測試場景用 segment 和 sphere 各打一發到平地和牆面，記錄實際拿到的
   法線。注意測試場景的 static scene 要用真的 catalog id，否則地面不存在而 API 照樣回傳成功。
3. **小速度水柱**：`speed = 0.05` 的 area effect 能正常 overlap、正常到期，而且沒有哪個
   以 `speed > 0` 判斷的路徑會把它當成會飛的東西而行為出錯。area effect 只做查詢、不是
   physics body，所以移動不會觸發 collider resync。

### 5.2 P0 驗證結果（2026-10-17）

**P0-1 client 端的撞擊路徑：client 不需要算法線。**
- client 不會預測「水柱彈撞擊 → spawn 水柱」這條鏈。`simulate_tick` 雖然每個模式都會跑，
  但 pure client 的 `world_` 裡沒有這些 projectile。client 是收到 server 的 projectile spawn
  record 才建立水柱，`initial_velocity` 直接從 record 拿（`kernel.cc` 收 spawn group 那段，
  record 由 `record.initial_velocity = projectile.initial_velocity` 填入）。
- record 以完整 float 傳送，所以水柱的方向在 client 上跟 server 完全一樣。
- 水柱的 `sync_mode` **不能**是 `kServerSnapshotOnly`，否則 client 收到 record 後直接
  `continue`，不會建立本機的水柱。
- client 不會預測水柱的推力，見 §2.1 的修正。
- 另外注意：`DerivedChainPresenter` 重建 derived chain 的 root 時，方向寫死成 (1,0,0)。
  水柱彈**不要**做成 targeted strike 的 derived chain。

**P0-2 表面法線：segment 是精確的，sphere 在地形 mesh 上不可靠。**
新增兩個測試到 `engine/src/tests/physics_tests/physics_world_test.cc`：
`hit_normal_is_the_struck_face_for_ray_and_sphere_cast` 和
`sphere_cast_normal_matches_ray_normal_on_terrain_mesh`。量測結果：

| 情境 | ray（segment） | sphere（r = 0.1） |
|---|---|---|
| 地板，垂直往下 | (0, 1, 0)，誤差 0° | (0, 1, 0)，誤差 0° |
| 地板，60° 入射 | (0, 1, 0)，誤差 0° | (0, 1, 0)，誤差 0° |
| 牆，正面 | (-1, 0, 0)，誤差 0° | (-1, 0, 0)，誤差 0° |
| 牆，45° 入射 | (-1, 0, 0)，誤差 0° | (-1, 0, 0)，誤差 0° |
| 繞 Y 轉 30° 的牆 | (0.866, 0, -0.5)，誤差 0° | 同左，誤差 0° |
| 牆的上緣（sphere 45° 往下打） | — | (-0.707, 0.707, 0)，兩個面的中間 |
| `undulating.joltmesh` 地形，301 個取樣 | 三角形的面法線 | 跟 ray 最多差 **37.5°**；>2° 的有 14 個（4.7%），>10° 的有 7 個（2.3%） |

結論：
- box（static obstacle）上兩種都精確，包含旋轉過的牆。
- 地形 mesh 上，sphere 的接觸點落在三角形的邊或頂點時，法線會混到相鄰的面。
- **水柱彈用 segment**（D9）。用 sphere 的話，大約每 40 發就有 1 發的方向偏超過 10°。

**P0-3 小速度水柱：沒有會出錯的路徑，但有兩條限制。**
- 檢查過所有用 `speed > 0`、`initial_velocity` 長度判斷的地方。門檻都是 `> 0` 或
  `> 0.0001`，0.05 m/s 都算「會動」，行為跟其他會飛的 area effect 一樣（每 tick 沿直線前進，
  一般 area effect 會穿過地形）。
- **水柱的 `motion_collision_mask` 必須是 0**：設了的話 loader 會接受（因為 speed > 0），
  但碰到 static world 時 area effect 會被 park，`initial_velocity` 歸零，
  `subject_direction` 變零向量，推力就不見了。
- **cylinder 永遠是直立的**（沿世界 Y 軸，`components.h` 的說明）。打在牆上時，水柱是
  一根以撞擊點為中心的**直立**圓柱，一半埋在牆裡，不是從牆面水平噴出來的水柱。命中範圍
  是牆前方一個半圓柱。推力方向仍然是牆的法線，只有命中範圍的形狀跟畫面不一樣。
  如果要水平的命中範圍，牆上可以改用 sphere area effect，或另外做會旋轉的 cylinder。
  這屬於新的設計問題，列為 Q6。

### 5.3 P1 實作紀錄（2026-10-17）

- K1：`TriggerEvent::surface_normal`；`process_projectile_hit_records` 的兩條路徑（一般、piercing）
  在撞到非 actor 時填入 `hit.normal`。
- K2：`KernelEventVec3Source_SurfaceNormal = 5`、`EventVec3Source::kSurfaceNormal`，ABI 107 → 108。
  `kernel_c_header_test.c` 的 ABI 版本與新 enum 值的 static assert 已更新。
- K3：見 §3.3。
- 測試（`projectile_interaction_test.cc`）：
  - `surface_normal_spawn_launches_along_the_struck_face`：斜射地板，spawn 方向是 (0,1,0) 而不是
    飛行方向；斜射牆面，spawn 方向是 (-1,0,0)；spawn 速度大小等於水柱的 0.05 m/s。
  - `surface_normal_spawn_is_dropped_on_an_actor_hit`：撞到 actor 時不 spawn。已用 mutation 確認
    不是空測：把零向量丟棄拿掉，這個測試會失敗。
  - `surface_normal_is_refused_outside_an_impact`：`on_expired` 不能綁這個 source。
- `kernel_c_header_test` 在 `sizeof(RenderEntityState) == 160u`（第 562 行）失敗，是 `main` 原本就有的：
  這次對 `kernel_types.h` 的改動只有註解、ABI 數字和一個 enum 值，沒有動到這個 struct。

---

## 6. 待定問題

2026-10-17 使用者決定 Q1–Q5 一律採預設值。

| # | 問題 | 預設 |
|---|---|---|
| Q1 | 法線要不要吸附到主軸？或者用 `normal.y` 的門檻分成「地板」和「牆」兩類，各給固定方向？ | 不吸附（D8） |
| Q2 | collision mask 要不要包含 `PROP`（巢、帳篷等）？ | 不包含 |
| Q3 | impulse 要不要改成**取代**目標原本的速度，像 `apply_pull` 那樣？ | 不改，維持「加上去」（D7） |
| Q4 | 撞牆得到的法線沒有往上的分量，目標貼地滑行可能被摩擦或移動輸入吃掉。要不要補一點往上的分量，還是靠 `impulse_lockout_ticks` 解決？ | 先靠 lockout，P4 量測後再決定 |
| Q5 | 水柱的 speed、lifetime、半徑、strength 的數值 | P3 調整 |
| Q6 | 打在牆上時，直立的 cylinder 命中範圍可以接受嗎？（§5.2 P0-3） | 2026-10-17 使用者決定先接受，P3 實際看過再說 |

---

## 7. 交付時要回報的事

- ABI：107 → 108（新增 `KernelEventVec3Source_SurfaceNormal`）。client 和 server 要用同一版。
- snapshot schema：不變。
- `bundle.bytes`：P3 加了 catalog 內容後需要更新，由使用者處理。
