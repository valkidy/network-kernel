# 空間位移方向（六軸）討論報告

狀態：**討論紀錄，延後實作。** 等有明確的 gameplay 設計目標再開工。
分支：`claude/water-jet`（文中提到的 `SurfaceNormal` 只存在這個分支，尚未 merge 到 `main`）。
日期：2026-10-17。

本文件整理水柱魔法（`docs/WATER_JET_PLAN.md`）實作過程中延伸出來的討論。結論是：這些需求
其實是同一個更一般的設計，也就是「以空間中的某一個點或參考框架為基準，對目標做位移」。
從 generic 的角度看，至少要能推算六個軸向。但目前 (a) gameplay 需求不明確，(b) 改動成本
不低，所以先記錄、延後。

---

## 1. 討論經過

| 順序 | 需求 | 結果 |
|---|---|---|
| 1 | 水柱打在哪個面，就沿那個面的法線推人 | 已實作：`KernelEventVec3Source_SurfaceNormal`（ABI 108），水柱 P0–P4 完成 |
| 2 | 水柱瓶在空中爆炸，找不到 surface normal 時怎麼 fallback | 只討論，未實作。建議退回飛行方向 `event.direction` |
| 3 | 新 projectile 撞 actor，推力方向 = 飛行前進方向 × actor up（cross） | 只討論。結論是「側推」 |
| 4 | 全部往同一側掃，可以設定左或右；瓶子取水平前進方向 | 只討論。設計出 `event.direction_left/right` 與共用的 `sweep_blast` |
| 5 | 純水平推力會不會因為沒離地而推不動 | 已實測並修正：平推只動 0.40 m，水柱改成 SPLIT `[6, 8]` |

第 2–4 項的共同點：都在問「推力方向要從哪個參考框架的哪一個軸取出來」。

---

## 2. 一般化的模型

一次位移可以拆成四個部分：

```
位移 = 參考框架（原點 + 三個基底軸）
     × 軸向選擇（±前、±右、±上，共六軸，可以混合）
     × 強度（SPLIT：水平 h + 垂直 v）
     + 套用方式（加到速度上 / 取代速度 / 拉到某一點）
```

### 2.1 參考框架

| 框架 | 原點 | 「上」 | 「前」 | 現有對應 |
|---|---|---|---|---|
| 世界 | — | 世界 Y | 世界 Z | `Literal` |
| 表面 | 撞擊點 | 表面法線 | 飛行方向投影到表面上 | `SurfaceNormal`（只有「上」） |
| 飛行方向 | 撞擊點 / 範圍效果中心 | 世界 Y | 飛行方向的水平分量 | `SubjectDirection`（只有「前」） |
| 放射 | 範圍效果中心 | 世界 Y | 中心 → 目標 | area effect 的 `Direction` |
| 目標自身 | 目標位置 | 目標的 up | 目標面向 | 無 |
| 施法者 | 施法者位置 | 世界 Y | 施法者瞄準方向 | 無 |

### 2.2 六軸

每個框架都可以取出 ±前、±右、±上。以飛行方向框架為例，`f` 是水平前進方向，up 是世界 Y：

```
+前 = f                 −前 = −f
+右 = cross(up, f)      −右 = cross(f, up)      （Unity 左手座標）
+上 = up                −上 = −up
```

例：水柱是「表面框架 +上」，側掃是「飛行方向框架 ±右」，拉向施法者是「施法者框架 −前」。

### 2.3 套用方式

| 方式 | 現有對應 |
|---|---|
| 加到速度上（impulse） | `apply_impulse`，RADIAL 或 SPLIT |
| 取代速度 | `apply_pull` 的速度是取代，不是相加 |
| 拉到某一點 | `apply_pull` 的 `TO_POINT`（錨點用 `SubjectPosition`） |
| 沿某方向拉 | `apply_pull` 的 `ALONG` |

---

## 3. 已經量到的限制（實作時一定會遇到）

1. **平推會被吃掉**。在地上的 actor 拿到純水平速度，只移動 0.40 m，下一個 tick 就歸零。只有
   離地的 actor 會保留動量。任何可能是水平的軸（±前、±右、牆面法線、放射方向）都要搭配 SPLIT
   的垂直抬升，而且 lockout 要大於滯空時間 2v/g（水柱 §5.7）。
2. **框架會退化**：
   - 飛行方向垂直時（直直往上、往下），水平前進方向是零，±前與 ±右都無法定義。
   - 撞到 actor 或 expiry 時沒有表面，表面框架不存在。
   - 目標剛好在範圍效果中心時，放射方向是零。
3. **零向量會拖垮整個 batch**。`apply_impulse` 拿到零方向不是跳過而已，而是讓整個 graph batch
   失敗，同一個 graph 裡的傷害也會一起消失。`spawn_projectile` 零方向會退回 +X。所以每個框架都要
   有明確的 fallback，或標記「零向量時只跳過這一個 action」（目前只有 `SurfaceNormal` 的 spawn
   有 `drop_on_zero_direction`）。
4. **範圍效果分配給多個目標時，`event.direction` 是放射方向**，每個目標都不同。要「整個爆炸共用
   一個方向」只能從 `subject_direction` 取。原地不動的範圍效果沒有 heading，現在是靠 speed 0.05
   的技巧把方向存在 `initial_velocity` 裡。
