# Dedicated Server Docker Image Plan

目標：把 dedicated server 包成 docker image，**部署在 x86_64 Windows 的 WSL2 上**，讓其他機器上的 Unity client 透過真實網路連過去，驗證遊戲在真實網路下的表現。

狀態：規劃（2026-10-21 定稿決策）。尚未實作。

## 決策（2026-10-21）

| # | 問題 | 決定 |
|---|---|---|
| 1 | Session 模型 | **常駐世界，同時最多 4 名玩家**。match 流程還不完整，不做「一場一個 container」。 |
| 2 | Build 與部署在哪裡 | **x86_64 Windows 上的 WSL2 + Docker**。同一台機器負責 build 和執行 server。 |
| 3 | 雲端平台 | **Edgegap 暫緩（pending）**，見 §7。GameLift 不在範圍內。 |
| 4 | OpenSSL | **由 Bazel 從 source 編**。缺少的 source 由使用者部署（清單見 §5）。 |

---

## 1. 現況（從 code 讀到的事實）

| 項目 | 現況 | 影響 |
|---|---|---|
| Server 進入點 | `//app:app --mode=dedicated_server --port=N`（`run_server.sh`） | 直接當 container entrypoint |
| 傳輸層 | GameNetworkingSockets 1.5.1，單一 UDP port | 只需要開一個 UDP port |
| Catalog | `--gameplay-catalog-bundle=bundle.zip`，client 從 server 同步 | image 內附 `bundle.zip`，路徑明確傳入 |
| Log | spdlog 到 stdout | `docker logs` 直接看 |
| **Linux build** | **不存在**。`third_party/BUILD` 的 `:openssl` 與 WORKSPACE 的 `zlib` alias 都只有 `macos` / `windows` 分支；`GameNetworkingSockets.BUILD` default 分支寫死 `OSX` | P0 阻擋項 |
| OpenSSL 來源 | macOS 用 Homebrew 的系統 lib；Windows 用 checked-in 預編 zip。**沒有任何平台從 source 編** | Linux 要新增 source build（§5） |
| zlib | 已有 `@fossils_zlib`（source） | Linux alias 指向它即可，不用新 source |
| **關機** | 主迴圈 `while (true)`，無 SIGTERM 處理 | `docker stop` 會等 10 秒後硬殺 |
| **玩家上限** | **沒有**。`gns_transport.cc` 的 `Connecting` 分支接受每一個連線；kernel API 也沒有踢人的函式 | 4 人上限需要一個小的 kernel 改動（§4 P1） |
| Build config | 非 `-c opt` 的 server 慢 27 倍 | image 一律 `-c opt` |

LAN discovery 是 opt-in API，dedicated server 沒開，container 內不用處理。

---

## 2. Session 模型比較

| | (a) 一場 match 一個 container | (b) 常駐世界（**採用**） |
|---|---|---|
| Server 何時結束 | 最後一人離開 N 秒後自行退出 | 不主動結束；手動 `docker stop`，或 crash 時由 Docker 重啟 |
| 需要的 server 改動 | idle 計時 + 退出流程 | 只要 SIGTERM 優雅關機 |
| 誰啟動 server | matchmaker / lobby（還不存在） | 人工在 WSL2 上 `docker compose up` |
| 世界狀態 | 每場重置 | 一直累積（respawn、spawner 持續跑） |
| 適合 | 正式上線 | **現在的實驗** |

(b) 的已知限制：世界跑很久之後的狀態累積（spawner、掉落物）沒被長時間測過。建議每輪測試前重啟 container。

---

## 3. 架構

```
Windows host (x86_64)
 └─ WSL2 (Ubuntu)
     └─ Docker
         └─ container (linux/amd64)
             /opt/nk/entrypoint.sh   ← 讀 NK_* env，轉成 flags
             /opt/nk/bin/server      ← //app:app (-c opt)
             /opt/nk/data/bundle.zip ← gameplay catalog bundle
             UDP 7801
```

