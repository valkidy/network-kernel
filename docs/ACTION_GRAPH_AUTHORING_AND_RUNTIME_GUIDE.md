# Action Graph Authoring and Runtime Guide

## 1. 文件目的

本文件定義 network-kernel 目前已實作的 Action Graph authoring 格式、runtime
契約、use cases、設計 guidelines 與整合檢查項目。適用對象包括：

- 撰寫 gameplay YAML 的內容製作者。
- 維護 catalog loader、Kernel ABI 與 simulation runtime 的工程師。
- 將 Prop、Projectile 或其他 gameplay system 接入 Trigger Event 的開發者。

Action Graph 用於描述「某個 gameplay fact 發生後，要依序提交哪些 gameplay
commands」。它不是 input mapping、condition polling 或 entity state machine。

```text
Gameplay System
→ Trigger Event
→ Captured Binding + Event Snapshot
→ Action Graph Evaluation
→ Validated Command Batch
→ Authoritative Gameplay Side Effects
```

目前實作版本日期：2026-08-23。

---

## 2. 核心模型

### 2.1 Action Graph Template

Action Graph Template 是可重複使用的資料驅動行為，概念上類似函式：

```text
Action Graph Template ≈ function
parameters            ≈ function arguments and defaults
actions               ≈ ordered function body
```

基本格式：

```yaml
id: action_apply_damage_at_activated

parameters:
  target: null
  amount: 1

actions:
  - type: apply_damage
    target: params.target
    amount: params.amount
```

規則：

1. `id` 在 catalog 內必須唯一。
2. `parameters` 宣告 graph 的輸入；`null` 表示 binding 必須提供。
3. `actions` 是有序 list，目前必須包含 1～8 個 action。
4. Action input 必須使用 `params.<name>`，不可由 parameter 動態決定 action type。
5. 同一 parameter 若被不同 action 欄位使用，其推導型別必須相容。
6. YAML parser 接受 non-scalar default，但能否使用仍由 action schema 決定；目前
   authoring 應優先使用 scalar default，Vec3 等 context value 應從 `event.*` 綁定。

### 2.2 Trigger Binding

Entity Template 或 Projectile Template 負責將 event 綁定到 graph，並提供實際參數：

```yaml
triggers:
  on_activated:
    action_graph: action_apply_damage_at_activated
    parameters:
      target: event.target
      amount: 25
```

Binding 只能決定參數值或值的來源，不能改變 graph 的 action 結構。

### 2.3 Runtime Context

```text
self              = 擁有 trigger binding 的 world entity
event.subject     = 此事件主要發生的 entity
event.instigator  = 造成事件的 actor/entity
event.target      = 事件涉及的另一個 entity
event.position    = authoritative event position
event.direction   = authoritative event direction
event.subject_direction = subject 當時的行進方向（只有 projectile trigger 有）
params.*          = binding/default 解析後的 graph parameters
```

`self` 與 `event.subject` 常相同，但語意不同，不應在 compiler 中合併。

Status lifecycle trigger 的 runtime context 另有固定語意：`self` 是受影響的
target，`event.instigator` 是儲存的最新成功 source。Refresh、stack 或 replace
更新 source 時，後續 lifecycle event 使用該最新成功 source；source entity 即使
despawn，provenance 仍保留原本的 net id 與 owner peer。

---

## 3. 已支援的 Trigger Events

Catalog compile 會依 trigger 驗證 `event.*` 欄位。未知欄位或不屬於該 event
schema 的欄位會在載入階段被拒絕。

| Trigger | 可用 event fields | 常見用途 |
|---|---|---|
| `on_activated` | `subject`, `instigator`, `target`, `position`, `direction` | 啟動機關、互動 prop |
| `on_collision` | `subject`, `target`, `position`, `direction` | 碰撞傷害、陷阱 |
| `on_health_depleted` | `subject`, `instigator`, `position` | 死亡前反應、反傷 |
| `on_destroy_entity` | `subject`, `instigator`, `position` | 銷毀時生成 entity |
| `on_projectile_impact` | `subject`, `instigator`, `target`, `position`, `direction`, `subject_direction` | 命中後生成爆炸物 |
| `on_expired` | `subject`, `instigator`, `position`, `direction`, `subject_direction` | projectile 到期後生成效果 |
| `on_apply` | `subject`, `instigator`, `target` | status 建立或 stack apply |
| `on_tick` | `subject`, `instigator`, `target` | status interval tick |
| `on_expire` | `subject`, `instigator`, `target` | status natural expire 或 remove |

