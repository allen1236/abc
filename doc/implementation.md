# `&minr` 實作導讀（對齊 `spec.md` / 論文 §4）

本文件把論文概念對應到 `src/minr/` 的函式與資料結構，依執行順序 walkthrough。  
CLI / report：`usage.md`。問題定義：`spec.md`。

## 0. 快速索引

```
src/minr/minr_cmd.cpp        &minr CLI
src/minr/minr.h              Minr_Man_t
src/minr/minr_core.cpp       3-valued propagation + target-induced constant cut；
                             k-timeframe MaxSAT；iterative MaxSAT；report
src/minr/minr_sat.cpp        SAT-based refinement；safe-realization / witness miter verify
src/minr/minr_ipamir_dyn.*   IPAMIR 動態載入
```

## 1. 入口：`minr_cmd.cpp`

`Minr_CommandAbc9Minr()` 解析參數並呼叫 `Minr_Solve()`。

| 論文章節 | CLI |
|----------|-----|
| §4.2 fixed-\(k\) MaxSAT | `-k`（無 `-O`） |
| §4.3 iterative MaxSAT | `-O 1` |
| §4.4 SAT-based refinement | `-x` |
| initial reset vector \(\hat\sigma\) | `-I` / `-r/-R/-D` |
| comparison timeframe 約束變體 | `-S` |

`-p`：MaxSAT solver backend（與 §4 問題定義無關）。

## 2. 中央狀態：`Minr_Man_t`（`minr.h`）

| 欄位群 | 論文對應 |
|--------|----------|
| `pInitStr` | initial reset vector \(\hat\sigma\) |
| `nFrames` | timeframe length \(k\) |
| `vPropVals`, `vCutNodes` | 3-valued simulation；target-induced constant cut \(V_C\) |
| `vPiVals` | initialization sequence \(\Pi\) |
| `vRoVals0` | final reset vector \(\hat\rho\)（timeframe 0） |
| `bestK`, `vBest*`, `vOptIter*` | iterative MaxSAT best-so-far |
| `fCecVerifyPass` | safe realization（Definition 1 / Fig. 1） |

## 3. §4.1：3-valued propagation + target-induced constant cut

`Minr_PropagateAndCut()`（`minr_core.cpp`）：

1. 將 present-state registers 設為 \(\hat\sigma\)（0/1/X）；PI 為 all-\(X\)（\(\hat\pi_X\)）。
2. `Minr_SimulateTimeframe()`：Kleene 3-valued AND / inverter（§2.2）。
3. `Minr_ExtractCut()`：自 COs backward traversal，收集 deterministic 0/1 frontier → \(V_C\)。
4. `specRoCutRatio`：\(R_{\mathrm{reset}}\) 落在 cut 上的比例（統計用）。

## 4. §4.2：\(k\)-timeframe MaxSAT encoding

`Minr_SolveSingleK()`：

### 4.1 TFI pruning

`vTfiRi`（\(t<k\) frames）、`vTfiCut`（comparison timeframe \(t=k\)）限制變數與 clauses 範圍。

### 4.2 Dual-rail hard constraints

- illegal \((1,1)\)；AND gate Kleene CNF；latch \(RO^{t+1}\equiv RI^t\)。
- \(t<k\)：PI binary；\(t=k\)：PI 預設 \(X\)（`-O 2` 可綁定前段 \(\pi_0\)）。

### 4.3 Comparison timeframe 約束

`Minr_ManUsesSpecRegAtLastTf()`：

- 預設：對 \(V_C\) 中 propagation 為 0/1 的節點強迫 Eq. (6)（3-valued cut 條件）。
- `-S`：強迫 \(R_{\mathrm{reset}}\) 在 \(t=k\) 等於 \(\hat\sigma\) 的 0/1。

### 4.4 Soft clauses（Eq. 10）

輔助變數 \(\gamma_r\)（code 中 `U_i`）表示 \(\hat\rho(r)=X\)；unit soft per register。

### 4.5 Solver

`Minr_SolveMaxSat()` → 外部 EvalMaxSAT 或 IPAMIR；`Minr_DecodeResult()` 得 \(\Pi\) 與 \(\hat\rho\)。

## 5. §4.3：Iterative MaxSAT solving

`Minr_SolveOptimize()`（`-O 1`）：

- Candidate timeframe lengths \(K=\{0,1,2,4,\ldots,256\}\) 或 `-K` dense sweep。
- 每輪 fixed-\(k\) MaxSAT；以 \(\mathrm{cost}(\hat\rho)\) 更新 best-so-far。
- Early stop：zero reset、改善不足、timeout/UNSAT 且已有 best（dense sweep 例外）。

## 6. §4.4：SAT-based refinement

`Minr_SatRefine()`（`minr_sat.cpp`）：

- 建 **witness-bound miter**（Fig. 4）：actual copy = unrolled + \(\hat\rho\) + \(\Pi\)；reference = witness \(\sigma^*\)（\(R_{\mathrm{reset}}\) 來自 \(\hat\sigma\)，\(R_{\mathrm{free}}\) 綁 reached state@\(k\)）。
- Algorithm 1：reset assumptions → UNSAT core → single-reset deletion。
- 輸出 refined \(\hat\rho'\)。

Refine modes（`-x`）決定 miter 約束細節；見 `Minr_BuildBinarySatModel()`。

## 7. 驗證與 report

`Minr_Solve()` post-processing：

1. **`Minr_VerifyResult()`**：3-valued simulation 檢查 comparison timeframe 的 cut / specified-\(R_{\mathrm{reset}}\) 條件。
2. **`Minr_SatVerify()`**：若 refinement 有 release，以 SAT 強化 cut 檢查。
3. **`Minr_CecVerify()`**：safe-realization miter（reference 用 \(\hat\sigma\)，actual 用 \(\hat\rho+\Pi\)；Fig. 1）。

`Minr_DumpReport()`：欄位名稱見 `usage.md` §0（多數沿用實作命名，語意對齊論文）。

## 8. 修改落點

| 變更 | 檔案 |
|------|------|
| CLI | `minr_cmd.cpp` |
| target-induced constant cut / MaxSAT | `minr_core.cpp` |
| iterative MaxSAT | `Minr_SolveOptimize()` |
| SAT-based refinement / miter | `minr_sat.cpp` |
| solver backend | `Minr_SolveMaxSat()` |

## 9. 論文外實作

- **`-O 2`**：`Minr_SolveOptimize2()`，多段 outer loop。
- **`-p`**：solver 執行方式，不影響 encoding。
