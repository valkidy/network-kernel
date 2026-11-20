# Host mode N6：兩台機器實測檢查清單

計劃：`docs/HOST_MODE_PLAN.md`，步驟 N6。日期：2026-11-21。

**目的**：在兩台實體機器上跑完流程 A（dedicated server）和流程 B（listen host）各一輪，確認 R1–N5a 在真實網路上能用。到目前為止只在單一 process 的 PlayMode 測試裡驗證過。

**要用的分支**（都還沒 merge）：

| repo | 分支 | 內容 |
|---|---|---|
| Unity | `claude/network-gate-n5a` `13e614c` | R1–R3、N1、N2、N4、N5a 疊在一起（不需要新的 C# API） |
| Unity（選用） | `claude/network-gate-prepared-n5b` `76ed730` | 再加上 N5b；需要 package 裡有 C# `claude/network-client-hold-csharp` `c81b5b63` |
| kernel | `claude/network-terminal-n3` `39b211c6` | camp 的 network terminal prop（entity 235，ui 5） |
| kernel | `claude/dedicated-lan-discovery` `365eb034` | dedicated server 回應 LAN discovery |

兩個 kernel 分支都從 main 開出，互不衝突（一個改 `game_server/`，一個改 `app/`）。

---

## 0. 事前準備（使用者處理）

- [ ] **kernel**：把兩個 kernel 分支合在一起，可以 merge 到 main，或另開一個暫時分支。
- [ ] **package**：用合起來的 kernel 重新產生 `bundle.bytes`，打包 `com.network-example.kernel`，再發佈到 Unity 讀的分支（目前是 `dev-latest`）。
  - 沒做這一步，camp 裡就沒有 network terminal，§2 的 terminal 項目都無法測。
  - 這次沒有 ABI 或 snapshot schema 的變動，package 裡的 dylib 可以沿用 ABI 110。
- [ ] **Unity**：checkout `claude/network-gate-n5a`，讓 package 解析到新的版本。如果 package 也包含了 C# 分支，可以改用 `claude/network-gate-prepared-n5b`。這時 §2.2 的門會先顯示「Connecting to …」，連線停住之後才出現；連不到的 IP 不會開門（§4.1 的預期結果也跟著改變）。
  - [ ] 打開 `Game.unity`，在 Editor 用 Role = Host 按 Play，確認 camp 裡看得到 network terminal（`(-4, 0, 5)`，和 level terminal 在不同側）。
- [ ] **build player**：Build Settings 裡只有 `Game.unity`。兩台機器各需要一份；如果兩台作業系統不同，就各 build 一次。
- [ ] **dedicated server**（只有流程 A 需要）：
  - 在合起來的 kernel 上執行：
    ```bash
    bazel build -c opt //app:app
    ```
  - 啟動時**一定要帶 `--port=7801`**，因為 `app` 的預設 port 仍然是 7777：
    ```bash
    bazel-bin/app/app --mode=dedicated_server --port=7801 --gameplay-catalog-bundle=bazel-bin/game_server/gameplay_catalog_bundle/bundle.zip --gameplay-catalog-entry=gameplay_catalog.yaml
    ```
    （`run_server.sh` 就是這個指令）
- [ ] **網路**：
  - [ ] 兩台機器在同一個子網路。記下各自的 IP（macOS：`ipconfig getifaddr en0`；Windows：`ipconfig`）。
  - [ ] 第一次啟動時防火牆會跳出詢問，選「允許」。需要開放的是 UDP 7801（遊戲）和 UDP 47777（LAN discovery）。
- [ ] **版本一致**：所有 player 和 dedicated server 必須用同一份 bundle。catalog sync 會比對 hash，不一致時連線會失敗。

**記錄位置**：

| 來源 | 路徑 |
|---|---|
| Player log（macOS） | `~/Library/Logs/DefaultCompany/unity-network-example/Player.log` |
| Player log（Windows） | `%USERPROFILE%\AppData\LocalLow\DefaultCompany\unity-network-example\Player.log` |
| dedicated server | 終端機輸出 |

在 log 裡搜尋這些字串：`HostMode listening on port`、`answering LAN discovery`、`leaving to join`、`falling back`、`returning to its own camp`、`Client local player ready`、`catalog sync failed`。

---

## 1. 單機啟動

在 A、B 兩台各做一次。

- [ ] **1.1** 不帶參數啟動 → 進入自己的 camp，畫面上沒有錯誤提示；log 有 `HostMode listening on port 7801`。
- [ ] **1.2** 走到 level terminal 按 F → 出現關卡列表（自己就是房主）→ 按 F 關閉。
- [ ] **1.3** 走到 network terminal 按 F → 出現 Network 面板；列表裡**不會**出現自己的 camp → 按 Esc 關閉。
- [ ] **1.4** 面板開著時，WASD 不會移動角色，滑鼠左鍵不會開火。

## 2. 流程 B：玩家 B 加入玩家 A 的 camp

A 和 B 都先不帶參數啟動，各自進入自己的 camp。

- [ ] **2.1 用 LAN 列表加入**：B 打開 network terminal → 列表出現 `<A 的裝置名稱>'s camp  <A 的 IP>:7801`。如果沒出現，按 R 重新搜尋。
- [ ] **2.2** B 選這一列按 Enter → 面板關閉，B 面前 3 m 出現一扇門，左上角提示 `A gate to … is open`。
- [ ] **2.3** B 先不要走進去，等 5 秒 → B 還在自己的 camp（還沒有連線）。
- [ ] **2.4** B 走進門 → 提示 `Joining …` → B 出現在 A 的 camp。
  - A 的畫面看得到 B，B 的畫面看得到 A。
  - B 的 log 有 `leaving to join <A 的 IP>:7801` 和 `Client local player ready`。