注意事項：

- `on_activated.event.target` 是 optional request input。若 graph 使用它，activation
  command 必須提供有效 target。
- Projectile 命中 world geometry 時，`on_projectile_impact.event.target` 可能為空；
  不需要 entity target 的 graph 應只使用 position/direction。
- Area effect projectile 對半徑內每個受影響 target 各發一次
  `on_projectile_impact`：`event.target` 是該 target，`event.direction` 是由爆心
  指向 target 的徑向單位向量。直接命中的 `event.direction` 則是彈道方向。
- `on_collision` 不提供 `event.instigator`；需要歸屬資訊的 collision 行為應由
  產生事件的 gameplay system 明確建模，而不是假設 target 是 instigator。
- Runtime binding validator 會再次執行同一套 schema 驗證，防止無效 ABI input。

---

## 4. 已支援的 Actions

### 4.1 `apply_damage`

```yaml
- type: apply_damage
  target: params.target
  amount: params.amount
```

輸入契約：

- `target`：有效 `EntityRef`，且 runtime entity 必須具有 `NetworkIdentity` 與
  `Health`。
- `amount`：1～65535 的正整數。
- damage source 自目前 graph 的 `self` 取得。

適用於 entity-backed triggers：`on_activated`、`on_collision`、
`on_health_depleted`、`on_destroy_entity`。

### 4.2 `spawn_entity`

```yaml
- type: spawn_entity
  entity_template: params.template
  position: params.position
  owner: params.owner
```

輸入契約：

- `entity_template` 必須解析為既有 Entity Template。
- `position` 目前必須綁定 `event.position`。
- `owner` 必須解析為有效 entity reference。

適用於 entity-backed triggers。

### 4.3 `spawn_projectile`

```yaml
- type: spawn_projectile
  projectile_template: params.template
  position: params.position
  direction: params.direction
```

輸入契約：

- `projectile_template` 必須解析為既有 Projectile Template。
- `position` 與 `direction` 目前必須分別綁定 `event.position`、
  `event.direction`。
- owner、shooter、weapon、action instance 等 attribution 由 execution provenance
  自動向下傳遞，不應重複出現在 YAML parameters。

### 4.4 `apply_impulse`

```yaml
- type: apply_impulse
  target: params.target
  strength: params.strength
  direction: params.direction
  collision_mask: actor | prop
  lockout_ticks: 20
```

輸入契約：

- `target`：有效 `EntityRef`。Runtime 只對兩種 target 生效：actor，或不在
  `carrying` 狀態的 prop。其他 target 靜默略過，不會讓整批失敗。
- `strength`：單位一律是 **m/s 的速度增量**，不參與質量計算。有兩種形式，語意
  刻意不同：
  - **純量**（`strength: 12.0`）：正的有限 float。Runtime 執行
    `velocity += normalize(direction) * strength`，三軸等向縮放。
  - **List**（`strength: [8.0, 4.0]`）：讀作 `[水平, 垂直]`。Runtime 執行
    `velocity += {dir.x * 水平, 垂直, dir.z * 水平}`——**垂直分量是絕對帶號增量，
    不是 `direction.y` 的縮放係數**。水平必須是非負有限值，垂直必須有限，兩者
    不能同時為零；水平為 0 就是純垂直發射。
  兩種形式都 author 在 **graph parameter 的 default 上**，不是寫在 action 裡——
  action 那一行永遠是 `strength: params.strength`：

  ```yaml
  parameters:
    strength: 12.0        # 純量形式
    # strength: [8.0, 4.0]  # list 形式：水平 8 m/s、垂直 4 m/s
  actions:
    - type: apply_impulse
      strength: params.strength
  ```

  若 trigger binding 自己傳了 `strength`，那一定是純量形式；只有「binding 沒有
  覆寫、且 parameter default 是 list」才會選到 list 語意。不使用 list 形式時行為
  與過去完全相同，既有 template 一個字都不用改。差別不是語法糖，見下方
  「為什麼垂直分量必須是絕對值」。
