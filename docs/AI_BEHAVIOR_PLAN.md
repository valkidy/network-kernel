# AI behavior 通用化與 remote command 實作計劃書

基準：main 4f295edc。前一份計劃：`docs/AI_CONTROLLER_PLAN.md`（D1–D19，P0–P6 已完成）。
本文件的決策編號用 B1…。D1–D4（行為）與 E1–E4（remote command）已於 2026-10-29 決定，見 §10 與 B8–B16。

---

## 1. 目標與範圍

目標：

- **(a)** AI agent 的行為由 YAML 決定：guard、patrol、lock_target。
- **(b)** runtime 可以動態切換行為，切換時帶上該行為需要的資料，資料不足就拒絕。
- **(c)** 玩家可以透過 remote command 改變友軍（`player_side`）agent 的行為。
- 之後新增行為（例如 `follow`）只需要新增一個 behavior class 與測試，不改 chaser 本體。

在範圍內：

- game_server：behavior 介面、chaser 重構、`set_behavior` 入口、lock_target、依 camp 建敵人表、
  remote command 的權限與資料檢查。
- kernel：AgentCommand 的傳送（client export、packet、server poll export）。
- catalog：友軍 template（camp、身體 layer、揮擊 mask 改到玩家這一側）。

不在範圍內：

- **`follow`（跟隨玩家）**：介面預留，本輪不實作。
- **Unity 端**：C# mirror、指令 UI、表現，由使用者處理。
- **Unity package build / stage / bump、merge 到 `feat-unity-plugin`**：由使用者處理。

---

## 2. 現況（main 4f295edc）

| 項目 | 現況 | 位置 |
|---|---|---|
| 角色 | `AgentControllerRole`：sentry、garrison、patrol（巡邏隊成員）、hunter | `agent_runtime.h:94` |
| 角色來源 | template 的 `default_controller`，生成來源的 `controller:` 可覆寫（舊計劃 D1） | `agent_runtime_manager.cc:630` |
| runtime 切換 | 底層已可行：每個 tick 用 `agent.controller_role` 查 binding，改值下一個 tick 生效。唯一使用處是 hunter 的 `on_target_lost: garrison` | `agent_runtime_manager.cc:246`、`:556` |
| chaser 的 role 分支 | 949 行中約 10 處依 role 分支，集中在三個地方 | 見下表 |
| 敵人表 | hunter 的 broad phase **只收玩家**，不看 camp | `agent_runtime_manager.cc:471` |
| vision | kernel 依 camp 判斷敵友（`classify_agent_relation`），只回報一個 `current_target_candidate` | `kernel.cc:278`、`ai_perception_adapter.cc:203` |
| client → server | 只有 `KernelGameplayRequest`（Consume…Transfer），整條流程在 kernel 內；game_server 不在這條路上 | `kernel_types.h:973`、`:2030` |
| 傷害的敵我 | 由 collider 的 `layer` / `collision_mask` 決定，**不看 camp** | 例：`39_projectile_gingerbread_giant_sweep_hit.yaml` |

chaser 的分支點：

| 分支點 | 位置 | 內容 |
|---|---|---|
| 沒看到目標時 | `agent_chaser_controller.cc:551–570` | hunter → Seek；squad → 回隊伍位置；garrison → 走 route；都沒有 → 站著 |
| 遺忘目標後 | `:668`、`:838`（兩份重複） | squad → Return；hunter → Seek；garrison → 另一套 |
| leash | `:113` | 依 `has_slot` 讀 patrol 或 garrison 的值 |

---

## 3. 決策

