# AI behavior 通用化與 remote command 實作計劃書

基準：main 4f295edc。前一份計劃：`docs/AI_CONTROLLER_PLAN.md`（D1–D19，P0–P6 已完成）。
本文件的決策編號用 B1…。D1–D4（行為）與 E1–E4（remote command）已於 2026-10-29 決定，見 §10 與 B8–B16。

**狀態（2026-10-29）：C1–C5 全部完成**，在疊起來的五個分支上，尚未 merge：
`claude/ai-behavior-c1` → `c2` → `c3` → `c4` → `c5`（`58568555`）。§4–§8 是計劃當時的設計；
實際做法與計劃不同的地方見 **§12 實作紀錄**，以 §12 為準。

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

各階段的分支、commit 與實際結果見 §12。

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

當時待查證的三件事，結果見 §12.6：kernel vision 能否查指定目標是否可見（可以）；玩家與敵方武器 mask
是否只打對面（友軍巨人的部分已用測試確認）；每個 peer 的指令頻率上限（暫定 16 / 秒，未量測）。

---

## 11. 交付

每個階段在 `claude/*` 分支上 commit 並測好後交給使用者，同時說明：

- 分支名稱與 commit。
- ABI 或 snapshot / packet schema 是否變動（C5 會變 packet schema）。
- `bundle.bytes` 是否需要更新（C4 會）。

C# mirror、Unity UI、package 的 build 與 merge 由使用者處理。

---

## 12. 實作紀錄（2026-10-29）

### 12.1 分支與 commit

| 階段 | 分支 | commit | ABI / schema | `bundle.bytes` |
|---|---|---|---|---|
| C1 | `claude/ai-behavior-c1`（從 main 4f295edc） | `92290225` | 不變 | 不需更新 |
| C2 | `claude/ai-behavior-c2`（疊在 C1 上） | `1e348115` | 不變 | **要更新**（catalog key 改名） |
| C3 | `claude/ai-behavior-c3`（疊在 C2 上） | `d1253ca3` | 不變 | **要更新**（catalog hash 納入 lock_target 設定） |
| C4 | `claude/ai-behavior-c4`（疊在 C3 上） | `191caa14` | 不變 | **要更新**（新 template） |
| C5 | `claude/ai-behavior-c5`（疊在 C4 上） | `58568555` | ABI 不變（新增 export，capability bit 53）；**packet schema 33 → 34** | **要更新**（`agent_routes` 納入 hash） |

client 和 server 必須一起升到 packet schema 34。Unity 端的 C# mirror（bit 53、兩個 struct、4 個
DllImport）、entity 40 的 view、指令 UI 都還沒做，由使用者處理。

### 12.2 C1：behavior class

- 新檔 `game_server/src/agent_behavior.h/.cc`。chaser 原本依 role 分支的地方（沒看到目標時、遺忘目標後兩份、
  leash、Return）改成呼叫 hook；Alert / 追擊 / Search / Seek 仍是 chaser 共用的一份。
- 選行為的順序：`has_slot` → squad，其次 hunter，其餘 guard，與原本的 if 順序一致。唯一的差異是「既是
  hunter 又有 slot」的 agent，正式環境不會出現（巡邏隊成員一定是 squad）。

### 12.3 C2：改名與 runtime 切換

與計劃不同、或計劃沒寫到的部分：

- **YAML 的 key 也改名**（使用者決定）：`default_controller` → `default_behavior`，`controllers` →
  `behaviors`，生成來源的 `controller:` → `behavior:`。舊 key 與舊名（`garrison`、指巡邏隊成員的
  `patrol`）在載入時拒絕，錯誤訊息會指出新名稱。導演 template 的 `ai.controller: director` 不變。
- **有 route 就是 patrol**（使用者決定），`type: patrol` 可省略；patrol 必須有 route；template 不能預設
  patrol（template 沒有 route）。runtime 和載入驗證也會推斷：程式碼組出來、只有 route 沒有 role 的 spec
  一樣視為 patrol，避免 route 被默默丟掉。
- **guard 與 patrol 各有自己的 leash**（使用者決定）：`behaviors.guard`、`behaviors.patrol`；hunter 用
  guard 的 leash。C++ 的 tuning 型別改為 `AgentSquadTuning`（巡邏隊）與 `AgentLeashTuning`。
