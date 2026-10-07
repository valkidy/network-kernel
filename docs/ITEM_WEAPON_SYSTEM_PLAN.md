# 道具系統與法杖武器系統 實作計劃書

狀態：**設計已定案（第九輪）。P1（K1–K4）已實作（`claude/item-weapon-p1`）；P2（K10 + 配裝模板）已實作（`claude/item-weapon-p2`）；P3（武器 item、K5 + K6）已實作（`claude/item-weapon-p3`）。三個分支依序疊加。ABI 101、snapshot schema 28、packet schema 28。** P0 量測測試已完成（見 §4）。
分支：本文件在 `claude/item-weapon-plan`；P0 測試在 `claude/p0-throw-and-unarmed-tests`（12795b3）。兩者都從 main 78d9350 分出，尚未 merge。
最後更新：2026-10-07（第八輪：營地選項在 prop 模板、臨時營地比照帳篷、L1/R1 重複按不作用、其他玩家武器的封包評估）。

本文件整理 2026-10-07 的需求草案與五輪討論。決策以 §2 為準；kernel / game_server / Unity 的分工在 §3；
實作中發現的問題記在對應章節；§8 列出待決問題。

---

## 1. 目標與範圍

使用者原話整理：

> 強化並驗證目前的道具系統和 gameplay 的關係，並且為了遊戲世界觀建立新的武器系統（法杖）。

| 主題 | 內容 |
|---|---|
| 配裝 | 初始營地選配裝模板（不限量）；臨時營地發放有限的共用庫存 |
| 武器 | 槍械改成法杖；武器變成 stateful item，可以撿、丟、交換 |
| 道具 | 補 MP 次數的 `fungible_mp_potion`；可投擲的補血藥水 |
| 死亡 | 重生套用配裝模板；（之後）任務道具在死亡地點掉落 |
| 操作 | PS 手把；道具模式 / 武器模式兩種狀態 |

作弊容許原則（需求 b）：需要複雜輸入的流程（選單、短按 / 長按、模式切換、快速投擲）只在 client 做，server 不驗證。
例外：**每次重生都會被套用的配裝模板必須由 server 驗證**（§3.2），否則改封包就能每次重生都拿到無限道具。

不在範圍內：
- 武器 modifier（需求 7g）：未來功能。武器做成 item 之後，modifier 可以存在武器 item 的 portable state，
  再透過每個 entity 各自的 `Kernel_ServerSetEntityWeaponMechanics` 套用，結構上接得上。
- 鍵盤操作：之後補。
- Unity package build / stage / bump：由使用者處理。

---

## 2. 決策

### 2.1 已定案

