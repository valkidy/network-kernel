# Dedicated Server Docker Image Plan

目標：把 dedicated server 包成 docker image 部署到 **Edgegap**，用本機 Unity client 連過去，
在真實網路延遲下驗證遊戲表現。

狀態：規劃（2026-10-21 定稿決策）。尚未實作。

## 決策（2026-10-21）

| # | 問題 | 決定 |
|---|---|---|
| 1 | Session 模型 | **常駐世界，同時最多 4 名玩家**。match 流程還不完整，不做「一場一個 container」。 |
| 2 | Linux build 在哪裡跑 | **x86_64 Windows 上的 WSL2 + Docker**（原生 amd64，不是模擬）。 |
| 3 | 平台 | **只做 Edgegap**。GameLift 不在範圍內。 |
| 4 | OpenSSL | **由 Bazel 從 source 編**。缺少的 source 由使用者部署（清單見 §5）。 |

---

## 1. 現況（從 code 讀到的事實）

| 項目 | 現況 | 影響 |
|---|---|---|
| Server 進入點 | `//app:app --mode=dedicated_server --port=N`（`run_server.sh`） | 直接當 container entrypoint |
| 傳輸層 | GameNetworkingSockets 1.5.1，單一 UDP port | Edgegap 支援 UDP port mapping |
| Catalog | `--gameplay-catalog-bundle=bundle.zip`，client 從 server 同步 | image 內附 `bundle.zip`，路徑明確傳入 |
| Log | spdlog 到 stdout | Edgegap 收 stdout，不用改 |
| **Linux build** | **不存在**。`third_party/BUILD` 的 `:openssl` 與 WORKSPACE 的 `zlib` alias 都只有 `macos` / `windows` 分支；`GameNetworkingSockets.BUILD` default 分支寫死 `OSX` | P0 阻擋項 |
| OpenSSL 來源 | macOS 用 Homebrew 的系統 lib；Windows 用 checked-in 預編 zip。**沒有任何平台從 source 編** | Linux 要新增 source build（§5） |
| zlib | 已有 `@fossils_zlib`（source） | Linux alias 指向它即可，不用新 source |
| **關機** | 主迴圈 `while (true)`，無 SIGTERM 處理 | Edgegap 停 deployment 時會被硬殺 |
| **玩家上限** | **沒有**。`gns_transport.cc` 的 `Connecting` 分支接受每一個連線；kernel API 也沒有踢人的函式 | 4 人上限需要一個小的 kernel 改動（§4 P1） |
| Build config | 非 `-c opt` 的 server 慢 27 倍 | image 一律 `-c opt` |

LAN discovery 是 opt-in API，dedicated server 沒開，container 內不用處理。

---

## 2. Session 模型比較

| | (a) 一場 match 一個 container | (b) 常駐世界（**採用**） |
|---|---|---|
| Server 何時結束 | 最後一人離開 N 秒後自行退出，呼叫 `ARBITRIUM_DELETE_URL` | 不主動結束；手動停 deployment，或 crash 時 container 退出 |
| 需要的 server 改動 | idle 計時 + 退出流程 + 自我刪除 | 只要 SIGTERM 優雅關機 |
| 誰開 deployment | matchmaker / lobby 依需求開（這部分還不存在） | 人工用 Edgegap dashboard / API 開一台，地址發給測試者 |
| 世界狀態 | 每場重置 | 一直累積（respawn、spawner 持續跑） |
| 成本 | 只在有人玩時計費 | 開著就計費，測完要記得停 |
| 適合 | 正式上線 | **現在的實驗：驗證真實 ping 下的表現** |

(b) 的已知限制：世界跑很久之後的狀態累積（spawner、掉落物）沒被長時間測過。實驗期間建議每次測試前重開 deployment。

---

## 3. 架構

```
┌──────────── docker image (linux/amd64) ────────────┐
│ /opt/nk/entrypoint.sh   ← 讀 ARBITRIUM_* env，轉成 flags │
│ /opt/nk/bin/server      ← //app:app (-c opt)        │
│ /opt/nk/data/bundle.zip ← gameplay catalog bundle   │
└─────────────────────────────────────────────────────┘
```

