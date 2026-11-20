# Host mode 連線機制計劃：開遊戲即 host、network terminal、單一 runner

狀態：**草案（2026-11-20），Q1–Q4 已定案；R0 完成，O5、O6 已定案（H11、H12）；R1–R3、N1–N5a 完成（unity `claude/network-gate-n5a` `13e614c`，疊在前面各步上；kernel `claude/dedicated-lan-discovery` `365eb034` 和 `claude/network-terminal-n3` `39b211c6`，都從 main 開出；都未 merge）。**
分支：`claude/host-mode-plan`（從 main `4e36d622`）。
日期：2026-11-20。

**目標：** Unity build player 不依賴固定的 dedicated server。玩家開遊戲就進入自己的 initial camp（本機 listen host）；在營地裡用 network terminal 輸入 IP 加入別人的世界，或由別人加入自己。dedicated server 仍可用，且兩種玩法走同一套流程。

---

## 1. 已定案的決定

| # | 決定 | 來源 |
|---|---|---|
| H1 | build player 開遊戲一律是**本機 listen host**，進入自己的 initial camp | 評估結果：沒有 server 就沒有 world，玩家無法在未連線時站在營地裡使用 terminal |
| H2 | 流程 A（dedicated）與流程 B（listen）統一成「本機 host → 透過 terminal 換成 client 連到某個 IP」 | 同上 |
| H3 | 房主沿用現有規則，不新增程式：listen 模式是 host 自己；dedicated 模式是 peer id 最小的玩家（最早進入的人），他離開後由下一位接手 | `game_server/src/scene_flow.cc` `level_owner_net_id()` |
| H4 | **Q1**：network terminal 誰都能用；在別人的 server 上多一個「回自己的營地」選項 | 2026-11-20 使用者採用建議 |
| H5 | **Q3**：terminal 面板列出 LAN discovery 找到的 host，可以直接點選；手動輸入 IP 當備用 | 2026-11-20 使用者採用建議 |
| H6 | **Q4**：命令列參數 `-ip <ip>`。有這個參數時 player 以 client mode 啟動、直接連線；沒有時以 host 啟動 | 2026-11-20 使用者決定 |
| H7 | IP 輸入只收 `0-9` 和 `.`，另外處理 Backspace、Enter、Esc，不使用虛擬鍵盤；port 固定（見 H11） | 需求 |
| H8 | **合併 `HostModeRunner` 與 `ClientRunner` 成單一 runner**，再在上面做原地切換 | 2026-11-20 使用者採用建議 |
| H9 | 不新增 `Kernel_ReStartClient`。kernel handle 的角色固定；切換角色 = `Kernel_Destroy` + `Kernel_Create`，重建本身就保證狀態乾淨 | 評估結果，見 §6 |
| H10 | 切換的體驗照 level gate 做（network gate）：連線準備好才開門、走進去才切換、portal 畫面保持全黑、穿過時 fade | 評估結果，見 §5 |
| H11 | **固定 port 統一為 7801**（原 O5）：dedicated、docker、防火牆規則維持不變，Unity host 從 7777 改成 7801 | 2026-11-20 使用者採用建議 |
| H12 | **dedicated server 也廣播 LAN discovery**（原 O6）：`app` 的 dedicated 模式呼叫 `Kernel_LANDiscovery_StartServer`，排進 N2；在那之前流程 A 用手動輸入 IP | 2026-11-20 使用者採用建議 |

---

## 2. 現況（2026-11-20 讀程式碼確認）

### 2.1 已經有的

- **kernel LAN discovery**：`Kernel_LANDiscovery_*`（`kernel_api.h:73-91`），預設 port 47777，結果附帶 `compatible` 欄位。C# wrapper `Runtime/Client/LANDiscovery.cs` 在 feat-unity-plugin。
- **listen server 綁在所有網卡**：`gns_transport.cc:260` 用 `Clear()` 再設 port。
- **level terminal**：catalog prop，entity template 231，ui_id 4，S→C 的 `OPEN_UI`（11）回應 OK / NOT_OWNER / BUSY。
- **先準備連線、之後再完成 handshake**：`Kernel_StartClientCatalogSync` 加上 `Kernel_ContinueClientHandshake`。`two_server_switch_smoke` 在 native 端量過：切換約 1 RTT + 10 ms（`claude/two-server-switch-smoke`，未 merge）。

### 2.2 缺的

