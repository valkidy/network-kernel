# 遠程單位後撤（retreat）實作計劃書

需求：玩家逼近遠程單位時，遠程單位先後退，拉開到自身武器射程的最大距離附近，再攻擊玩家。

狀態：R0 完成（2026-10-30，見 §7），R1 尚未開始。量測基準：main 4f295edc；實作基準：main 351993a2（已合併 `claude/ai-behavior-*`）。

---

## 1. 現況（main 4f295edc，2026-10-30 量測）

**結論：這個行為目前不存在。** 玩家逼近時，遠程單位停在原地繼續開火。

用真實地圖量測（暫時的 probe，未 commit）。玩家從單位前方 18 m 出發，以 3 m/s 直線走向單位，停在 3 m：

| 單位 | 停下 / 恢復追擊 / 射程 | 結果 |
|---|---|---|
| gingerbread_mage（31） | 16 m / 20 m / 22 m | 停在 16 m 後不再移動。玩家走到 3 m，它仍站著開火（10 秒內 5 → 20 發） |
| beam_drone（36） | 8 m / 10 m / 14 m | 一樣：停在 8 m 後不動，玩家到 3 m 時仍站著開火 |

原因都在 `game_server/src/agent_chaser_controller.cc`：

- **`chase_move`（main:518）只有「停下」與「靠近」兩種動作。**
  - 距離 ≤ `stop_distance_meters` 時進入 `chase_holding`，移動為 0。
  - 距離 ≥ `resume_distance_meters` 時才重新靠近。
  - 沒有任何「太近就後退」的分支。
- **間距邏輯會取消遠離目標的移動。** 4f295edc 加入的 spacing 會把偏離目標超過 90° 的移動直接歸零。註解寫明：有腳單位無法不轉身就後退。
- **`range_meters.min` 只決定能不能用武器，不會讓單位移動。** 距離小於 min 時，武器 skill 只是不能用；單位沒有任何移動反應。法師和無人機都沒有設定 min。

可以直接利用的現有條件：

- **追擊時朝向和移動是分開設定的。** 朝向由 `facing_rotation_from_vision_toward` 設定，移動由 `chase_move` 回傳（main:983–995）。所以沒有腳的單位理論上可以面向玩家倒退走，但需要先量測確認（R0）。
- **navmesh 已經載入。** `PatrolNavigation` 目前只提供 `snap`（把點拉到可走的地面上）和 `find_route`（找路線），沒有 raycast。

---

## 2. 移動速度是核心問題

| 單位 | 移動方式 | 速度（m/s） |
|---|---|---|
| player | character | **5.0** |
| gingerbread_mage | character | 1.8 |
| beam_drone | hover | 4.0 |
| hive_airship | hover | 1.5 |

玩家比所有遠程單位都快。玩家只要持續逼近，單位就永遠拉不開距離。

如果照字面實作「後退到最大射程**之後**才攻擊」，被追著的單位就會一直後退、一槍都不開。這比現在站著開火更弱。

所以有兩個決定一定要先做（見 §4 的 Q1、Q2）：

- 後退時能不能開火。
- 後退多久之後放棄，改成原地應戰。

---

## 3. 設計（草案）

### 3.1 設定

在 template 的 `ai.locomotion` 下新增 `retreat` 區塊。沒寫就是現在的行為：

```yaml
ai:
  locomotion:
    stop_distance_meters: 16.0
    resume_distance_meters: 20.0
    retreat:
      trigger_distance_meters: 8.0     # 比這更近就開始後退
      target_distance_meters: 19.0     # 退到這個距離就停下（預設 = 最遠武器射程 × 0.9）
      input_magnitude: 1.0             # 後退速度，佔 move_speed 的比例
      max_ticks: 90                    # 單次後退最多幾 ticks，到了就放棄後退、原地應戰（Q2）
      cooldown_ticks: 150              # 放棄或完成之後，多久不能再後退
      fire_while_retreating: true      # Q1
```

**距離帶。** 以法師為例：

| 距離 | 行為 |
|---|---|
| d < trigger（8 m） | 後退，直到 d ≥ target（19 m）、超時，或無路可退 |
| trigger ≤ d ≤ resume（8～20 m） | 停下開火。原本的 hold 區間從 [stop, resume] 往內延伸到 [trigger, resume] |
| d > resume（20 m） | 靠近，和現在一樣 |

**載入時檢查**（錯誤寫法直接拒絕）：

