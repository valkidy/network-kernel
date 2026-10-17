# 水柱衝擊魔法實作計劃書

狀態：**可行性評估完成，計畫待確認；尚未開始實作。**
分支：`claude/water-jet-plan`，從 `main`（dcc805da，ABI 107）開。實作時另開
`claude/water-jet`。
最後更新：2026-10-17。

本文件整理 2026-10-17 的可行性評估：`apply_impulse` 再擴充，做出「水柱打在哪個表面，
就沿那個表面的法線把人沖開」的魔法。第 2 節是決策，第 3 節是 kernel 改動，第 6 節是
還沒定案的問題。

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
| D2 | 方向放在**水柱的 spawn 速度裡**：水柱 template 的 `speed` 設一個很小的正值，`initial_velocity = normal * speed` | 不需要在 `ProjectileState` 另存方向欄位。client 預測 impulse 時讀的就是 `projectile.initial_velocity`（`kernel.cc` 預測 `SubjectDirection` 那一段），而速度本來就會同步，所以**不用動 snapshot schema** |
| D3 | 表面法線放在新的 `TriggerEvent::surface_normal`，用新的 `KernelEventVec3Source_SurfaceNormal = 5` 讀出來 | 現在 projectile 撞擊事件的 `direction` 填的是飛行方向，`CollisionHit::normal` 算出來了卻沒往下傳 |
| D4 | `surface_normal` **只在撞到非 actor 時填值**，其他情況一律是零向量 | 走 rewind 的歷史 hitbox 那條路沒有法線，actor 的法線對這個魔法也沒意義 |
| D5 | `surface_normal` 是零向量時，`spawn_projectile` **放棄這次 spawn**，不退回預設方向 | `spawn_projectile_from_template` 遇到零向量會用 `normalized_or(direction, {1,0,0})` 補一個方向，水柱就會朝 +X 推人 |
| D6 | 水柱彈的 collision mask 是 `TERRAIN \| STATIC_OBSTACLE` | 使用者指定。PROP 要不要包含見 Q2 |
| D7 | impulse 維持「**加**到目標原本的速度上」 | 現有語意。要不要改成取代見 Q3 |
| D8 | 法線**不吸附**到主軸，直接用物理算出來的值 | 牆是什麼角度就推往什麼方向。是否要吸附見 Q1 |

### 2.1 為什麼不在 `ProjectileState` 另存方向

另存方向也能讓原地不動的水柱保有方向，但 client 預測 impulse 時也要讀到它，所以必須
同步給 client，也就要 bump snapshot schema。D2 用的是本來就會同步的速度，零改動。

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

法線的精準度：
- segment（ray）projectile：Jolt 的 `GetSurfaceNormal`，是真正的表面法線，最準。
- sphere 或 box projectile：shape cast 的 `-mPenetrationAxis`，是接觸法線。平面上沒問題，
  打在邊和角上會偏。**建議水柱彈用 segment，或很小的 sphere。**

### 3.2 K2：新的 vec3 source

- `kernel_types.h`：`KernelEventVec3Source_SurfaceNormal = 5`，附上說明；ABI 107 → 108，
  版本歷史加一條。
- `components.h` 的 `EventVec3Source` 加 `kSurfaceNormal`。
- `action_graph.cc`：`event_vec3_source_from_kernel` 和解析 `EventVec3Expression` 的地方
  加上 `kSurfaceNormal → event.surface_normal`。

### 3.3 K3：`spawn_projectile` 接受 `SurfaceNormal`

- `kernel.cc` 有**兩張表**都把 `spawn_projectile` 的 direction 限定成 `Direction`
  （大約在 1929 行和 3769 行）。兩張都要改成也接受 `SurfaceNormal`。
  `entity_trigger_action_parity_test` 會抓這兩張表有沒有對齊。
- 只有 projectile 的撞擊觸發才可以用 `SurfaceNormal`。其他事件（例如 item 使用、
  `on_expired`）沒有表面，loader 要拒絕。
- runtime：`spawn_projectile` 拿到的 direction 是零向量、而 source 是 `SurfaceNormal` 時，
  不產生這個 command（D5）。要確認這樣放棄不會拖垮同一個 graph 裡的其他 action：
  只丟掉這一個 spawn，不要讓整個 batch 失敗。

### 3.4 不需要改的部分

- `apply_impulse`：`SubjectDirection` + `RADIAL` 已經能用，loader 也已經接受。
- `area_effect_system.cc`：`subject_direction` 本來就是 `normalize(initial_velocity)`。
- client 預測：讀的就是 `initial_velocity`。
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

### 5.1 P0 驗證項目

1. **client 端的撞擊路徑**：client 有沒有預測水柱彈的撞擊並在本機 spawn 水柱？如果有，
   法線來自 client 自己的 collision world，要確認 client 上也有同一份 static collision，
   而且走的是同一個 `process_projectile_hit_records`。
2. **地形上的法線**：在測試場景用 segment 和 sphere 各打一發到平地和牆面，記錄實際拿到的
   法線。注意測試場景的 static scene 要用真的 catalog id，否則地面不存在而 API 照樣回傳成功。
3. **小速度水柱**：`speed = 0.05` 的 area effect 能正常 overlap、正常到期，而且沒有哪個
   以 `speed > 0` 判斷的路徑會把它當成會飛的東西而行為出錯。area effect 只做查詢、不是
   physics body，所以移動不會觸發 collider resync。

---

## 6. 待定問題

| # | 問題 | 預設 |
|---|---|---|
| Q1 | 法線要不要吸附到主軸？或者用 `normal.y` 的門檻分成「地板」和「牆」兩類，各給固定方向？ | 不吸附（D8） |
| Q2 | collision mask 要不要包含 `PROP`（巢、帳篷等）？ | 不包含 |
| Q3 | impulse 要不要改成**取代**目標原本的速度，像 `apply_pull` 那樣？ | 不改，維持「加上去」（D7） |
| Q4 | 撞牆得到的法線沒有往上的分量，目標貼地滑行可能被摩擦或移動輸入吃掉。要不要補一點往上的分量，還是靠 `impulse_lockout_ticks` 解決？ | 先靠 lockout，P4 量測後再決定 |
| Q5 | 水柱的 speed、lifetime、半徑、strength 的數值 | P3 調整 |

---

## 7. 交付時要回報的事

- ABI：107 → 108（新增 `KernelEventVec3Source_SurfaceNormal`）。client 和 server 要用同一版。
- snapshot schema：不變。
- `bundle.bytes`：P3 加了 catalog 內容後需要更新，由使用者處理。
