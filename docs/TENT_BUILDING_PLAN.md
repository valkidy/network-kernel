# 帳篷建築（Tent）實作計劃書

狀態：**設計 review 中，尚未實作 Task A 的 kernel / game_server 部分。**
分支：`claude/tent-building`（從 `main` 0152baf 開出）。
已有 commit：`076727d`（catalog + 可行性 probe）、`2557411`（kit 改為瓶子大小）。
最後更新：2026-10-01。

本文件整理 2026-09-30 的設計討論與實測結果，供重新 review 設計使用。
第 9 節列出目前設計已知的弱點與值得重新思考的地方。

---

## 1. 目標與範圍

設計目標（使用者原話整理）：

> 建築物 prop，提供可讓玩家 interact 的介面，但從生成開始有一個規範好的
> 存在時間，避免數量重複過多。例如：休息 UI 可供玩家整備、商店 UI 可讓玩家交易。

拆成兩個 task：

| Task | 內容 | 狀態 |
|---|---|---|
| **A：帳篷 prop** | 丟擲與生成、存在時間、數量上限、進出帳篷流程（移動遮罩、無敵、凍結輸入、倒數）、所有退出路徑 | catalog + probe 已做；kernel / game_server 未做 |
| **B：帳篷內互動（鍋爐）** | 投擲物品進鍋爐，依組合配方給全隊 buff | 使用者另行設計，未開始 |

不在範圍內：
- **中型箭塔**：已移到另一個對話（allied_beam_sentry 系的 actor，與帳篷的 prop 路線不同）。
- **Unity package build / stage / bump**：由使用者處理。

---

## 2. 已定案的決策

| # | 決策 | 備註 |
|---|---|---|
| D1 | 帳篷是建築 prop，有 HP，HP 歸 0 時被破壞（同蟲巢），住客被迫退出 | 不照抄蟲巢 `on_health_depleted` 反傷攻擊者 |
| D2 | 丟擲流程比照 ice block：瓶子大小的 kit 飛行，落地後在接觸點展開成帳篷 | 見 §3.1 |
| D3 | 落地推開單位：沿用靜態障礙物的物理推開，不另外掛 AoE action graph | 實測有效，見 §4 |
| D4 | 落在 prop 上懸空、或撞到 prop 側面半嵌入：都視為「玩家失誤」，只能等帳篷自然消失 | **硬性要求：退出時出口點必須先確認是空的** |
| D5 | 存在時間：生成起 L = 5400 ticks（3 分鐘）；第一位玩家進入時，剩餘時間縮為 min(剩餘, N)，N = 900（開發）/ 1800（正式） | 只會縮短，不會延長 |
| D6 | UI 必須顯示帳篷剩餘秒數，避免操作到一半被無預警中斷 | 透過 status 的 `expire_tick` 達成，不改 schema |
| D7 | 帳篷自成一個 population group；第一版全場上限 4、擠掉最舊的；最終目標是每人上限 | 背包會限制攜帶數量；4 人小隊開局 1 個帳篷 |
| D8 | 進入帳篷的共用規則：玩家移到帳篷內部 + 無敵 + 暫停輸入；內部 UI 依建築種類各自設計 | 休息帳篷與商店共用 |
| D9 | 進入帳篷不再單純回血；回血等 buff 改由 Task B 的鍋爐決定 | `tent_rest` status 只負責倒數 |
| D10 | Task B：配方是組合；丟歪不消耗道具；每個帳篷有投放次數上限 | 細節未定 |

---

## 3. 架構

### 3.1 Catalog（已 commit）

```
tent_kit (item 3010)              背包裡的道具：pickupable, throwable
  └─ throw: identity_preserving → tent_kit_prop
tent_kit_prop (entity 215)        飛行中的瓶子，collider = collision_damage_prop_hitbox (1 m 方塊)
  └─ on_collision terrain|static_obstacle
       → action_spawn_ice_and_damage_self_at_collision
         spawn tent @ event.position，自毀
tent (entity 214)                 一般 prop（非 item-backed）
  ├─ lifecycle: 5400 ticks, population_group: tent
  ├─ health 600, impulse_resistance 15
  ├─ collider: tent_hitbox (32)  oriented_box, 半寬 1.2, 高 2.4, layer damageable
  ├─ interaction: [interactable], range 2.0
  └─ on_activated → action_noop
prop_population_rules: tent (id 2, max_alive 4)
```