| # | 決策 | 備註 |
|---|---|---|
| D1 | **初始營地 = 方案 A（發放站，不限量）**。在初始營地選的配裝就是這個玩家的「配裝模板」 | 第四輪改定 |
| D2 | 初始營地目前放在場景裡（測試環境的限制），玩家可以自由進出、反覆修改 | 第五輪。之後的 scene file 也只是換生成方式 |
| D3 | **中途修改配裝採 (b)：立刻套用**，身上的道具和武器全部換成新模板。接受初始營地暫時變成補給站；之後由設計流程限制進入次數 | 第五輪 |
| D4 | 配裝選項比照 `player.yaml` 的 `inventory_slots`：一個 `[item_template, quantity]` 是一個選項，選一次佔一格，可以重複選。格數上限 = `inventory_slot_capacity`（預計調成 4–8） | 第五輪 |
| D5 | **武器也在初始營地選**，每個類別一格 | 第五輪 |
| D6 | **重生套用配裝模板。** 中途加入和重新連線的玩家都重新選一次（握手時沒有玩家身份，分不出重連） | 第四輪 |
| D7 | **臨時營地 = 方案 B（真的容器，有限、共用）**。只能領取、不能存回；營地被毀時庫存消失 | 第三、五輪 |
| D8 | 臨時營地比照帳篷，用一個「營地 kit」道具生成；初始庫存寫在營地 prop 模板上 | 第五輪。之後會新增 scene file 配置地圖初期的重要物件，目前只有 game rule 能生成 |
| D9 | **臨時營地容器只同步給在營地裡面的人**（沿用帳篷的 `Sheltered` 進出流程）。在營地裡的人不能移動、不會受傷，可以接受 | 第四、五輪。見 §3.4 |
| D10 | **武器做成 stateful item** | 第二輪 |
| D11 | 每個類別固定一格：Wand(0)、Scepter(1)、Staff/Greatstaff(2)、Tome/Grimoire(3)。撿到同類別的武器時自動交換，舊的丟在腳下 | 第二輪 |
| D12 | 武器格數和道具格數分開計算 | 需求 3 |
| D13 | 只有玩家能撿武器 | 需求 7f |
| D14 | `magazine_size` = MP；`ammo_cost_per_commit` = 每次施法耗的 MP；`reserve_magazines` = **可以回滿 MP 幾次** | 第二輪。和現行 code 的語意相同，不用改 |
| D15 | `fungible_mp_potion` 補的是**目前手上**的武器 | 第二輪 |
| D16 | mp_potion 比例取整：`max(1, round_half_up(上限 × 比例))`，結果不超過上限（模板的 `reserve_magazines`）。沒有武器或 reserve 已滿時拒絕使用、不消耗 | 第四輪的建議，第五輪未反對。見 §3.5 |
| D17 | **建築淘汰規則：剛蓋好的建築永遠不被淘汰；在其餘建築裡淘汰 `(importance, spawn_tick)` 最小的。** `importance` 是 prop 模板上的 uint8。被 projectile 打壞仍照現有規則 | 第二輪 |
| D18 | **開放空手，但不能做任何攻擊。** 道具照常可用 | 第五輪。見 §3.6 |
| D19 | **死亡掉落第一版不做。** 之後要做時，item 帶一個 uint8 標記，只有「任務道具」和「來自地圖的武器」會在死亡時掉落 | 第五、六輪。見 §3.8 |
| D20 | 操作：L1 短按 = 武器模式 → 道具模式；R1 短按 = 反向；Circle = 丟棄 | 第五輪。完整按鍵表見 §3.9 |
| D21 | 法杖操作分類：Rifle/Shotgun、Rocket、雷射（按住持續）維持不變；新增「按住蓄力、放開施法，未蓄滿放開視為取消」 | 需求 7c |
| D22 | 從臨時營地拿到的武器**不算**「來自地圖的武器」，`drop_tag` 為 0。臨時營地是消耗性資源 | 第七輪（原 G6） |
| D23 | 在初始營地重新套用配裝時，**只清掉 `drop_tag == 0` 的物品**，保留任務道具和地圖武器。地圖武器和模板武器同類別時，換上模板的武器，地圖武器丟在腳下（比照 D11） | 第七輪（原 G7） |
| D24 | 初始營地的選項清單寫在營地 prop 模板上 | 第八輪（原 G2） |
| D25 | 臨時營地的配置比照帳篷：壽命、shelter 容量，並與帳篷共用同一個 population group。誰先被淘汰由 D17 的 `importance` 決定 | 第八輪（原 G3）。main 上 `tent` group 上限 4 |
| D26 | 已經在道具模式時短按 L1 不作用；已經在武器模式時短按 R1 也不作用 | 第八輪（原 G4） |
| D27 | 其他玩家手上的武器用 snapshot 欄位同步（§3.10 的 A）：隊友記錄多 1 B 武器 id，空手用專用值。併進 K6 | 第九輪（原 G1） |
| D28 | 初始營地由 catalog 頂層的 `scene_props:` 放進場景，game_server 在 kernel 開始執行後的第一個 tick 放置。這是未來 scene file 的替代品。game rule 放不了：等待節點不能生成東西、一個節點只能有一個效果，而永遠不完成的節點會讓整個 rule 永遠無法完成 | P2 實作時決定 |
| D29 | **武器類別用數字 0–3**（weapon template 的 `category:`），就是武器容器的格子編號。同一套格子同時適用法杖和槍械，不綁定名稱 | 第十輪，使用者指定 |

---

## 3. 架構

### 3.1 分工總表