- **Unity 端完全沒有接 LAN discovery**：`Assets/Scripts` 沒有任何引用，只在 package 的 `Samples~` 裡有範例。
- **Unity player 沒有解析命令列參數**：找不到 `GetCommandLineArgs`。
- **build 的第一個 scene 是 `Client.unity`**：所以現在 build 出來的 player 一開就自動連 scene 裡存的位址 `127.0.0.1:7801`（R0 確認；程式碼預設值才是 7777）。
- **`NetworkClient` 載完 catalog 就直接 `ContinueClientHandshake()`**（feat-unity-plugin `NetworkClient.cs:351`），沒辦法停在中間。
- **HostMode 是功能縮水版**：`HostModeRunner`（897 行）沒有物品操作、loadout、camp stock、shelter、建築內部、portal、手上武器顯示、鎖定武器、物品 HUD，原始碼注釋寫明 *"The host has no item controls"*。這些都在 `ClientRunner`（2787 行）裡。只要 H1 成立，host 玩家就需要這些功能。

### 2.3 kernel 對 host 玩家使用這些功能的支援（讀程式碼，未實測）

| API | listen mode 的行為 |
|---|---|
| `copy_owned_inventory_containers`、`copy_inventory_slots`、`poll_inventory_deltas` | 直接讀權威的 `item_store_`；inventory delta 只有 UI 會拿走 |
| `local_shelter_state`、`local_weapon_state` | 有 `is_server_mode` 分支 |
| `send_game_message` | host 自己的玩家走專門的本機路徑，不經過網路 |

預期不需要改 kernel。R3 要實測的項目（**2026-11-20 已實測，全部正常**，結果在本節最後）：
- listen mode 下 container 的 `sync_state` 沒有設定（只有 client 分支會設）
- `portalDirector` 依賴 snapshot tick，host 沒有 snapshot
- 建築內部的 spawn tick 也是同樣的問題

---

**R3 實測結果**（host 角色，PlayMode `GameRunnerHostFeatureTests`）：

| 項目 | 結果 |
|---|---|
| 武器欄位由 inventory 填入 | ✅ inventory 有 11 個物品，武器是 3020、3023、3025、3027；`HasWeaponLoadout` 為 true，`TryGetLocalWeaponState` 成功 |
| container `sync_state` | ✅ 不是問題：server 端的 `ItemStore::container_view` 一律設成 `Ready`（`item_system.cc:1441`） |
| 進入 camp 的建築 | ✅ 進入建築 2，ui 2（loadout）；UI 打開，建築內部也有解析出來 |
| 建築的 spawn tick | ✅ `ResolveBuildingSpawnTick` 在 render state 沒有 spawn tick 時，會改用 `ShelterSpawnTickTracker` |
| 開關卡、穿過 gate | ✅ `level_terrain_seed2` 載入 slot 1（offset_x 4096）；gate 開啟，玩家傳送到出口（kernel 送出 `ActorTeleported`） |
| portal 的 tick | ✅ server 模式的 `local_shelter_state` 回傳 `current_tick` 當 authoritative tick；穿過之後 `HoldsLocalMovement` 會放開 |
| abort 回營地 | ✅ `SCENE_ABORT` 之後 outcome=4，slot 卸載，玩家回到營地 |

沒有測到的：用打敗 20 個敵人的成功條件走回程門回營地（R3 用 abort 代替），以及兩個 process 之間的連線（N6）。

---

## 3. 啟動規則

| 啟動方式 | 結果 |
|---|---|
| build player，不帶參數 | host session：自己的營地、廣播 LAN discovery、可以用 network terminal |
| build player，帶 `-ip <ip>` | client session，連 `<ip>:7801`；格式錯誤、連線失敗或之後斷線，都退回 host session |
| Editor Play | 依照 runner 上 Inspector 的設定選角色（保留現在「Play 之前先填好 IP」的流程） |

`-ip` 的解析規則：
- 用 `IPAddress.TryParse` 檢查，而且必須是剛好 4 段的 IPv4，接上 `:7801`（H11）。
- 格式錯誤就退回 host，並在畫面上顯示原因。

啟動指令範例：
- macOS：`open -a Game.app --args -ip 192.168.1.20`
- Windows：`Game.exe -ip 192.168.1.20`

同一台電腦開兩個 player 測試時，第二個要帶 `-ip 127.0.0.1`。如果兩個都不帶參數，第二個會綁 7801 失敗，要在 N2 處理（本機 host 改綁其他 port，或顯示提示）。

---

### 3.1 N1 實作備註（2026-11-20）

- **不做 Boot scene**：R2 之後只剩 `Game.unity` 一個 scene，所以 runner 在 player build 裡自己讀命令列就夠了，不需要再多一個 scene 來分派。
- **解析**：`LaunchOptions.Parse`（`Assets/Scripts/Common/LaunchOptions.cs`）
  - `-ip` 不分大小寫，值必須是剛好 4 段十進位數字、每段 0–255，接上 `:7801`。
  - 拒絕的寫法包括 `1.2`、十六進位、帶 port、主機名稱。
  - 只在 player build 讀一次（`!Application.isEditor`）；Editor 照 Inspector 的設定。
