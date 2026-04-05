# README: `&minr` Paper/Dev Guideline

> 本文件提供 `&minr` 專案的完整 context，供新的 AI agent 或合作者快速理解：研究動機、問題定義、方法、程式架構、以及目前暫定的實驗設計。  
> Implementation 細節請參考同目錄的 `dev.md`。

---

## 1. Summary

我們研究 sequential circuit 的 reset 縮減：  
在允許 reset 後施加長度 `k` 的 primary-input initialization sequence 的前提下，利用 MaxSAT 最大化不需要 reset 的 flip-flop（把其初值視為 X / don't-care），並在初始化結束時（`t = k`）保證一組由目標初始狀態推導出的 **constraint cut** 值一致。

---

## 2. Motivation

### 2.1 Why reduce reset?

在大型設計中，對大量 flip-flop 加 reset 會帶來：

* **Area**：resettable FF cell 通常較大/複雜
* **Power**：reset network switching、cell power
* **Routability / congestion**：全域 reset tree 佈線壓力
* **Timing / robustness**：reset deassert synchronization、recovery/removal constraints

### 2.2 Trade reset hardware for a short initialization sequence

很多 FF 的初值不必硬體 reset：  
可用短暫的 initialization sequence（幾個 cycles）把必要狀態帶到可接受的初始化條件。  
時間成本通常可忽略，而 reset 硬體成本可顯著下降。

---

## 3. Related Work (Positioning)

大多數 prior work 與本研究的 formulation 不完全相同，建議依以下三群定位：

### (A) Redundant reset removal via observability / ODC / sequential analysis

* 目標：找出 reset value 永遠不可觀測、或 X 不會 propagate 到觀測點的暫存器。
* 特點：偏 "可觀測性/冗餘性" reasoning；不一定合成 input sequence；通常非 target-specific。

### (B) Circuit initialization / MaxSAT / MaxBMC (maximize known registers)

* 目標：找 initialization sequence 讓更多 FF 變成 known（0/1），或在 length vs #known 做 Pareto。
* 特點：target state 通常不是 user-specified；更像 "maximize known" 而非 "minimize reset hardware"。

### (C) ATPG / State justification / functional justification

* 目標：指定內部條件（可視為 state cube），找序列去 justify。
* 特點：可 target-specific，但通常以 test/coverage 為目標，不以 reset 硬體最小化為目標。

**本研究定位**：target-driven + minimize reset hardware（maximize initial X），並合成短序列。

---

## 4. Problem Definition

### 4.1 Inputs

* Sequential circuit（AIG/GIA）
* `k`：初始化序列長度（timeframe expansion depth）
* Target seed state `I`：長度 = #regs，字元 ∈ `{0,1,x}`

  * `0/1`：指定該 register 的目標/seed 值
  * `x`：不指定（未知/自由）

### 4.2 Outputs

* PI initialization sequence：`I^0 .. I^{k-1}`（binary）
* 哪些 FF 初值可設為 X（不需要 reset）：maximize count
* 哪些 FF 必須固定為 0/1（需要 reset 或其它保證）

### 4.3 Feasibility note

特定 target state（例如 all-0）可能不可達，導致 `UNSAT`。  
因此實驗中建議同時使用：

* canonical target（all-0）與
* reachable targets（由模擬抽樣得到）

---

## 5. Method Overview

整體方法由四個步驟組成：

### 5.1 Step A: Golden Propagation + Cut Selection (single timeframe, 3-valued)

在建立 CNF 前，先用三值 `0/1/X` forward propagation 產生 **constraint cut**：

**假設：**

* 所有 PI = `X`
* 對每個 register output：

  * `I[i]=0/1` → 視為常數 0/1
  * `I[i]=x` → 視為 X

**Propagation：**

* 在 combinational AIG slice 上用 Kleene 三值規則推進（AND + inverter）
* 得到每個節點的值：0/1/X

**Cut extraction（Reverse DFS from COs）：**

Constraint cut 只需 enforce 從 CO（PO / RI）能觀察到的 known 值。因此採用從 CO 往回走的方式，只收集「從 output 視角真正需要的」known 節點：

* 對每個 CO：
  * 若 CO 的值為 known（0/1）：直接加入 cut，不往回走
  * 若 CO 的值為 X：從 CO 的 driver 開始反向 DFS
    * 遇到 known（0/1）節點 → 加入 cut，停止（不再往 fanin 走）
    * 遇到 X 節點 → 繼續往 fanin 走
    * 遇到 Const0 或已訪問 → 跳過
* 結果：
  * `cutNodes[]`：cut 上的節點集合（ordered by ObjId）
  * 每個 cut node 的 golden 值由 `vPropVals[]` 查表得到

> **為何不用 forward pass（遍歷所有 known/unknown boundary node）？**  
> Forward pass 會把所有位於 known/unknown 邊界的 node 都加入 cut，但其中有些 node 的值從任何 CO 都觀察不到，enforce 它們屬於 over-constraint。例如 `((a&b)&c)&d`（c=PI, a=b=1, d=0），forward pass 會把 `a&b=1` 加入 cut（因為 fanout `(a&b)&c` 為 X），但從 output 看 `d=0` 已足以保證結果，`a&b=1` 並非必要。Reverse DFS 只收集 output-observable 的 known 邊界，產生更小、更精確的 cut。