| 需求 | Kernel | game_server | Unity | 只改 YAML |
|---|---|---|---|---|
| 初始營地 / 配裝模板 | K10 訊息通道 | 驗證、套用、重生 | 選配裝 UI | 營地 prop、選項清單 |
| 臨時營地 | K9 容器同步 + Transfer | 生成庫存 | 營地 UI | 營地 kit、prop |
| 建築重要度 | K1 | — | — | `importance` 欄位 |
| 投擲藥水 | K2 | — | — | prop + graph |
| mp_potion | K3 | — | — | item |
| 空手 | K4 | 配裝可為空 | 空手的動畫 / HUD | — |
| 武器 item | K5、K6 | 配裝套用武器 | 拾取、丟棄、HUD | 武器 item 模板 |
| 蓄力施法 | K7 | — | 蓄力表現 | action 模板 |
| 換下的武器自動 reload | K8 | — | 顯示 | — |
| 死亡掉落（之後） | K11、K12 | 監聽死亡事件、建立物品時給標記 | — | 任務道具的預設標記 |
| 操作 | — | — | 全部 | — |

### 3.2 初始營地與配裝模板（D1–D6）

流程（P2 已實作，`game_server/src/loadout_director.cc`）：

1. 初始營地是 prop 219 `initial_camp`，由 `scene_props:` 放進場景（D28）。`on_activated` 綁 `open_ui {ui_id: 2}`。
   不需要 shelter，也不需要容器同步。任何模板有 `loadout:` 的 prop 都是配裝營地。
2. 玩家互動後，game_server 透過 K10 送 `GAME_SERVER_MESSAGE_LOADOUT_OFFERS` 給他，這也是 client 打開 UI 的訊號。
3. client 送 `LOADOUT_SELECT`，用選項的 index 指名；index 可以重複，一次挑選佔一格。
4. game_server 驗證：
   - 目標是配裝營地；
   - 玩家還活著，且距離在營地的互動範圍 + 1 m 內；
   - 挑選數 ≤ `inventory_slot_capacity`；
   - 每個 index 都有效。
5. 驗證通過就記成這個玩家的配裝模板，並**立刻套用**（D3）：清空道具容器，再依模板建立。0 個挑選代表回到預設。回 `LOADOUT_RESULT`。
6. 重生時套用同一份模板；玩家離線就忘掉（重連要重選，D6）。還沒選的玩家用 `player.yaml` 的預設值。

訊息格式和結果碼寫在 `game_server/public/game_server_types.h`（`GAME_SERVER_MESSAGE_LOADOUT_*`、`GAME_SERVER_LOADOUT_RESULT_*`），有 `GAME_SERVER_CAPABILITY_LOADOUT_MESSAGES` 標記。Unity 照這份實作。

建議的營地 prop 設定：不寫 `health`（打不壞）、不寫 `lifecycle`（永久）。
`lifetime_ticks: 0` 是載入錯誤（必須為正）；不寫 `lifetime_ticks` 才是永久。

注意：
- **格子是用「選了幾個選項」算，不是用 inventory 實際佔幾格。** 容器會先把同種 fungible 合併成一疊，例如選兩次 `[potion, 2]` 只佔一格。實際佔的格子只會比選的少。
- 立刻套用會讓從臨時營地拿到的東西一起被換掉（它們的標記是 0）。這是 D3 的結果。
- 重新套用時，模板道具的格子可能被保留下來的任務道具佔掉。放不下的模板道具怎麼處理，實作時要定（建議：放不下的就不發）。
- 重生套用的物品是憑空建立的，不從任何庫存扣。總量由 `team_revive_times` 控制。

### 3.3 武器 item（D10–D14，K5、K6）

現況（main 78d9350）：
- `WeaponState` 固定 4 格（`components.h:288`、`components.h:412`），剛好對上 4 個類別。
- 每 tick 輸入的 `selected_weapon` 是**武器 id**，不是 slot 編號（`action_system.cc:271`）。
- 武器 mechanics 是每個 entity 各自設的：game_server 對每一把呼叫 `Kernel_ServerSetEntityWeaponMechanics`。只用 `Kernel_ServerCreateEntity` 建出的玩家打不出任何東西（P0 量到）。
- snapshot 只送 `active_weapon_slot` 和 `active_weapon_ammo`（`snapshot.h:91`），不送每格的武器 id，也不送 reserve。
- `RenderEntityState` 沒有任何武器欄位：**其他玩家看不到你手上拿的是哪一把**。現在每個玩家的武器配置都一樣所以沒差；配置可變之後就有差。封包成本評估見 §3.10。
- inventory 只同步給容器的擁有者。