- **退回 host**：`fallBackToHostOnDisconnect` 在 player build 會自動打開，Editor 預設關閉。
  - client 啟動失敗、catalog sync 失敗或之後斷線時，runner 會先停用再啟用，讓 `OnDisable` 釋放 client session 留下的所有東西，再以 host 角色從頭開始。
  - 畫面左上角會顯示原因 8 秒（`OnGUI`）。
  - 連到沒有人在聽的位址，要等 GNS 的連線逾時（約 10 s）才會退回。N5 的 network gate 可能需要更短的 timeout。
- **host port**：預設值改成 7801（H11）。原本排在 N2，因為 `-ip` 一定連 7801，所以提前到 N1 做。

### 3.2 N2 實作備註（2026-11-20）

- **host**（`GameRunner.Host.cs`）
  - 依序嘗試 `hostPort` 到 `hostPort + 10`。只有 catalog 載入成功、但 listen 失敗時才換下一個 port；catalog 本身載入失敗時，換 port 也沒用，所以直接停止。
  - 換了 port 會在畫面上顯示實際的 port。
  - listen 成功後開始回應 LAN discovery（udp 47777），名稱是 `<裝置名稱>'s camp`，回報的 port 就是實際綁定的 port。
  - discovery 的 port 被占用時只會警告，camp 仍然可以用 IP 加入。
- **dedicated**（`app/dedicated_server_app.cc`）
  - 開始 listen 之後回應 LAN discovery，名稱是 `dedicated server :<port>`。
  - 失敗時只警告。乾淨結束時會在 kernel 之前先停止並銷毀 discovery。
- **已知的限制**
  - 同一台電腦上的第二個 host，可能因為 47777 被第一個占用而無法回應 discovery；`SO_REUSEADDR` 在不同作業系統上的行為不同，沒有驗證。
  - docker 裡的 dedicated server 收不到 LAN 上的廣播：47777 沒有對外開放，而且廣播本來就過不了 docker 的 NAT。這種情況請用 IP 加入。

## 4. 單一 runner 的架構

### 4.1 Session 介面（放在 Unity 專案，不放 package）

```text
INetworkSession
  Kernel, LocalPeerId, LocalPlayerNetId, IsReady, IsDisconnected
  Update(dt, events), GetRenderStatesAtTime(t, states)
  TrySubmitInput(input), Dispose()
  Catalog (bundle bytes + entry path, when available)
HostSession   : wraps NetworkHost  (+ GameServer initial-join forwarding)
ClientSession : wraps NetworkClient (+ catalog sync, connection state, cache)
```

### 4.2 R0 結果：兩個 scene 與兩個 runner 的差異（2026-11-20，unity main `bc1c272`）

#### 4.2.1 Scene 檔

**結論：除了 runner 本身，兩個 scene 實質上相同，合併成一個 scene 沒有障礙。**

| 項目 | `Client.unity` | `HostMode.unity` | 合併時怎麼處理 |
|---|---|---|---|
| 根物件 | Main Camera、Directional Light、`Network Demo Client`、`Terrain` | Main Camera、Directional Light、`Network Demo HostMode` | 只留一個 runner 物件 |
| `Terrain` 群組 | 6 個舊的 terrain mesh（`plane_200x200`、`undulating_*`、`plane_200x200_4zones*`、`giantwood_hollow_spiral`）全部 `m_IsActive: 0`；唯一啟用的 `default` 掛在停用的 `plane_200x200` 底下，所以實際上也是停用的 | 無 | 刪掉。執行時的地形由 `NetworkTerrainView` 從 kernel 建立 |
| URP 額外資料 | 相機和燈光各有 `UniversalAdditional*Data` | 無（Unity 執行時會補預設值） | 保留 Client 的 |
| Camera、RenderSettings、NavMesh 設定 | 相同 | 相同 | — |
| 燈光 | shadow strength 0.95 | shadow strength 1 | 用 0.95 |
| LightmapSettings | 多一行 `m_PVRFilteringGaussRadiusAO: 1` | 無 | 無影響 |

相機位置不重要，執行時由 `NetworkDemoScene.EnsureDefaultView()` 建立的 follow camera 接手。

#### 4.2.2 Runner 的序列化設定

`HostMode.unity` 只存了 `port`、`maxEvents`、`maxRenderStates` 三個欄位，其他欄位都用程式碼的預設值。

