# 遠程單位後撤（retreat）實作計劃書

需求：玩家逼近遠程單位時，遠程單位先後退，拉開到自身武器射程的最大距離附近，再攻擊玩家。

狀態：R0–R4 全部完成（2026-10-30，見 §7–§12）。巡邏隊成員也會後退（Q4 已推翻，見 §11）。airforce 巡邏隊由使用者自行設定。量測基準：main 4f295edc；實作基準：main 351993a2（已合併 `claude/ai-behavior-*`）。

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

---

## 8. R1 實作結果（2026-10-30，分支 `claude/ranged-retreat`）

**Q1–Q5 採用 §4 的建議**：後退時可以開火、超時放棄、不支援有腳單位、巡邏隊成員不後退、只看目前目標。

**改動**（只改 game_server；kernel、ABI、snapshot schema 都沒變）：

- `agent_chaser_controller.h`：新增 `AgentRetreatTuning`，掛在 `AgentChaseTuning::retreat`。
  - `trigger_distance_meters` 為 0 表示關閉，這是預設值。
- `agent_runtime.h`：每個 agent 新增後退狀態：
  - `retreating`、`retreat_ticks`、`retreat_cooldown_ticks`
  - 卡住偵測用的 `retreat_window_start` / `retreat_window_ticks`
- `gameplay_config.cc`：
  - 解析 `ai.locomotion.retreat`；不認得的欄位會被拒絕；`trigger_distance_meters` 必填。
  - 這些設定都會納入 catalog hash。
  - 載入檢查放在 `resolve_agent_skills` 的最後，因為那裡同時有武器射程、視野距離和 stop / resume。
  - `target_distance_meters` 沒寫時 = min(0.9 × 最遠武器射程, resume)。
  - 錯誤訊息以 `actor template ai.locomotion.retreat` 開頭。
- `agent_chaser_controller.cc`：
  - 新增 `update_retreat`，只在 pursuit 分支、而且目標看得到時，接在 `chase_move` 之後執行。後退時：
    - 取代 `move`，移動方向為遠離目標。
    - 朝向照舊面向目標。
    - 不啟動衝撞 / 跳撲。
    - `fire_while_retreating: false` 時不送攻擊 input。
  - 結束後退的條件（任何一種結束都會開始 cooldown，並設定 `chase_holding`，所以結束後不會馬上走回去）：
    - 距離 ≥ target。
    - 達到 `max_ticks`。
    - 卡住：每 15 ticks 檢查一次，實際移動距離 < 30% 的預期距離。
    - 距離 leash 不到 0.5 m。
    - 該 tick 沒有在追擊、或看不到目標。
  - 巡邏隊成員（`patrol.has_slot`）以及跳撲 / 衝撞中的單位不會後退。

**和計劃的差異：**

- R1 沒有用 `snap` 探路。chaser controller 拿不到 navmesh，只有 manager 有。改用「實際移動距離」偵測卡住，這擋得住牆和道具，但**擋不住懸崖或 navmesh 邊緣**（單位會掉下去）。懸崖與替代方向（±45° / ±90°）留給 R2，需要把 navmesh raycast 交給 controller。
- 卡住偵測沒有自動化測試：平地上沒有牆。R2 在真實地圖上測。

**測試 `ai_retreat_test`（新增，通過）：**

- 載入：
  - 合法設定可載入；target 沒寫時預設為 19.8 m。
  - 出貨的法師沒有 retreat（`trigger_distance_meters` 為 0）。
  - 以下 10 種錯誤寫法被拒絕：trigger ≥ stop、target < stop、target > resume、沒有 trigger、不認得的欄位、input_magnitude 為 0、max_ticks 為 0、sentry、有腳的巨人。
- 行為（平地；設定在程式裡指定，不依賴出貨內容）：