設計：
- 武器 item = stateful item，多一個 `weapon:` 欄位指向 weapon template。weapon template 多一個 `category` 欄位（wand / scepter / staff / tome），決定它放哪一格。
- 玩家有兩個容器：道具容器（`inventory_slot_capacity` 格）和武器容器（4 格，格子編號 = 類別）。
- **K5：** 武器容器的內容決定 `WeaponState`：
  - 容器要支援「依類別指定格子」，現有容器只會找空格放。
  - 裝上一把武器時，同時寫入 `WeaponState` 並掛上 mechanics；卸下時兩者都清掉。
  - ammo 和 reserve 平常留在 `WeaponState`，只在卸下、丟棄、死亡時寫回武器 item 的 portable state，避免每發子彈都送一次 inventory delta。
  - 撿到同類別武器：新的進來、舊的變成腳下的 prop，必須在**同一個 commit** 完成。
  - 只有玩家能撿武器：Pickup 時檢查請求者是玩家。
- **K6：** 給擁有者的武器配置同步（每格的武器 id + reserve）。**會升 snapshot schema 和 ABI。**
  需求 7e「client 查詢 reserve 是否用完」也由這一項解決。

代理（agent）的武器不變，仍由 `weapon_slots` 決定，不做成 item。

P3 實作紀錄：
- weapon template 的 `category:` 是 0–3 的數字（D29）。item 模板寫 `weapon: <武器名稱或 id>`，loader 自動加上 `weapon_ammo`、`weapon_reserve` 兩個 uint32 portable state 欄位（預設值是新武器的彈匣和 reserve），kernel 用同樣的 FNV id（`KERNEL_PORTABLE_FIELD_WEAPON_AMMO/_RESERVE`）讀寫。
- 武器狀態在不在手上時都存在 item 上：換裝重建前先把舊格子寫回 item（不管 item 已經在哪）；每個 tick 結束把 reserve（和收起的武器的 ammo）寫回，透過既有的 inventory 同步送給擁有者。手上那把的 ammo 仍由 snapshot 報。
- 地上的武器都用同一個 prop 220 `weapon_pickup`；client 依 item 模板的 `weapon_id` 畫外觀。武器 item 3020–3026。
- 預設手上的武器變成「類別最小的那把」（rifle），不再是 `weapon_slots` 的第一把（meteor staff）。
- `KERNEL_HELD_WEAPON_NONE` = 255，武器 id 不能用 255。
- 還沒做：K12 `drop_tag`。所以 D23「重新套用配裝只清掉 tag 0」目前做不到：重新套用會清掉武器容器裡的全部武器，包括地圖撿來的。

### 3.4 臨時營地（D7–D9，K9）

| 項目 | 做法 |
|---|---|
| 生成 | 營地 kit 道具投擲落地後生成營地 prop，比照帳篷的 `spawn_entity`。之後也可由 scene file 配置 |
| 庫存 | 寫在營地 prop 模板上。營地生成時建立一個擁有者 = 營地 prop 的容器（`Kernel_ServerCreateInventoryContainer` 的 owner 可以是任何 entity） |
| 誰看得到 | 擁有者，**或正在這個營地裡面的人**（`Sheltered.shelter_net_id == 營地`） |
| 領取 | 新的 `Transfer` domain action，只能從營地容器拿到自己身上。server 驗證：請求者在這個營地裡、自己的容器放得下 |
| 搶最後一個 | 沿用現有規則：先 commit 的贏，後到的收到穩定的拒絕 |
| 營地被毀 | kernel 在銷毀建築前就先把裡面的人放出來（帳篷 K3 已做），之後容器和庫存一起刪掉 |

K9 要改的 kernel 部分：

1. 「誰能看到這個容器」現在是 `owner_entity_id != session->player`，散在 `kernel.cc` 四處（5612、6543、15081、15146）。
   集中成一個 `can_observe_container(session, container)`。
2. 進入營地時送一次完整的容器內容，並建立這個人的同步進度；之後沿用增量同步。
3. 離開營地時，client 丟掉營地容器的副本。client 已經知道自己的 `shelter_net_id`，可能不需要新封包，要確認。
4. client 端查詢「目前看得到的容器」的 API（現有的 `CopyOwnedInventoryContainers` 只列出自己的）。
5. `Transfer` domain action。

依 `ITEM_PROP_CLIENT_SERVER_SYNC_POLICY.md:20`，現行設計明確不同步給旁觀者、不做 chest。K9 等於第一次開放這件事，要同步更新那份文件。

### 3.5 道具