| 欄位 | Client scene 的值 | HostMode 實際的值 | 合併時用 |
|---|---|---|---|
| 位址或 port | `127.0.0.1:7801`；清單還有 `192.168.1.102:7801`、`192.168.1.107:7801` | port `7777` | **7801（H11）** |
| `maxEvents` / `maxRenderStates` | 2048 / 2048 | **256 / 256** | 2048 |
| `enableVisualDebug` | 0 | 1（程式碼預設） | 0 |
| `logActionResultFailures` / `logFireStalls` | 0 / 0 | 1 / 1（程式碼預設） | 0 / 0 |
| `inputSubmissionRateHz` | 30 | 沒有這個欄位 | 只用在 client |
| `portalArrivalBlendTicks`、`enableDiagnostics`、`logRemotePresentation`、`localAgentSettings` | 有 | 沒有這些欄位 | 照 Client 的值 |

**順帶發現的問題**：host 的 render state buffer 只有 256。camp 裡有幾百個 agent 時，超過的部分會被截掉（log 會出現 `render state buffer reached capacity`），所以 host 玩家可能看不到某些實體。合併後這個問題自然消失。

#### 4.2.3 Runner 每個 frame 的行為

| 行為 | `HostModeRunner` | `ClientRunner` | 合併後 |
|---|---|---|---|
| 啟動 | 讀本機 bundle → 設定武器 loadout、tracer、targeted strike、骨架 manifest → `NetworkHost.Start` → 建地形 → scene presenter → level terminal → 轉送初始加入 | `NetworkClient` → `ConfigurePhysicsBeforeStart` → 開始 catalog sync；連上後才設定 catalog，失敗會重試 | 由 session 決定 catalog 何時可用，runner 收到之後才做設定 |
| input 送出的條件 | `IsLocalClientReady && HasWeaponLoadout`，每個 frame 都送 | `IsReady && synchronizedCatalogConfigured`，30 Hz | **條件改成 client 的版本**（見下方說明）；送出頻率依角色 |
| 選單開著時的 input | 送出全部是零的 input | `SampleCurrentInput()`（R2 時確認它對選單的處理） | 統一 |
| aim | 每個 frame 都更新 | agent 模式和交接時跳過 | 照 client |
| event 處理 | 轉送初始加入給 `GameServer` | 診斷 log、連線狀態、agent 受到的傷害 | 轉送放在 `HostSession` |
| game message | 只交給 scene presenter | `HandleGameMessages`：scene 加上 loadout offers 等 | 照 client |
| 斷線 | 不會發生 | 清掉 input、物品、HUD、shelter、地形，並停止 | 照 client，之後接「退回 host session」 |
| level terminal | 自己寫的 F 鍵處理，`SubmitGameplayRequest(Activate)` | 走 `NetworkItemPropController` 的互動 | 刪掉 host 版，統一走物品互動 |
| charge 和 strike marker | `UpdateChargeAndMarkerRay`（合在一起） | `UpdateChargeTracker` 加 `UpdateStrikeMarkers` | 照 client |
| local action gate | 傳入 render count | 傳入本機玩家狀態，包含死亡 flag | 照 client |
| 只有 client 有的 | — | shelter、建築內部、portal、手上武器、物品操作、loadout、camp stock、物品 HUD、鎖定武器、agent 觀察、issue timeline、remote presentation probe | 全部變成兩種角色都有；agent 見 O1，診斷見 O4 |

**順帶發現的問題**：host 送 input 的條件包含 `HasWeaponLoadout`，所以空手的 host 玩家不能移動。client 的註解特別寫了 *"an unarmed player still moves"*，兩邊行為不一致。合併後改用 client 的條件，就會修正這一點。

#### 4.2.4 對合併計劃的影響

- R2 合併 scene 時不用搬任何物件，直接以 `Client.unity` 為基礎，刪掉 `Terrain` 群組。
- 估計不變。
- 發現 port 不一致與 dedicated 沒有 LAN discovery，已於 2026-11-20 定案為 H11、H12。

### 4.1.1 R1 實作備註

- 介面成員目前只有 `Kernel`、`LocalPeerId`、`LocalPlayerNetId`、`IsReady`、`IsDisconnected`、`Update`、`GetRenderStatesAtTime`、`TrySubmitInput`。catalog 和 `Dispose` 留到 R2，等到 session 真的擁有它包住的物件時再加。
- adapter **不擁有**包住的物件，`NetworkHost` 和 `NetworkClient` 仍然由 runner 負責 dispose。
- `HostSession.IsReady` = `IsRunning && IsLocalClientReady`；`IsDisconnected` = `!IsRunning`（host 沒有連線可以斷）。
- `NetworkItemPropController` 和 `ItemPropRequestSender` 的參數型別改成 `INetworkSession`。為了讓 diff 最小，參數名稱仍然叫 `client`，R2 再改名。
- host 的 level terminal 改成透過 `ItemPropRequestSender.Submit(hostSession, …)` 送出。多了 `IsReady` 和 request id 的檢查，但 host 本來就先檢查過 `IsLocalClientReady`，所以行為不變。