- 數值關係必須是 `trigger < stop ≤ target ≤ resume`。這樣退完之後會落在停下區，不會反覆後退又靠近。
- `target` 不能超過最遠武器 skill 的 `range_meters.max`，也不能超過視野距離。
- `max_ticks` 必須大於 0。
- 只允許用在 garrison 和 hunter。sentry 和巡邏隊成員見 Q4。
- 有腳的 template 一律拒絕（Q3）。

### 3.2 執行

在 `chase_move` 加一個後退分支，放在 hold 判斷之前。每個 agent 多存三項狀態：`retreating`、`retreat_ticks`、`retreat_cooldown_ticks`。

1. **開始條件：** 目標看得到、距離 < trigger、cooldown 為 0、不在跳撲 / 衝撞 / 蓄力中。
2. **方向：** 從目標指向自己的水平方向，也就是正後方。
3. **可行性：** 每 tick 往選定方向探 2 m（R2 用 navmesh raycast；R1 先用 `snap` 判斷前方那個點是否可走）。
   - 正後方不通時，依序試 ±45°、±90°。
   - 全部不通就是「被逼到角落」：結束後退，原地應戰，並開始 cooldown。
4. **結束條件：** 距離 ≥ target、達到 `max_ticks`、被逼到角落、或失去目標。結束時開始 cooldown。
5. **朝向：** 和現在一樣，每 tick 面向目標，只有移動方向朝後。
6. **開火：**
   - `fire_while_retreating: true`：照現有的 skill 選擇規則開火（D14），手上武器射程夠就射。
   - `fire_while_retreating: false`：後退期間不送 attack input。
7. **leash：** 後退的位移也算進 leash。超過 leash 時，視為被逼到角落。
8. **spacing：** 後退期間，spacing 的「遠離目標就歸零」規則要讓開，只套用同伴之間的推開。

### 3.3 不需要的改動

- **kernel / ABI / snapshot schema：** 不需要改。後退只是 game_server 送出的移動輸入，client 透過既有的 snapshot 看到。
- **`bundle.bytes`：** 只有在出貨 template 加上 `retreat` 之後才需要更新。
- **Unity：** 倒退走需要往後的動畫。client 已經會送 `MoveX` / `MoveY`（相對於面向的移動方向）給 animator；要不要做倒退的 blend 是 Unity 端的工作（使用者處理）。

---

## 4. 待決問題（實作前需要決定）

| # | 問題 | 建議 |
|---|---|---|
| Q1 | 後退時能不能開火？ | **可以**（`fire_while_retreating: true` 作為預設）。玩家的速度是法師的 2.8 倍；不開火的話，被追的法師永遠不會攻擊 |
| Q2 | 拉不開距離時怎麼辦？ | 後退 `max_ticks`（建議 90，3 秒）之後放棄，原地應戰，冷卻 `cooldown_ticks`（建議 150）後才能再後退 |
| Q3 | 有腳的單位（21/22/23/211/39）要不要支援？ | **第一版不支援**，載入時拒絕。有腳單位的身體會轉向移動方向，後退會背對玩家而失去視野。另外視野方向的修正（fbf21954）還在 `claude/giant-vision-melee`，沒進 main |
| Q4 | 巡邏隊成員要不要後退？ | 建議**不要**。巡邏隊成員有隊形位置與巡邏隊 leash，後退會打亂隊形。第一版只給 garrison / hunter |
| Q5 | 太近的判斷要用目前目標，還是最近的敵人？ | 第一版用**目前目標**。換成最近的敵人時，會和「目標仍看得到就不換」（D8）衝突 |
| Q6 | 要先套用在哪些出貨單位？ | **已決定（2026-10-30）：只調整 gingerbread_mage 和 beam_drone。** 數值草案：法師 trigger 8 / target 19；無人機 trigger 4 / target 12 |
| Q7 | 要做在哪個基準上？ | **已決定（2026-10-30）：`claude/ai-behavior-*` 已合併進 main（351993a2），從 main 開始。** 分支 `claude/ranged-retreat` |

---

## 5. 階段