- 實際的 hook：`validate`、`enter`、`exit`、`leash_meters`、`on_idle`、`on_return`、
  `on_target_forgotten`；C3 再加上 `locked_target`、`after_perception`（取代 §4 草案的 `pick_target`）。
  guard / patrol 的資料（route 的 navmesh 展開）由 runtime 套用，behavior 只負責檢查與清掉自己的狀態。
- `set_behavior` 不允許：sentry 跨到其他行為（或反過來）、切進 squad、巡邏隊成員切出去。
- 生成來源的 spec、hunter 的 `on_target_lost: guard` 都走同一個 `apply_behavior`；這兩條不經過
  `set_behavior` 的檢查，也不放手正在打的目標（與原本行為一致）。

### 12.4 C3：lock_target

- 只能由 `set_behavior` 建立；`default_behavior` 和生成來源寫 `lock_target` 會被拒絕（沒有目標 id）。
- 看得到 = 目標在 agent 的 vision 三份清單（hostile / ally / neutral，各最多 16）裡。只看鎖定的目標，
  不理會其他敵人；看不到時沿用 hunter 的路徑欄位走 Seek，每 `repath_ticks` 重新尋路；沒有 leash。
- 結束條件：目標消失或死亡，或連續 `give_up_ticks`（預設 600）tick 沒看到 → 切到 `on_target_lost`
  （guard 或 hunter）。
- `behaviors.lock_target.target_relations` 本輪只接受 `[hostile]`（B9）。其他行為收到帶
  `target_net_id` 的 spec 會拒絕。
- **與 §5.2 不同，且維持如此（使用者決定，2026-10-29）**：看到目標後仍先經過 Alert（`alert_ticks`，
  巨人與 gingerbread 是 90 ticks）才追擊，不跳過。lock_target 與其他行為共用同一個狀態機，行為一致。

### 12.5 C4：camp 敵人表與友軍巨人

- hunter 的敵人表依 camp 分兩份：敵方 hunter 看玩家 + `player_side` agent；友方 hunter 看敵方 agent，
  只在場上有友方 hunter 時才建。**行為改變**：敵方 hunter 現在也會追 `player_side` agent（例如
  `allied_beam_sentry` 27）。template → camp 用 hash map 快取。1 km bench 沒有重新量。
- 友軍巨人：entity 40 `allied_gingerbread_giant`、collider 60、projectile 40、weapon 23，與 39 只差
  camp、身體 layer、骨架 layer、揮擊 mask 這幾處。沒有任何生成來源會生出它。id 在 main 4f295edc 上是空的，
  merge 時要和其他分支對一次號碼。

### 12.6 C5：remote command

- **傳送方式**：實作時發現 kernel 已有通用的 game message channel（ABI 101，loadout 在用），可以不改
  kernel 就做完；**使用者決定維持 R3 專用 packet**。
- kernel：`Kernel_SubmitAgentCommand` / `Kernel_PollAgentCommandOutcomes`（client 或 listen host）、
  `Kernel_ServerPollAgentCommands` / `Kernel_ServerSendAgentCommandOutcome`（server），capability bit 53
  （`0x0020000000000000`）。`KernelAgentCommand` 32 bytes、`KernelAgentCommandOutcome` 24 bytes，
  KernelAbiInfo 不變。requester 由 server 從 session 填入。queue 上限 256，超過發 Error event 36。
  packet 34 / 35。
- **指令 struct 與 §8.2 草案不同**：沒有 `position`（guard 的 anchor 沒做，見 §12.7）；多了 server 填的
  `requester_net_id`。
- **檢查順序與 §8.3 不同**：頻率限制改為第一步，避免洗封包時還做其他檢查。peer 斷線時清掉它的紀錄。
  頻率限制後來分成兩層，見 §12.9。
- 間隔寫在 template 的 `ai.remote_command.min_switch_interval_ticks`（預設 10），只有 `player_side`
  template 能寫。只有指令成功的切換會重新計時；內部事件的切換不受影響。
- **route catalog 與 §8 不同**：沒有開新的 catalog 目錄，改成 `gameplay_catalog.yaml` 裡的
  `agent_routes:` 清單（id、name、waypoints，世界座標），不用改 52 個測試 glob。附了一條 route 1
  `camp_perimeter`，座標綁在 4zones v2 地圖上，換地圖要跟著改。

### 12.7 測試