5. **graph ABI 只有固定的 source，沒有運算式**。每多一種「框架 × 軸」就要一個新的 enum 值。
   6 個框架 × 6 個軸就是 36 種，再加上 fallback 的組合，逐一列舉的方式撐不久。
6. **resolver 只讀事件本身**（`resolve` 時只看 `TriggerEvent`）。目標自身框架、施法者框架需要
   讀世界狀態（rotation、瞄準方向），不是 producer 多填欄位，就是 resolver 要能存取 World。
7. **client 預測只涵蓋一種情況**。目前 client 只在「會飛的範圍效果撞上 static world」那一刻預測
   impulse，而且 direction source 是寫死的 switch。每加一種 source 都要補上，否則預測結果會跟
   server 不一致。
8. **entity 觸發與 projectile 觸發是兩張表**。瓶子（entity `on_collision`）跟法杖（projectile
   `on_projectile_impact`）要用同一種方向，兩張表、`gameplay_config.cc` 兩條路徑都要改，
   `entity_trigger_action_parity_test` 會檢查兩邊有沒有對齊。

---

## 4. 實作選項與成本

| 選項 | 做法 | 優點 | 成本 |
|---|---|---|---|
| A. 逐一新增 enum | 每個需求加一個 `KernelEventVec3Source` 值（例如 `DirectionLeft/Right`） | 每次改動小，不改 struct layout，ABI 加值即可 | 組合數一多就失控；fallback 只能硬寫在各個值裡 |
| B. 結構化方向描述 | `KernelActionDefinition` 加 `{frame, axis, fallback}` 欄位，一次涵蓋六軸與所有框架 | 一次到位，authoring 可以直接寫 `frame: heading, axis: right, fallback: up` | struct layout 改變：ABI bump、C# mirror、所有 managed struct 位移；兩張表、loader、resolver、client 預測都要改 |
| C. B + 世界狀態框架 | 在 B 的基礎上加入目標自身、施法者框架 | 涵蓋「推到目標背後」「拉向施法者」這類需求 | resolver 要能讀 World，或每個 producer 都要多填目標 rotation、施法者瞄準方向 |

粗估：A 每一個新軸大約是 P1+P2 的規模（約 6 個檔案加測試）；B 是一次 ABI layout 變更，加上
Unity 端的 mirror 更新；C 還要動到事件 producer。

---

## 5. 為什麼延後

- **gameplay 不明確**：目前只有水柱一個確定的需求，已經用 `SurfaceNormal` 做完。側掃、空中爆炸
  的 fallback、拉向施法者都還只是「可以做」，沒有對應的武器或道具設計。
- **選 A 還是 B 取決於需求數量**：只有一兩個新軸時 A 比較划算；需要三個以上的框架時，B 的一次性
  成本才值得。沒有需求清單之前無法判斷。
- **改動成本不低**：B 和 C 都會改 ABI layout，連帶 Unity 端的 mirror 與 package，需要跟其他功能的
  ABI 變更排程。

---

## 6. 重新開工的條件與建議步驟

開工前先有：
1. 至少三個具體的 gameplay 案例，每個寫清楚「哪個框架、哪個軸、套用方式、退化時怎麼辦」。
2. 每個案例由誰發出：法杖（projectile 觸發）、瓶子（entity 觸發）或範圍效果。

建議步驟：
1. 把案例填進 §2 的表格，算出實際需要的「框架 × 軸」組合數。
2. 組合數 ≤ 2 → 選 A；需要 ≥ 3 個框架 → 選 B；需要目標自身或施法者框架 → C。
3. 不管選哪個，§3 的八條限制都要在計畫裡逐條處理，尤其是 1（抬升）和 3（零向量）。
4. 端到端測試要檢查目標**實際的位移**，不能只看第一個 tick 的速度方向（水柱 P4 的教訓）。

---

## 7. 目前已有、可直接沿用的部分

- `KernelEventVec3Source_SurfaceNormal`（ABI 108）與 `drop_on_zero_direction`：表面框架的「上」。
- speed 0.05 的範圍效果技巧：把一個方向帶給原地不動的範圍效果，讓整個爆炸共用。
- `SPLIT` 強度模式：水平軸搭配抬升。
- `water_jet_staff_test` 的驗證方式：用自己的 `PhysicsWorld` 算出期望方向，追蹤目標直到靜止。

## 8. 已討論但未實作的設計（供日後參考）

- **空中爆炸的 fallback**：情況 A（`on_expired`）只需要 catalog 內容；情況 B（同一次 impact
  裡有 normal 用 normal、沒有就用飛行方向）需要 `SurfaceNormalOrDirection` source；情況 C
  （投擲出去的瓶子）要在 thrown prop 的碰撞 producer 裡填 `surface_normal`。
- **側掃**：`event.direction_left/right` 由發射者計算，spawn 一個共用的 `sweep_blast`（speed
  0.05，`apply_impulse` 用 `subject_direction`，SPLIT 加抬升）。左右由發射者的 binding 決定，
  法杖與瓶子共用同一個 blast 與 graph。投擲物沿拋物線飛行時水平方向不變，所以瓶子的側推方向
  等於丟出時的方向。
