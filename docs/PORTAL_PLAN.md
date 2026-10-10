# 傳送門（Portal）實作計劃書

狀態：**設計已定案，尚未實作。** 下一步是 P0 實測（§6）。
分支：`claude/portal`（從 main fc4e964 開出）。
最後更新：2026-10-10（第六輪 review：單面進入的碰撞盒、落點校正連觸發區一起檢查、門被打壞沿用現有機制）。

本文件整理 2026-10-10 六輪設計討論的結論。

---

## 1. 目標與範圍

使用者原始需求：

1. 第一版用 prop 測試。
2. 只支援丟擲（throw），不支援 pickup、drop、use。落地位置生成傳送門的入口。
3. 落點邏輯比照帳篷：伺服器可以適度校正成合法的落地位置。位置合法但太高、玩家碰不到，視為玩家操作失誤。
4. 正式版的終點應該是另一張地圖的起始位置。目前沒有地圖系統，所以終點先設在目前地圖的某處。
5. 終點可以設定方向（forward）：平行於 xz 平面，或朝 −y（需要測試 camera follower）。不允許朝 +y，因為根本無法正常穿越。

不在範圍內：
- **丟出去的 prop 穿過傳送門**（D18）。
- **雙向傳送**：終點只有演出，沒有任何碰撞偵測（D8）。
- **地圖系統**：目的地暫時寫成固定座標（D3）。
- **Unity package build / stage / bump**：由使用者處理。

---

## 2. 決策

| # | 決策 | 理由／備註 |
|---|---|---|
| D1 | 用 portal kit（item）丟出，比照 tent kit 的流程；kit 碰撞時生成入口並毀掉自己 | 沿用現有機制，丟擲與生成不需要改 kernel |
| D2 | 入口用 `spawn_entity placement: clear` 生成（帳篷 K6 的 `find_clear_spawn_spot`） | 只保證不重疊，不檢查玩家碰不碰得到，符合需求 3 |
| D3 | 目的地寫在入口 template 的 `portal.destination`（世界座標） | 正式版改成「地圖 id + 起始點」，寫在同一個區塊 |
| D4 | `portal.forward` 載入時正規化；`y > 0` 拒絕載入。水平方向：Unity 鏡頭轉向 forward。朝 −y：玩家往下掉，鏡頭維持原本的 yaw | 玩家輸入的 move 是世界座標，鏡頭 yaw 由 Unity 管理，所以需要傳送通知（K5） |
| D5 | 搬著的 prop（`CarriedBy`）在傳送前放下，留在入口 | 比照進入帳篷：`drop_carried_props`（`systems.cc` 的 shelter 進入流程） |
| D6 | 只有玩家 entity 能傳送 | `27_allied_beam_sentry` 也是 player side，用陣營篩選擋不掉 |
| D7 | 入口與出口一起管理：kernel 依 `portal.exit_template` 在入口生成時一併生成出口；入口被刪除時，用**同一個 despawn 原因**一起刪除出口 | 出口沒有碰撞盒、沒有 HP、沒有 lifecycle、不加入 population group，壽命完全跟著入口 |
| D8 | 單向；出口只有演出 | |
| D9 | `portal.arrival_grace_ticks`：抵達後一段時間內忽略**所有**傳送門 | 防止有人把傳送門丟在終點上，造成每個 tick 都被傳回終點的循環 |
| D10 | 帳篷住客（`Sheltered`）與 HP 為 0 的 actor 不觸發任何 overlap trigger | kernel 的統一規則，不寫在資料裡。性質同 `InstigatorSheltered`。落點校正有 bug 導致帳篷與傳送門重疊時，靠這條保底 |
| D11 | `portal.arrival_immunity_ticks`：抵達後短暫無敵 | 照 revive 的 `DamageImmunity`。伺服器已經在終點，client 還在播銜接動畫，這段時間玩家看不到四周 |
| D12 | 走過去就傳送；client 用銜接動畫蓋住 RTT 的等待 | 動畫要有逾時取消（client 預測碰到門，但伺服器沒有觸發）；動畫期間送零輸入 |
| D13 | 觸發區是 collider template，`purpose: trigger`，由 `physics.trigger_collider_template` 引用 | 不新增 template 類別。kernel 把 trigger 用途的碰撞盒註冊到新的**內部** physics layer `kTrigger`，不再當成 static obstacle，不用掉 `KERNEL_COLLISION_LAYER_*` bit |
| D14 | 觸發篩選分三層：陣營用 `collision_mask: player_side`；身分用 trigger 欄位 `instigators: players`；狀態用 kernel 規則（D10） | 不擴充 collision mask：mask 是固定分類，放不進「在帳篷裡」這種狀態；另外加一個 `player` bit 會和 `player_side` 混淆，所有讀 mask 的系統都受影響 |
| D15 | 碰撞盒幾何（側視圖）：紅色 hitbox 涵蓋底座、門框和門；綠色觸發區只在開口那一面（local −z，朝向丟的人）；底部都貼地；綠色頂部比紅色低 5 cm；綠色伸進紅色 0.1 m；綠色凸出門面 0.3–0.5 m | 只能從開口那一面進入。生成的 rotation 是 `yaw = atan2(x, z)`，local +z 朝丟擲方向（`systems.cc:190`） |
| D16 | 落點校正把紅色加綠色一起拿去跟 terrain 和 static obstacle 檢查 | 避免開口那一面埋在牆裡。校正是往丟擲的反方向退，也就是往開口那一側退 |
| D17 | 入口有 HP；門被子彈或爆炸打壞，完全沿用現有機制，不做特殊處理。入口和出口消失時都播建築物被破壞的演出 | 紅色是 `layer: damageable`，跟 `tent_hitbox` 一樣 |
| D18 | 第一版不讓丟出去的 prop 穿過傳送門 | 正式版的終點在另一張地圖，飛行中的 prop 要整包搬到另一個 world；在同一張地圖裡還要處理軌跡同步、碰撞檢查掃過整張地圖、client 預測、玩法上的濫用。見 §8 |
| D19 | 地圖原點放在中央（±4 km）；地圖做大時由 Unity 端加 floating origin | float32 在 4–8 km 的最小間距是 0.49 mm，傳送目的地不是瓶頸。kernel 不用改 |
| D20 | 幾乎垂直往下丟：水平分量是 0，rotation 是單位旋轉，門朝世界座標 +z | 視為玩家操作失誤 |