為什麼要拆成 kit 和 tent：**item-backed prop 不能宣告 lifecycle 或 population**
（`gameplay_config.cc:8721`），帳篷兩者都需要。拆開後的附帶好處：帳篷不是世界道具，
無法被撿回去（實測 pickup 被拒，`InvalidContext`）。

碰撞盒尺寸的三個限制：
- 必須完整包住 `player_hit_aabb`（半寬 0.35、高 1.8）並留餘量，住客才不會露出可被直接命中的部位。
- 半寬不能太大，否則近戰 chaser 被牆擋住、到不了攻擊距離。
- 高度 ≥ 半寬，避免被推開的單位從屋頂出去。

### 3.2 進出流程（未實作）

由 game_server 新增的 `ShelterDirector` 負責。

**進入**（收到 `EntityActivated`，目標是帳篷，且玩家不在任何帳篷內）：
1. 若是這個帳篷的第一位住客：`ClampPropLifetime(tent, N)`，取得剩餘時間 R。
2. 設玩家移動遮罩為 `terrain`（`Kernel_ServerSetEntityMovementCollisionMask`，已存在）。
3. 把玩家移到帳篷中心（`Kernel_ServerSetEntityTransform`，已存在）。**順序必須是先設遮罩再移動**，否則下一個 tick 會被推出去。
4. 上入住鎖（凍結移動輸入、拒絕動作與 domain action，只放行對這個帳篷的 Activate）。
5. 設 `DamageImmunity` 為 R ticks；掛 `tent_rest` status，持續時間為 R。兩者在帳篷自然消失時同時到期。

**退出**（任一觸發）：
1. 在帳篷周圍找空的出口點（物理 overlap 查詢；帳篷嵌在 prop 或牆裡時要往外找）。
2. 把玩家移到出口點。
3. **之後**才恢復遮罩（0 = 引擎預設）。順序顛倒會被推往最短軸方向。
4. 解除入住鎖、清除無敵、移除 `tent_rest`。

**退出觸發一覽：**

| 觸發 | 偵測方式 | 備註 |
|---|---|---|
| 住客再按一次互動 | `EntityActivated`（玩家已在這個帳篷內） | 入住鎖唯一放行的請求 |
| 帳篷 HP 歸 0 | 帳篷 despawn | |
| 帳篷存在時間到 | 帳篷 despawn（reason Expired） | **不觸發 `on_destroy_entity`** |
| 被數量上限擠掉 | 帳篷 despawn（reason CapacityEvicted） | **不觸發 `on_destroy_entity`** |
| 玩家死亡 | 死亡事件 | 需確認與復活流程的互動 |
| 玩家斷線 | PlayerLeft | |

### 3.3 Kernel 需要新增的部分（未實作）

| # | 項目 | 用途 | ABI 影響（預期） |
|---|---|---|---|
| K1 | `KernelEventType_EntityActivated`（`net_id` = 被啟動者，`code` = 啟動者） | game_server 目前**完全無從得知**遠端玩家啟動了什麼：request 在 kernel 內處理，結果只回給該 peer | 新 enum 值；需確認舊 client 會忽略未知事件 |
| K2 | `Kernel_ServerClampPropLifetime(net_id, max, &remaining)` | 首次進入時縮短存在時間 | 新 export |
| K3 | `Kernel_ServerSetEntityDamageImmunity(net_id, ticks)` | 住客無敵（`DamageImmunity` 已存在，目前只有復活時設定） | 新 export |
| K4 | `Kernel_ServerApplyStatusEffect` / `Kernel_ServerRemoveStatusEffect`（可覆寫持續時間） | `tent_rest` 倒數；Task B 的 buff 也會用到 | 新 export |
| K5 | 入住鎖 `Kernel_ServerSetEntityShelter(net_id, shelter_net_id)` | 凍結輸入與動作，只放行對該帳篷的 Activate | 新 export |

新 export 依慣例放在 capability flag 後面，不升 ABI（見 memory `additive-abi-no-bump`）。
macOS 的 export 清單在 BUILD.bazel、Windows 在 .def，兩邊都要加。

### 3.4 Client（Unity，使用者負責）

- 依 `template_id` 開對應的 UI（休息 / 商店）。
- 倒數：進入前用 `spawn_tick + L`；進入後讀本地玩家 `tent_rest` 的 `expire_tick`（`Kernel_QueryStatusEffects`，本地玩家可靠）。
- UI 開著時不送移動輸入（避免預測與伺服器拉扯，見 §7 T3）。
- 帳篷 despawn 或玩家死亡時關閉 UI。

---