| # | 決策 | 備註 |
|---|---|---|
| B1 | **採方案 C：behavior class**。只在現有分支點抽介面；Alert / Chasing / Attack / Search 的共用 state machine 只留一份 | 不把每個行為拆成完整 controller，避免追擊與攻擊邏輯重複 |
| B2 | **sentry 維持獨立 controller** | 不會移動，結構和 chaser 不同 |
| B3 | **behavior 自己持有資料並自己檢查**（`validate()`）；所有切換都走同一個 `set_behavior` | YAML 生成、內部事件、remote command 共用同一條路 |
| B4 | **guard 永遠切得成功**：沒給 anchor 就用當下位置，是所有失敗與 `on_target_lost` 的萬用退路 | |
| B5 | **lock_target 不無限追擊**：`on_target_lost` + `give_up_ticks` 收尾 | 避開舊計劃 D9（pursuer）的未決問題 |
| B6 | **remote command 用專用 packet（方案 R3）**；kernel 只負責傳送與 peer → player 對應，權限與資料檢查全在 game_server | R1（擴充 gameplay request）與 R2（道具 + graph action）的比較見 §8.1 |
| B7 | **ABI 不 bump**：新的 export 放在 capability flag 後面；**packet schema 要 bump** | client 和 server 必須用同一版 |
| B8 | **改名**（D1）：YAML 用 `guard` / `patrol` / `lock_target`；現在的巡邏隊成員 `patrol` 改名 `squad`；`garrison` 與舊的 `patrol` 在載入時拒絕 | catalog 一次遷移完；不留別名，避免 `patrol` 兩種意思並存 |
| B9 | **lock_target 只鎖敵對 camp，但保留彈性**（D2）：目標關係由 `target_relations` 決定，本輪載入驗證只接受 `[hostile]` | 之後要「鎖定我方 actor 補血」時，放寬驗證並讓 skill 挑選能對友軍出手，不用改介面 |
| B10 | **交戰中切換立刻放手**（D3） | 切換當下生效，見 §5.2 |
| B11 | **remote command 改變同一隻 agent 的行為，間隔至少 10 ticks**（D3）：太快就拒絕，回報 `too_soon` | 只限制 remote command；內部事件（`on_target_lost`、`give_up_ticks`）不受限。間隔寫在 YAML（`remote_command.min_switch_interval_ticks`，預設 10） |
| B12 | **觸發來源**（D4）：本輪只做內部事件與 remote command；game rule 觸發等有用例再做 | |
| B13 | **任何活著的玩家都能指揮任何 `player_side` agent**（E1） | 不記 owner，沒有重連換 net_id 的問題；檢查流程保留一步給之後的 owner 規則 |
| B14 | **一個指令只指揮一隻 agent**（E2） | 要指揮一群，由 client 送多個指令 |
| B15 | **remote command 的 patrol 只能選 catalog 的 route**（E3），指令帶 `route_id` | 現在的 route 都寫在生成來源裡，沒有 id；要新增具名 route 的 catalog（C5 前置） |
| B16 | **回報結果**（E4）：`request_id` + `status` + `rejection_reason` | 見 §8.4 |

---

## 4. behavior 介面

```cpp
class AgentBehavior {
public:
    virtual ~AgentBehavior() = default;
    // 切換前檢查資料；失敗就不切換，回傳拒絕原因。
    virtual BehaviorRejection validate(const AgentRuntimeState&, const BehaviorData&) const = 0;
    // 切入時接手資料並重設自己的狀態；切出時清掉。
    virtual void enter(AgentRuntimeState&, const BehaviorData&) = 0;
    virtual void exit(AgentRuntimeState&) = 0;
    // 沒看到目標時的移動（取代 :551–570）。
    virtual void on_idle(AgentRuntimeState&, const BehaviorContext&) = 0;
    // 遺忘目標後要進的 state（取代 :668、:838）。
    virtual AgentSentryState on_target_forgotten(AgentRuntimeState&) = 0;
    // leash 的 anchor 與距離；沒有 leash 時回傳空值（取代 :113）。
    virtual std::optional<Leash> leash(const AgentRuntimeState&) const = 0;
    // 用 vision 的候選，或用鎖定的 id。
    virtual std::uint32_t pick_target(const AgentRuntimeState&, const PerceptionSnapshot&) const = 0;
};
```

介面是從 §2 的分支點整理出來的。C1 的重構如果發現第四個分支點，就補進介面，不在 chaser 裡加 if。

---

## 5. 行為與資料

| 行為 | 對應現有 | 需要的資料 | validate 拒絕條件 |
|---|---|---|---|
| `guard` | garrison，沒有 route | anchor（預設為當下位置）、leash 可選 | 無（B4） |
| `patrol` | garrison + route | route（世界座標，至少 2 點） | route 少於 2 點 |
| `lock_target` | 新增 | 目標 net_id、`on_target_lost`、`give_up_ticks` | 目標不存在、已死，或關係不在 `target_relations` 內（B9） |
| `squad` | 現在的 `patrol`（巡邏隊成員） | 巡邏隊 slot | 不能由 `set_behavior` 切入；只由巡邏隊指定 |
| `hunter` | 現有 | `target_source`、`on_target_lost` | 沿用舊計劃 P6 |

名稱依 B8；舊名（`garrison`、指巡邏隊成員的 `patrol`）在載入時拒絕。

YAML 草案（合併順序沿用舊計劃 D1：程式預設 → template → 生成來源）：

```yaml
ai:
  default_behavior: guard
  behaviors:
    guard:       {leash_meters: 15.0, leash_resume_meters: 5.0}
    patrol:      {leash_meters: 15.0, leash_resume_meters: 5.0}   # route 由生成來源給
    lock_target: {on_target_lost: guard, give_up_ticks: 600, target_relations: [hostile]}
  remote_command:
    min_switch_interval_ticks: 10   # B11；只有 player_side 的 template 能寫
```

