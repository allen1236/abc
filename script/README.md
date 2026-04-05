# `script/` — minr 實驗與迴歸工具

本目錄放與 `&minr`（ABC 指令）相關的 Python 腳本。實作與 CLI 細節見 `src/minr/dev.md`；研究脈絡見 `src/minr/spec.md`。

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

固定多組 benchmark、多個 **`-k`**、seed、DC ratio，並對較小電路加跑 **`-O 1`**；與 `minr_regression_baseline.json` 比對解的指標（`required_reset`、`r_s`、verify 等），**不**比對兩種模式互斥時間。

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

## `parallel.py` — 批次實驗（平行）

`benchmarks × seeds × dc_ratio` 平行跑 `&minr`，依完成順序寫入 **一筆一列** 的 detail CSV。

```bash
python3 script/parallel.py [output_prefix] [max_workers]
```

- **第一參數**：輸出 CSV 檔名前綴（預設見腳本內）。
- **第二參數**：並行數（可改 `MINR_EXP_WORKERS`）。
- 實驗矩陣（`BENCHMARKS`、`K` 或 `OPTIMIZE_MODE`、`SEEDS`、`DC_RATIO`、`TOTAL_TIMEOUT` 等）在腳本開頭 **常數區** 修改。

---

## `k.py` — 依 k 掃描匯出 CSV

搭配 **`&minr -O 1 -k K_MIN -K K_MAX`**（dense k sweep），從 report 解析每個 k 的指標，寫入 CSV。並行介面與 `parallel.py` 類似：

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
| 指令列旗標、執行流程、report 欄位 | `src/minr/dev.md` |
| 問題定義、演算法、`-O` 語意 | `src/minr/spec.md` |
| 本目錄腳本 | 本檔 `script/README.md` |
