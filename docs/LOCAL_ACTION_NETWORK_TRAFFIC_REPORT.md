# Local Action Network Traffic Report

Deterministic 60-second C++ application-message simulation. Transport framing, encryption, UDP/IP and link overhead are excluded.

- ABI: 41; protocol: 1; snapshot schema: 14; packet schema: 16
- Module: 0.6.4+r139; git: 3e50541; platform/config: macos/debug
- Input packet: 85 B
- Owner result batch: `36 + 12N` B (97 records at 1,200 B)
- Remote presentation batch: `36 + 20N` B (58 records at 1,196 B)
- Snapshot configured upper-bound: 18,000 B/s per client
- Stats Off/Basic/Detailed alter counters only; wire bytes are identical

| Peers | Commits/s | Relevance | Remote demand B/s | Delivered B/s | kbit/s | Dropped records | Packets | Avg packet B | Max packet B | Avg records/batch |
|---:|---:|:---|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 1 | sparse | 112.00 | 112.00 | 0.90 | 0 | 120 | 56.00 | 56 | 1.00 |
| 2 | 1 | full | 112.00 | 112.00 | 0.90 | 0 | 120 | 56.00 | 56 | 1.00 |
| 2 | 10 | sparse | 1120.00 | 1120.00 | 8.96 | 0 | 1200 | 56.00 | 56 | 1.00 |
| 2 | 10 | full | 1120.00 | 1120.00 | 8.96 | 0 | 1200 | 56.00 | 56 | 1.00 |
| 8 | 1 | sparse | 768.00 | 768.00 | 6.14 | 0 | 480 | 96.00 | 96 | 3.00 |
| 8 | 1 | full | 1408.00 | 1408.00 | 11.26 | 0 | 480 | 176.00 | 176 | 7.00 |
| 8 | 10 | sparse | 7680.00 | 7680.00 | 61.44 | 0 | 4800 | 96.00 | 96 | 3.00 |
| 8 | 10 | full | 14080.00 | 14080.00 | 112.64 | 0 | 4800 | 176.00 | 176 | 7.00 |
| 32 | 1 | sparse | 3072.00 | 3072.00 | 24.58 | 0 | 1920 | 96.00 | 96 | 3.00 |
| 32 | 1 | full | 20992.00 | 20992.00 | 167.94 | 0 | 1920 | 656.00 | 656 | 31.00 |
| 32 | 10 | sparse | 30720.00 | 30720.00 | 245.76 | 0 | 19200 | 96.00 | 96 | 3.00 |
| 32 | 10 | full | 209920.00 | 209920.00 | 1679.36 | 0 | 19200 | 656.00 | 656 | 31.00 |
| 32 | 30 | full overload | 629760.00 | 262016.00 | 2096.13 | 1044480 | 24960 | 629.85 | 656 | 29.69 |

## Thresholds and highest scenario

The highest case is 32 peers x 30 commits/s, full relevance. Its delivered remote presentation is 262016.00 B/s server aggregate and 8188.00 B/s for the busiest client.

- 1,200 B action packet: PASS (measured max 656 B)
- 8 KiB/s per-client remote token bucket: PASS (8188.00 B/s sustained average)
- 256 KiB/s server remote token bucket: PASS (262016.00 B/s)
- Overload budget drop: PASS (1044480 records dropped, never deferred)
- Highest per-client bidirectional application traffic: 30178.00 B/s (input 2550.00 + snapshot cap 18000 + owner 1440.00 + remote 8188.00)
- Highest server outbound application traffic: 884096.00 B/s

## Reproduction

```sh
bazel test //engine/src/tests/kernel_tests:network_stats_test
bazel-bin/engine/src/tests/kernel_tests/network_stats_test --report=/tmp/LOCAL_ACTION_NETWORK_TRAFFIC_REPORT.md
```