### 4.1.2 R2 實作備註

- `ClientRunner` 改名為 `GameRunner`，是 `sealed partial class`：
  - `GameRunner.cs`：共用部分加 client 角色
  - `GameRunner.Host.cs`：啟動 listen host、轉送 host 自己玩家的初始加入
  - `.meta` 的 GUID 沒變，所以 scene 和序列化設定都保留。
- 新增 `role` 欄位，預設是 `Client`，所以 scene 裡原本的設定照舊。host 用的欄位是 `hostPort`（暫時仍是 7777，N2 改成 7801）和 `hostGameplayCatalogEntry`。
- **host 現在走和 client 相同的畫面處理**：物品、loadout、shelter、portal、建築內部、物品 HUD 都會執行。武器欄位改由 inventory 填入，不再用舊的 `TryReadPlayerWeaponLoadout`。這些在 host 上是否都正確，是 R3 要實測的。
- 保留的角色差異：
  - host 每個 frame 都送 input，client 照 `inputSubmissionRateHz` 送
  - catalog：host 讀本機 bundle，client 等 sync 完成
  - 只有 client 才會偵測斷線並停止
- `Client.unity` 改名為 `Game.unity`，是 build 裡唯一的 scene；`HostMode.unity` 和 `HostModeRunner.cs` 已刪除。
- **還沒做的**：
  - `Game.unity` 裡停用的 `Terrain` 群組沒有刪（手動改 scene YAML 有風險，在 Editor 裡刪比較安全；它是停用的，不影響執行）
  - 共用程式碼裡的 log 前綴仍然寫 `Client`
  - item controller 的參數名稱仍然叫 `client`
- 測試和工具（原 R4 的一部分）已經一起改好：`NetworkPresentationClockTests`、`GameRunnerAgentModeTests`、`VideoCaptureMenu`、`EditorBuildSettings`，以及注釋。

### 4.3 原地切換時避免狀態殘留

每個 session 的畫面物件都掛在一個專用的子 GameObject 底下，切換時整個銷毀再重建。這是用重建來保證乾淨，不靠手動清單。手動清單的問題就是 H9 拒絕 `Kernel_ReStartClient` 的理由。

---

## 5. Network terminal 與 network gate

### 5.1 Terminal prop

- **catalog**：新增 entity template `network_terminal`，給新的 `ui_id`（暫定 5），擺進 camp 的 scene template。
- **game_server**：ui 5 要和 ui 4 一樣，不能交給 `ShelterDirector`。`ShelterDirector` 會把其他所有 `UiOpened` 都當成進入建築；合併後 host 也會走物品操作的 Activate 路徑，所以這個路由必須補上。
- **Unity**：距離判定沿用 level terminal 的讀法，面板只在本機打開，不經過 server。
  - 面板內容：LAN discovery 列表（H5）、手動輸入（H7）
  - 在別人的 server 上多一個「回自己的營地」（H4）

### 5.1.1 N3 實作備註（2026-11-20）

- **entity template**：`235_prop_network_terminal.yaml`，range 3 m，`action_open_ui` ui 5。我查過所有分支，235 和 ui 5 都沒有被用過。
- **擺放位置**：camp 的 (-4, 0, 5)，站在地面上（測試量到 y 0.79，地面也是 0.79）。
  - **注意**：`0_scene_initial_camp.yaml` 是 Unity 的 scene exporter 產生的檔案，我是手動加上這一筆。Unity 的 camp scene 也要放上這個 prop，否則下次匯出時會被蓋掉。
- **和原計劃不同**：§5.1 原本寫「面板只在本機打開，不經過 server」。實作改成**沿用 level terminal 的流程**：activate 之後 game server 一律回應 `OPEN_UI`（ui 5，OK），Unity 收到 `OPEN_UI` 再打開面板。
  - 好處是 Unity 只需要多處理一個 ui id，距離判定和 F 鍵都直接沿用。
  - 在別人的 server 上也能用，所以「回自己的營地」可以從同一個面板觸發。
