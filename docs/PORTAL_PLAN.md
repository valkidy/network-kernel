# 傳送門（Portal）實作計劃書

狀態：**設計已定案；P0 實測完成（§7.1）；P1（K1、K2、K6，ABI 103）完成（§6.1）。** 下一步是 P2。
分支：`claude/portal`（從 main fc4e964 開出）。
最後更新：2026-10-10（第七輪 review：與帳篷共用 population group 並改名 `building`；觸發區分成 on_enter / on_overlap / on_exit）。

本文件整理 2026-10-10 七輪設計討論的結論。

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
| D21 | 傳送門與帳篷共用 population group；group 從 `tent` 改名為 `building` | 沿用帳篷 D 決策「所有建築共用一個 group」。lifetime 是各 template 的 lifecycle 欄位，不是 group 的屬性，所以名稱不加 timed。共用的後果見 §4 的 K6 |
| D22 | 觸發區比照物理引擎分成三個 trigger：`on_enter`（進入時一次）、`on_overlap`（重疊期間每個 tick）、`on_exit`（離開時一次）。傳送門用 `on_enter` | 語意見 §4 的 K2 |

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
  population_group: building                   # 與帳篷共用（D21）
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
  on_enter:                                     # NEW；另有 on_overlap、on_exit（D22）
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

另外需要一個 item template（`fungible_portal_kit`，只有 throw）。population rule 沿用現有的 id 2，只改名（K6）。

---

## 4. Kernel 需要新增的部分

| # | 內容 | 對應決策 |
|---|---|---|
| K1 | **觸發區碰撞盒。** `purpose: trigger` 註冊到內部 `kTrigger` layer（目前 prop 的碰撞盒一律是 static obstacle，`kernel.cc:4902`）；解析 `physics.trigger_collider_template` 並檢查用途；`find_clear_spawn_spot` 改成明確使用 `collider_template` 加上觸發區，不再拿「第一個 Hit box」 | D13、D16 |
| K2 | **`on_enter` / `on_overlap` / `on_exit` entity trigger。** 每個 tick，對放在地上、有觸發區的 prop 做 overlap 查詢，依 `collision_mask`、`instigators` 和 kernel 規則篩出「有資格的重疊 actor」，跟上一個 tick 的集合比較。語意見下方 | D6、D10、D14、D22 |
| K3 | **`portal:` 區塊。** 解析與檢查（`forward` 正規化、`y > 0` 拒絕、`exit_template` 存在）；入口生成時生成出口並記錄關聯；在 `destroy_entity_with_context` 裡用同一個 despawn 原因刪除出口（位置同帳篷 K3 釋放住客） | D3、D4、D7 |
| K4 | **`teleport` action。** 依序：`drop_carried_props` → 移到 destination（目的地被擋住時用 `available_lift` 或站立點搜尋）→ 速度歸零、清掉 `ImpulseLockout` 和 stagger、清掉 controller height（同 `Kernel_ServerSetEntityTransform`）→ 加上 arrival grace 和 `DamageImmunity` → 送出 K5 | D5、D9、D11 |
| K6 | **population group 改名 `tent` → `building`。** 改 `gameplay_catalog.yaml`、`216_prop_tent.yaml`、`223_prop_field_camp.yaml`；`gameplay_config_test.cc:1328` 寫死了 group 名稱 `"tent"`，要一起改。`max_alive: 4` 與 `cleanup_runs_on_destroy: true` 現在也套用到傳送門（傳送門沒有 on_destroy graph，不受影響） | D21 |
| K5 | **傳送通知。** 給被傳送的玩家（帶 net_id、destination、forward），讓 Unity 結束銜接動畫並轉鏡頭。另外要讓**其他 client 的內插不要跨過傳送**：P0 量到觀察者會在舊位置與新位置的連線上畫出一個中間幀（R2）。做法待定：snapshot 帶「這個 tick 被傳送」的標記，或 client 端對單一 snapshot 間隔內的超大位移不做內插 | D4、D12、R2 |

