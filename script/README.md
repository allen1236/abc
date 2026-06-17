# `script/` — minr 實驗與迴歸工具

本目錄放與 `&minr` 相關的 Python 腳本。術語與論文對照見 `doc/spec.md` §0；CLI / report 見 `doc/usage.md`。

## 論文指標 ↔ CSV / report

| 論文 | 腳本解析欄位 |
|------|----------------|
| reset cost | `required_reset` |
| reset ratio \(RR\) | `r_f` |
| timeframe length \(k\) | `best_k`（`-O 1`）或 per-\(k\) 欄位（`k.py`） |
| iterative MaxSAT stage | `-O 1` / `OPTIMIZE_MODE=1` |
| SAT-based refinement | `-x` / `REFINE_MODE` |
| initial reset vector（partial spec） | `-D` / `DC_RATIO` |

## 前置條件

- **Python**：建議 3.9+（`parallel.py` / `k.py` 依賴 `ThreadPoolExecutor.shutdown(cancel_futures=...)`）。
- **`abc` 二進位**：在專案根目錄執行 `make abc`；腳本預設使用 `<repo>/abc`。
- **工作目錄**：從 **專案根目錄** 執行下列指令（路徑與 `read_aiger` 相對路徑一致）；MaxSAT 透過 IPAMIR 載入 `third_party/EvalMaxSAT2022/libipamirEvalMaxSAT2022.so`（相對於行程 cwd）。
- **環境變數**（選用）：
  - `ABC`：覆寫 `abc` 可執行檔路徑（`minr_regression.py` 使用）。
  - `MINR_EXP_WORKERS`：並行實驗的 worker 數（`parallel.py`、`k.py`）。

## 目錄配置（慣例）

| 路徑 | 說明 |
|------|------|
| `script/exp/` | 實驗輸出的彙總／統計 CSV（`parallel.py`、`k.py`、`stat.py`） |
| `script/log/` | 各 job 的 minr report（`-o` 寫入的檔案），檔名常含 seed、DC、`-t` 等後綴 |
| `script/regression_results/` | `minr_regression.py` 每次 run 的 JSON 與文字 summary（目錄內檔案多由 `.gitignore` 忽略） |
| `script/minr_regression_baseline.json` | 迴歸 golden metrics（需與目前 key 格式一致） |

---

## `minr_regression.py` — CI／本機迴歸

固定多組 benchmark、多個 fixed-\(k\) 子問題、seed、DC ratio，並對較小電路加跑 **iterative MaxSAT**（`-O 1`）；與 baseline 比對 reset cost、`r_f`（**reset ratio**）、verify 等。

```bash
# 與 baseline 比對（通過則 exit 0）
python3 script/minr_regression.py

# 改電路集合、參數或 minr 行為後，重寫 baseline
python3 script/minr_regression.py --update-baseline

# 只預覽將執行的 abc 指令
python3 script/minr_regression.py --dry-run
```

輸出會區分兩塊彙總「**相對 baseline 的** `runtime_sec` / `refine_sec` 差異」：僅固定 `-k` 一區、僅 `-O 1` 一區。詳見腳本頂部 docstring。

---

## `parallel.py` — 批次實驗（iterative MaxSAT + refinement）

`benchmarks × seeds × dc_ratio` 平行跑 `&minr -O 1`（§4.3 iterative MaxSAT；可開 `-x` 做 §4.4 refinement）。

```bash
python3 script/parallel.py [output_prefix] [max_workers]
```

- **第一參數**：輸出 CSV 檔名前綴（預設見腳本內）。
- **第二參數**：並行數（可改 `MINR_EXP_WORKERS`）。
- 實驗矩陣（`BENCHMARKS`、`K` 或 `OPTIMIZE_MODE`、`SEEDS`、`DC_RATIO`、`TOTAL_TIMEOUT` 等）在腳本開頭 **常數區** 修改。

`-O 1` 時 CSV 的 **`opt_status`** 描述 iterative MaxSAT 結束原因；見 `doc/usage.md`。

---

## `k.py` — fixed-timeframe 子問題掃描（§4.2）

對每個 timeframe length \(k\in[K_{\min},K_{\max}]\) 各跑一次 **single-\(k\)** MaxSAT（不帶 `-O`），每個 \(k\) 享有完整 `-t` 預算；用於 \(k=0\) baseline 或 per-\(k\) 曲線。

```bash
python3 script/k.py [output_prefix] [max_workers]
```

`K_MIN`、`K_MAX`、benchmark 列表等在腳本內設定。

---

## `stat.py` — detail CSV → 統計 CSV

將一個或多個 **detail CSV**（例如 `parallel.py` 產物）聚合成 **stat CSV**（依電路、DC 等分組平均／驗證通過率等）。

```bash
python3 script/stat.py script/exp/detail1.csv script/exp/detail2.csv -o script/exp/out_stat.csv
```

未指定 `-o` 時，預設輸出為 `<第一個輸入檔>_stat.csv`。

---

## `benchmark_io_csv.py` — benchmark I/O 表

掃描 `benchmarks/iscas89` 與 `benchmarks/itc99` 的 `.aig` / `.aag` 檔首列，輸出 inputs／outputs 對照 CSV。

```bash
python3 script/benchmark_io_csv.py
python3 script/benchmark_io_csv.py -o script/exp/benchmark_io.csv
```

---

## 與 `&minr` 文件的對照

| 主題 | 文件 |
|------|------|
| 問題定義、safe realization、§4 方法 | `doc/spec.md` |
| 指令列、report ↔ 論文術語 | `doc/usage.md` |
| 程式 walkthrough | `doc/implementation.md` |
| 實驗與 §5 對應 | `doc/experiment.md` |
| 本目錄腳本 | 本檔 `script/README.md` |