**投擲藥水（K2，已完成）。** 量測結果見 §4.1。做法：prop 218 `potion` 的 `on_collision` 跑 `action_heal_target_and_consume_self_at_collision`，補血和自傷兩個 action 都加 `when: event.has_target`：打中 actor 有 target，落地沒有。kernel 不需要拆碰撞事件。
- **載入時檢查：** 可投擲的 item-backed prop 如果沒有包含 terrain 的 `on_collision`，就是載入錯誤。否則投出去就會永遠掉下去、物品消失。

**mp_potion（K3）。** 新的 graph action `refill_weapon_reserve`：
- 參數：`target`、固定次數 `amount` 或比例 `ratio`（二選一）。
- 只補目前手上的武器（D15）。上限 = 模板的 `reserve_magazines`。
- 取整依 D16：

| 上限 | 50% | 30% |
|---|---|---|
| 1 | 1 | 1 |
| 3 | 2 | 1 |
| 6 | 3 | 2 |

- 沒有武器或 reserve 已滿時，要在 server 接受使用請求時就拒絕。如果等 graph 執行時才失敗，道具已經扣掉了（現行規則：commit 之後 graph 失敗不退還）。

### 3.6 空手（D18，K4）

code 檢查結果（main 78d9350）：

| 項目 | 現況 | 要做的事 |
|---|---|---|
| YAML loader | 要求 `weapon_slots` 1–4 把（`gameplay_config.cc:4077`） | 只對玩家放寬到 0；agent 維持至少 1 把 |
| `Kernel_ServerSetEntityCombatState` | 擋下 slot 數 0，且要求 active slot < slot 數（`kernel.cc:5927`） | 放寬，並定義 slot 數 0 時 active slot 的意義 |
| 開火 / reload | 武器 id 不在身上就什麼都不做，也沒有 rejection 事件（P0 量到） | 不用改 |
| snapshot | 沒有可用武器時不送武器區塊（`snapshot_builder.cc:104`） | 不用改 |
| **client 本地武器狀態** | **bug：** 沒有武器區塊時 `apply_authoritative_local_weapon` 直接 return（`kernel.cc:9287`），保留上一把武器的彈藥 | 沒有區塊時把狀態標成無效 |
| `active_weapon_id()` | slot 數 0 時回傳 0，撞到 rifle 的 id | 唯一的呼叫者 `current_weapon_definition_for_entity`（`weapon_system.cc:37`）沒有人用，刪掉 |
| server 的 `Kernel_GetLocalWeaponState` | slot 數 0 時回傳 false | 不用改；Unity 要處理 false |
| 道具系統 | 不依賴 `WeaponState` | 不用改 |

Unity 端（不在這個 repo）：slot 數 0 時的動畫、瞄準 IK、HUD 需要另外確認。

### 3.7 法杖操作（D21，K7、K8）

- **K7 蓄力施法：** 新的 trigger mode `charge`。現在只有 Press 和 Hold（`kernel_types.h:1137`）。
  - 按住時開始累積，放開時如果累積的 tick 夠了才施法；不夠就取消，不扣 MP。
  - server 本來就從輸入序列知道按了多久，順手驗證幾乎沒有成本。
  - 新增 enum 值是新增型的 ABI，用 capability flag 擋住。
- **K8 換下的武器自動 reload：** 每格記錄「reload 完成的 tick」，換回這把武器時再檢查，到了就補滿 MP、扣一次 reserve。
  計算是 deterministic 的，client 可以自己算。等 K6 完成後再做。

### 3.8 死亡掉落（D19，K11、K12，第一版不做）

**掉落標記（K12）。** 每個 item instance 帶一個 uint8 `drop_tag`：

| 值 | 意義 | 何時給 |
|---|---|---|
| 0 | 無（死亡時不掉） | 配裝模板建立的物品、臨時營地的庫存（D22） |
| 1 | 任務道具 | item 模板上寫的預設值，建立時帶入 |
| 2 | 來自地圖的武器 | game rule / scene 在地圖上建立武器時指定 |

- **標記放在 instance 上，不放在模板上。** 同一把武器模板可能來自配裝，也可能是在地圖上撿到的，只有建立物品的那一刻才知道來源。
  任務道具則可以由模板給預設值。
- **標記跟著物品走：** 撿起、投擲、放下、拆疊時都保留。從配裝來的武器被交換丟在地上、再被別人撿走，仍然是 0，不會在他死亡時掉落。
- **fungible 疊加要求標記相同**，比照現有規則（portable state 和 cooldown 相同才能疊）。
  否則地圖上撿到的物品會混進配裝的那一疊，掉落時就分不清哪些是哪裡來的。