**K2 的觸發語意（D22）：**
- 每個 trigger prop 記住上一個 tick「有資格的重疊 actor」集合。這個 tick 新出現的 actor 觸發 `on_enter`，持續存在的觸發 `on_overlap`，消失的觸發 `on_exit`。
- 進入的那個 tick 只觸發 `on_enter`，下一個 tick 才開始 `on_overlap`（同 Unity 的 Enter／Stay）。
- **失去資格就算離開**：actor 進入帳篷、死亡、或進入 arrival grace 時觸發 `on_exit`；重新取得資格時再觸發 `on_enter`。所以站在傳送門裡等 grace 結束，會在結束那個 tick 被傳送。陷阱可以靠 `on_exit` 清掉自己給的 status。
- **被刪除不觸發**：trigger prop 被刪除時，不對裡面的 actor 觸發 `on_exit`（清理交給 `on_destroy_entity`）；actor 被刪除或斷線時也不觸發。同 Unity 的行為。
- 被傳送的玩家，下一個 tick 不在觸發區裡，會觸發入口的 `on_exit`（傳送門沒有寫，所以沒有效果）。
- 三個 trigger 都可以各自寫 `collision_mask`、`instigators`；同一個 prop 上必須一致，否則載入時拒絕（集合只有一份）。

**共用 group 的後果（K6）：** 4 個名額由帳篷、field camp、傳送門共用，丟出新的傳送門可能擠掉最舊的帳篷，裡面的住客會被擊退（帳篷 K4 的設計行為）。這是帳篷計畫已經接受的「建築太多是 gameplay 設計問題」。

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
| **P0 實測（已完成）** | 測試直接呼叫 `Kernel_ServerSetEntityTransform` 做傳送，量測 R1–R3（§7.1）。入口的落點校正需要綠色觸發區，留到 P1 再測 | 不變 |
| P1（已完成） | K1 + K2 + K6，見 §6.1 | 103 |
| P2 | K3 + K4 | 103 |
| P3 | K5；Unity 的銜接動畫與鏡頭（使用者） | 103，可能加 packet 或 snapshot schema |

### 6.1 P1 實作紀錄（2026-10-10，分支 `claude/portal-p1`）

- **ABI 103**：`KernelEntityTemplateDefinition` 末尾新增 `trigger_collider_template_id`、`enter_trigger`、`overlap_trigger`、`exit_trigger`、`overlap_trigger_mask`、`overlap_trigger_instigators`；新增 enum `KernelOverlapInstigators`（`_Actors = 0`、`_Players = 1`）。Unity 的 C# 對應結構要跟著改（使用者負責）。
- **K1 與計畫的差異**：D13 原本寫「註冊到新的內部 physics layer `kTrigger`」。實作改成**觸發區根本不放進 physics world**（`push_collider_into_physics` 對純 trigger 用途回傳 false），只存在 collider registry 裡，由 `OverlapTriggerSystem` 拿它的形狀做查詢。效果相同（移動、projectile 碰撞檢查、落點校正都看不到它），但不用新增 Jolt layer。
- **K1 落點校正**：`find_clear_spawn_spot` 除了第一個 Hit box，也檢查 trigger 用途的 box（D16）。
- **K2**：`OverlapTriggerSystem` 每個 tick 在 `CollisionTriggerSystem` 之後執行。查詢對象是 actor 的**移動膠囊**，camp 讀膠囊碰撞盒的 `layer`。執行順序：exit → enter → overlap，再依觸發區、actor 的 net_id 排序。事件：instigator 和 target 都是 actor，position 是 actor 位置，direction 從觸發區指向 actor。
- **K6**：population group `tent` 改名 `building`（`gameplay_catalog.yaml`、`216_prop_tent.yaml`、`223_prop_field_camp.yaml`、`gameplay_config_test.cc`）。
- YAML：collider 的 `purpose: trigger` 現在可以用了（原本只有 skeleton collider 接受）。
- 測試：
  - `//engine/src/tests/kernel_tests:trigger_volume_placement_test`：門口前方有一個方塊、但門框沒有被擋到時，只檢查門框的話會留在原位（對照組，z = 0）；連觸發區一起檢查會退到 z = −1。
  - `//game_server:trigger_volume_test`：用 YAML 撰寫的測試門，三個 trigger 各自生成一個 marker prop 來計數。驗證：進入一次、每 tick overlap、離開一次；玩家可以直接走過觸發區；`instigators: players` 擋掉敵人；camp mask 擋掉玩家；死亡算離開、復活算進入；被刪除不觸發 exit。
- 已知限制：
  - **沒有移動膠囊的 actor 永遠不會觸發**（例如不會動的 sentry）。傳送門只給玩家用，沒有影響。
  - 帳篷住客那個情境的測試是**空的**：進入帳篷會把移動遮罩改成只有 terrain，膠囊因此離開 physics world，查詢本來就找不到它；拿掉 `Sheltered` 規則測試照樣通過。規則保留作為保險，測試只能證明結果，不能證明規則。

---

## 7. 尚未量測的風險