- **game_server**：新增常數 `GAME_SERVER_UI_NETWORK_TERMINAL 5u`。ui 5 和 ui 4 一樣不會交給 `ShelterDirector`。`OPEN_UI` 的 wire format 沒有改，現在統一由 `answer_open_ui` 送出。
- **測試**（-c opt）：
  - `scene_flow_startup_test`：terminal 在地面上；guest 和 host 都收到 OK；沒有人進入建築
  - `placeable_registry_test`
  - `scene_template_catalog_test`：camp 的 prop 數從 2 改成 3
  - `drop_tag_test`、`loadout_test`、`stamp_sites_test`
- **交付**：
  - catalog 內容有變，`bundle.bytes` 要重新產生，Unity package 要重新打包（使用者處理）。在那之前 Unity 的 camp 裡看不到這個 terminal。
  - C# 的 `GameServerConstants` 還沒有 ui 5 的常數，N4 在 Unity 端先用本地常數。

### 5.1.2 N4 實作備註（2026-11-20）

- **面板**：`NetworkTerminalView`（`Assets/Scripts/UI/`），外觀和 `LevelSelectView` 一致。列出的內容：
  - LAN discovery 找到的 camp 和 server。打開時查詢一次，按 R 重新查詢。自己的 camp 會被排除：port 相同，而且 IP 是 loopback 或本機的網卡位址。
  - 地址列：只收 `0-9` 和 `.`，最多 15 個字。輸入來源是 `Keyboard.onTextInput`，不使用虛擬鍵盤。Backspace 刪除，port 固定 7801，格式錯誤時在面板裡顯示原因。
  - 在別人的 server 上，多一列「Return to my camp」（H4）。
  - 關閉：F、Esc 或手把的 east 鍵。
- **流程**：server 送 `OPEN_UI`（ui 5，OK）→ `SceneTablePresenter.NetworkTerminalOpened` → `GameRunner` 打開面板。面板打開期間，runner 不送移動、動作或物品操作的 input。
- **切換**：
  - `GameRunner.JoinServer(address)`：把角色改成 client，打開 `fallBackToHostOnDisconnect`，然後重新啟動 runner（先停用再啟用）。
  - `GameRunner.ReturnToOwnCamp()`：把角色改成 host，然後重新啟動 runner。
  - 這是 N5a 的「穿門時才開始連線」之前的直接版本：從面板確認後立刻切換。
- **距離判定**：catalog reader 把 ui 5 和 ui 4 一起算進 `LevelTerminalRanges`，所以 network terminal 也用 F 鍵在範圍內啟用。ui 5 在 Unity 端暫時用本地常數，C# package 的 `GameServerConstants` 還沒有。
- **測試**：
  - EditMode `NetworkTerminalViewTests`：輸入過濾、地址格式、Enter 加入、從自己的 camp 不會出現回家選項
  - PlayMode：手動組一個 `OPEN_UI` 5 的訊息，確認面板會打開；host 加入另一個 server 之後再回到自己的 camp（log 確認 7972 → 7971 → 7972）
- **還沒驗證的**：
  - 實際按鍵的輸入（`onTextInput`、F 鍵開啟）沒有做自動測試
  - 從真正的 prop 啟用面板，要等 `bundle.bytes` 重新產生
  - 在有畫面的 Editor 裡看面板的外觀

### 5.2 Network gate

| 階段 | 做法 |
|---|---|
| 準備 | terminal 送出 IP → 建立 client session，catalog sync 完成後停在 handshake 前（遠端 server 這時還沒生成任何東西）→ 營地裡出現一扇還沒開的門 |
| 開門 | 準備完成就開門；失敗就不開門並顯示原因，玩家留在自己的營地 |
| 穿過 | Unity 判斷本機玩家進入門的範圍（kernel portal 沒辦法傳送到另一個 process）→ `ContinueClientHandshake` → 關掉 host session → 重建畫面，用 fade 蓋過去 |
| 逾時 | 準備階段的等待算在 sync 的 `timeout_ms` 裡（預設 30 s）。要把 timeout 拉長，或在逾時後把門關回去並自動重新準備 |

需要的 package 變動：`NetworkClient` 加一個「準備好就停住」的選項和公開的 `Continue()`。這是單純新增，不碰 ABI。**package 重新打包由使用者處理。**

---

### 5.2.1 N5a 實作備註（2026-11-21）

- **開門**：在 terminal 選了 server 或輸入有效地址後，不會立刻切換，而是在玩家面前 3 m 開一扇門（`NetworkGate`）。
  - 外觀沿用 level gate 入口的 prefab（template 232）；沒有這個 prefab 時，用兩根柱子加一根橫樑代替。
  - 畫面左上角會提示這扇門通往哪裡。
- **穿過**：玩家走進門的中心 1.2 m 範圍內（高度差 3 m 以內）時，才呼叫 `JoinServer`；連不上會照 N1 的規則退回自己的營地。
  - 門是**純 client 端**的東西，因為沒有任何 server 同時涵蓋兩個世界，kernel portal 傳送不過去。