| 階段 | 內容 | 改動 | 驗證 |
|---|---|---|---|
| **R0** | 量測，不寫功能碼：沒有腳的單位面向玩家倒退時，速度是否正常；hover 單位倒退是否正常；開火的 action 期間是否還能移動 | 無（暫時的 probe） | 法師、無人機各一次 |
| **R1** | `retreat` 設定與載入檢查；`chase_move` 的後退分支（正後方、`snap` 探路、`max_ticks`、cooldown、`fire_while_retreating`） | game_server | 新測試 `ai_retreat_test`（平地）：玩家停在 5 m，法師退到 ≥ 19 m 再開火；玩家持續追趕時，3 秒後放棄並原地開火；錯誤設定被拒絕 |
| **R2** | 被逼到角落與替代方向：在 `PatrolNavigation` 加 `raycast`（包裝 `dtNavMeshQuery::raycast`），依序試 ±45° / ±90° | game_server | 真實地圖：背後是牆或懸崖時改走側邊；三面都被擋時原地應戰 |
| **R3** | 和 leash / spacing 的整合；hunter 與 garrison 的回歸測試 | game_server | 既有的 AI 測試全部通過；量測後退不會讓單位離開 leash |
| **R4** | 出貨內容（Q6） | catalog | 依使用者的決定；需要更新 `bundle.bytes` |

ABI：無。snapshot schema：無。`bundle.bytes`：只有 R4 需要。

---

## 6. 成本控制

- 每個階段先用最小的 probe 或單一測試確認，再跑相關的 AI 測試（`ai_*`、`agent_*`、`patrol_*`、`*hunter*`）。不跑整個 repository 的測試，除非另外要求。
- 只讀和該階段直接相關的檔案：`agent_chaser_controller.cc/.h`、`patrol_navigation.cc/.h`、`gameplay_config.cc` 的 `ai.locomotion` 解析段落。
- 如果 R0 發現 hover 單位或開火期間不能倒退，就先停下來說明，再決定是否需要改 kernel。
- 每個階段完成時回報：分支與 commit、讀過的檔案、改過的檔案、搜尋、build / test 指令，以及 ABI / snapshot / `bundle.bytes` 是否有變動。

---

## 7. R0 量測結果（2026-10-30，main 351993a2）

用暫時的 probe 在真實地圖上量測，probe 沒有 commit。

**R0-1：kernel 層直接送輸入，不經過 AI。** probe 自己送移動輸入（`Kernel_ServerEnqueueEntityInput`），並固定朝向目標（`Kernel_ServerEnqueueEntityTransform`）。目標在西方，移動往東：

| 項目 | gingerbread_mage（character） | beam_drone（hover） |
|---|---|---|
| 往目標前進 | 1.80 m/s | 4.00 m/s |
| 面向目標後退，每 tick 重送朝向 | 1.80 m/s，朝向不變 | 4.00 m/s，朝向不變，高度維持 |
| 後退，不重送朝向 | 1.80 m/s，朝向不變 | 4.00 m/s，朝向不變 |

結論：

- 沒有腳的單位後退時，速度和前進一樣。
- kernel 不會把它們轉向移動方向；朝向完全由 AI 決定。
- hover 單位後退時高度正常。

**R0-2：後退時開火。** 這部分改用真正的 AI 開火。做法：暫時修改 `chase_move`，讓它在 hold 時改成遠離目標（只在 probe 用，沒有 commit）。玩家站著不動，起點距離：法師 10 m、無人機 5 m。

| 項目 | gingerbread_mage | beam_drone |
|---|---|---|
| 有 action 進行時的移動速度 | 1.80 m/s（319 ticks，沒有任何一 tick 停下） | 4.00 m/s（540 ticks，全程都在 action 中，沒有停下） |
| 沒有 action 時的移動速度 | 1.80 m/s | —（光束是持續按住的武器，一直在 action 中） |
| 20 秒內開火 | 18 發 | 6 次光束 |
| 狀態 | 一直維持 Attack（看得到目標） | 一直維持 Attack |

結論：

- 開火的 action 不會讓單位停下。action template 本來就沒有「定身」的 flag。
- 後退時照常選武器、照常開火（Q1 的建議可行）。

**R0 的附帶發現：**

- 只用 kernel、不 tick `GameServer` 時，probe 送出的開火 input 沒有啟動任何 action，原因沒有追查。改用真正的 AI 量測後就不需要了。R1 的測試應該經過 `GameServer`。
- 暫時修改的 `chase_move` 只是在 hold 時往後退，會在 stop 與 resume 之間來回（法師 16～20 m、無人機 8～10 m）。這正是 R1 要用 trigger / target 距離帶取代的行為。

**對計劃的影響：** 不需要改 kernel；R1 可以照 §3 進行。