- 建立物品的 API（`Kernel_ServerCreateInventoryItem`、`Kernel_ServerCreateWorldItem`）要能帶入標記，item 查詢也要回報它。
  inventory 同步要不要送給 client，看 UI 是否需要顯示（例如在任務道具上加圖示）。
- 用 uint8 列舉（互斥）而不是 bit flag：一個物品只有一個來源。之後有新的掉落類別就加值。

縮小範圍後，原本死亡掉落的三個問題大多消失：
- **不會複製物品：** 帶標記的物品不在配裝選項裡，重生不會再發一份。前提是驗證配裝時拒絕任務道具（併入 T8）。
- **不會越積越多：** 數量少，任務道具也本來就該留在場上，不需要壽命限制。
- **誰可以撿：** 沿用現行規則，所有人都可以。

之後要做時的實作：
- 現有 API 都不能直接用：
  - `Kernel_ServerCreateWorldItem` 會建立**新的**物品，id 不同，portable state 也不會帶過去。
  - Place 會拒絕已經死亡的玩家（`InstigatorDead`，`item_gameplay_system.cc:535`）。
- **K11：** 新的 server API，把 inventory 裡的物品原封不動移到地上：保留 id、portable state 和標記，在死亡地點周圍散開，略過重疊檢查。
  武器要先把 `WeaponState` 寫回武器 item。之前延後的「死亡時丟下 carry 中的 prop」可以一起處理。
- game_server：監聽 `EntityDied`，挑出 `drop_tag != 0` 的物品並呼叫 K11。

### 3.9 操作介面（Unity）

kernel 不需要知道玩家在哪個模式：R2 在武器模式送 Fire 輸入，在道具模式送 Throw 請求。

| 按鍵 | 武器模式 | 道具模式 |
|---|---|---|
| Square（JoystickButton0） | Reload | 使用道具 |
| Cross（JoystickButton1） | 撿拾 | 撿拾 |
| Circle（JoystickButton2） | 丟棄 | 丟棄 |
| Triangle（JoystickButton3） | 快速投擲目前選的道具（需為 throwable） | 同左 |
| L2 | 瞄準 | 瞄準 |
| R2 | 開火 | 投擲 |
| L1 短按 | 切到道具模式 | — |
| L1 長按 | 開道具選單 | 開道具選單 |
| R1 短按 | — | 切到武器模式 |
| R1 長按 | 開武器選單 | 開武器選單 |

選單打開時，Cross / Triangle 用來切換選項，放開 L1 / R1 表示選定；這時不觸發撿拾和快速投擲。

已經在道具模式時短按 L1、已經在武器模式時短按 R1，都不作用（D26）。

待確認：發射雷射或蓄力中途快速投擲，kernel 會不會擋下來，還沒確認。

### 3.10 其他玩家手上的武器：封包成本評估（原 G1）

數字出自 `SERVER_DATA_SYNC_PACKET_SIZE_REPORT.md`，以下是推算，不是量測：

- 每個 client 每次 snapshot 的預算是 1,200 B，預設每秒 15 次（144 kbit/s）。
- 其他玩家的記錄每筆 76 B；agent 走另一種記錄，不受影響。
- 自己的記錄已經有 4 B 的武器區塊（active slot、reload 中、剩餘彈藥），只送給自己。
- 4 人同隊時，每個 snapshot 最多帶 3 位隊友。

| 做法 | 平常的成本 | 換武器時的成本 | 複雜度 |
|---|---|---|---|
| **A. snapshot 欄位：** 其他玩家的記錄多 1 B 武器 id | 每位隊友 +1 B，4 人時每個 snapshot 最多 +3 B，約佔預算 0.25%，每個 client 約 45 B/s | 0 | 低：跟 owner / health 區塊一樣只對玩家寫 |
| B. 只在變更時送：可靠封包 + 進入 relevance 時附在 spawn 記錄裡 | 0 | 每次換武器，對每個看得到他的人送一個約 34 B 的封包（28 B 標頭 + 記錄） | 中：要處理進入 relevance、可靠封包和 snapshot 到達順序不一致（開火動作可能先用舊武器的外觀畫） |