| 測試 | 涵蓋 |
|---|---|
| `ai_behavior_switch_test`（新） | 每種拒絕、guard ↔ patrol（patrol 2 秒走 9.83 m、guard 0.00 m）、交戰中切換立刻放手、hunter 進出、各行為讀自己的 leash；lock_target 的每種拒絕、忽略身旁敵人、追到 2.9 m、目標消失轉 guard、`give_up_ticks` 後放棄 |
| `ai_allied_agent_test`（新） | 友軍巨人揮擊打到敵人、不傷在揮擊範圍內的玩家；敵人打得傷它；友方 hunter 追敵人而不是較近的玩家；敵方 hunter 追友軍巨人 |
| `ai_agent_command_test`（新） | §8.3 每種拒絕；第 9 tick `too_soon`、第 10 tick 接受；20 個裡 4 個被限流、下一秒恢復；listen host 上 submit → GameServer → outcome 的完整來回 |
| `agent_command_test`（kernel，新） | codec、偽造的 requester 被換掉、無 session 的 peer 不被聽到、256 上限、listen host 路徑、capability |
| 既有測試 | 7 個依新名稱更新期待值，並加上舊 key、舊名、guard 帶 route、patrol 沒 route 的拒絕案例 |

最後一次全跑（C5，`//engine/...` + `//game_server/...`）：213 個中 212 個通過。
`weapon_template_loader_test` 在 main 4f295edc 上本來就紅（weapon 22 用了 `shared_reload`，測試只允許
10/11/20；友軍的 weapon 23 也一樣）。`world_rule_director_test` 在平行全跑時曾一次 bind port 失敗，
序列跑 5/5 通過。`terrain_scaling_bench` 的原始檔不存在，build 不起來，與本計劃無關。

### 12.8 仍待決定 / 未做

- guard 的 anchor：已做，見 §12.11。
- 允許 lock_target 鎖定友軍（補血）：格式已預留（B9），需要 skill 依目標關係挑選、item / weapon 能對友軍出手。
- `follow`（跟隨玩家）：介面可以承載，本輪沒做。
- `weapon_template_loader_test` 的允許清單要加上 22 和 23。
- Unity 端（C# mirror、entity 40 的 view、指令 UI）由使用者處理。

### 12.9 指令冷卻（2026-10-29，`claude/ai-behavior-command-cooldown` `2649ba15`，疊在 C5 上）

使用者的意圖：讓玩家感覺 AI agent 接指令比較遲緩，避免短時間內頻繁切換狀態，讓每個指令都是慎重的決定。

- **每個玩家的冷卻**：玩家的指令被執行後，`agent_commands.peer_cooldown_ticks`（`gameplay_catalog.yaml`，
  預設 150 = 5 秒）內他送的所有指令都以 `RateLimited` 拒絕。只有**被執行**的指令才開始冷卻，被拒絕的
  （打錯目標、不是友軍）不算。設為 0 就沒有冷卻。
- **洗封包防護**：每個 peer 每秒 16 個，所有指令都算，與冷卻分開。實作時發現防護窗口每秒重設時把冷卻紀錄
  一起清掉（冷卻實際只有 1 秒），已修正並由測試涵蓋。
- **不延遲執行**：被接受的指令立刻生效（與 B10 一致），遲緩感只來自下一個指令要等。client UI 可以顯示冷卻。
- **每隻 agent 的間隔**維持 10 ticks（B11）：主要的限制是玩家冷卻；agent 的間隔只在多個玩家指揮同一隻
  agent 時有作用。
- 測試（`ai_agent_command_test`）：第 0 和第 149 tick 被拒、第 150 tick 執行（跨過多個防護窗口）；另一個
  玩家不受影響，只受 agent 的 10 ticks 限制；被拒絕的指令之後馬上送正確的指令會被執行；20 個洗封包中
  4 個被擋。

### 12.10 guard 的站崗位置（anchor）：做之前的現況（2026-10-29，已由 §12.11 取代）

- guard 沒有固定的站崗位置。它的 leash anchor（`agent.patrol.leash_anchor`）是**開始追擊時站的位置**：
  從 Idle 看到目標進入 Alert、從 Idle / Seek 被打、從 Seek 看到目標時寫入。
- leash 只在 `behaviors.guard.leash_meters` > 0 時作用：追出這個距離就進 Return，直線走回 anchor，到
  `leash_resume_meters` 內再回 Idle。目前**沒有任何 template 設 guard 的 leash**，所以 guard 會一直追到
  看不到目標為止。
- 失去目標（Search 超過 `forget_ticks`）後 guard 直接在當下位置回到 Idle，**不會走回 anchor**；只有超過
  leash 才會回去。所以打完一場後 guard 會停在戰鬥結束的地方。