---

## 3. Catalog（示意）

id 用 `<id>` 佔位，實作時依 catalog 命名規則（`$id_<kind>_$name`）分配。標 `NEW` 的是新欄位或新 action。

```yaml
# collider_templates/<id>_collider_portal_hitbox.yaml   紅色：底座 + 門框 + 門
# local +z = 丟擲方向；開口在 −z。底部在 y = 0。
id: <id>
name: portal_hitbox
shape: oriented_box
center: {x: 0.0, y: 1.3, z: 0.15}
half_extents: {x: 1.0, y: 1.3, z: 0.15}
purpose: hit
layer: damageable
```

```yaml
# collider_templates/<id>_collider_portal_gate.yaml     綠色：只在開口那一面
# z 範圍 [−0.4, 0.1]：凸出門面 0.4 m，伸進紅色 0.1 m；頂部比紅色低 5 cm。
id: <id>
name: portal_gate
shape: oriented_box
center: {x: 0.0, y: 1.275, z: -0.15}
half_extents: {x: 0.9, y: 1.275, z: 0.25}
purpose: trigger
```

```yaml
# entity_templates/<id>_prop_portal_entrance.yaml
id: <id>
name: portal_entrance
entity_type: prop
impulse_resistance: 1000.0
lifecycle:
  lifetime_ticks: 5400
  population_group: portal
health:
  hp: 600
  max_hp: 600
physics:
  collider_template: portal_hitbox
  trigger_collider_template: portal_gate       # NEW；必須是 purpose: trigger
portal:                                         # NEW；只能寫在 prop 上
  exit_template: portal_exit
  destination: {x: 120.0, y: 0.0, z: -40.0}
  forward: {x: 0.0, y: 0.0, z: 1.0}             # y > 0 拒絕載入
  arrival_grace_ticks: 30
  arrival_immunity_ticks: 60
triggers:
  on_overlap:                                   # NEW
    collision_mask: player_side
    instigators: players                        # NEW：players | actors
    action_graph: action_teleport_through_portal
    parameters:
      target: event.instigator
      portal: self
```