### 5.1 lock_target 細節

- 目標位置用 net_id 從 actor snapshot 讀（舊計劃 D12）。vision 只用來判斷看不看得到。
- **待查證**：kernel vision 能否回答「指定的目標在不在 visible 列表裡」（`max_visible_hostiles`
  存在，但 adapter 只取 `current_target_candidate`）。不行的話要在 adapter 補讀。
- 看不到時沿用 hunter 的 Seek（navmesh 尋路，沒有 navmesh 就直線）。
- 目標死亡、離開世界、超過 `give_up_ticks` → 切到 `on_target_lost` 指定的行為。
- 目標重連後 net_id 會變：視為目標失去，不追蹤。
- `target_relations`（B9）：`validate()` 用 `classify_agent_relation` 判斷目標關係。本輪載入驗證只接受
  `[hostile]`。之後開放 `ally`（例如補血）時還要處理：skill 的 `when` 能依目標關係挑選、
  item / weapon 能對友軍出手，以及鎖定友軍時不該進入 Attack 的近戰距離邏輯。

### 5.2 切換時的狀態

- 預設：回到 Idle，清掉 seek path、route cursor 和注意點。
- 正在交戰中被切換：立刻放手（B10）。清掉目標與 engagement，進行中的 action 照常結束
  （actions advance every tick，不強制中斷），之後由新行為接手。
- remote command 造成的切換，同一隻 agent 間隔至少 `min_switch_interval_ticks`（B11）。
  agent 上記錄最後一次 remote 切換的 tick；內部事件的切換不更新它，也不受它限制。
- lock_target 切入時直接把目標設成 attention point，跳過 Idle。

---

## 6. 依 camp 建敵人表

- `update_hunters` 的敵人表改成依 camp 建立：對每個 camp 收集它的敵對 actor（玩家與 agent 都算），
  沿用 `classify_agent_relation` 的規則。
- 帳篷、死亡、斷線的排除條件不變（舊計劃 Q3）。
- 沒有任何 `player_side` agent 時，只建敵方的表，成本和現在一樣。
- 這一步不做的話，`player_side` 的 hunter 或 lock_target 會去追玩家。

---

## 7. 友軍 template（catalog）

以 gingerbread giant 為例，**新增**一組友軍 template，不改 39（地圖仍把 39 當敵人生成）：

| 項目 | 敵方（39） | 友軍 |
|---|---|---|
| entity template `camp` | `enemy_side` | `player_side` |
| bounds collider `layer` | `hostile_side` | `player_side` |
| skeleton `collision_flags.layer` | `hostile_side` | `player_side` |
| 揮擊 projectile `collision_mask` | `player_side` | `hostile_side` |
| weapon | 22 | 新的 weapon，指向新的 projectile |

id 依 catalog naming rule 與當時的最大 id 決定，並注意平行分支的 id 衝突。
**待查證**：玩家武器的 mask 是否都是 `hostile_side`（會不會誤傷友軍）；
敵方武器是否都打 `player_side`（友軍會不會被打傷）；area effect 用 `actor` mask 的會敵我都打。

---

## 8. remote command

### 8.1 方案比較

| 方案 | 做法 | ABI / schema | 結論 |
|---|---|---|---|
| R1 | gameplay request 加 domain action `CommandAgent` | struct 要擴充，packet schema 要 bump | 不採用：把 AI 指令塞進道具與物件的語意 |
| R2 | 道具 → `on_item_used` → 新 graph action `set_agent_behavior` | graph action 依往例要 bump ABI | 不採用：一次只能帶一個目標和一個位置，指令和道具綁死 |
| **R3** | 專用 AgentCommand packet | 新增 export（capability flag，不 bump ABI）；packet schema 要 bump | **採用**（B6） |

### 8.2 傳送路徑

```
client: Kernel_SubmitAgentCommand(cmd)
   → reliable packet（新 packet type）
server kernel: 記下 requester 的 player net_id，放進佇列
game_server: Kernel_ServerPollAgentCommands → 檢查 → set_behavior → 回報結果
```

指令格式（草案）：

```c
typedef struct KernelAgentCommand {
    uint32_t struct_size;
    uint64_t request_id;
    uint32_t agent_net_id;     // 一次一隻（B14）
    uint8_t  behavior;         // KernelAgentBehavior_*
    uint8_t  reserved0;
    uint16_t route_id;         // patrol：catalog 的具名 route（B15）
    uint32_t target_net_id;    // lock_target
    KernelVec3 position;       // guard 的 anchor
} KernelAgentCommand;
```

`requester_peer` 與 requester 的 player net_id 由 server kernel 填入，不信任 client 送來的值。