| 情境 | 結果 |
|---|---|
| A 法師，玩家站在 5 m | 後退 1 次、234 ticks，停在 19.04 m 不再移動；14 發，其中 11 發在後退中 |
| B 法師，玩家以 3 m/s 持續逼近（法師 1.8 m/s） | 後退 3 次，最長 90 ticks（= max_ticks）；最後原地應戰在 5 m；22 發，其中 8 發在後退中 |
| C 法師，`fire_while_retreating: false` | 後退中 0 發，停下後 6 發 |
| D 無人機，玩家站在 3 m | 後退 1 次、49 ticks，停在 9.53 m；3 次光束 |

**`bundle.bytes`：** 需要重新產生。出貨 template 雖然都沒有加 retreat，但 retreat 欄位已經納入 catalog hash，所有 template 的 hash 都會改變（和 `giant-chase-spacing` 的情況相同）。R4 加上法師與無人機的數值後，需要再產生一次。

---

## 9. R2 實作結果（2026-10-30，分支 `claude/ranged-retreat`）

**改動**（只改 game_server；kernel、ABI、snapshot schema 都沒變）：

- `PatrolNavigation::walkable_line(from, to, half_extent)`：包裝 Detour 的 `dtNavMeshQuery::raycast`。
  - 從 `from` 直線走到 `to`，途中遇到 navmesh 邊緣（懸崖、牆）就回傳 false。
  - `from` 對應 navmesh 時，垂直範圍只抓 ±3 m。這樣站在高台上不會被當成高台下方的地面。
  - 沒有 navmesh，或 `from` 不在 navmesh 上時，回傳 nullopt。
- `AgentChaserController::tick` 新增 `navigation` 參數（預設 nullptr）。manager 傳入自己的 `patrol_navigation_`。
  - 沒有把指標存進 controller 設定，因為 manager 可能被 move。
- 後退方向依序嘗試：正後方、±45°、±90°。每個方向往前探 2 m，必須仍在 navmesh 上。
  - 上一 tick 用的方向如果仍然可走，就繼續用，避免在兩個方向之間來回切換。
  - 五個方向都不行就是被逼到角落：結束後退，原地應戰。
  - 沒有 navmesh、或單位不在 navmesh 上時，視為可走，也就是 R1 的行為。
- hover 單位（`AgentChaserConfig::hovers`，從 template 的 movement controller 推得）不看 navmesh，因為它會飛越懸崖而不是掉下去。卡住偵測照舊適用。

**測試 `ai_retreat_test` 新增情境（通過）：**

| 情境 | 結果 |
|---|---|
| E 法師在高台上，背後 3 m 內是懸崖，載入 navmesh | 地點由測試在 navmesh 上搜尋，這次是 (97.0, 11.35, −93.0)。用了 0° / 45° / 90° 三個方向，沿著懸崖邊側移 10.4 m；0 ticks 離開 navmesh，沒有往下掉 |
| E' 同一地點，不載入 navmesh（對照組） | 直接往後走下懸崖，掉了 20.7 m，被 kill floor 移除 |

E' 證明 E 的結果確實來自 navmesh 檢查，不是地形剛好擋住。

**已知限制（留給 R3 或之後）：**

- **懸崖就在正後方時，只能側移，拉不開距離。** ±90° 的方向和玩家的距離幾乎不變：E 只從 5 m 拉到 6.16 m，之後由卡住偵測結束後退。安全上正確，但效果有限。如果希望這種情況下乾脆原地應戰，可以拿掉 ±90°（只剩正後方與 ±45°），由使用者決定。
- **卡住偵測仍然沒有自動化測試。** navmesh 已經把靜態的牆和道具挖掉，所以真實地圖上會觸發卡住偵測的，只剩其他角色或動態物件擋路。
- **測試地點是搜尋出來的。** navmesh 重新 bake 後地點會換，但測試不會因此壞掉。目前找到的是高台東緣、靠近地圖邊界的位置，懸崖下方就是地圖外。

---

## 10. R4 出貨內容（2026-10-30，分支 `claude/ranged-retreat`）

使用者決定只調整 gingerbread_mage 和 beam_drone（Q6）。