```yaml
# entity_templates/<id>_prop_portal_exit.yaml   純演出
id: <id>
name: portal_exit
entity_type: prop
```

```yaml
# action_graph_templates/action_teleport_through_portal.yaml
id: action_teleport_through_portal
parameters:
  target: null
  portal: null
actions:
  - type: teleport                # NEW
    target: params.target
    portal: params.portal
```

```yaml
# entity_templates/<id>_prop_portal_kit.yaml   流程同 tent_kit
id: <id>
name: portal_kit
entity_type: prop
health: {hp: 1, max_hp: 1}
physics:
  collider_template: collision_damage_prop_hitbox
triggers:
  on_collision:
    collision_mask: terrain|static_obstacle     # 必須包含 terrain；不加陣營位元
    action_graph: action_spawn_building_and_damage_self_at_collision
    parameters:
      template: portal_entrance
      position: event.position
      direction: event.direction
      owner: self
      source: self
      self_damage: 1
```

另外需要一個 item template（`fungible_portal_kit`，只有 throw），以及 population rule（見 §9 待決 Q1）。

---

## 4. Kernel 需要新增的部分

| # | 內容 | 對應決策 |
|---|---|---|
| K1 | **觸發區碰撞盒。** `purpose: trigger` 註冊到內部 `kTrigger` layer（目前 prop 的碰撞盒一律是 static obstacle，`kernel.cc:4902`）；解析 `physics.trigger_collider_template` 並檢查用途；`find_clear_spawn_spot` 改成明確使用 `collider_template` 加上觸發區，不再拿「第一個 Hit box」 | D13、D16 |
| K2 | **`on_overlap` entity trigger。** 每個 tick，對放在地上、有觸發區的 prop 做 overlap 查詢；依 `collision_mask`、`instigators` 篩選；略過 `Sheltered`、HP 為 0、還在 arrival grace 期間的 actor；以 instigator 執行 graph。觸發語意（只在進入時觸發，或重疊期間每個 tick 都觸發）在 K2 決定，傳送門兩種都可以 | D6、D10、D14 |
| K3 | **`portal:` 區塊。** 解析與檢查（`forward` 正規化、`y > 0` 拒絕、`exit_template` 存在）；入口生成時生成出口並記錄關聯；在 `destroy_entity_with_context` 裡用同一個 despawn 原因刪除出口（位置同帳篷 K3 釋放住客） | D3、D4、D7 |
| K4 | **`teleport` action。** 依序：`drop_carried_props` → 移到 destination（目的地被擋住時用 `available_lift` 或站立點搜尋）→ 速度歸零、清掉 `ImpulseLockout` 和 stagger、清掉 controller height（同 `Kernel_ServerSetEntityTransform`）→ 加上 arrival grace 和 `DamageImmunity` → 送出 K5 | D5、D9、D11 |
| K5 | **傳送通知。** 給被傳送的玩家（帶 net_id、destination、forward），讓 Unity 結束銜接動畫並轉鏡頭 | D4、D12 |

版本影響：
- 新 trigger、新 action 和新欄位都會讓 **ABI 從 102 升到 103**，catalog schema 也要加欄位。
- K5 如果用新 event，packet schema 可能要改；如果放進 snapshot，snapshot schema 一定要改，有三個寫死版本號的測試要一起更新。
- 多個分支平行開發時，ABI 版本和 catalog id 在 git 上 merge 不會衝突，但執行時會撞號，merge 時兩者都要檢查。

---

## 5. Client（Unity，使用者負責）

- C# 對應結構：ABI 103 的新欄位與 enum。
- 本地玩家碰到綠色時開始播銜接動畫，播放期間送零輸入；收到 K5 才結束；逾時就取消。觸發區的形狀從 catalog bundle 的 collider 資料取得。
- 收到 K5：水平的 forward 把鏡頭 yaw 轉過去；朝 −y 時維持 yaw，需要實測 camera follower 在玩家落下時的表現。
- 入口和出口的模型；兩者消失時都播建築物被破壞的演出。
- 地圖變大時再加 floating origin（D19）。

---

## 6. 實作順序