- `direction`：只能綁定 `event.direction`、`event.subject_direction`，或由 graph 的
  `direction` parameter 提供 vec3 default。Vec3 default 必須非零且有限，且目前只
  允許用在名為 `direction` 的 parameter 上。
- `collision_mask`：`actor`、`prop` 或兩者的 or 組合；省略時預設 `actor`。不接受空
  mask 或其他 collision layer bit。
- `lockout_ticks`：非負整數，上限 `KERNEL_MAX_IMPULSE_LOCKOUT_TICKS`（300，30 Hz
  下的十秒）。擊退期間有多少 tick 不允許 target 自己的 movement input 重新決定
  水平速度，詳見下方 Authoring 注意。**省略或 0 等同舊行為**，既有 template 不需
  改動。時長 author 在 action 上而非 target 上，所以重型火箭可以鎖得比輕推久。
- 此 action 不接受 `position`、`amount`、`owner`、`entity_template`、
  `item_template`、`quantity` 欄位。

適用於 entity-backed 與 projectile-backed triggers。Status lifecycle trigger 不接受
`apply_impulse`。`event.direction` 只在 `on_activated`、`on_collision`、
`on_projectile_impact`、`on_expired`、`on_item_used` 可用，因此
`on_health_depleted` 與 `on_destroy_entity` 上的擊退必須改用 vec3 default。

`event.subject_direction` 與 `event.direction` 的差別，在 area effect 上最明顯：
`direction` 是徑向，一次爆炸對每個目標各是一個向量；`subject_direction` 是**該
projectile 自己的行進方向**，同一次事件的所有目標共用同一個。會移動的 area effect
（`speed` 非零）要「把捲到的東西往前帶」時用它，固定爆炸要「往外炸開」時用
`direction`。

它只在 `on_projectile_impact` 與 `on_expired` 可用——其他 trigger 的 subject 沒有
行進方向可報。而且**不會動的 projectile 不能引用它**：`speed` 為 0 的 area effect
上寫 `event.subject_direction` 會在載入期被拒，因為它只會是零向量，而零向量會讓
整個 batch 在 runtime 被拒。

Runtime 效果：

- Actor：`Velocity` 增加該速度量，同時 `MovementState` 被強制設為 airborne，並清除
  ground normal 與支撐體。若不清掉 grounded 狀態，下一 tick 的 movement solver 會
  依斜面重算垂直速度，把上拋量吃掉。
- Prop：切換為 in-flight、解除 carry、重新啟用 collider，之後由 thrown prop motion
  以**直線** motion model 前進，不套用重力。In-flight prop 只有在 collision trigger
  system 掃到 static contact 時才會落地並轉回 placed，而該 system 只處理有
  `on_collision` binding 的 prop：因此要被推動的 prop 應同時 author `on_collision`
  trigger，否則會一路直線滑行到 lifetime 結束。

`impulse_resistance` 由 actor 或 prop entity template authoring（預設 0），語意是
**門檻而非減量**：未超過門檻時整個 impulse 被忽略，不做任何衰減。比較用的量值：
純量形式就是 `strength` 本身（位元不變，既有 template 的判定結果不可能移動）；
list 形式取 `max(水平, |垂直|)`，所以一個推不動重型目標的水平力道，仍可能靠夠大的
垂直分量把它頂起來。

```yaml
# actor 或 prop entity template
entity_type: prop
impulse_resistance: 15.0   # 有效強度 <= 15.0 的 impulse 完全推不動
```