## 4. 實測事實

來源：`//game_server:tent_feasibility_probe_test`（`game_server/tests/tent_feasibility_probe_test.cc`），2026-09-30。

| 項目 | 結果 |
|---|---|
| 平地落點 | 帳篷生成在 kit 停下位置前方約 0.6 m（`event.position` 是接觸點，不是停止位置） |
| 平平地丟向冰塊 | kit 撞到側面，帳篷生在側面上，半嵌入冰塊（重疊約 3.4 m³） |
| 遮罩只有 terrain（冰塊瓶子的設定） | kit 穿過冰塊，帳篷完全在冰塊內（約 9.2 m³） |
| 帳篷生成時的旋轉 | 依 `event.direction` 只轉 yaw（實測 90°），不傾斜 |
| 互動距離 | 3D 距離。地面距中心 1.8 m 可互動；2.5 m 被拒（OutOfRange）。**可互動的範圍只有 1.6–2.0 m 的窄環** |
| 撿起帳篷 | 被拒（帳篷不是世界道具） |
| 落點下的單位 | 帳篷出現後，所有單位（含玩家）被推出最近的一面，全部在帳篷外 |
| 四位住客、遮罩只有 terrain | 4 人都留在中心不動。`terrain|actor` 也一樣。預設遮罩的對照組：4 人都被推出去 |
| beam 射擊住客 | 帳篷吃下全部 43 發，住客 0 發 |
| 法師榴彈 | 帳篷被打中，**住客仍被範圍傷害打到**（需要無敵） |
| spammer | 連對照組都沒打中任何人，無結論 |
| kit 投擲距離 | 沿用 `grenade_shell_projectile`，飛了約 40 m |

其他 probe 中確認的事：prop 的 entity state 回報的 template id 是 0；`owner_peer` 為 0 建立的 prop 屬於 hostile 陣營（實際丟出的帳篷繼承丟擲者的 peer，是玩家陣營）。

---

## 5. 存在時間與數量

- **L = 5400**：從生成開始計時。懸空或半嵌入的帳篷也是等這段時間結束。
- **N = 900 / 1800**：第一位玩家進入時，剩餘時間縮為 min(剩餘, N)。4 人共用同一段時間。
- 目前 kernel 沒有在執行中修改剩餘時間的 API（`PropLifecycle.remaining_lifetime_ticks` 只在生成時從 template 設定）→ 需要 K2。
- 數量：`tent` group 上限 4，滿了擠掉最舊的（目前唯一的溢出規則）。最終目標是每人上限（依 `owner_peer` 分組，需改 kernel）。

---

## 6. Task B 摘要（鍋爐，使用者另行設計）

已定案：組合配方、丟歪不消耗、每個帳篷有投放次數上限。

目前的構想：
- 瞄準和投擲只是 client UI 的表演，伺服器不模擬軌跡。
- 命中時送 `KernelGameplayRequest`：`target_net_id` = 帳篷，新增 domain action `Offer`。
- 伺服器驗證後消耗道具，記錄到該帳篷的鍋爐狀態。
- 配方表放在 game_server catalog（帳篷 template 上），不放 action graph（graph 不能依道具分支，也不能以「所有玩家」為目標）。

已知限制：
- status 能執行的效果只有傷害、回血、移速修正。其他 buff 類型都要新增 kernel 的 status action。
- 鍋爐狀態要同步給帳篷內所有住客的 UI，目前沒有同步管道，可能要改 schema。
- 同一個 channel 的 status 會互相覆蓋。

Task A 為 B 預留的接口：
- 「帳篷 → 住客」的查詢；
- 帳篷消失的通知；
- 入住鎖的請求白名單（A 只放行 Activate，B 要加入 `Offer`）。

---

## 7. 測試計畫

| # | 項目 | 狀態 |
|---|---|---|
| T1 | 架在 prop 上的帳篷，站在地面碰不到 | 部分：嵌在側面的情況已測（碰不到）；落在頂上未測 |
| T2 | 敵方攻擊會被帳篷擋住 | 已測：beam 擋住、榴彈的範圍傷害擋不住 |
| T3 | pure client 預測本地玩家時是否和伺服器互相拉扯（移動遮罩只存在伺服器，不同步） | **未測，風險最高**；需兩個 process，等流程實作後 |
| T4 | 落地推開單位 | 已測 |
| T5 | 每條退出路徑都安全，無敵與 status 都清除；嵌入時出口點空著 | 未測，需要流程 |
| T6 | 帳篷不會重疊 prop | 已測：**會重疊**，已決定接受（D4） |
| T7 | 四人同住不互推 | 已測 |