**採用 A（D27）。** 成本小到在預算裡看不出來，也不需要處理進入 relevance 和封包順序。
武器 id 和同一筆記錄裡的 action timeline 一起到，開火動作不會配錯法杖。

細節：
- 送的是**武器 id**，不是 slot 編號。每個人的武器配置不同，對別人來說 slot 編號沒有意義。
- 空手需要一個專用值（例如 255），因為 0 是 rifle 的 id。
- 會改 snapshot schema，併進 K6 一起升，只升一次。

---

## 4. 實測事實

### 4.1 投擲藥水（`thrown_potion_probe_test`）

把 `fungible_potion` 設成 `[consumable, pickupable, throwable]`，並補一個 item-backed prop：

| 寫法 | 丟向隊友 | 丟向地面 | 撿回 |
|---|---|---|---|
| prop 沒有 `on_collision` | 隊友沒被補血；potion 穿過地面一直往下掉（y < -40），永遠停在 InFlight | 一樣穿過地面 | 被拒絕（InvalidContext），物品消失 |
| prop 有 `on_collision` 補血 | 隊友 50 → 80；**potion 穿過隊友繼續飛**，落在後方地上 | 正常落地；同一個 graph 再觸發一次，target 是 0 | 成功，數量合併回 2 |

- 投擲中的 prop 只有綁了 `on_collision` 才會跟世界做碰撞檢查；loader 不會擋下沒有綁的寫法。
- 第二種寫法可以無限補血：丟、撿、再丟。

### 4.2 空手開火（`unarmed_fire_probe_test`）

- 0 把武器在兩個入口都被擋：YAML loader 和 `Kernel_ServerSetEntityCombatState`，後者會保留原本的配置。
- 只剩 meteor staff 時，送出 id 0（rifle）、已丟掉的 id 14、或 Reload，都不開火、不扣彈藥、不啟動 action。
  即使 rifle 的 mechanics 還留在 entity 上也一樣。
- 對照組：留下的 staff 照常發射（ammo 3 → 2，meteor 打中 grunt）。

### 4.3 其他

- main 上 tent population group 的上限是 4。
- inventory 只同步給擁有者（`ITEM_PROP_CLIENT_SERVER_SYNC_POLICY.md:20`）。
- client 能送給 server 的只有每 tick 的輸入和 `KernelGameplayRequest`（一次一個道具，只能是現有的 domain action）。
  `Kernel_InvokeRpcCommand` 是 control-plane 用的。所以 K10 需要新的通道。

---

## 5. Kernel 工作項目

| # | 內容 | 規模 | ABI / schema |
|---|---|---|---|
| K1 | 建築 `importance` + 淘汰規則（D17）。**已完成**（036f92a） | 小 | ABI 101：`KernelPropDefinition.importance` |
| K2 | 可投擲 potion；缺 terrain 的 `on_collision` 時載入錯誤。**已完成**（c3cf49b），不需要改 kernel：graph 的 `when: event.has_target` 已能分開兩種碰撞 | 小 | 無 |
| K3 | `refill_weapon_reserve` graph action + 使用前檢查 + `fungible_mp_potion`（3013）。**已完成**（ca547d8） | 小到中 | ABI 101：新 action type + 兩個欄位 |
| K4 | 空手：放寬三個入口（loader 解析、`validate_gameplay_config`、`SetEntityCombatState`）、修 client 舊彈藥 bug、刪死 code。**已完成**（8e8ba53） | 小 | 無 |
| K5 | 武器 item：依類別指定格子的武器容器、裝卸同步 `WeaponState`、portable state 寫回、自動交換、只有玩家能撿。**已完成**（69f32e6、daaf88d、167fbd6） | **大** | ABI 101：item 模板多 4 個欄位、`Kernel_ServerCreateWeaponContainer`、容器 view 的 `container_kind` |
| K6 | 給擁有者的武器配置和 reserve 同步（沿用 inventory 同步，零新封包）；每個玩家記錄裡的武器 id（D27）。**已完成**（438c822） | 中 | snapshot schema 28、packet schema 28；`RenderEntityState` 大小不變 |
| K7 | `charge` trigger mode | 中 | 新增型 |
| K8 | 換下的武器自動 reload | 小到中 | 依 K6 |
| K9 | 臨時營地容器：營地內的人看得到、`Transfer`、進出時同步 | 中到大 | 新封包、新 API |
| K10 | client 和 game_server 之間的通用訊息，雙向（kernel 只轉送）。**已完成**（7096283） | 小到中 | ABI 101 內新增（capability bit 50）；packet schema 27 |
| K11 | （之後）帶標記物品的死亡掉落：原封不動移到地上、散開 | 中 | 新 API |
| K12 | item instance 的 uint8 `drop_tag`：建立時帶入、跟著物品走、疊加要求相同 | 小到中 | 建立 API 多一個參數、item view 多一個欄位 |

