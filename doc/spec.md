# `&minr` 規格（high-level，術語對齊論文）

本文件描述 `&minr` 的問題定義、**safe realization** 正確性概念，以及論文 §4 的方法概觀（偏演算法，不進入實作細節）。  
CLI 與 report 欄位：`usage.md`。程式 walkthrough：`implementation.md`。

論文標題：*Reset Minimization in Sequential Circuits via Bounded Initialization Sequences*（內部參考：`doc/archive/paper.txt`）。

## 0. 術語對照（論文 ↔ 本 repo）

| 論文 | 意義 | `&minr` / report |
|------|------|------------------|
| sequential circuit \(N=\langle I,O,R,F\rangle\) | PI、PO、**state variables (registers)** \(R\) | AIG / GIA；`ff` = \(\|R\|\) |
| **initial reset vector** \(\hat\sigma\in\{0,1,X\}^{|R|}\) | 使用者指定的啟動需求（startup requirement） | `-I`；report `target_state` |
| **reset registers** \(R_{\mathrm{reset}}\) | \(\hat\sigma(r)\in\{0,1\}\) 的 register | report `specified_regs` = \(\|R_{\mathrm{reset}}\|\) |
| **free registers** \(R_{\mathrm{free}}\) | \(\hat\sigma(r)=X\) 的 register | initial vector 中 `x` 的個數 |
| **compatible state** \([\hat\sigma]\) | 與 \(\hat\sigma\) 在 \(R_{\mathrm{reset}}\) 上一致的二元狀態集合 | （概念；無單一 report 欄位） |
| **final reset vector** \(\hat\rho\in\{0,1,X\}^{|R|}\) | 在 timeframe 0 實際需要的硬體 reset 規格（待最小化） | report `FF reset requirements`；`required_reset` = \(\| \{r:\hat\rho(r)\in\{0,1\}\} \|\) |
| **initialization sequence** \(\Pi=\langle\pi_0,\ldots,\pi_{k-1}\rangle\) | 長度 \(k\) 的 **PI sequence** | report `[details]` PI sequence |
| **timeframe length** \(k\) / **\(k\)-timeframe expansion** | 展開 \(t=0..k\)（共 \(k+1\) 個 timeframe） | `-k`；report `k` / `best_k` |
| **comparison timeframe** | \(t=k\)（與 reference 比對的 frame） | 文中「\(t=k\)」|
| **combinational inputs (CIs)** | \(I \cup R\)（present-state） | GIA 的 PI + present-state latch outputs |
| **combinational outputs (COs)** | \(O \cup R'\)（next-state） | PO + register inputs (RI) |
| **target-induced constant cut** \(V_C\) | §4.1 由 \(\hat\sigma\) 導出的結構 cut | `vCutNodes`；`last_tf_constr=cut` |
| **safe realization** \(S_k \preceq_F \hat\sigma\) | Definition 1：reachable states 功能上實現啟動需求 | CEC miter（§3、Fig. 1） |
| **iterative MaxSAT solving** | §4.3：對多個 \(k\) 重複 fixed-\(k\) MaxSAT | `-O 1` |
| **SAT-based refinement** | §4.4：在 exact Boolean 下釋放 \(\hat\rho\) 中多餘的 0/1 | `-x` |
| **reset ratio** \(RR=\#\mathrm{Reset}/\#\mathrm{Reg}\) | reset cost 指標 | report `r_f`（%） |

> **reset**（論文 §1）：指 dedicated **hardware reset** 在 reset 訊號有效時直接把 register 設成指定值；不含 scan-based 或純 sequence-based 初始化。

## 1. 問題背景與目標

Initializing a sequential design 在 power-up、recovery 等情境需要建立期望的 operating condition。對 register 施加 **hardware reset** 有 area、routing、power、timing 成本。

本工作研究 **hardware-reset minimization under safe realization**（Problem Statement, §3）：

- **給定**：sequential circuit \(N\) 與 **initial reset vector** \(\hat\sigma\)。
- **求**：**final reset vector** \(\hat\rho\) 與 **initialization sequence** \(\Pi\)（長度 \(k\)），使得在 timeframe \(k\) 的 reachable state set \(S_k\) **safely realizes** \(\hat\sigma\)，且 \(\hat\rho\) 中 0/1 的個數（**reset cost**）最小。

與 prior circuit-initialization 不同：目標不是把 register 驅到任意 known value，而是在 **explicit startup requirement** 下，用 initialization sequence 補償可移除的 reset，並以 **safe realization**（而非 bitwise matching）定義正確性。

## 2. 輸入與輸出

### 2.1 輸入

- **Sequential circuit**：實作以 **AIG** 表示（§2.1）；register 為 state variables \(R\)。
- **Timeframe length** \(k\)：bounded initialization 的序列長度；展開 \(t=0,\ldots,k\)。
- **Initial reset vector** \(\hat\sigma\)：長度 \(|R|\)，每 bit 為 \(0/1/X\)。
  - \(0/1\)：該 register 屬於 **reset registers** \(R_{\mathrm{reset}}\)。
  - \(X\)：該 register 屬於 **free registers** \(R_{\mathrm{free}}\)（unspecified）。

\(\hat\sigma\) 描述 **startup requirement**，不是要求最終狀態與 \(\hat\sigma\) 逐 bit 相等（§5.2 與 safe realization 的動機）。

實作上 `-r/-R/-D` 可從 simulation 產生隨機的 \(\hat\sigma\)（含 partial specification），對應論文 §5.3 的實驗設定。

### 2.2 輸出

- **Initialization sequence** \(\Pi\)：\(k\) 個 binary PI vectors。
- **Final reset vector** \(\hat\rho\)：timeframe 0 的 reset 規格；\(X\) 表示該 register 不需 hardware reset。
- **Objective**：最小化 \(\mathrm{cost}(\hat\rho)=|\{r\in R:\hat\rho(r)\in\{0,1\}\}|\)（等價最大化 free entries）。

## 3. Safe realization（Definition 1）

令 \(S\subseteq B^{|R|}\) 為在施加某 **final reset vector** 與 **initialization sequence** 後可達的狀態集合。

\(S\) **safely realizes** initial reset vector \(\hat\sigma\) under \(F\)（記為 \(S\preceq_F\hat\sigma\)），若且唯若：

\[
\forall \sigma\in S,\ \exists \sigma^*\in[\hat\sigma],\ \forall \pi\in B^{|I|},\ F(\pi,\sigma)=F(\pi,\sigma^*).
\]

直觀上（Fig. 1 miter）：**unrolled circuit** 以 \(\hat\rho\) 在 timeframe 0 初始化並套用 \(\Pi\)；**reference circuit** 以 \(\hat\sigma\) 初始化；在 **comparison timeframe** \(t=k\) 兩邊共用同一 PI assignment，所有 **COs**（PO + next-state）須相等。

這是對傳統 bitwise reset matching 的 **functional relaxation**；當 \(\hat\sigma\) fully specified 時退化成對單一 \(\sigma^*\) 的檢查（§3）。

實作最終以 **CEC miter**（`Minr_CecVerify`）檢查此條件；見 `implementation.md` §6.2。

## 4. 方法概觀（對應論文 §4）

### 4.1 Target-induced constant cut（§4.1）

直接驗證 safe realization 是 \(\forall\exists\forall\)（QBF）。為可擴展性，先導出較強的 **structural sufficient condition**：

1. 以 **3-valued simulation**（Kleene semantics，§2.2）在 \(\hat\sigma\) 與 all-\(X\) PI 下評估電路。
2. 從每個 CO 向 fanin **backward traversal**；遇到 deterministic 0/1 節點即納入 **target-induced constant cut** \(V_C\) 並停止該分支。
3. 在 **comparison timeframe** \(t=k\)，要求 reached state 在 cut 上的值等於 propagation 得到的常數 \(\alpha\)（Eq. 5–6）。

Theorem 1：滿足 cut 條件 ⇒ safe realization（保守但可 MaxSAT 編碼）。

實作 `-S` 可改在 \(t=k\) 直接約束 \(R_{\mathrm{reset}}\) 而非 \(V_C\)（研究用變體；預設為 cut 模式）。

### 4.2 \(k\)-timeframe MaxSAT encoding（§4.2）

對固定 \(k\)，將問題編成 **partial MaxSAT**：

- **Hard**：\(k\)-timeframe transition（Eq. 7）、\(\hat\sigma_0=\hat\rho\)、PI sequence binary、comparison timeframe 的 cut 條件（Eq. 8–9）。
- **Soft**：對每個 \(r\in R\) 鼓勵 \(\hat\rho(r)=X\)（Eq. 10）。

3-valued logic 以 **dual-rail** Boolean encoding（§4.2 末段）送入 MaxSAT solver。

### 4.3 Iterative MaxSAT solving（§4.3）

Fixed-\(k\) 子問題只對單一 timeframe length 最優。實務上對 candidate set \(K=\langle 0,1,2,4,8,\ldots\rangle\) 迭代求解，在時間預算內保留 **best-so-far** \((\hat\rho,\Pi)\)（以 \(\mathrm{cost}(\hat\rho)\) 評估）。

對應 CLI **`-O 1`**。`-K` 為 dense sweep 變體（實驗用；論文主流程為 geometric \(K\)）。

### 4.4 SAT-based refinement（§4.4）

MaxSAT 解在 3-valued domain 下保守。Refinement 在 **witness-bound miter**（Fig. 4）上以 exact SAT 嘗試把 \(\hat\rho\) 中多餘的 0/1 **release** 為 \(X\)，採 **UNSAT-core-guided** 流程（Algorithm 1）。

對應 CLI **`-x`**。Refinement **sound but not complete**（§4.4）。

## 5. 實作擴充（論文未述，CLI 存在）

- **`-O 2`**：outer-loop heuristic，串接多段 initialization；論文主實驗 flow 為 §4.3 + §4.4。
- **`-p`**：MaxSAT solver backend 切換（不影響問題定義）。

---

## 參考來源（本 repo 內）

- 論文抽取文字：`doc/archive/paper.txt`
- 較早草稿：`doc/archive/draft_spec.md`、`doc/archive/draft_dev.md`