- **其他規則**：
  - 沒有人走進去的門，60 秒後會自動關閉。
  - 再開一次門，會移到新的位置並改成新的目的地。
  - 「Return to my camp」仍然立刻切換，不經過門。
- **和 §5.2 的差異**：
  - portal 畫面全黑：沿用的 prefab 本身就沒有接上 portal 畫面，沒有特別處理。
  - fade：沒有做。切換時 runner 會整個重建，所以改用畫面上的提示文字「Joining …」，這段文字在重建之後仍會顯示。
- **測試**：PlayMode `Host_WalksIntoTheNetworkGateAndJoins`。開門之後等 30 個 frame，確認沒有連線；把玩家移進門裡之後，成功加入 7981，而且門已經關閉。
- **還沒做的**：N5b，也就是先建立連線、準備好才開門。這需要 C# package 的變動。

## 6. 不採用的做法

| 做法 | 不採用的原因 |
|---|---|
| `Kernel_ReStartClient`（同一個 handle 從 host 換成 client） | 省下的只有重建 handle 的時間，幾乎是零；要在 `reset_runtime_state`（`kernel.cc:8456`）大約 60 個欄位的手動清單上再加 stop 和丟 catalog，漏一項就會把 host 的狀態帶進 client session；而且不能消除 runner 的分裂 |
| 不合併 runner，只在換角色時換 scene（S1） | 估計 2–3 天，但 host 仍然缺物品和 shelter；要補就得把約 1000 行再複製一份到 `HostModeRunner` |
| 「第一個進入的人是 host」另外做一套 | 現有的 `level_owner_net_id()` 已經是這個行為 |

---

## 7. 步驟

### 7.1 Runner 合併（Unity，`unity-network-example` 的 `claude/*` 分支）

| 步驟 | 內容 | 估計 | 完成條件 |
|---|---|---|---|
| R0 ✅ | 比對 `Client.unity` 和 `HostMode.unity` 的場景物件與序列化設定；把兩種角色的行為差異列成表 | 0.5 天 | **完成（2026-11-20）**，結果在 §4.2 |
| R1 ✅ | `INetworkSession` 加上 `HostSession` 和 `ClientSession`；`NetworkItemPropController` 改用介面（目前只用到 `IsReady`、`IsDisconnected`、`LocalPeerId`、`LocalPlayerNetId` 和 4 個 kernel 呼叫） | 0.5–1 天 | **完成（2026-11-20）**：unity `claude/session-interface-r1` `9da4114`（從 main `bc1c272`）。EditMode 32/32 通過（NetworkSessionTests、NetworkItemProp*、NetworkPresentationClockTests、ClientRunnerAgentModeTests），沒有新的編譯警告 |
| R2 ✅ | `ClientRunner` 改成 `GameRunner`，host 也走同一條路；刪掉 `HostModeRunner`；合併成一個 scene | 2–3 天 | **完成（2026-11-20）**：unity `claude/game-runner-r2` `27bee9a`。EditMode 192/192 通過（相關 fixture 加上 NetworkRenderingTests）；新增的 PlayMode `GameRunnerRoleSmokeTests` 2/2 通過（host 進營地並畫出世界；client 連上同一個 process 裡的 listen server 並畫出世界）。開關卡、回營地留給 R3 和 N6 |
| R3 ✅ | 實測 host 玩家的物品、loadout、shelter、portal、建築內部，包含 §2.3 的三個項目 | 1–2 天 | **完成（2026-11-20）**：unity `claude/host-features-r3` `42ee799`。新增 PlayMode `GameRunnerHostFeatureTests` 3/3 通過，連同 R2 的 smoke test 共 5/5。沒有發現需要修改的地方，見 §2.3 |
| R4 | 更新測試和工具（見 §8） | 0.5 天 | EditMode batchmode 全部通過，或者跟合併前的紅燈集合相同 |

### 7.2 連線功能

