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

目前實作版本日期：2026-07-26。

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
params.*          = binding/default 解析後的 graph parameters
```

`self` 與 `event.subject` 常相同，但語意不同，不應在 compiler 中合併。

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
| `on_projectile_impact` | `subject`, `instigator`, `target`, `position`, `direction` | 命中後生成爆炸物 |
| `on_expired` | `subject`, `instigator`, `position`, `direction` | projectile 到期後生成效果 |

注意事項：

- `on_activated.event.target` 是 optional request input。若 graph 使用它，activation
  command 必須提供有效 target。
- Projectile 命中 world geometry 時，`on_projectile_impact.event.target` 可能為空；
  不需要 entity target 的 graph 應只使用 position/direction。
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

目前 projectile-backed triggers 只接受 `spawn_projectile` actions。

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

- `game_server/action_graph_templates/action_apply_damage_at_collision.yaml`
- `game_server/entity_templates/collision_damage_prop.yaml`

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
(request_id, trigger event type, sequence)
```

- `request_id` 識別外部 request 或內部 deterministic event request。
- `trigger event type` 避免同一 request 的不同 gameplay fact 互相誤判。
- `sequence` 區分同一 request/tick 內的多個 event occurrence。

成功 commit 後才將 key 寫入 World-level ledger。Duplicate 的處理結果是 idempotent
success/no-op：不重新執行 commands，也不回報錯誤。失敗 batch 不寫入 ledger，可在
條件修正後重試。

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

目前 Kernel ABI version 為 52。

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
- 生命週期 outcome 留在 Projectile/Entity Lifecycle System。

### 9.2 應避免

- 不要用 parameter 動態選擇 action type。
- 不要在 Action Graph 中處理 raw input mapping、按鍵 debounce 或 interaction range
  check。
- 不要引用 trigger schema 不提供的 `event.*` 欄位。
- 不要假設 optional `event.target` 永遠存在。
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
- Projectile trigger graph 使用非 `spawn_projectile` action。
- Projectile impact/expired graph reference cycle。

Runtime protection 包含：

- Binding event type 與實際 event 不一致。
- 無效/已不存在的 damage target 或 owner。
- 非 finite position/direction。
- 非正整數或超過 `uint16` 的 damage amount。
- 非 authoritative execution 不會產生 gameplay commands。

---

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

## 12. 延後項目：Item System Integration

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

- Authoring config：`game_server/gameplay_config.h`
- YAML loader/compiler：`game_server/gameplay_config.cc`
- Kernel ABI：`engine/src/kernel/public/kernel_types.h`
- Compiled runtime model：`engine/src/world/public/components.h`
- Evaluator/dispatcher：`engine/src/simulation/src/action_graph.cc`
- Entity command executor：`engine/src/simulation/src/systems.cc`
- Projectile command executor：`engine/src/simulation/src/projectile_system.cc`
- Exactly-once ledger：`engine/src/world/public/world.h`
- 原始設計背景：`docs/plan/Trigger Event Tag 與 Action Graph 設計.md`