目前只有 `ice_block.yaml` author 了這個欄位（`15.0`），其餘 template 一律使用預設
值 0。這個數字的實際效果值得記住：`action_rocket_explosion_at_target` 的
`strength: [12.0, 5.0]` 有效強度是 `max(12, 5) = 12`，低於 15，所以火箭的爆風推不動
冰塊——這是刻意的，掩體不該被一發火箭吹走。

Authoring 注意：

- Impulse 在 trigger 所屬 tick 的 command commit 階段套用，於**下一個** movement
  tick 才反映到位置。
- **水平擊退需要 `lockout_ticks` 才會成立。** Movement solver 每個 tick 由 input
  重新決定水平速度，而且不只玩家會被影響：repo 內每個 AI controller 都刻意每 tick
  送剛好一個 input（見 `game_server/src/agent_chaser_controller.cc` 的註解），sentry
  更會另外每 tick 無條件覆寫整個 `Velocity`。所以「沒有 input 的 AI actor 會保留
  水平速度繼續滑行」就 movement solver 單獨看雖然成立，實務上沒有任何 actor 處在
  那個狀態。`lockout_ticks > 0` 會在 target 上掛一個 server-only 的
  `ImpulseLockout`，在時效內同時擋掉兩個覆寫點：movement solver 不用 input 重建
  水平速度，`EntityStateSystem::set_velocity` 直接回 false。閘門放在 set_velocity
  這一層而不是各個 controller 裡，所以新寫的 AI controller 不必知道這條規則。
- Lockout 的解除條件是 **N ticks 或落地，取先到者**；但落地解除不會在武裝的那一
  tick 生效。apply_impulse 會把 actor 強制設為 airborne，站在地上的目標挨了純水平
  擊退後，同一個 movement step 就會重新落地——若不排除武裝當 tick，長距離平推
  剛好是 lockout 完全失效的那個 case。
- **為什麼垂直分量必須是絕對值。** Area effect 交給 apply_impulse 的
  `event.direction` 是「爆心 → 命中點」的徑向單位向量，爆心與目標同高時
  `direction.y ≈ 0`。若 list 形式的垂直數字是 `direction.y` 的縮放係數，乘上零
  還是零，一個平地爆炸無論垂直數字寫多大都做不出向上發射。改成絕對增量之後，
  `[8.0, 4.0]` 就直接讀作「水平推 8、垂直抬 4」，而 `[0.0, 10.0]` 是純發射。
  想要「垂直分量跟隨徑向的正負」時，純量形式就是現成的退路。
- **Area effect 預設打不到發射它的人**。overlap query 以 shooter 作為
  `ignored_entity_net_id`，而 spawn chain 會把 shooter 一路帶下去，所以 rocket 與
  它命中後生成的爆炸過濾的是同一個 actor。要做自推（rocket jump）必須在該 area
  effect projectile template 上 author `hit_instigator: true`；同一個 query 同時
  餵 damage 與 impact trigger，所以打開它等於連自傷一起打開。注意側別 mask 不能
  用來做這件事：`GameplaySide` 只掛在 prop 上，actor 完全不帶，所以 `hostile_side`
  之類的 mask 對 actor 不生效。
- Client prediction 只在「本地預測的 area effect projectile 擊中本地玩家」這條路徑
  上預先套用 impulse，而且該 area effect 必須 author
  `sync_mode: local_predicted_deterministic`（預設是 `server_snapshot_only`），
  若是本地玩家自己發射的，則該 template 必須 author `hit_instigator: true`——
  否則 authority 會把他過濾掉，預測就會套一個永遠不會被 snapshot 確認的位移。
  其餘情況一律等 authoritative snapshot。Client 對本地玩家預先套用 impulse 時
  也會同步起算 lockout，並用與 authority 相同的推導擋住預測用的水平 input——
  兩邊必須一致，否則預測會走開 N ticks 再被 reconciliation 硬拉回來。

目前 projectile-backed triggers 接受 `apply_damage`、`apply_health_change`、
`apply_impulse` 與 `spawn_projectile` actions。

Status lifecycle 目前支援的 actions 為 `apply_damage`、health change、status
apply/remove 與 speed modifier。Lifecycle safety contract 僅允許 `on_apply` 使用
damage、health change、speed modifier；`on_tick` 與 `on_expire` 不提交 speed
modifier，lifecycle callback 也不允許任意 nested status mutation。