- [ ] **2.5** B 走到 level terminal 按 F → 提示自己不是房主，看不到關卡列表。
- [ ] **2.6** A 走到 level terminal 選一個關卡 → 兩人都看到 gate 打開 → 兩人都走進去 → 進入關卡。
- [ ] **2.7** 在關卡裡玩一陣子：互相看得到對方移動、開火、受傷。
- [ ] **2.8** 完成任務（預設是打倒 20 個敵人）→ 出現回程門 → 兩人回到 camp。也可以由 A 中止關卡，兩人會直接回到 camp。
- [ ] **2.9** B 打開 network terminal → 最後一列是 `Return to my camp` → 選它 → B 回到自己的 camp；log 有 `returning to its own camp`。A 的畫面上 B 消失。
- [ ] **2.10 用輸入的 IP 加入**：B 打開 terminal 直接打字。
  - 只會接受數字和 `.`，字母和其他符號都會被忽略。
  - 打錯時用 Backspace 刪除。
  - 輸入 A 的 IP 按 Enter → 開門 → 走進去 → 加入 A。
- [ ] **2.11 格式錯誤**：輸入 `192.168.1` 按 Enter → 面板裡出現紅色的錯誤說明，不會開門。
- [ ] **2.12 用命令列加入**：B 關掉遊戲，改用 `-ip` 啟動，應該會直接進入 A 的 camp。
  - macOS：
    ```bash
    open -a unity-network-example.app --args -ip <A 的 IP>
    ```
  - Windows：`unity-network-example.exe -ip <A 的 IP>`
- [ ] **2.13 host 離開**：B 在 A 的 camp 時，A 關掉遊戲 → 大約 10 秒後，B 退回自己的 camp，畫面提示 `Disconnected from …`；log 有 `falling back`。

## 3. 流程 A：dedicated server

在第三台機器、或 A 或 B 其中一台上啟動 dedicated server（見 §0）。啟動 log 要有 `answering LAN discovery on udp 47777 for game port 7801`。

- [ ] **3.1** A 打開 network terminal → 列表出現 `dedicated server :7801` → 加入（開門、走進去）。
- [ ] **3.2** B 用同樣方式加入 → 兩人在 dedicated server 的 camp。
- [ ] **3.3** B 走到 level terminal → 提示不是房主（A 比較早進來，peer id 比較小，所以 A 是房主）。
- [ ] **3.4** A 開關卡 → 兩人進入 → 完成或中止 → 回到 camp。
- [ ] **3.5 房主交接**：A 用 `Return to my camp` 離開 → B 走到 level terminal → 現在 B 可以開關卡。
- [ ] **3.6** 關掉 dedicated server（Ctrl-C）→ 大約 10 秒後，B 退回自己的 camp。

## 4. 異常情況

- [ ] **4.1 連到沒有人在聽的 IP**：輸入一個不存在的 IP → 開門 → 走進去 → 大約 10 秒後退回自己的 camp，畫面提示 `Could not join …`。
- [ ] **4.2 同一台電腦開兩個 player**（都不帶參數）：
  - 第二個會提示 `Port 7801 is in use; your camp is on port 7802`。
  - 第二個的 LAN discovery 可能會失敗（47777 被第一個占用）；把結果記下來。
- [ ] **4.3 版本不一致**（選做）：用不同的 bundle build 一個 player 去加入 → 應該在 catalog sync 階段失敗，然後退回自己的 camp。記下畫面上和 log 裡實際的訊息。
- [ ] **4.4 防火牆擋住**（選做）：在 host 那台拒絕防火牆詢問 → 另一台從列表看不到它，輸入 IP 也連不上。確認退回 camp 的提示看得懂。

---

## 5. 結果記錄

| 項目 | 結果（✅ / ❌ / 跳過） | 備註（log 片段、截圖、作業系統） |
|---|---|---|
| 1.1–1.4 單機 | | |
| 2.1–2.4 LAN 加入、開門 | | |
| 2.5–2.8 關卡循環（listen） | | |
| 2.9 回自己的 camp | | |
| 2.10–2.12 輸入 IP、`-ip` | | |
| 2.13 host 離開 | | |
| 3.1–3.6 dedicated | | |
| 4.1–4.4 異常 | | |

失敗的項目請附上兩邊的 Player.log（或 dedicated server 的輸出），以及大約的發生時間。

---

## 6. 已知的限制（不算失敗）

- 退回自己的 camp 要等 GNS 的連線逾時，大約 10 秒。
- 切換時沒有 fade，只有畫面上的提示文字。
- 門的 portal 畫面沒有顯示對面的景象。
- 每次切換世界（加入別人的世界或回到自己的 camp），物品、彈藥和裝備都會變回那個 camp 的預設值，因為存檔系統和角色資料都還沒做。
- 只有 host 有武器預測；client 開火只有外觀效果，數值由 server 決定。
- docker 裡的 dedicated server 不會出現在 LAN 列表裡，要用 IP 加入。
- 手把無法輸入 IP，只能用手把選 LAN 列表。