| 階段 | 內容 | ABI |
|---|---|---|
| **P0 實測** | 只用現有欄位：portal kit 沿用 tent kit 的 graph 生成一個只有紅色碰撞盒的入口；測試直接呼叫 `Kernel_ServerSetEntityTransform` 做長距離傳送（dedicated server + pure client 兩個 process）。量測 §7 的 R1–R3 | 不變 |
| P1 | K1 + K2 | 103 |
| P2 | K3 + K4 | 103 |
| P3 | K5；Unity 的銜接動畫與鏡頭（使用者） | 103，可能加 packet 或 snapshot schema |

---

## 7. 尚未量測的風險

| # | 風險 | 量測方式 |
|---|---|---|
| R1 | 本地玩家的跳躍：表現層位移超過 2 m 時會直接跳過去（`kernel.cc:11725`，`kPredictionCorrectionSnapDistanceMeters`），預期不會慢慢滑。待確認 pending input 重播後是否穩定 | P0 |
| R2 | 其他 client 看到的傳送者：遠端 actor 的內插遇到大距離跳躍時，會不會一路滑過整張地圖 | P0 |
| R3 | relevance 空窗：傳送後 relevance 中心換地方，目的地附近的 entity 會陸續出現。量測要幾個 snapshot 才補齊 | P0 |
| R4 | `CarriedBy` 的 prop 位置似乎不會跟著搬運者更新（沒有找到相關程式）。D5 讓傳送不受影響，但這本身是一個潛在問題；目前 catalog 沒有可以搬的 prop | 之後 |
| R5 | 終點附近的觀察者能不能看到出口：出口是一般 prop，照 relevance 同步，應該沒問題 | P2 |

---

## 8. D18 的完整考量（丟出去的 prop 穿過傳送門）

1. **跨地圖**：飛行中的 prop 帶著 item 實例、擁有者和 graph 綁定，要整包搬到另一個 world。這是主要的阻礙。
2. **軌跡同步**：client 用「發射點 + 初速」計算拋物線（`ThrownPropMotion`）。傳送後要同步一段新的軌跡，可能要改 snapshot schema。
3. **丟的人自己的預測**：client 不知道有傳送門，位移超過 2 m 時會直接跳過去。
4. **飛行中的碰撞檢查**：傳送那一 tick 從「上一個位置」掃到「新位置」，會橫跨整張地圖，必須重設。
5. **出口速度**：入口沒有正面方向，最簡單的規則是「保留速率，方向改成出口的 forward」。
6. **紅色先攔下 prop**：同一個 tick 內，觸發判斷要排在落地判斷之前。
7. **玩法**：炸彈傳到終點、丟的人看不到結果；portal kit 被傳過去，在終點又生出一個傳送門。

---

## 9. 待決問題

- **Q1**：傳送門要自己一個 population group（示意中是 `portal`），還是跟帳篷共用？帳篷目前的 group 名字是 `tent`、`max_alive: 4`。如果共用，group 最好改名成 `building`。
- **Q2**：K2 的觸發語意（只在進入時觸發，或重疊期間每個 tick 都觸發）。傳送門兩種都可以，之後的陷阱、壓力板會需要。
- **Q3**：K5 走 event 還是 snapshot 欄位。

---

## 10. 相關檔案

- 帳篷前例：`docs/TENT_BUILDING_PLAN.md`（K3 刪除前釋放住客、K6 落點校正、shelter 區塊）
- `engine/src/simulation/src/player_movement.cc`：`find_clear_spawn_spot`（1136）、`available_lift`
- `engine/src/simulation/src/systems.cc`：`yaw_rotation_from_direction`（190）、spawn placement（約 1634）、`Kernel_ServerSetEntityTransform` 的實作（約 3205）、shelter 進入時 `drop_carried_props`（約 3383）、`revive`（3411）
- `engine/src/kernel/src/kernel.cc`：prop 碰撞盒的 layer 對應（4902）、本地玩家的表現層 snap（11725）
- `engine/src/kernel/public/kernel_types.h`：`KernelColliderPurpose_Trigger`（1936）、`KernelEntityTriggerActionType`、`KernelEventVec3Source_Literal`
- `game_server/src/gameplay_config.cc`：`physics.collider_template` 的解析
