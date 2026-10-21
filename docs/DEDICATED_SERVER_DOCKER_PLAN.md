# Dedicated Server Docker Image Plan

目標：產出一個可以交給外部託管平台部署的 dedicated server docker image。
候選平台：**Edgegap** 與 **Amazon GameLift Servers**（Managed Containers fleet）。

狀態：規劃（2026-10-21）。尚未實作。

---

## 1. 現況（從 code 讀到的事實）

| 項目 | 現況 | 對 container 的影響 |
|---|---|---|
| Server 進入點 | `//app:app --mode=dedicated_server --port=N`（`run_server.sh`） | 可直接當 container 的 entrypoint |
| 傳輸層 | GameNetworkingSockets 1.5.1，單一 UDP port | 兩個平台都支援 UDP port mapping |
| Catalog | `--gameplay-catalog-bundle=bundle.zip`，client 從 server 同步 catalog | image 內附 `bundle.zip`，路徑要明確傳入，不靠 `argv[0]` 推算 |
| Log | spdlog 到 stdout | 兩個平台都收 stdout，不用改 |
| **Linux build** | **不存在**。`third_party/BUILD` 的 `:openssl` alias 只有 `macos` / `windows` 分支；`GameNetworkingSockets.BUILD` 的 default 分支寫死 `OSX` define | **P0 阻擋項**：image 只能跑 Linux |
| **關機** | `dedicated_server_app.cc` 主迴圈是 `while (true)`，沒有 SIGTERM 處理 | 平台縮容 / 結束 session 時會被硬殺；GameLift 需要主動回報 `ProcessEnding` |
| Session 模型 | 一個 process 跑一個常駐世界（respawn、patrol、spawner） | 兩個平台都以「一場 session = 一個 process / container」計費，需要決定 server 何時結束（見 §6） |
| Build config | 記憶：非 `-c opt` 的 server 慢 27 倍 | image 一律 `-c opt` |
| 開發機 | Apple Silicon（arm64） | Edgegap 要 `linux/amd64`；在 Mac 上用 qemu 跑 Bazel 會非常慢 → 需要原生 x86_64 Linux 的 build 環境 |

LAN discovery 是 opt-in 的 kernel API，dedicated server 沒有開，container 內不用處理。

本計畫**不動 kernel ABI、snapshot schema、packet schema**，Unity package 的 `bundle.bytes` 也不用更新。所有改動都在 `app/`、`third_party/`、`tools/docker/`。

---

## 2. 兩個平台的要求

### Edgegap
- 吃任何 registry 的 docker image（Edgegap 自己的 registry 最省事），**linux/amd64**。
- 在 「app version」 設定 container 內部 port（例如 `7801/UDP`）、vCPU、記憶體。對外 port 是隨機分配的，透過 deployment API 回傳給 matchmaker / client。
- 啟動時注入 `ARBITRIUM_*` 環境變數（port mapping JSON、request id、自我刪除用的 `ARBITRIUM_DELETE_URL` + token）。**變數名稱實作前要對照官方文件確認。**
- 不需要 SDK。Server 結束時呼叫 delete URL（或直接退出）讓 deployment 收掉。

### Amazon GameLift Servers（Managed Containers）
- Image 必須放在**同帳號、同 Region 的 ECR**。
- Container group definition 定義 container port range；GameLift 把它映射到 fleet 的 connection port range，container port 不能直接連。
- Game server container 是 essential：它掛掉整個 container group 會重啟。
- **必須整合 Server SDK ≥ 5.2.0**（C++）：`InitSDK` → `ProcessReady(port, log paths, callbacks)` → `OnStartGameSession` 裡 `ActivateGameSession` → 結束時 `ProcessEnding`；另外要實作 `OnHealthCheck`、`OnProcessTerminate`。
- 可先用 **Anywhere fleet** 在自己的機器上測 SDK 整合，不用先上 ECR。
- 也支援 Graviton（arm64），但先只做 amd64。

### 比較

| | Edgegap | GameLift Containers |
|---|---|---|
| Server 端程式改動 | 幾乎沒有（env → flags、自我刪除） | 要接 SDK（CMake 專案，需要包進 Bazel） |
| Image 存放 | 任何 registry | 只能 ECR |
| 最早可測時間 | P2 結束就能部署 | P4，SDK 整合後 |
| 風險 | 低 | SDK 的 Bazel 化與相依（asio/websocketpp/openssl/rapidjson） |

**建議：先做 Edgegap，同時把 server 改成「平台中立 + adapter」結構，GameLift 當第二個 adapter 接上。**

---

## 3. 架構

```
┌──────────── docker image (linux/amd64) ────────────┐
│ /opt/nk/entrypoint.sh   ← 讀平台 env，轉成 flags   │
│ /opt/nk/bin/server      ← //app:app (-c opt)        │
│ /opt/nk/data/bundle.zip ← gameplay catalog bundle   │
└─────────────────────────────────────────────────────┘
```

- **Platform adapter 放在 `app/`，不進 kernel。** 新增 `--platform=none|edgegap|gamelift`（預設 `none`，行為和今天一樣）。
  - `none`：今天的行為 + SIGTERM 優雅關機。
  - `edgegap`：只在 entrypoint 層處理（讀 `ARBITRIUM_*`、退出時 `curl` delete URL），C++ 不需要 HTTP client。
  - `gamelift`：C++ adapter 呼叫 Server SDK。只在 `--define=gamelift=1` 的 build variant 編進去，預設 build 不帶這個相依。
