# `&minr` 使用手冊（CLI / 參數 / 輸出）

本文件以目前實作為準，說明 `&minr` 的命令列、report 格式與常見操作。  
名詞定義與論文對照：`spec.md` §0。

## 0. 論文術語 ↔ CLI / report（速查）

| 論文 | CLI / report |
|------|----------------|
| initial reset vector \(\hat\sigma\) | `-I`；report `target_state` |
| final reset vector \(\hat\rho\) | report `[details]` FF reset requirements |
| initialization sequence \(\Pi\) | report `[details]` PI sequence |
| timeframe length \(k\) | `-k`；report `k` / `best_k` |
| reset registers / free registers | `specified_regs` / \((ff - \text{unspecified count})\) |
| reset cost \(\|\{r:\hat\rho(r)\in\{0,1\}\}\|\) | `required_reset` |
| reset ratio \(RR=\#\mathrm{Reset}/\#\mathrm{Reg}\) | `r_f` |
| iterative MaxSAT solving (§4.3) | `-O 1` |
| SAT-based refinement (§4.4) | `-x` |
| target-induced constant cut | 預設 `last_tf_constr=cut`；`-S` → `specified_ro` |
| comparison timeframe \(t=k\) | 最後一個 unrolled frame |

## 1. 在 ABC 裡呼叫 `&minr`

```bash
./abc -c "read_aiger <path/to/design.aig>; &get; &minr <args...>"
```

`&get` 將 network 轉成 GIA（AIG）；`&minr` 以此為輸入。

## 2. 基本用法範例

### 2.1 Fixed-timeframe 子問題（單一 \(k\)）

```bash
./abc -c "read_aiger benchmarks/iscas89/s27.aig; &get; &minr -k 8 -o _/tmp/minr.log"
```

### 2.2 以 random simulation 產生 initial reset vector

```bash
./abc -c "read_aiger benchmarks/iscas89/s27.aig; &get; &minr -k 8 -r 0 -R 100 -o _/tmp/minr.log"
```

### 2.3 對 \(\hat\sigma\) 加入 partial specification（`-D`）

隨機將一部分 reset registers 改為 free（\(X\)），對應論文 §5.3；需搭配 `-r`。

```bash
./abc -c "read_aiger benchmarks/iscas89/s27.aig; &get; &minr -k 8 -r 0 -R 100 -D 50 -o _/tmp/minr.log"
```

### 2.4 Iterative MaxSAT solving（§4.3，`-O 1`）

```bash
./abc -c "read_aiger benchmarks/iscas89/s27.aig; &get; &minr -O 1 -t 600 -o _/tmp/minr.log"
```

### 2.5 Dense sweep（實驗用 `-K`）

```bash
./abc -c "read_aiger benchmarks/iscas89/s27.aig; &get; &minr -O 1 -K 40 -k 0 -t 600 -o _/tmp/minr.log"
```

> `-K` 僅在 `-O 1` 有效；dense sweep 不因 early stop 提前結束（除非 `-t` 用盡）。

## 3. 完整參數列表（`src/minr/minr_cmd.cpp`）

```
&minr [-k <int>] [-I <string>] [-r [<seed>]] [-R <num>] [-D <pct>] [-l <N>] [-L <T>]
      [-o <file>] [-v <level>] [-t <sec>]
      [-x <mode>] [-X] [-c <nConf>] [-C]
      [-O <mode>] [-K <N>]
      [-p] [-S]
```

### 3.1 問題設定

- **`-k <int>`**：**timeframe length** \(k\)（非負）。single-\(k\) 模式必填；`k=0` 僅有 comparison timeframe 前的初始 frame。
- **`-I <01x...>`**：**initial reset vector** \(\hat\sigma\)（長度 = `nRegs`）。預設全 `0`（fully specified）。字元 `0/1/x`。

### 3.2 產生 initial reset vector（random target）

- **`-r [seed]`**：以 random simulation 產生 reachable \(\hat\sigma\)。
- **`-R <num>`**：simulation 的 timeframe 數（預設 `k`）；`0` 表示不模擬、直接隨機 \(\hat\sigma\)。
- **`-D <pct>`**：在 `-r` 產生的 \(\hat\sigma\) 上，隨機將 `pct%` 的 **reset registers** 改為 free（\(X\)）；`pct` ∈ 1..99。
- **`-l <N>`** / **`-L <T>`**：在 `-r`（及可選 `-D`）之後，以 PO golden 比對剔除 redundant specified register。對 \(\hat\sigma\) 跑 `N` 次 iteration、每次 `T` cycle 隨機 simulation；若翻轉某 specified register 的 t=0 初始值不影響任何 cycle 的 PO，則將該 bit 改為 free（`x`）。**必須同時給定 `-l` 與 `-L`，且需搭配 `-r`**。與 `-R`（target 產生深度）獨立。

範例：

```bash
./abc -c "read_aiger benchmarks/iscas89/s27.aig; &get; &minr -k 8 -r 0 -R 100 -D 50 -l 10 -L 200 -o _/tmp/minr.log"
```

### 3.3 SAT-based refinement（§4.4）

- **`-x <mode>`**：`0` off；`1` CEC-style miter；`2` cut / specified-\(R_{\mathrm{reset}}\)@\(k\)；`3` eq cut。
- **`-X`**：refine 時將 \(R_{\mathrm{free}}\) 綁到 unrolled circuit 在 \(t=k\) 的 register 值。
- **`-c <nConf>`**：refine SAT conflict limit（0 = unlimited）。
- **`-C`**：core-only refine（跳過 single-reset deletion trial）。

### 3.4 Iterative MaxSAT / 時間預算

- **`-O 1`**：**iterative MaxSAT solving**（§4.3）；`-O 2` 為 outer-loop heuristic（論文外擴充）。
- **`-K <N>`**：dense \(k\) sweep（`-O 1` only）。
- **`-t <sec>`**：**thread CPU** 時間預算（iterative 或 single-\(k\) 全域上限）。

### 3.5 Comparison timeframe 約束與 solver

- **`-S`**：在 \(t=k\) 約束 \(R_{\mathrm{reset}}\)（非 target-induced constant cut）。若 \(\hat\sigma\) 全為 \(X\) 則自動退回 cut。
- **`-p`**：MaxSAT backend（in-process IPAMIR）；預設為外部 `EvalMaxSAT_bin`。

### 3.6 輸出

- **`-o <file>`**：structured report（INI-like）。
- **`-v <level>`**：0/1/2。
- **`-h`**：usage。

## 4. report 格式（`-o`）

由 `Minr_DumpReport()` 產生。

### 4.1 `[circuit]`

`name`, `inputs`, `outputs`, `ff`（\(\|R\|\)）, `nodes`。

### 4.2 `[settings]`

- `k`：採用的 timeframe length
- `target_state`：**initial reset vector** \(\hat\sigma\)
- `random_seed` / `random_cycles` / `dontcare_pct`：`-r/-R/-D` 相關
- `prune_iters` / `prune_cycles` / `target_pre_prune` / `prune_necessary`：`-l/-L` 相關（僅啟用時）
- `refine_mode`：`-x`
- `last_tf_constr`：`cut`（target-induced constant cut）或 `specified_ro`

### 4.3 `[refine]`（`-x>0`）

`released`, `by_trial`, `by_core`, `refine_sec`, `reset_before`（refinement 前 reset cost）。

### 4.4 `[result]`

| 欄位 | 論文對應 |
|------|----------|
| `specified_regs` | \(\|R_{\mathrm{reset}}\|\) |
| `required_reset` | \(\mathrm{cost}(\hat\rho)\) |
| `r_f` | **reset ratio** \(RR\) |
| `r_s` | reset cost / \(\|R_{\mathrm{reset}}\|\)（僅當 initial vector 有 specified entries） |
| `r_f_before_refine` / `r_s_before_refine` | refinement 前的 \(RR\) |
| `cut_verified` | 3-valued / SAT 對 cut（或 specified-\(R_{\mathrm{reset}}\)）的檢查 |
| `cec_verified` | safe-realization miter（Fig. 1 風格） |
| `runtime_cpu_sec` / `runtime_sec` | thread CPU（+ 外部 solver child CPU） |
| `opt_status` / `best_k` | iterative MaxSAT（`-O 1`）狀態與選定 \(k\) |

### 4.5 `[details]`

- PI sequence：\(\Pi=\langle\pi_0,\ldots,\pi_{k-1}\rangle\)
- FF reset requirements：**final reset vector** \(\hat\rho\)

### 4.6 `[iterations]`（`-O 1`）

每次 fixed-\(k\) MaxSAT 嘗試的 \(k\)、reset cost、耗時。

### 4.7 `[optimize2_outer]`（`-O 2`）

outer-loop segment 摘要。

## 5. 環境變數（debug）

- `MINR_TMPDIR`：外部 solver 暫存（預設 `_/tmp`）
- `MINR_DEBUG_EVALMAXSAT_TIMEOUT_MULT` / `MINR_DEBUG_EVALMAXSAT_NO_TIMEOUT`
- `MINR_DUMP_CNF` / `MINR_DUMP_WCNF` / `MINR_DUMP_ONLY`
- `MINR_NO_SOFT=1`：關閉 soft clauses（僅可行性）

## 6. 常見誤解

- **`-t` 為 thread CPU**，非 wall clock。
- **`-p` 只切 MaxSAT backend**；target-induced constant cut 與 3-valued propagation 仍會執行。
- **`-S` 改 comparison timeframe 約束**，不是 refinement mode。
- Report 的 `target_state` 是 **initial** \(\hat\sigma\)，不是 optimized 的 \(\hat\rho\)。