---

## 8. 待決問題

1. 退出方式：「在帳篷裡再按一次互動」是否可以？
2. 玩家死亡時的退出：死亡和復活流程如何與入住狀態互動？
3. kit 需要自己的丟擲軌跡（目前約 40 m）：期望距離是多少？
4. 互動距離：目前 2.0 只有 0.4 m 寬的可用範圍。改成 2.5，架在冰塊頂上的帳篷仍碰不到（地面距離至少 3.4 m）。
5. N 在正式版是 1800；4 人共用，是否足夠完成整備與鍋爐互動？

---

## 9. 已知弱點與值得重新思考的地方

以下是目前設計中，我認為 review 時值得重新檢視的點：

1. **空帳篷不吸引攻擊。** 原始需求是「有 HP，會吸引 hostile_side 攻擊」。目前的設計靠「住客仍是目標、帳篷包住住客」讓帳篷挨打，所以**沒人住的帳篷幾乎不會被攻擊**，HP 只有在有人休息時才有意義。真正讓敵人主動攻擊 prop，需要把 prop 加入敵人的視野候選（kernel 改動）。chaser 的接近距離是以目標中心計算，也要一起改。
2. **一個建築需要 5 項 kernel 改動。** K2 到 K5 是互相分離的原語：遮罩、無敵、鎖、status 分開設定，game_server 要在每條退出路徑都記得逐一清除，漏一項就會留下「永久無敵」或「永久穿牆」的玩家。替代方案是在 kernel 裡做單一的「shelter」概念：進入時一次設定遮罩、無敵、鎖；解除時一次全部清除。export 比較少，清除也是原子操作，但比較不通用。
3. **移動遮罩只存在伺服器。** 這是 T3 的風險來源。如果實測確實互相拉扯，要嘛把遮罩同步給 client（改 schema），要嘛 client 在帳篷內停止預測本地玩家。
4. **撞到側面就半嵌入，是丟擲時很常見的情況。** 平平地丟幾乎都會撞到側面。雖然已接受為玩家失誤，但玩家的感受可能是「莫名其妙浪費一個帳篷」。如果之後想改，選項有：生成前檢查位置（`require_clear`，擋到就不生成並退還道具），或新增「停止位置」這個事件來源（只能消除 0.6 m 的偏差，解決不了側面問題）。
5. **住客全部疊在同一點。** 4 位住客在伺服器上位置完全重疊；client 端如果仍渲染住客的模型，需要隱藏或另外安排。
6. **入住期間看不到外面。** 休息 UI 開著時世界照常運行，住客對帳篷外的威脅（例如即將被打爆）只能靠帳篷 HP 判斷。UI 可能需要顯示帳篷 HP。
7. **Population 第一版是全場共用。** 多人時，A 丟的新帳篷可能擠掉 B 正在休息的帳篷。開局只有 1 個帳篷道具、上限 4 時很少發生，但最終需要每人上限。
8. **帳篷是 player side，但敵人的 AoE 也會打到帳篷**（法師榴彈實測打中帳篷）；而玩家自己的 AoE（例如 frag 瓶，對所有陣營造成 1000）同樣會炸掉自己的帳篷。是否需要友軍保護，未討論。
9. **spammer 的攻擊對帳篷是否有效，沒有結論**（對照組就沒命中）。spammer 的投射物沒有指定陣營，依文件記載會穿過所有指定陣營的 collider，可能完全無視帳篷。

---

## 10. 相關檔案

- Catalog：`game_server/gameplay_catalog/entity_templates/{tent,tent_kit_prop}.yaml`、`collider_templates/tent_hitbox.yaml`、`item_templates/tent_kit.yaml`、`action_graph_templates/action_noop.yaml`、`gameplay_catalog.yaml`（population rule）
- 測試：`game_server/tests/tent_feasibility_probe_test.cc`
- 相關 kernel 位置：
  - `kernel.cc` 視野候選迴圈（只看有 vision config 的實體）
  - `systems.cc` 飛行中 prop 落地（`static_contact`）、`update_prop_lifetimes`、`enforce_prop_population_limit`
  - `damage_system.cc`（`DamageImmunity`）
  - `kernel_api.h`（`Kernel_ServerSetEntityMovementCollisionMask`）
- 參考前例：蟲巢的出兵流程（`spawner_director.cc`，先設遮罩再移動；出口點依碰撞盒驗證）