- **Server 生命週期 hook**（三個 adapter 共用）：`on_ready(port)`、`on_session_start()`、`should_exit()`、`on_exit(reason)`。主迴圈從 `while (true)` 改成 `while (!should_exit())`。
- **Image tag**：`<git short sha>-abi<N>-proto<N>`。client 和 server 必須用同一版 ABI/protocol，tag 上看得到才能避免部署錯版本。

### Dockerfile（multi-stage）

1. **builder**：`ubuntu:24.04` + bazelisk（鎖 7.4.1）+ clang/gcc + `libssl-dev`，`bazel build -c opt --config=linux //app:app //game_server/gameplay_catalog_bundle:bundle.zip`。
2. **runtime**：`debian:bookworm-slim`（或 `ubuntu:24.04`）+ `libssl3` + `ca-certificates` + `curl`（Edgegap 自我刪除用），non-root user，`EXPOSE 7801/udp`，`ENTRYPOINT ["/opt/nk/entrypoint.sh"]`。

Builder 必須在**原生 x86_64 Linux** 上跑（CI runner 或遠端 Linux 機器）。在 Mac 上 `docker buildx --platform linux/amd64` 會走 qemu 模擬，Bazel 全量 build 會慢到不能用；只適合產出後的 runtime 層。

---

## 4. 分階段

### P0 — Linux build（阻擋項）
- `third_party/BUILD`：`:openssl` 加 Linux 分支。先用系統 `libssl-dev`（`cc_library` + `linkopts = ["-lssl", "-lcrypto"]`）；之後要 hermetic 再換。
- `GameNetworkingSockets.BUILD`：default 分支的 `OSX` 改成依平台選 `POSIX` / `LINUX`。
- 確認 Jolt、Recast、ozz、yaml-cpp 在 Linux + `-c opt` 都能編。
- **完成條件**：在 x86_64 Linux（docker 內）`bazel build -c opt //app:app` 成功，server 起得來並印出 build info。

### P1 — Container 友善的 server
- SIGTERM / SIGINT → 設 flag → 主迴圈退出 → `Kernel_Destroy` → exit 0。
- Lifecycle hook 介面（§3），`--platform=none` 預設。
- Idle exit policy（見 §6 決策）：例如「最後一個玩家離開後 N 秒無人 → 退出」，用 flag 控制，預設關閉。
- **完成條件**：`docker stop` 在 1 秒內乾淨退出（exit code 0），log 有 shutdown 訊息。

### P2 — Image + 本機 smoke test
- `tools/docker/Dockerfile`、`tools/docker/entrypoint.sh`、`tools/docker/build_image.sh`（帶 tag 規則）。
- 本機 smoke：`docker run -p 7801:7801/udp`，從 Mac 用 `//app:app --mode=client --address=127.0.0.1:7801` 連進去，確認 catalog 同步、能進場。
- **完成條件**：image < 200 MB；本機 client 連得上；`docker stop` 乾淨退出。

### P3 — Edgegap
- App version：`7801/UDP`、vCPU/記憶體依 server perf baseline 決定。
- entrypoint 讀 `ARBITRIUM_*`，結束時呼叫 delete URL。
- 用 Edgegap API 手動開一個 deployment，從 Unity client 用回傳的 `IP:external port` 連進去。
- **完成條件**：真機 client 連上 Edgegap deployment，遊玩後 server 自行結束且 deployment 被收掉。

### P4 — GameLift
- 把 GameLift Server SDK 5.x（C++）包成 Bazel target（`third_party/gamelift_server_sdk.BUILD`），只在 `--define=gamelift=1` 下編。
- `app/gamelift_adapter.cc`：`InitSDK` / `ProcessReady` / `ActivateGameSession` / `ProcessEnding` / health check。
- 先用 **Anywhere fleet** 在 Linux 機器上驗證，再推 ECR 建 Managed Containers fleet。
- **完成條件**：Anywhere fleet 上能建 game session、client 連得上、session 結束後 process 正確回報 `ProcessEnding`。

### 不在範圍內
- Matchmaking / lobby / client 拿到 server 位址的流程（client 怎麼知道 IP:port）。
- CI/CD pipeline 本身（只提供 build script）。
- arm64（Graviton）image。

---

## 5. 驗證策略（token / cost）
- 每階段只做 smoke test：build 一個 target、起一個 container、連一個 client。
- 不跑全量 `bazel test`；P0 只確認 `//app:app` 在 Linux 能編能跑。
- 測試用的 Linux build 產物放 scratchpad，不 commit。
- Unity package 打包與 merge 到 `feat-unity-plugin` 不在本計畫內，由使用者處理。

---

## 6. 需要使用者決定

1. **Session 模型**：今天是常駐世界。上平台後要：
   - (a) 一場 match = 一個 container，沒人了就退出（兩個平台的計費模型都偏這種），或
   - (b) 長駐世界，平台只負責託管（Edgegap 要把 deployment 設成不自動收；GameLift 用一個長時間的 game session）。
2. **Linux build 在哪裡跑**：GitHub Actions x86_64 runner、自有 Linux 機器，還是雲端 VM。
3. **平台順序**：建議 Edgegap → GameLift。如果最後只會選一個，P4 或 P3 可以整段跳過。
4. **OpenSSL**：先用系統套件（快），還是一開始就 hermetic（Bazel 自己編，image 可重現性較好）。