- **設定走環境變數**：`NK_PORT`（預設 7801）、`NK_MAX_PLAYERS`（預設 4）、`NK_NETCODE`（預設 `standard`）。entrypoint 轉成 server flags。之後接 Edgegap 只要在 entrypoint 加一段讀 `ARBITRIUM_*`，image 本身不用改。
- **Image tag**：`<git short sha>-abi<N>-proto<N>`。client 和 server 必須同版 ABI/protocol，tag 上看得到才不會跑錯版本。
- **Dockerfile（multi-stage）**
  1. **builder**：`ubuntu:24.04` + bazelisk（鎖 7.4.1）+ gcc/clang + `perl` + `make`（OpenSSL configure 需要），build `-c opt --config=linux //app:app //game_server/gameplay_catalog_bundle:bundle.zip`。
  2. **runtime**：`ubuntu:24.04`（與 builder 同 glibc），non-root user，`EXPOSE 7801/udp`，`ENTRYPOINT ["/opt/nk/entrypoint.sh"]`。OpenSSL 靜態連結，runtime 不需要 `libssl3`。
- **啟動方式**：`tools/docker/compose.yaml`，`restart: unless-stopped`、`ports: "7801:7801/udp"`、`stop_grace_period: 5s`。

### WSL2 環境設定
- Repo clone 在 WSL 檔案系統（`~/src/...`），不放 `/mnt/c`（Bazel 會慢好幾倍）。
- `%UserProfile%\.wslconfig`：記憶體 ≥ 16 GB；網路模式見下方「外部 client 怎麼連進來」的實測結果。
- Docker：Docker Desktop（WSL2 backend）或 WSL 內直接裝 Docker Engine 擇一。
- Build 一律在 builder stage 裡跑，glibc 版本與 runtime image 一致。

### 外部 client 怎麼連進來
WSL2 的 UDP 要經過 Windows 才會到 container。

**實測（2026-10-21，Docker Desktop 4.96.0，WSL NAT 模式）**：用 `alpine nc -lu` 接 7802/udp——
- container 內部、WSL 的 `127.0.0.1`：收得到。
- Windows 送到 `127.0.0.1`：**收不到**。Docker Desktop / WSL 的 localhost 轉發在這台機器上不轉 UDP，`netstat` 也看不到 7801/7802。
- Windows 送到 WSL 的 IP（`wsl hostname -I`，例如 `172.26.150.212`）：收得到。

**結論（同日，改用 mirrored 後實測）**：`.wslconfig` 設 `networkingMode=mirrored`（WSL 直接拿到 Windows 的 `192.168.1.x`），再用**系統管理員** PowerShell 加 Hyper-V 防火牆規則：

```powershell
New-NetFirewallHyperVRule -Name "network-kernel-udp" -DisplayName "network-kernel UDP 7801-7802" -Direction Inbound -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol UDP -LocalPorts 7801,7802 -Action Allow
```

| 從哪裡連 | 連哪個位址 | 狀態 |
|---|---|---|
| 同一台 Windows | `127.0.0.1:7801` | 可行（mirrored） |
| 區網其他電腦 | Windows 的 WiFi IP，例如 `192.168.1.102:7801` | **可行**：Mac native client 經 WiFi 連入，catalog 同步（528178 B、約 2.7 s）、PlayerJoined |
| 網際網路 | 路由器把 UDP 7801 轉到 Windows 的 WiFi IP | 未測 |

**macOS client 的區域網路權限（2026-10-21 實測）**：Unity Editor 連 Windows 的 `192.168.1.102:7801` 一律約 10 秒逾時（`Native catalog sync failed: Disconnected`，連 `Connected` 都沒有），但同一台 Mac 從「終端機」跑 native client 連同一位址成功。原因是 macOS 15 的「隱私權與安全性 → 區域網路」權限算在負責的 App 頭上：終端機有權限，Unity（或啟動它的 Unity Hub）沒有。連本機 `127.0.0.1` / 自己的 IP 不受影響，所以本機測試看不出來。
- 驗證 / 繞路：從終端機啟動 Editor（`/Applications/Unity/Hub/Editor/<版本>/Unity.app/Contents/MacOS/Unity -projectPath ~/Projects/unity-network-example`），沿用終端機的權限 → 連上。
- 正式設定：在「區域網路」裡打開 Unity 與 Unity Hub，兩者完全結束後重開。
- Windows 有兩張 Wi-Fi（`.102`、`.104`）在同一網段，曾被懷疑是回覆走錯網卡；終端機的 native client 在同樣配置下能連，所以不是原因。