### 8.3 game_server 端的檢查（依序）

1. 下指令的玩家活著。
2. agent 存在，camp 是 `player_side`。
3. 權限：本輪任何活著的玩家都可以（B13）；這一步保留給之後的 owner 規則。
4. 行為是可由 remote command 切入的：`guard`、`patrol`、`lock_target`；`squad`、`hunter` 不行。
5. 這隻 agent 距離上次 remote 切換已滿 `min_switch_interval_ticks`（B11），否則回報 `too_soon`。
6. 該行為的 `validate(data)` 通過；patrol 的 `route_id` 要存在於 catalog（B15）。
7. 每個 peer 的指令頻率上限（防洗封包，和 B11 的每隻 agent 間隔是兩回事；數值實作時量測後決定）。

拒絕原因（`rejection_reason`）草案：`requester_dead`、`agent_missing`、`not_player_side`、
`behavior_not_allowed`、`too_soon`、`invalid_data`、`rate_limited`。

### 8.4 結果回報（E4）

採用（B16）。比照 `KernelGameplayRequestOutcome` 做 `KernelAgentCommandOutcome`
（request_id、status、rejection_reason），由 game_server 透過 export 交給 kernel 回傳給 client。
outcome 的保存上限沿用 gameplay request 的教訓：每個 peer 斷線時清除，dedicated server 設上限。

---

## 9. 階段

| 階段 | 內容 | ABI / schema | 驗證 |
|---|---|---|---|
| C1 | 純重構：squad / garrison / hunter 搬成 behavior class，**行為完全不變** | 不變 | 現有 AI 測試全綠（main 自 2026-10-25 起 199/199，任何紅燈都是回歸） |
| C2 | `set_behavior` 入口、`validate()`、`enter` / `exit`；YAML 格式與改名（B8，catalog 一次遷移） | 不變；**`bundle.bytes` 要更新**（使用者處理） | 新測試：guard ↔ patrol 互切、資料不足被拒絕、交戰中切換立刻放手（B10）、舊名載入被拒絕 |
| C3 | lock_target | 不變 | 新測試：鎖定、Seek、目標死亡後切到 `on_target_lost`、`give_up_ticks` |
| C4 | 依 camp 建敵人表；友軍 template（catalog） | 不變；**`bundle.bytes` 要更新**（使用者處理） | 新測試：友軍 lock_target 追敵人、不追玩家；友軍揮擊傷敵不傷玩家 |
| C5 | remote command：具名 route catalog（B15）、client export、packet、server poll export、檢查流程（含 B11 的 10 ticks 間隔）、outcome | 新增 export（不 bump ABI）；**packet schema 要 bump** | kernel 層級的兩個 peer 測試：接受，以及 §8.3 的每一種拒絕（含 `too_soon`：第 9 tick 被拒、第 10 tick 接受） |

順序理由：C1 先證明介面能承載現有三個行為；C3 是第一個驗證「新增行為只需要一個 class」的案例；
C5 只是另一個觸發來源，依賴 C2 的 `set_behavior`。

schema bump 時記得更新寫死版本號的三個測試（見 memory：schema bump touch points）。

---

## 10. 已決事項（2026-10-29）

| # | 問題 | 決定 | 落在 |
|---|---|---|---|
| D1 | 命名 | 改名：guard / patrol / lock_target，巡邏隊成員改為 `squad` | B8 |
| D2 | lock_target 的目標範圍 | 只鎖敵對 camp，但保留彈性（例如之後鎖定我方 actor 補血） | B9 |
| D3 | 交戰中被切換 | 立刻放手；remote command 改變同一隻 agent 的行為間隔至少 10 ticks | B10、B11 |
| D4 | 觸發來源 | 內部事件 + remote command；game rule 觸發之後再做 | B12 |
| E1 | remote command 權限 | 任何玩家可指揮任何友軍 | B13 |
| E2 | 指令粒度 | 一次一隻 | B14 |
| E3 | patrol route 來源 | 只能選 catalog 的具名 route | B15 |
| E4 | 結果回報 | 做 outcome | B16 |

仍待查證（實作時處理）：kernel vision 能否查指定目標是否可見（§5.1）；玩家與敵方武器 mask 是否只打對面（§7）；
每個 peer 的指令頻率上限數值（§8.3）。

---

## 11. 交付

每個階段在 `claude/*` 分支上 commit 並測好後交給使用者，同時說明：

- 分支名稱與 commit。
- ABI 或 snapshot / packet schema 是否變動（C5 會變 packet schema）。
- `bundle.bytes` 是否需要更新（C4 會）。

C# mirror、Unity UI、package 的 build 與 merge 由使用者處理。