| # | 風險 | 量測方式 |
|---|---|---|
| R1 | 本地玩家的跳躍 | **P0 已量測，沒問題**（§7.1） |
| R2 | 其他 client 看到的傳送者 | **P0 已量測，有一個中間幀**（§7.1），列入 K5 |
| R3 | relevance 空窗 | **P0 已量測，沒問題**（§7.1） |
| ~~R4~~ | ~~`CarriedBy` 的 prop 不跟著搬運者~~：**撤回，判斷錯誤**。`ItemGameplaySystem::update_carried_props`（`item_gameplay_system.cc:1508`）每個 tick 把搬著的 prop 移到搬運者位置加上 `carry_offset`。D5（放在入口）仍然成立，但是設計選擇，不是繞過 bug | — |
| R5 | 終點附近的觀察者能不能看到出口：出口是一般 prop，照 relevance 同步，應該沒問題 | P2 |

### 7.1 P0 實測結果（2026-10-10）

測試：`game_server/tests/portal_feasibility_probe_test.cc`（分支 `claude/portal-p0`）。一個 dedicated server 加兩個真實 client，經 localhost UDP 連線，全部使用出貨的 catalog 和地面，只用 public API（跟 Unity 一樣）。伺服器跑真的 `GameServer`，但清掉 `game_rules`，地圖上沒有敵人。傳送用 `Kernel_ServerSetEntityTransform` 代替，被傳送的玩家在傳送期間持續往前走。tick 和 snapshot 都是 30 Hz。

| # | 情境 | 結果 |
|---|---|---|
| 對照 | 正常行走 30 幀 | 走了 5.00 m，畫面上每幀最大位移 0.206 m（沒有 snap） |
| R1 | 被傳送的玩家自己的畫面，跳 30 m | **一次 snap（33.6 m），2 幀後畫在伺服器的位置；沒有中間幀；預測沒有失敗** |
| R2 | 觀察者看被傳送的玩家（兩端都在 relevance 範圍內） | **畫出 1 個中間幀**，位置 (-8.65, 0, 3.98)，在舊位置到新位置的連線上；之後 snap。5 幀後追上，之後是一般的內插延遲（走路中約 0.65 m） |
| R3 | 跳 94 m，超過 40 m 的 relevance 半徑，終點有另一個玩家和兩頂帳篷 | **2 幀後全部畫出來** |

解讀：
- R1：kernel 現有的「超過 2 m 就 snap」（`kernel.cc:11725`）已經足夠，本地玩家不需要額外處理。
- R2：遠端內插會在「傳送前最後一個 snapshot」和「傳送後第一個 snapshot」之間混合，所以會畫出一個中間位置。30 Hz 的 snapshot 只有一幀；snapshot 頻率較低或畫面更新率高於 30 fps 時，這段混合會畫更多幀。列入 K5 處理。
- R3：entity 少的情況下，relevance 換地方沒有空窗。終點附近 entity 很多時（例如數百個 agent），snapshot 的 byte budget 可能讓補齊變慢，這次沒有量。

限制：
- localhost 幾乎沒有延遲，所有等待時間在真實網路下都要加上 RTT。
- 測試期間 client 印出兩次 `render clock reset: ~5000 ms behind its target`。測試每幀只 sleep 1 ms，模擬時間比真實時間快，推測是測試本身造成的，但沒有確認是否影響 R2 的幀數。

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

- ~~Q1~~：已定案，D21。
- ~~Q2~~：已定案，D22。
- **Q3**：K5 走 event 還是 snapshot 欄位。

---

## 10. 相關檔案

- 帳篷前例：`docs/TENT_BUILDING_PLAN.md`（K3 刪除前釋放住客、K6 落點校正、shelter 區塊）
- `engine/src/simulation/src/player_movement.cc`：`find_clear_spawn_spot`（1136）、`available_lift`
- `engine/src/simulation/src/systems.cc`：`yaw_rotation_from_direction`（190）、spawn placement（約 1634）、`Kernel_ServerSetEntityTransform` 的實作（約 3205）、shelter 進入時 `drop_carried_props`（約 3383）、`revive`（3411）
- `engine/src/kernel/src/kernel.cc`：prop 碰撞盒的 layer 對應（4902）、本地玩家的表現層 snap（11725）
- `engine/src/kernel/public/kernel_types.h`：`KernelColliderPurpose_Trigger`（1936）、`KernelEntityTriggerActionType`、`KernelEventVec3Source_Literal`
- `game_server/src/gameplay_config.cc`：`physics.collider_template` 的解析