- **Edgegap 相關全部在 entrypoint script。** 常駐模型下 server 不需要知道自己在 Edgegap 上；`ARBITRIUM_*` 只用來印出對外 port 方便除錯。C++ 不加 HTTP client、不加平台 adapter。
- **Image tag**：`<git short sha>-abi<N>-proto<N>`。client 和 server 必須同版 ABI/protocol，tag 上看得到才不會部署錯版本。
- **Dockerfile（multi-stage）**
  1. **builder**：`ubuntu:24.04` + bazelisk（鎖 7.4.1）+ gcc/clang + `perl` + `make`（OpenSSL configure 需要），build `-c opt --config=linux //app:app //game_server/gameplay_catalog_bundle:bundle.zip`。
  2. **runtime**：`debian:bookworm-slim`，non-root user，`EXPOSE 7801/udp`，`ENTRYPOINT ["/opt/nk/entrypoint.sh"]`。OpenSSL 靜態連結，runtime 不需要 `libssl3`。
- **Build 環境（WSL2）**
  - Repo clone 在 WSL 檔案系統（`~/src/...`），不放 `/mnt/c`。
  - `.wslconfig` 記憶體 ≥ 16 GB。
  - Build 一律在 builder stage 裡跑，glibc 版本與 runtime image 一致。
  - 推到 Edgegap container registry。

---

## 4. 分階段

### P0 — Linux build（阻擋項）
- `third_party/BUILD`：`:openssl` 加 `//engine:linux` 分支 → 新的 source-built target（§5）。
- WORKSPACE `zlib` alias：加 `@//engine:linux` → `@fossils_zlib//:zlib`。
- `GameNetworkingSockets.BUILD`：default 分支的 `OSX` define 改成依平台選（Linux 用 `POSIX` + `LINUX`）。
- 確認 Jolt、Recast、ozz、yaml-cpp、protobuf 在 Linux + `-c opt` 能編。
- **完成條件**：WSL2 的 builder container 內 `bazel build -c opt //app:app` 成功，server 起得來並印出 build info。

### P1 — Container 友善的 server
- SIGTERM / SIGINT → 設 flag → 主迴圈退出 → `Kernel_Destroy` → exit 0。
- **4 人上限**：`KernelSessionRulesConfig` 加一個 `max_connected_peers` 欄位（0 = 不限，維持現行行為），transport 在 `Connecting` 時超過上限就拒絕連線。app 加 `--max-players=N`，entrypoint 預設 4。
  - 這是 kernel 改動。欄位用 `struct_size` 版本化、預設值維持舊行為，預期**不需要 ABI bump**；實作時要確認。
  - 被拒絕的 client 目前只會看到連線中斷，沒有「伺服器已滿」的訊息。實驗階段可接受。
- **完成條件**：`docker stop` 1 秒內乾淨退出；第 5 個 client 被拒絕，前 4 個不受影響。

### P2 — Image + 本機 smoke test（WSL2）
- `tools/docker/Dockerfile`、`tools/docker/entrypoint.sh`、`tools/docker/build_image.sh`（含 tag 規則）。
- 本機 smoke：`docker run -p 7801:7801/udp`，Windows 端 client 連 `127.0.0.1:7801`，確認 catalog 同步、能進場。
- **完成條件**：image < 200 MB；本機 client 連得上；`docker stop` 乾淨退出。

### P3 — Edgegap 部署
- App version：internal port `7801/UDP`，vCPU / 記憶體依 server perf baseline 選；確認 deployment 沒有預設最長存活時間會把常駐世界收掉。
- 用 dashboard 或 API 開一個 deployment，選離測試者近的地區。
- Unity client 用 Edgegap 回傳的 `IP:external port` 連進去（Mac 與 Windows 各一台）。
- **完成條件**：4 名玩家同時在 Edgegap 上的常駐世界裡玩；記錄實測 RTT 與 server `tick cadence` log；手動停 deployment 時 server 乾淨退出。

### 不在範圍內
- GameLift。
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

## 7. 交接資訊
- 本計畫不改 snapshot / packet schema。P1 的 session rules 欄位預期不需要 ABI bump（實作時確認）。
- Unity package 的 `bundle.bytes` 不需要更新。