Status replacement semantics：same-channel replace 依序執行 old `on_expire`、移除
old modifier、建立 new instance、執行 new `on_apply`。Refresh 保留 instance 與
stack count，只更新 latest source、owner peer 與 expiry。Stack 保留 instance 與
applied tick、增加 stack count；達到 max stack 時依 `refresh_on_stack` 決定是否只
更新 expiry。這些語意不由 Action Graph YAML 重新定義。

---

## 5. Multi-action Use Case：同時傷害自己與碰撞目標

這是 multi-action 的標準 vertical slice。Graph 定義兩個依序執行的 damage
actions：

```yaml
id: action_apply_damage_at_collision

parameters:
  source: null
  target: null
  self_damage: 1
  target_damage: 1

actions:
  - type: apply_damage
    target: params.source
    amount: params.self_damage

  - type: apply_damage
    target: params.target
    amount: params.target_damage
```

Prop binding：

```yaml
id: 201
name: collision_damage_prop
entity_type: prop

health:
  hp: 1
  max_hp: 1

physics:
  collider_template: collision_damage_prop_hitbox

triggers:
  on_collision:
    action_graph: action_apply_damage_at_collision
    parameters:
      source: self
      target: event.target
      self_damage: 1
      target_damage: 25
```

執行結果：

1. Batch 先確認 prop 與 collision target 都是有效 damage target。
2. 兩個 commands 通過 preflight 後，依 YAML 順序提交。
3. Prop 受到 1 點傷害；collision target 受到 25 點傷害。
4. 同一 logical collision event 重送時不會再次套用。

參考實作：

- `game_server/gameplay_catalog/action_graph_templates/action_apply_damage_at_collision.yaml`
- `game_server/gameplay_catalog/entity_templates/collision_damage_prop.yaml`

---

## 6. 其他 Use Cases

### 6.1 啟動 Prop 後傷害指定目標

```yaml
triggers:
  on_activated:
    action_graph: action_apply_damage_at_activated
    parameters:
      target: event.target
      amount: 25
```

Guideline：呼叫 activation API 時必須提供 target；若此互動不保證有 target，應改用
不依賴 `event.target` 的 graph。

### 6.2 Entity 銷毀時生成另一個 Entity

```yaml
triggers:
  on_destroy_entity:
    action_graph: action_spawn_entity_at_destroy_entity
    parameters:
      template: collision_damage_prop
      position: event.position
      owner: event.instigator
```

Binding 與 event snapshot 必須在原 entity 刪除前保存。Graph 不應再提交
`destroy_self`；實際刪除仍由 Entity Lifecycle System 負責。

### 6.3 Projectile 命中後生成 Explosion Projectile

```yaml
triggers:
  on_projectile_impact:
    action_graph: action_spawn_projectile_at_impact
    parameters:
      template: rocket_explosion
      position: event.position
      direction: event.direction
```

Guidelines：

- `hit_response` 負責 projectile 的 destroy/continue/bounce/attach outcome。
- Action Graph 只負責命中後的 gameplay side effects。
- 若 parent projectile 與 explosion 都有 damage，兩者採明確加成語意。
- Graph 只在 authoritative simulation 執行；client prediction 不得再次生成。

### 6.4 Projectile 到期後生成效果

```yaml
triggers:
  on_expired:
    action_graph: action_spawn_projectile_at_expired
    parameters:
      template: lingering_area
      position: event.position
      direction: event.direction
```

`on_expired` 與 `on_projectile_impact` 是不同 gameplay facts。兩者可使用相同
action schema，但 graph ID、trigger binding 與 exactly-once key 應清楚反映各自語意。

### 6.5 爆炸命中後造成傷害與擊退

Area effect projectile 對半徑內每個 target 各發一次 `on_projectile_impact`，因此
同一份 graph 會對每個受影響 target 各執行一次：