注意：mirrored 模式下 Windows 的 `netstat` 看不到 WSL/Docker 開的 port，不能拿來判斷有沒有在聽；一般 Windows 防火牆規則也管不到 WSL，要用 Hyper-V 規則。

### 「真實 ping」的三種測法

| 測法 | 延遲來源 | 需要的設定 | 說明 |
|---|---|---|---|
| 區網 | Mac / 其他電腦在同一個網路 | 上表前三關 | RTT 約 1–5 ms，只能驗證連線流程 |
| 網際網路 | 遠端測試者經路由器連進來 | 上表四關全部 | 最接近真實；受家用上傳頻寬限制 |
| 人工注入 | container 內 `tc netem` 加延遲 / 掉包 | container 加 `cap_add: NET_ADMIN`，runtime image 加 `iproute2` | 可重現；適合跟 client 的 fake lag 交叉比對 |

建議順序：區網確認能連 → `tc netem` 做可重現的延遲測試 → 有遠端測試者再開路由器轉發。

---

## 4. 分階段

### P0 — Linux build（阻擋項）
> 狀態（2026-10-21）：**完成**（branch `claude/dedicated-server-p0`，d97d27d6）。WSL2 上 `tools/docker/build_linux.sh` 通過：gcc 13.3.0、`build_config=release`，server 跑滿 10 秒，tick cadence 30.00 Hz、work avg 65 us / max 170 us（空世界、無玩家）。
>
> - OpenSSL 4.0.3 用 genrule 跑它自己的 `Configure` + `make`（`third_party/openssl_linux.BUILD`），不需要 `rules_foreign_cc` / `bazel_skylib`。
> - 唯一的 gcc 問題：GNS 的 `, ## __VA_ARGS__` 在 ISO `-std=c++20` 下不會吞掉逗號，Linux 上 GNS 改用 `-std=gnu++20`。
> - Server 實際綁定所有介面（GNS 收到的是 cleared address）；log 裡的 `listening on 127.0.0.1` 是寫死的字串，P1 一併修正。

- `third_party/BUILD`：`:openssl` 加 `//engine:linux` 分支 → 新的 source-built target（§5）。
- WORKSPACE `zlib` alias：加 `@//engine:linux` → `@fossils_zlib//:zlib`。
- `GameNetworkingSockets.BUILD`：default 分支的 `OSX` define 改成依平台選（Linux 用 `POSIX` + `LINUX`）。
- 確認 Jolt、Recast、ozz、yaml-cpp、protobuf 在 Linux + `-c opt` 能編。
- **完成條件**：WSL2 的 builder container 內 `bazel build -c opt //app:app` 成功，server 起得來並印出 build info。

### P1 — Container 友善的 server
> 狀態（2026-10-21）：**完成**（branch `claude/dedicated-server-p0`）。macOS 上驗證：SIGTERM 後 26 ms 內 exit 0；`gns_transport_test` 用真實 loopback UDP 驗證上限 1 時第 2 個 client 被拒、第 1 個離開後第 3 個進得來；`kernel_api_test` 驗證舊的 8 bytes struct 仍被接受（該測試之後會停在 main 上既有的 `KERNEL_ABI_VERSION == 72u` 舊斷言，與本次無關）。
>
> - 上限做在 `GnsTransport::set_max_connections`，只有 dedicated server 會設；listen server 忽略這個欄位。
> - `Kernel_SetSessionRules` 改成只複製 caller 宣告的 `struct_size` bytes，最小接受 8（原本兩個欄位）。**不需要 ABI bump**，Unity 端的 C# struct 不改也能用。
> - Linux 上尚未重跑；P2 的 image smoke 會涵蓋。