---

### 5.2 Step B: Unroll + Dual-Rail MaxSAT (k timeframes)

#### Dual-rail encoding

對每個信號 `x`、每個時間點 `t` 建兩個布林變數：

* `x_T^t`：x 確定為 1
* `x_F^t`：x 確定為 0

語意：

* 1 → (T=1, F=0)
* 0 → (T=0, F=1)
* X → (T=0, F=0)
* Illegal → (T=1, F=1)

Hard constraint（所有節點都加）：

* `¬x_T^t ∨ ¬x_F^t`（禁止 illegal）

#### Gate constraints (Kleene AND)

對 AND gate `z = a ∧ b`：

* `z_T ↔ (a_T ∧ b_T)`
* `z_F ↔ (a_F ∨ b_F)`
  （含 inverter edge 的 T/F swap）
  → 轉成固定 6 條 CNF clauses。

#### Unrolling / latch transition

展開 `t = 0..k`：

* 每個 timeframe 有一份 combinational network dual-rail 變數與 gate constraints
* latch transition（`t = 0..k-1`）：
  * `RO_i^{t+1} ↔ RI_i^t`

#### PI constraints

* `t=0..k-1`：PI 必須 binary（不可 X）
  * `(PI_T^t ∨ PI_F^t)`
* `t=k`：PI 預設強制 X（用來驗證 reset 不依賴後續輸入）
  * `¬PI_T^k` 且 `¬PI_F^k`

> 註：在 heuristic 的 `-O 2` 外迴圈中，後續 iteration 會把 `t=k` 的 PI 固定成上一段的 `t=0` PI（見第 8 章）。

#### Cut equality constraints at t=k

對每個 cut node `c`：

* 若 golden(c)=1：
  * Hard: `c_T^k` 與 `¬c_F^k`
* 若 golden(c)=0：
  * Hard: `c_F^k` 與 `¬c_T^k`

#### Objective: maximize initial unknown registers (t=0)

對每個 register i，新增 helper 變數 `U_i` 表示 `Ri^0` 為 X：

* `U_i ↔ (¬Ri_T^0 ∧ ¬Ri_F^0)`

Hard 定義 clauses：

* `U_i → ¬Ri_T^0`
* `U_i → ¬Ri_F^0`
* `(Ri_T^0 ∨ Ri_F^0 ∨ U_i)`

Soft clause（weight=1）：

* `(U_i)`
  MaxSAT solver 會最大化滿足的 soft clauses，等價最大化 initial X registers。

---

### 5.3 Step C: SAT-Based Post-Refine (optional, `-x <mode>`)

MaxSAT solver 回傳的解（哪些 FF 在 `t=0` 需要固定為 0/1）可以再做一輪 SAT-based refine，嘗試釋放更多 FF 到 X，同時維持正確性。

本專案目前採用的 refine 是 **UNSAT-core +（可選）逐一試放** 的方式（而非 pre-processing 的 forced-known 或 greedy post-relax）。

* `-x 0`：關閉 refine
* `-x 1`：CEC output equiv（在 `t=k` 對 PO+RI 做等價約束）
* `-x 2`：constant cut（在 `t=k` 對 constraint cut 的 known-valued nodes 做等價約束）
* `-x 3`：eq cut（抽取更小的 eq-cut，在 `t=k` 做等價約束）

相關參數：

* `-X`：將 target state 中的 don't care RO（`x`）在 refine/verify 時 **bind 到 unrolled `t=k` 的 RO 值**（只在會建 target circuit 的模式有效）
* `-c <nConf>`：SAT refine conflict limit（0=unlimited）。若初次 solve 因 conflict limit 變成 UNKNOWN，refine 不會釋放 FF
* `-C`：core-only refine（跳過逐一試放，只做一次 UNSAT-core bulk release）

---

## 6. Verification

三種驗證方式：

* **Simulation-based** (`Minr_VerifyResult`, `cut_verified`)：用 decoded PI sequence 和 reset 值做 x-simulation t=0..k，檢查 t=k 的 cut 值是否與 golden 一致。**永遠執行**（也用於計算 `sim_reg_mismatch` 統計量）。
* **SAT-based** (`Minr_SatVerify`)：建立 cut-constraint 的 SAT 檢查，嘗試找 counterexample。若 UNSAT → 驗證通過。**在 refine 釋放過 FF 時額外執行**；結果會覆蓋 `cut_verified`。
* **CEC-based** (`Minr_CecVerify`, `cec_verified`)：建立 GIA miter 做完整 combinational equivalence checking，比較 **所有** combinational output（PO + RI），提供比 cut-based 更強的正確性保證。**永遠執行**（在上述兩種驗證之後）。

### CEC Verification 細節