```yaml
id: action_rocket_explosion_at_target

parameters:
  target: null
  amount: 45
  strength: 12.0
  direction: null

actions:
  - type: apply_damage
    target: params.target
    amount: params.amount

  - type: apply_impulse
    target: params.target
    strength: params.strength
    direction: params.direction
    collision_mask: actor | prop
```

Projectile binding：

```yaml
triggers:
  on_projectile_impact:
    action_graph: action_rocket_explosion_at_target
    parameters:
      target: event.target
      amount: 45
      strength: 12.0
      direction: event.direction
```

此處的 `event.direction` 是爆心指向 target 的徑向向量，因此擊退方向隨相對位置改變。
若要的是固定方向（例如把每個 target 都往同一邊推），改成不在 binding 提供
`direction`，由 graph parameter 的 vec3 default 決定：

```yaml
parameters:
  direction: {x: 0.0, y: 1.0, z: 0.0}
```

但「徑向推開 **並且** 往上頂」不能靠改 `direction` 做到——平地爆炸的徑向向量
`y ≈ 0`，換成固定向上又會失去徑向。這是 `strength` list 形式存在的理由：

```yaml
parameters:
  strength: [8.0, 4.0]   # 沿徑向推 8 m/s，另外垂直抬 4 m/s
```

配合 `lockout_ticks` 才能讓水平那一半真的留在 target 身上，見 §4.4。

Guidelines：

- Damage 與 impulse 屬於同一個 batch：任一 command preflight 失敗時兩者都不提交。
- `collision_mask` 決定這個擊退是否也推得動 prop；只想推 actor 時省略即可。
- 擊退強度與傷害是各自獨立的數值，不應以 damage 反推 strength。

參考實作：

- `game_server/gameplay_catalog/action_graph_templates/action_rocket_explosion_at_target.yaml`
- `game_server/gameplay_catalog/projectile_templates/rocket_explosion.yaml`

---

## 7. Command Batch 與 Exactly-once 契約

每個 Trigger Event 產生一個有序 command batch。

### 7.1 Batch Preflight

1. 解析所有 binding expressions。
2. 驗證所有 parameter/action input types。
3. 驗證 runtime targets、template references 與 finite vectors。
4. 任一 command 驗證失敗時，整批拒絕，不提交 side effect。
5. 全部通過後才依 YAML 順序 commit。

目前支援的本機 simulation commands 會在 preflight 後提交。未來新增會呼叫外部
服務或產生不可逆 side effect 的 action 時，必須先提供 transactional queue、rollback
或其他明確的 all-or-nothing 機制，不能直接插入既有 batch commit loop。

### 7.2 Deduplication Key

```text
(requester peer, request_id, trigger event type, sequence)
```

- `requester peer` 將不同 peer 的 request namespace 分離。
- `request_id` 識別外部 request 或內部 deterministic event request。
- `trigger event type` 避免同一 request 的不同 gameplay fact 互相誤判。
- `sequence` 區分同一 request/tick 內的多個 event occurrence。

World-level ledger 使用 bounded retention，且以 authoritative simulation tick 作為
唯一時間單位。`history_ms` 只在 server tick config 初始化時換算成
`ceil(server_tick_rate * history_ms / 1000)` 個 ticks，再加一個 tick 作為 boundary
allowance；client presentation 的毫秒內插不參與 ledger retention。因此
exactly-once 只保證在此合法 replay window 內。
Lookup 使用 hash index，retention 使用獨立的 commit-tick ordering。每個 simulation
tick 集中 prune 一次，且使用 wrap-safe tick distance。

Batch 必須先完成 dedup reservation 才能提交 side effects。Duplicate 的處理結果是
idempotent success/no-op：不重新執行 commands，也不回報錯誤。Capacity 滿時不驅逐
仍在 window 內的 entry；reservation 失敗時不執行 side effect。失敗 batch 取消
reservation，成功 batch 才轉為 committed entry。

### 7.3 Deterministic Ordering

Dispatcher 依下列欄位排序 queued triggers：

```text
server_tick
→ event.subject
→ sequence
→ event.type
→ event.target
→ request_id
```