- SIGTERM / SIGINT → 設 flag → 主迴圈退出 → `Kernel_Destroy` → exit 0。
- `dedicated_server_app.cc` 的 `listening on 127.0.0.1:{}` 改成實際的綁定方式（所有介面）。
- **4 人上限**：`KernelSessionRulesConfig` 加一個 `max_connected_peers` 欄位（0 = 不限，維持現行行為），transport 在 `Connecting` 時超過上限就拒絕連線。app 加 `--max-players=N`。
  - 這是 kernel 改動。欄位用 `struct_size` 版本化、預設值維持舊行為，預期**不需要 ABI bump**；實作時要確認。
  - 被拒絕的 client 目前只會看到連線中斷，沒有「伺服器已滿」的訊息。實驗階段可接受。
- **完成條件**：`docker stop` 1 秒內乾淨退出；第 5 個 client 被拒絕，前 4 個不受影響。

### P2 — Image + WSL2 部署
> 狀態（2026-10-21）：WSL2 上 `build_image.sh` 通過（991aba08）：image `network-kernel-server:991aba08-abi109-proto3` **136 MB**；client container 連入、server 出現 PlayerJoined；`docker stop` 282 ms、exit 0。compose 常駐後，Windows 本機 Unity client 連 `127.0.0.1:7801` 成功；**P2 完成**。執行 `tools/docker/build_image.sh`：build → stage → image → 自動 smoke（另一個 container 用 client 模式連進來、server log 要出現 PlayerJoined、`docker stop` 要 exit 0）。
>
> - Runtime base 改用 `ubuntu:24.04`（不是 `debian:bookworm-slim`）：binary 是在 ubuntu 24.04 上編的，需要同版 glibc（2.39）。bookworm 只有 2.36。
> - 只有 binary + `bundle.zip`：bundle 模式下 catalog、navmesh、collision scene 全部從 bundle 讀。
> - Tag：`<sha>[-dirty]-abi<N>-proto<N>`，外加 `latest`。
> - macOS 上已驗證：entrypoint 的參數組合能啟動 server；catalog sync client 連進來時 server log 出現 `event type=2`（smoke 依賴這一行）。

- `tools/docker/Dockerfile`、`tools/docker/entrypoint.sh`、`tools/docker/compose.yaml`、`tools/docker/build_image.sh`（含 tag 規則）。
- 在 WSL2 上 `docker compose up -d`；Windows 本機的 client 連 `127.0.0.1:7801` 做第一輪 smoke。
- **完成條件**：image < 200 MB；Windows 本機 client 連得上、catalog 同步、能進場；`docker compose down` 乾淨退出。

### P3 — 真實網路測試
> 狀態（2026-10-22）：工具已加（branch `claude/dedicated-server-p0`），尚未在 WSL2 上跑。
>
> - Runtime image 加 `iproute2`；compose 加 `cap_add: NET_ADMIN`（只有 `docker exec -u root` 用得到，server 仍以 nk 執行）。
> - `tools/docker/netem.sh <delay_ms> [jitter_ms] [loss_pct]` / `off` / `show`：即時改，不用重啟 server。
> - netem 只作用在 container 的**出站**（server → client）：delay 直接加在 RTT 上；loss 只丟 server → client 的封包。要對稱的雙向掉包需要再加 ifb，目前不做。
> - 另一個選項是 GNS 內建的 `NETWORK_KERNEL_FAKE_LAG_MS` / `_JITTER_MS` / `_LOSS_PCT` 環境變數（啟動時讀，要重啟）。兩者可以交叉比對。
>
> **量測方式**：Mac 的 Unity（從終端機啟動，見 §3 的區域網路權限）每秒在 Editor.log 印一行 `[G0]`，含 `rtt` / `jitter` / `loss` / `worldFreezeFrames` / `jumps` / `maxJump` / `staleActors`。每個條件玩 1–2 分鐘，從 Editor.log 取這些行做統計；server 端看 `tick cadence`。
>
> | 條件 | netem 指令 |
> |---|---|
> | 基準（區網） | `netem.sh off` |
> | +50 ms | `netem.sh 50` |
> | +100 ms | `netem.sh 100` |
> | +150 ms | `netem.sh 150` |
> | +100 ms、±20 ms jitter、1% loss | `netem.sh 100 20 1` |