每個項目實際的 ABI 版本號等實作時再定。並行的分支會撞版本號和 catalog id，merge 時兩者都要檢查。

---

## 6. 建議分期

| 期 | 內容 | 理由 |
|---|---|---|
| P0 | 量測現行行為 | **已完成**（§4） |
| P1 | K1、K2、K3、K4 | 都是小改動、彼此獨立，不用升 snapshot schema |
| P2 | K10 + game_server 配裝模板（先只有道具）。**已完成**（0b799fa） | 初始營地可以先用現有的武器配置跑起來 |
| P3 | K5 + K6，配裝模板加上武器。**已完成** | 最大的一塊，snapshot schema 在這裡升一次 |
| P4 | K9 臨時營地 | 依賴 shelter 流程（已在 main） |
| P5 | K7、K8 | K8 依賴 K6 |
| 之後 | K12 掉落標記、K11 死亡掉落 | 第一版不做（D19）。K12 可以提早，跟 K5 一起做 |

---

## 7. 測試計畫

| # | 內容 |
|---|---|
| T1 | P0 兩個量測測試（已完成，12795b3） |
| T2 | 淘汰規則：新建築不被自己淘汰；同重要度先淘汰最舊的；被打壞不受影響 |
| T3 | 投擲藥水：打中 actor 補血並消耗；落地可撿回；只打中一次 |
| T4 | 可投擲 prop 缺 `on_collision` 時載入失敗 |
| T5 | mp_potion：取整表、上限、沒武器 / 已滿時拒絕且不消耗 |
| T6 | 空手：可設成 0 把；開火、reload 都沒反應；client 本地武器狀態變成無效；道具照常可用 |
| T7 | 武器 item：撿同類別時交換，新舊武器的 ammo / reserve 都保留；agent 撿不到 |
| T8 | 配裝模板驗證：超過格數、超過 `max_stack`、不在清單、同類別兩把、任務道具，都被拒絕 |
| T9 | 配裝模板套用：修改時立刻換掉 `drop_tag == 0` 的物品、保留帶標記的；同類別時地圖武器被換到腳下；重生時依模板 |
| T10 | 臨時營地：營地內的人看得到容器、外面的人看不到；只能領取；搶最後一個時先到的贏；被毀時庫存消失、人先被放出 |
| T11 | 蓄力：未滿放開不扣 MP；滿了放開才施法 |
| T12 | 自動 reload：換下後經過 reload 時間再換回，MP 補滿、reserve 扣一 |
| T13 | （之後）死亡掉落：只有 `drop_tag` 1、2 的物品掉落；id、portable state、標記都保留；可撿回 |
| T14 | 掉落標記：撿起、投擲、拆疊後保留；標記不同的 fungible 不疊加 |

catalog 驅動的測試要自己掛上武器 mechanics、載入地面場景，並保留對照組（P0 的經驗）。

---

## 8. 待決問題

目前沒有。實作中發現的問題記在對應的章節。

---

## 9. 相關檔案

- `docs/ITEM_PROP_AUTHORING_AND_RUNTIME_GUIDE.md`、`docs/ITEM_PROP_CLIENT_SERVER_SYNC_POLICY.md`
- `docs/WEAPON_AUTHORING_GUIDE.md`
- `docs/TENT_BUILDING_PLAN.md`（shelter 進出流程）
- `engine/src/world/public/components.h`（`WeaponState`、`Sheltered`）
- `engine/src/simulation/src/systems.cc`（淘汰、投擲碰撞）
- `engine/src/simulation/src/action_system.cc`、`weapon_system.cc`
- `engine/src/sync/src/snapshot_builder.cc`
- `engine/src/kernel/src/kernel.cc`（combat state、容器同步、client 本地武器狀態）
- `game_server/tests/thrown_potion_probe_test.cc`、`game_server/tests/unarmed_fire_probe_test.cc`（在 `claude/p0-throw-and-unarmed-tests`）