同 tick 可能多次發生的事件必須配置穩定且不重複的 sequence。不得使用永遠為 0 的
sequence 代表多個不同 collision/impact occurrences。

---

## 8. Kernel ABI 與 Compiled Representation

目前 Kernel ABI version 為 82；packet schema version 為 24，snapshot schema
version 為 19。

```text
KernelActionTriggerDefinition
├─ legacy first-action mirror
├─ action_count
└─ actions[KERNEL_MAX_ACTION_GRAPH_ACTIONS]
```

- `KERNEL_MAX_ACTION_GRAPH_ACTIONS = 8`。
- `action_count > 0` 時，固定 action array 是權威資料。
- Legacy scalar fields 暫時保存第一個 action 的 mirror，供過渡相容使用。
- Runtime compiler 將 ABI definitions 轉換為 `ActionGraphTemplate::actions` vector。
- Catalog hash 會包含 graph action list，action 順序改變也會改變 hash。

新增 action type 時必須同步更新：

1. YAML schema 與 loader。
2. `ActionGraphActionConfig`。
3. Kernel ABI action definition 與 ABI version。
4. Runtime compiled action variant。
5. Binding validator、evaluator 與 command executor。
6. Catalog hash。
7. C header、catalog、simulation 與 completion smoke tests。

---

## 9. Authoring Guidelines

### 9.1 應遵守

- Graph ID 使用 `action_<verb>_<context>` 等可搜尋且穩定的命名。
- Graph 描述可重用行為；entity-specific 數值與 reference 放在 binding。
- Required parameter 使用 `null`，可合理共用的數值才提供 default。
- 使用 `self`、`event.*` 表達 runtime context，不把它們當普通字串常數。
- 保持 action list 短小且具單一 gameplay 目的。
- 依 action 順序具有語意時，新增測試固定該順序。
- Projectile attribution 一律使用 execution provenance。
- 擊退方向以 gameplay 需求選擇來源：需要隨相對位置變化時綁 `event.direction`，需要
  固定方向時用 `direction` parameter 的 vec3 default。
- 生命週期 outcome 留在 Projectile/Entity Lifecycle System。

### 9.2 應避免

- 不要用 parameter 動態選擇 action type。
- 不要在 Action Graph 中處理 raw input mapping、按鍵 debounce 或 interaction range
  check。
- 不要引用 trigger schema 不提供的 `event.*` 欄位。
- 不要假設 optional `event.target` 永遠存在。
- 不要用 `apply_impulse` 表達持續性的位移或速度變化；持續效果屬於 status effect 的
  speed modifier，`apply_impulse` 只是一次性的速度增量。
- 不要讓 predicted client 與 authoritative server 各執行一次 gameplay side effect。
- 不要用多個 actions 模擬缺乏 transaction semantics 的外部工作流。
- 不要為 inventory-only item 強迫建立沒有 gameplay 必要的 ECS entity。

---

## 10. Validation 與常見錯誤

Catalog load/compile 會拒絕：

- 空白或重複的 graph/parameter ID。
- 0 個或超過 8 個 actions。
- 未支援的 action type 或 action field。
- Action 引用未宣告的 parameter。
- Binding 遺漏 required parameter。
- Binding 傳入未宣告或重複 parameter。
- Parameter type 與 action schema 不相容。
- 未知或不適用於 trigger 的 `event.*` expression。
- 無效 Entity/Projectile Template reference。
- Projectile trigger graph 使用 `apply_damage`、`apply_health_change`、
  `apply_impulse`、`spawn_projectile` 以外的 action。
- Status lifecycle graph 使用 `apply_impulse`。
- `apply_impulse` 的 `strength`：純量形式非有限或不為正；list 形式不是剛好兩個
  數字、水平為負、任一項非有限，或兩項同時為零。
- `apply_impulse` 的 `lockout_ticks` 超過 `KERNEL_MAX_IMPULSE_LOCKOUT_TICKS`。
- 非 `strength` parameter 使用 list default。
- `apply_impulse` 的 `collision_mask` 為空，或含 `actor`/`prop` 以外的 bit。
- `apply_impulse` 的 `direction` 既不是 `event.direction`，該 parameter 也沒有 vec3
  default。