- 依 §3 打通 Windows 防火牆，Mac 上的 Unity client 從區網連進來。
- `tc netem` 注入固定延遲（例如 50 / 100 / 150 ms）與掉包，記錄 client 表現與 server `tick cadence` log。
- （選用）路由器轉發 UDP 7801，讓遠端測試者連進來。
- **完成條件**：4 名玩家同時在常駐世界裡玩；有一份紀錄列出各延遲條件下的 RTT 與觀察到的問題。

### 不在範圍內
- Edgegap（pending，§7）、GameLift。
- Matchmaking / lobby；client 取得 server 位址靠手動發送。
- CI/CD pipeline（只提供 build script）。
- arm64 image。
- Unity package 打包與 merge 到 `feat-unity-plugin`（使用者負責）。

---

## 5. OpenSSL source build：需要使用者部署的 source

做法：用 **`rules_foreign_cc` 的 `configure_make`** 跑 OpenSSL 自己的 `Configure` + `make`，產出靜態 `libssl.a` / `libcrypto.a`。手寫 OpenSSL 的 `cc_library` 不可行：它的 header 與 assembly 要 perl script 產生。

目前 WORKSPACE **沒有** `rules_foreign_cc`、`bazel_skylib`。需要以下 source（版本為建議值，部署時請鎖定 sha256）：

| Source | 建議版本 | 用途 |
|---|---|---|
| OpenSSL source tarball | 3.5.x（LTS） | 本體 |
| `rules_foreign_cc` | 0.12.x 以上（支援 Bazel 7 WORKSPACE 模式的版本） | `configure_make` rule |
| `bazel_skylib` | 1.7.x | `rules_foreign_cc` 的相依 |
| `rules_foreign_cc_dependencies()` 會額外下載的東西（例如 `bazel_features`、`rules_python`，以及 make / pkg-config 的 source） | 依選定的 `rules_foreign_cc` 版本 | 使用者自行部署時，要在 `rules_foreign_cc_dependencies()` 關掉自動下載（`register_built_tools = False` 等參數），改用 builder image 內的系統 `make` |

Builder image 要有 `perl`（OpenSSL `Configure` 需要）和 `make`。這兩個是 build 工具，不進 runtime image。

**OpenSSL configure 參數草案**：`linux-x86_64 no-shared no-tests no-docs no-apps`。GNS 只用到 EVP（AES-GCM、SHA、X25519/Ed25519），之後可以再用 `no-*` 砍掉不需要的模組縮短 build 時間。

只影響 Linux。macOS（Homebrew）與 Windows（預編 zip）不動。

---

## 6. 驗證策略（token / cost）
- 每階段只做 smoke test：build 一個 target、起一個 container、連一個 client。
- 不跑全量 `bazel test`；P1 的 kernel 改動只跑 transport / session rules 相關測試。
- 測試用的 Linux build 產物放 scratchpad，不 commit。

---

## 7. Edgegap（pending）

暫緩，WSL2 的測試結果出來後再決定是否繼續。P0–P2 的產出可以直接沿用，接 Edgegap 時只需要：

- Image 改推到 Edgegap container registry（必須是 `linux/amd64`，WSL2 build 出來的就是）。
- App version 設定 internal port `7801/UDP`、vCPU / 記憶體（依 server perf baseline 選）。
- entrypoint 加一段讀 `ARBITRIUM_*` 環境變數（對外 port mapping 等），印在 log 方便除錯。**變數名稱實作前要對照官方文件確認。**
- 確認 deployment 沒有預設最長存活時間會把常駐世界收掉。
- Client 用 Edgegap 回傳的 `IP:external port` 連線。

---

## 8. 交接資訊
- 本計畫不改 snapshot / packet schema。P1 的 session rules 欄位預期不需要 ABI bump（實作時確認）。
- Unity package 的 `bundle.bytes` 不需要更新。