建兩個電路的 miter：
1. **Target circuit**（單 timeframe）：將 target initial state 中 0/1 的 register output 替換為 constant，PI 為自由變數。
2. **Unrolled circuit**（k+1 timeframes）：t=0 根據 decoded reset value 設定 RO（0/1 為 constant，X 為 free variable），apply decoded PI pattern；t=1..k-1 接 latch transition 並 apply PI pattern；t=k 的 PI 與 target circuit 共享。
3. **Miter output**：所有對應 PO/RI pair 的 XOR 做 OR → 單一 output。

利用 structural hashing 建構 GIA（`Gia_ManHashAlloc`），自動做 constant propagation。若 miter 不是 trivially constant 0，則用 SAT sweeping（`Cec_ManSatSweeping` / `&fraig`）驗證。

若 target state 包含 don't care (`x`) registers，CEC verify 會把這些 target RO **bind 到 unrolled `t=k` 的對應 RO**，因此不需要跳過。

---

## 7. Random Target Derivation (`-r`)

除了使用者直接指定 target state (`-I`)，也支援隨機模擬產生 target state：

* `-r [seed]`：啟用隨機 target derivation
* `-R <num>`：模擬幾個 timeframe（default = k）

流程：
1. 若有 `-I`：以該值為初始狀態開始模擬
2. 若無 `-I`：以隨機狀態為初始狀態
3. 每個 timeframe：隨機 PI → simulate → RI 作為下一個 state
4. 最終 state 成為 target（覆蓋原 `-I` 或 default all-0）

這確保 target 是一個 reachable state，避免 UNSAT 問題。

### 7.1 Don't Care Masking (`-D`)

在 random target derivation 後，可進一步將部分 register 設為 don't care：

* `-D <pct>`：將 floor(nRegs × pct / 100) 個 register 隨機設為 `x`（需搭配 `-r`）
* 使用 Fisher-Yates shuffle 從 nRegs 個 register 中均勻隨機選取

效果：
* 減少 constraint cut 中的節點數（don't care RO 會降低可觀測到的 known frontier）
* 允許更多 FF 免除 reset（solver 有更大的自由度）
* `sim_reg_mismatch` 統計量只計算 specified (0/1) register，don't care register 不納入分母
* CEC verification 不需要跳過：target 的 don't care ROs 會被 bind 到 unrolled `t=k` 的 RO 值（見第 6 章）

---

## 8. Optimize Mode (`-O`)

### 8.1 動機

選擇 k 對結果影響很大：k 太小可能結果不佳，k 太大 solver 太慢。Optimize mode 自動搜尋最佳 k。

### 8.2 做法

本專案把 **`-O 1` 當成主要 flow**，會在固定 schedule 上 sweep `k` 以找 best-so-far；`-O 2` 僅作為電路太大時的 heuristic fallback。

#### `-O 1`：k sweep（主要 flow）

* 固定 k schedule：`{0, 1, 2, 4, 8, 16, 32, 64, 128, 256}`（doubling + 含 `k=0`）
* 可搭配 `-t <sec>` 設定總時間預算

每輪迭代：
1. 計算剩餘時間 `tRemain = totalTimeout - elapsed`，不足 1s 則停止
2. 以當前 k 呼叫 `Minr_SolveSingleK`（MaxSAT 求解），solver timeout = tRemain
3. 若結果比目前最佳更好（更少 reset）→ 更新 best-so-far
4. Early stop 條件：
   * `reset_needed == 0`（完美解）
   * 相比上一輪改善 < `MINR_EARLY_STOP_IMPROVEMENT_RATIO`（見 `minr.h`）
   * Solver timeout 且已有 best
   * Solver 回傳 UNSAT 且已有 best（與 timeout 同樣處理）

每輪迭代記錄 `(k, nResets, solverStatus, timeMs)`，最終輸出於 report 的 `[iterations]` section。

迭代結束後，恢復 best solution，進入共用的 post-processing（refine + verify + report）。

> 註：概念上，single-k（沒有 `-O`）可視為 `-O 1` sweep 裡「固定某個 k」的單次迭代；差異只在是否自動 sweep 與 best-so-far tracking。

#### `-O 2`：outer-loop heuristic（僅大電路 fallback）

`-O 2` 把總預算切成多個 segment（約 `1/MINR_OPT_BUDGET_PARTS`），每個 segment 內跑一段有限的 k schedule；當 segment 到時會對 segment-best 解做 cut verify +（可選）SAT refine，然後用該解的 reset configuration 生成新的 target state，進入下一個 outer iteration。  
此外，從第 2 段開始會把 `t=k` 的 PI 固定成上一段的 `t=0` PI，以便把多段序列串接成較長的初始化序列。

---

## 9. Command interface

Command: `&minr`

```
&minr [-k <int>] [-I <string>] [-r [<seed>]] [-R <num>] [-D <pct>]
      [-o <file>] [-v <level>]
      [-t <sec>] [-x <mode>] [-X] [-c <nConf>] [-C] [-O <mode>] [-h]
```

> 參數細節與 report 欄位定義請參考 `src/minr/dev.md`（本目錄）。