- 舊計劃 D5 寫「garrison 的 anchor 為蟲巢或路線上最近的點」，P2b 實作時改成「開始追擊的位置」，蟲巢的
  位置從沒有傳給 agent。
- 蟲巢（`208_prop_gingerbread_nest.yaml`）的 `spawner:` 沒有 `behavior:` 區塊，生出的 gingerbread 用
  template 預設的 `default_behavior: guard`；`28_gingerbread.yaml` 只有 `behaviors.squad`（巡邏隊用），
  沒有 `behaviors.guard`，所以 leash 為 0。生出來後先走 `entry` 的出口路線（`exit` 在蟲巢外 3 m），走完
  在出口原地 guard，四處張望。spawner 的 `behavior:` 只能寫 `type`（和 hunter 的參數），不能寫 route。
- 要做「指定站崗位置」需要：guard 的資料多一個 anchor（預設為當下位置，B4）；失去目標後走回 anchor（目前
  不會）；生成來源可以給 anchor（例如蟲巢給自己的位置）；remote command 的 struct 加上 position（append
  在 struct 尾端，struct_size 判斷新舊）。

### 12.11 guard 的站崗位置（anchor）：實作（`claude/ai-behavior-guard-anchor` `ade284ce`，疊在冷卻上）

使用者決定（2026-10-29）：走回崗位用 navmesh 路徑；回去的路上照常接戰；remote command 指定的位置限制在
agent 30 m 內；蟲巢的守軍以走出蟲巢的位置為崗位，不需要 YAML。

- **崗位**：guard 第一次進入 Idle 時把當下位置記為崗位（B4）；`set_behavior` 的 spec 可以帶
  `has_anchor` / `anchor` 指定。切成 guard 時清掉舊崗位，沒指定就再取當下位置。只有 guard 收 anchor，
  其他行為收到會拒絕；非有限值拒絕。
- **走回去**：Idle、離崗位超過 1 m 時，runtime 用 navmesh 規劃一次路徑（借用 `seek_path`，沒有 navmesh
  就直線），guard 沿路走回去；離開 Idle（接戰、因 leash 進 Return）就丟掉路徑，下次回到 Idle 再規劃。
  路上看到敵人照常進入 Alert。到崗位後照舊四處張望。**行為改變**：所有 guard 打完都會回到崗位，以前會停在
  戰鬥結束的地方。leash 的語意不變（仍從開始追擊的位置算）。
- **蟲巢守軍**：走完 `entry` 出口路線後第一次 Idle 的位置，即蟲巢外 3 m 的出口附近。同一個蟲巢的守軍
  崗位都在出口附近，會擠在一起。
- **remote command**：`KernelAgentCommand` 的 `reserved0` 改成 `flags`（`KERNEL_AGENT_COMMAND_FLAG_POSITION`），
  並在尾端加上 `position`：struct 48 bytes、AgentCommand packet payload 32 bytes。C5 的格式尚未 merge，
  所以仍是 packet schema 34（沒有再 bump）。game_server 只在 Guard 上接受位置，且必須在 agent 30 m 內；
  其他行為帶這個 flag、或有不認得的 flag bit，都以 InvalidData 拒絕。
- **測試**：新的 `ai_guard_anchor_test`（被拉開 8.2 m，目標消失後 195 ticks 回到崗位並留在那裡；
  `set_behavior` 指定的崗位走得到；沒指定時取當下位置；hunter / patrol 帶 anchor、非有限值都被拒絕）；
  `ai_agent_command_test` 加上 20 m 接受、40 m 與錯誤 flag 拒絕；kernel codec 測試涵蓋 flags 與 position。
- **`knockdown_recovery_test` 的修正**：它原本挑「最後找到的蟲巢守軍」，從 -x 丟瓶子。守軍守在蟲巢出口後，
  蟲巢正好擋在那隻 agent 和玩家之間，恢復後走不過去（另一次則是被另一隻守軍擋到瓶子）。改成測試自己在空地
  放一隻 gingerbread，取樣從 100 ticks 加長到 150（飛行在第 40 tick 落地，原本的窗口剛好不夠）；斷言不變。
- 全跑：214 個中 213 個通過，只剩原本就紅的 `weapon_template_loader_test`。
- **觀察到、尚未處理**：追擊時 agent 直線走向目標，遇到蟲巢這類障礙物會卡住（不是 anchor 造成的，但守軍
  守在蟲巢旁邊讓它更常發生）。