| 單位 | trigger | target | max_ticks | cooldown_ticks | 理由 |
|---|---|---|---|---|---|
| gingerbread_mage | 8 m | 19 m | **330（11 秒）** | 150 | 1.8 m/s 從 0 m 退到 19 m 約需 10.6 秒 |
| beam_drone | 4 m | **9.5 m** | 90（預設） | 150（預設） | target 不能超過 resume（10 m） |

**和 §4 草案的差異：**

- **法師的 `max_ticks` 不能用建議的 90。**
  - 90 ticks 只夠退 5.4 m。從 8 m 開始退，會停在約 13 m；那裡已經在 trigger 之外，之後不會再後退，所以永遠到不了 19 m。
  - 第一次試的 210 也不夠：玩家站在 5 m 時，會在 17.6 m 超時。
  - 因此改成 330，讓它從任何距離都能退到 19 m。
  - 代價：玩家持續追趕時，它會連續後退 11 秒，不過期間照常開火。
- **無人機的 target 從草案的 12 m 改成 9.5 m。** 草案的 12 m 超過 resume（10 m），會在載入時被拒絕。

**量測（`ai_retreat_test` 的 F、G，直接使用出貨的數值，平地）：**

| 情境 | 結果 |
|---|---|
| F 法師，玩家站在 5 m | 後退 234 ticks，停在 19.04 m；14 發，其中 11 發在後退中 |
| G 無人機，玩家站在 3 m | 後退 49 ticks，停在 9.53 m |

**目前在地圖上看不到這個行為（需要使用者決定）：**

- **gingerbread_mage 只會以 `1_patrol_gingerbread_warband` 巡邏隊成員的身分生成。** 依照 Q4，巡邏隊成員不會後退。
- **beam_drone 只屬於 `2_patrol_gingerbread_airforce`。** 這個巡邏隊目前沒有啟用（`gameplay_catalog.yaml` 的 `patrols: ids: [1]`），所以地圖上沒有無人機。
- 要在 Unity 裡看到效果，可以選擇以下其中一種：
  - (a) 讓巡邏隊成員也能後退（推翻 Q4；需要處理和隊形位置、巡邏隊 leash 的互動）。
  - (b) 啟用 airforce 巡邏隊（`patrols: ids: [1, 2]`），但無人機仍然是巡邏隊成員，同樣受 Q4 限制。
  - (c) 另外加一個單獨生成的法師 / 無人機（例如 world rule 的 `spawn_group`），它們是 guard，會後退。

**決定（2026-10-30）：(c)。** 在 game_rule（101）加入兩個節點 `lone_mage`、`lone_drone`。玩家抵達時，用 `spawn_group` 各生成一隻，和巨人的生成方式相同。world_rule（100）在出貨 catalog 裡沒有 preload，所以不用它。

| 單位 | 位置 | 地點檢查（navmesh） |
|---|---|---|
| gingerbread_mage | (−55, 0.8, −75) | 10 m 內高度 0.18～0.54 m，16 個方向都能走 20 m；離玩家出生點約 32 m，離 open combat 巨人的路線 19 m |
| beam_drone | (−62, 1.5, −88) | 離法師約 15 m；hover，不受 navmesh 影響 |

**量測（`ai_retreat_test` 的 H：出貨 catalog、真實地圖、preload 的 director、玩家從出生點抵達）：**

- 兩隻都是 guard，不是巡邏隊成員。
- 玩家放在法師面前 5 m：法師後退到 19.04 m。
- 玩家放在無人機面前 3 m：無人機後退到 9.53 m。

**注意：兩隻距離只有約 15 m，打其中一隻時，另一隻也會被拉進來。** 測法師時玩家在無人機的 20 m 視野內，所以輪到無人機時，它已經從 (−62, −88) 移動到 (−60.9, −80.9)。如果想分開測，需要把兩隻再拉遠。

**回歸測試：** 70 個測試通過，包括所有會啟動任務流程的測試。新增生成會讓之後生成的 net id 改變，所以特別包含了依賴生成順序的 `knockdown_recovery_test`、`pass_giant_route_test`、`shipping_catalog_flow_test`。

