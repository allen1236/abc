# `&minr` 實驗與重現（benchmarks / scripts / 數據）

本文件說明 `script/` 實驗工具與論文 §5 實驗設定的對應。  
方法與術語：`spec.md`。CLI / report：`usage.md`。

## 0. 前置條件

- 從 **repo 根目錄**執行腳本
- `make abc` 產生 `./abc`
- Python 3.9+
- Benchmarks：`benchmarks/iscas89`、`benchmarks/itc99`（論文 §5：ISCAS'89、ITC'99；已 `strash;&dc2` 前處理後納入 repo，見 commit `b351d9e65` / `9b5884f68`）

## 1. 與論文 §5 的對應

| 論文 §5 | 本 repo |
|---------|---------|
| iterative MaxSAT stage，600s budget | `parallel.py` 的 `TOTAL_TIMEOUT=600`、`OPTIMIZE_MODE=1`（`-O 1`） |
| SAT-based refinement afterward | `REFINE_MODE=1`（`-x 1`） |
| 10 random **initial reset vectors** | `SEEDS`（如 0–9）+ `-r` |
| partial specification（§5.3） | `-D` / `DC_RATIO` |
| **reset ratio** \(RR=\#\mathrm{Reset}/\#\mathrm{Reg}\) | CSV / report 的 `r_f` |
| reset cost | `required_reset` |
| selected timeframe length | `best_k` |
| refinement runtime | `refine_sec` |

論文 Table 1 的 baseline reset ratio（\(k=0\)）對應 per-\(k\) 掃描或 report 中的 `k0_*` 欄位（`k.py` / detail CSV）。

## 2. 目錄慣例

| 路徑 | 用途 |
|------|------|
| `script/log/` | `&minr -o` report |
| `script/exp/` | detail / stat CSV |
| `script/regression_results/` | `minr_regression.py` 中間產物 |
| `script/minr_regression_baseline.json` | 迴歸 golden |

## 3. `parallel.py`：批次實驗

Grid：`benchmarks × seeds × dc_ratio`。

每 job：

```text
read_aiger benchmarks/...; &get; &minr -O 1 ... -o <log>
```

對應 **iterative MaxSAT solving**（§4.3）+ **SAT-based refinement**（§4.4，若 `REFINE_MODE>0`）。

```bash
python3 script/parallel.py --prefix <prefix> [--max-workers N]
```

常數區：`BENCHMARKS`, `SEEDS`, `DC_RATIO`, `OPTIMIZE_MODE`, `TOTAL_TIMEOUT`, `REFINE_MODE`。

## 4. `stat.py`：彙總

依 `(circuit, dc_ratio)` 分組，平均：

- reset cost / **reset ratio**（`required_reset`, `r_f`, `r_s`）
- `cut_verified`, `cec_verified`（cut 條件 vs safe realization）
- `runtime_cpu_sec` / `runtime_sec`

## 5. `k.py`：fixed-\(k\) 掃描

每個 \(k\) 各給完整 `-t` 跑 **§4.2 fixed-timeframe** 子問題（不帶 `-O`），用於 ablation 或 \(k=0\) baseline。

## 6. `minr_regression.py`

固定 benchmark 子集做 CI 迴歸；比對 `required_reset`、`r_f`、`r_s`、verify 與 baseline JSON。

## 7. 時間欄位

`-t` 與 `runtime_cpu_sec` 為 **thread CPU**；外部 EvalMaxSAT 的 child CPU 會加回（見 `usage.md`）。

## 8. 文件交叉引用

- `spec.md`：safe realization、initial/final reset vector、iterative MaxSAT、refinement
- `implementation.md`：`Minr_DumpReport()`、miter 實作
- `usage.md`：report 欄位 ↔ 論文對照表