- 非 `direction` parameter 使用 vec3 default，或 vec3 default 是零向量／非有限值。
- Entity template 的 `impulse_resistance` 非有限或為負。
- Projectile impact/expired graph reference cycle。

Runtime protection 包含：

- Binding event type 與實際 event 不一致。
- 無效/已不存在的 damage target 或 owner。
- 非 finite position/direction。
- 非正整數或超過 `uint16` 的 damage amount。
- 未通過形式檢查的 impulse strength、非有限或零長度的 impulse direction（整批拒絕）。
- Impulse target 不是 actor，也不是非攜帶中的 prop，或有效強度未超過
  `impulse_resistance`（略過該 command，不影響同批其他 commands）。
- 非 authoritative execution 不會產生 gameplay commands。

---

Owner peer 的 status full-state 使用 reliable delivery；remote action presentation
events 是 best-effort，遺失時由後續 reliable full-state 修正 owner state。Remote
presentation dedup 不改變 authoritative status ledger 的 exactly-once contract。

## 11. 新 Use Case 整合流程

1. 確認需求代表已發生的 gameplay fact，而不是 input command 或 state transition。
2. 選擇既有 trigger；若無適合 schema，再新增 typed Trigger Event。
3. 優先重用既有 graph，否則建立新的通用 graph template。
4. 在 entity/projectile template 提供 binding parameters。
5. 確認所有 `event.*` 欄位屬於該 trigger schema。
6. 確認 batch 內所有 actions 可提供一致的 transaction semantics。
7. 為 multi-action ordering、invalid expression 與 duplicate dispatch 增加測試。
8. 執行 catalog test、最小 simulation/kernel test，再執行 completion smoke。

Review checklist：

- [ ] Graph action count 在 1～8。
- [ ] Required parameters 都由 binding 提供。
- [ ] Action input types 可由 schema 唯一推導。
- [ ] Optional target 的空值行為已定義。
- [ ] Trigger event 有穩定 request ID 與 sequence。
- [ ] Duplicate dispatch 不會重複產生 side effect。
- [ ] Client prediction 不會執行 authoritative graph。
- [ ] Entity/projectile lifecycle 只有一個權威系統。
- [ ] Catalog hash 與 ABI 變更已納入測試。

---

## 12. Pending Status Semantics

- Resistance — pending
- Immunity — pending
- Dispel — pending

Pending 項目不是 authoring contract：不接受對應 YAML key、不保留 ABI 欄位、不建立
runtime 或 gameplay behavior tests。它們在未來定義前不影響目前 catalog、packet、
snapshot 或 status lifecycle semantics。

## 13. 延後項目：Item System Integration

以下項目尚未納入目前 Action Graph contract：

- `kItemUsed` / `OnItemUsedTriggerTag`。
- Inventory item trigger binding。
- `ItemInstanceRef`。
- 非 ECS item 的 `self` 語意。
- Item-used event dispatch 與 item lifecycle。

必須先由 Prop/Inventory System 決定 item 是 world entity、inventory-only instance、
template + quantity，或可在 world/inventory 間轉換的 instance。Action Graph 不應在
資料模型確定前把所有 item reference 強制收斂成 `EntityId`。

---

## 13. 相關檔案

- Authoring config：`game_server/src/gameplay_config.h`
- YAML loader/compiler：`game_server/src/gameplay_config.cc`
- Kernel ABI：`engine/src/kernel/public/kernel_types.h`
- Compiled runtime model：`engine/src/world/public/components.h`
- Evaluator/dispatcher：`engine/src/simulation/src/action_graph.cc`
- Entity command executor：`engine/src/simulation/src/systems.cc`
- Projectile command executor：`engine/src/simulation/src/projectile_system.cc`
- Exactly-once ledger：`engine/src/world/public/world.h`
- 遊戲內手測載具：`docs/ACTION_GRAPH_TEST_BOTTLES.md`
- 原始設計背景：`docs/plan/Trigger Event Tag 與 Action Graph 設計.md`