---

## 11. 改成巡邏隊成員後退（2026-10-30）

使用者決定取消單獨生成的法師與無人機，改由巡邏隊中的法師與無人機後退。

**改動：**

- **推翻 Q4：** `update_retreat` 不再排除巡邏隊成員（`patrol.has_slot`）。
  - 巡邏隊成員的後退仍受巡邏隊 leash 限制，從離隊的位置算起：法師 30 m、無人機 24 m，後退在離 leash 0.5 m 前停下。
  - 戰鬥結束後照舊走回隊形位置。
- **移除 game_rule（101）裡的 `lone_mage`、`lone_drone`**，恢復到 R4 時的內容。§10 的單獨生成已經撤回。
- **airforce 巡邏隊在出貨 catalog 裡仍然沒有啟用**（`patrols: ids: [1]`），地圖上依然沒有無人機。是否啟用由使用者決定。

**量測（`ai_retreat_test` 的 H：出貨 catalog、真實地圖、完整任務流程）：**

| 情境 | 結果 |
|---|---|
| warband 的法師（巡邏隊成員），玩家放在它面前 5 m | 後退到 19.01 m |
| airforce 的無人機（巡邏隊成員；測試在 catalog 複本中啟用 airforce），玩家放在 3 m | 後退到 9.50 m |

**測試時發現的兩件事：**

- **第一個 warband 生成後不到 2 秒就被 retire。** 它的路線只有約 20 m（18,−8 → 28,−25），成員因此離開巡邏隊。測試改成等一個「已經在巡邏隊中持續 2 秒」的成員。出貨時的巡邏路線可能也有很短的情況，但這不是本功能的範圍。
- **剛生成的巡邏隊成員，朝向還是生成時的方向**，下一 tick 才轉向巡邏隊的行進方向。測試改用 kernel 回報的 `vision_forward` 來決定玩家放在哪裡，不再自己從四元數換算（原本的換算在非單位旋轉下方向相反）。

---

## 12. R3 整合結果（2026-10-30，分支 `claude/ranged-retreat`）

airforce 巡邏隊由使用者自行設定，本階段不動 catalog 的 `patrols`。

**改動：**

- **spacing：** 後退時也套用 `chase.spacing_meters` 的推開力。
  - 做法：在選定的後退方向上加上 push，並限制在 `input_magnitude` 之內。
  - 加了 push 之後的方向必須仍然通過 navmesh 檢查，否則丟掉 push，只用原本的後退方向。
  - 原本後退時完全繞過 `chase_move`，所以同模板的單位在後退期間不會互相推開。
- **leash：** R1 已經做好（距離 leash 不到 0.5 m 就結束後退），本階段補上測試。
- **hunter：** 不用改程式；本階段補上測試。

**測試 `ai_retreat_test` 新增情境（通過）：**

| 情境 | 結果 |
|---|---|
| I 法師，guard leash 6 m，玩家在 5 m | 後退 92 ticks，離起點最遠 5.52 m（leash − 0.5）；**沒有進入 Return**；之後原地繼續開火（12 發，其中 8 發在後退結束後） |
| J 法師當 hunter，玩家在 5 m | 後退到 19.04 m 後停住，行為和 guard 相同 |
| K 兩隻法師相距 1 m，spacing 4 m | 後退結束時相距 **7.88 m** |
| K' 同上，spacing 0（對照組） | 相距 3.30 m |

K' 證明 K 的間距確實來自 spacing push，而不只是從同一點往外後退時自然散開。

**附註：**

- 出貨 template 裡只有巨人設定了 spacing，而巨人是有腳單位，不能後退。所以 spacing 的整合目前只會影響之後自行設定 spacing 的遠程單位。
- 法師與無人機的巡邏隊 leash 是 30 m、24 m，從離隊的位置算起。單次後退的位移最多約 19 m、9.5 m（玩家貼在面前時），所以只有在離隊後已經追了一段距離時，leash 才會提早結束後退。
