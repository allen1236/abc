# `&minr` — Hardware-Reset Minimization via Bounded Initialization Sequences（ABC fork）

本 repository 為 ABC fork，新增指令 `&minr`，實作論文 *Reset Minimization in Sequential Circuits via Bounded Initialization Sequences* 的流程：

在給定 **initial reset vector** \(\hat\sigma\)（register 上的 \(0/1/X\) 啟動需求）與 bounded **initialization sequence** 長度 \(k\) 下，求 **final reset vector** \(\hat\rho\) 與 **initialization sequence** \(\Pi\)，使 reachable states **safely realize** \(\hat\sigma\)，並最小化 hardware **reset cost**（\(\hat\rho\) 中 0/1 的 register 數）。

此專案同時提供：
- `&minr` 的 C/C++ 實作（`src/minr/`）
- 可重現的實驗與迴歸腳本（`script/`）
- 文件（`doc/`；舊草稿在 `doc/archive/`）

## 快速開始

### 編譯

在 repo 根目錄：

```bash
make abc
```

### 執行 `&minr`（單次固定 k）

```bash
./abc -c "read_aiger benchmarks/iscas89/s27.aig; &get; &minr -k 8 -o _/tmp/minr.log"
```

### 執行 `&minr`（iterative MaxSAT solving，§4.3：`-O 1`）

```bash
./abc -c "read_aiger benchmarks/iscas89/s27.aig; &get; &minr -O 1 -t 600 -o _/tmp/minr.log"
```

`&minr` 的完整參數（包含 `-r/-D/-x/-K/-S/-p`）請見 `usage.md`。

## 文件導覽（先看這裡就能進入狀況）

- `doc/spec.md`：問題定義、safe realization、§4 方法（術語對齊論文）
- `doc/usage.md`：`&minr` 的 CLI/參數、輸入輸出與 report 格式
- `doc/implementation.md`：逐段對齊 `src/minr/` 的程式 walkthrough（資料結構/函式與 spec 對照）
- `doc/experiment.md`：benchmark、腳本、如何跑批次實驗與彙總數據

較早的設計草稿（含論文抽取文字）在 `doc/archive/`。

## Repository 內與 `&minr` 直接相關的位置

```
src/minr/
  minr_cmd.cpp        &minr CLI 解析與參數檢查
  minr_core.cpp       3-valued cut、MaxSAT、iterative MaxSAT、report
  minr_sat.cpp        SAT-based refinement、safe-realization verify
  minr.h              Minr_Man_t 與參數/統計欄位
script/
  parallel.py         批次平行跑 benchmark×seed×dc，產 detail CSV
  stat.py             detail CSV → stat/barplot CSV
  k.py                per-k 掃描匯出 CSV（與 -O 1 不同，會對每個 k 各跑一次）
  minr_regression.py  迴歸測試（與 baseline 比對）
doc/
  spec.md / usage.md / implementation.md / experiment.md
  archive/            舊草稿（draft_*.md、paper.txt）
_/                    本機工作目錄（tmp、log、opt 快取；見 .gitignore）
```

## Solver backend（和 `-p` 的關係）

`&minr` 有兩種 MaxSAT 後端：
- **預設（不加 `-p`）**：呼叫外部 `EvalMaxSAT_bin`（WCNF 檔 → 子程序 stdout），暫存檔預設寫到 `_/tmp`（可用 `MINR_TMPDIR` 改）
- **加 `-p`**：用 IPAMIR `.so` 以 in-process 方式呼叫 `EvalMaxSAT2022`（預設 `.so`：`third_party/EvalMaxSAT2022/libipamirEvalMaxSAT2022.so`）

兩種後端求的是**同一個** WCNF；差異只在 solver 執行方式與時間統計（詳見 `usage.md` 的時間欄位說明）。

---

## Upstream ABC

本 repo 基於 ABC；若你要看 ABC 本身的使用方式與背景，建議參考 upstream：`https://github.com/berkeley-abc/abc`。