| 步驟 | 內容 | 在哪裡做 | 估計 |
|---|---|---|---|
| N1 ✅ | `-ip` 解析；退回 host 的處理；host port 改成 7801 | Unity | **完成（2026-11-20）**：unity `claude/launch-options-n1` `b2ffd51`。EditMode 44/44、PlayMode 6/6（包括「連不上就退回 host」）。**不做 Boot scene**，見 §3.1 |
| N2 ✅ | host session 啟動時廣播 LAN discovery；綁 7801 失敗時的處理（O2）；`app` 的 dedicated 模式廣播 LAN discovery（H12） | Unity 加本 repo 的 `app/`（commit 分開） | **完成（2026-11-20）**：unity `claude/lan-discovery-n2` `0510511`（PlayMode 7/7）；kernel `claude/dedicated-lan-discovery` `365eb034`（`-c opt //app:app` 在 port 7871 實跑，用 NKLAN01 查詢收到 port=7871 的回應） |
| N3 ✅ | `network_terminal` entity template、ui 5 的路由、擺進 camp | game_server 加 catalog（本 repo 的 `claude/*` 分支） | **完成（2026-11-20）**：kernel `claude/network-terminal-n3` `39b211c6`。6 個 game_server 測試通過，見 §5.1.1 |
| N4 ✅ | terminal 面板（列表、輸入、回自己的營地） | Unity | **完成（2026-11-20）**：unity `claude/network-terminal-panel-n4` `c2564c8`。EditMode 48/48、PlayMode 9/9，見 §5.1.2 |
| N5a ✅ | network gate v1：穿門時才開始連線 | Unity | **完成（2026-11-21）**：unity `claude/network-gate-n5a` `13e614c`。PlayMode 10/10，見 §5.2.1 |
| N5b | network gate v2：`NetworkClient` 的暫停選項，連線準備好才開門 | C# package（獨立的 `claude/*-csharp` 分支）加 Unity | 0.5–1 天 |
| N6 | 檢查清單：`docs/HOST_MODE_N6_CHECKLIST.md`。兩台機器實際跑完一輪流程 A 和流程 B | 手動 | 1 天 |

**合計**：R0–R4 約 4.5–7 天；N1–N6 約 5.5–7 天；全部約 10–14 天。先前評估的 6–10 天只包含 runner 合併、原地切換、Boot 和 `-ip`，沒有包含 LAN discovery、terminal prop 和面板。

**順序**：R0 → R1 → R2 → R3 → R4 → N1、N2、N3（三者互不相依）→ N4 → N5a → N6 → N5b。N5b 可以晚點做。

---

## 8. 會一起壞掉、要一起改的地方

- **`Assets/Tests/EditMode/NetworkPresentationClockTests.cs`**：用檔案路徑讀兩個 runner 的原始碼文字來檢查。
- **`Assets/Tests/EditMode/ClientRunnerAgentModeTests.cs`**：用 reflection 依私有欄位和方法名稱存取。
- **`Assets/Editor/VideoCapture/VideoCaptureMenu.cs:556-559`**：用 type 名稱做 switch。
- **`ProjectSettings/EditorBuildSettings.asset`**：兩個 scene 改成一個 scene（`Game.unity`，R2 已改）。
- **只是注釋提到 `ClientRunner.Update`**：`NetworkRenderStateApplier.cs:416` 和 `NetworkActorRigPrefabBuilder.cs:703`。

---

## 9. 版本與交付

| 項目 | 是否變動 |
|---|---|
| ABI | 不變 |
| snapshot / packet schema | 不變 |
| catalog 內容 | N3 會變：`bundle.bytes` 要重新產生；package 要重新打包（使用者處理） |
| catalog 格式 | 預期不變；N3 若需要新的 catalog key，就照 schema 變動的規格處理 |
| C# package | N5b 會變（單純新增，不碰 ABI） |

`engine/` 和 `game_server/` 的 commit 與 `plugins/` 的 commit 要分開。merge 到 `feat-unity-plugin` 和 package 打包都由使用者處理。

---

## 10. 尚未決定

| # | 問題 | 建議 |
|---|---|---|
| O1 | local agent（bot）模式要不要也支援 host？ | 先維持只有 client 能用，不列入估算 |
| O2 ✅ | 兩個 player 在同一台電腦都不帶參數時，第二個 host 要怎麼辦？ | **N2 已實作**：綁 7801 失敗就改綁 7802–7811 的第一個可用 port，並在畫面上顯示實際的 port；LAN discovery 回報的就是實際的 port |
| O3 | terminal 面板要不要支援手把輸入？ | v1 不支援；LAN discovery 列表可以用手把選 |
| O4 | `ClientRunner` 大約 500 行的診斷 log（其中 `[AmmoDiag]` 標了 TEMP）要在合併時一起清掉嗎？ | 搬到獨立的診斷元件；TEMP 的部分刪掉 |

---

## 11. 成本控制

- 每個步驟都先做 smoke test，不跑完整的 build 或 test，除非使用者要求。
- 只讀跟這個步驟直接相關的檔案。
- R3 如果發現需要改 kernel，先停下來說明，不擴大範圍。
- 每個步驟結束時回報：讀了哪些檔案、改了哪些檔案、做了哪些搜尋、跑了哪些 build 或 test。
