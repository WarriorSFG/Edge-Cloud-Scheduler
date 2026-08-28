## System Architecture & Objectives

The scheduling environment consists of one local computer and $K$ identical remote computers connected via a single shared network link. The system must process $R$ streaming requests interactively, balancing two primary objectives:

* **Throughput (Output Rate):** Maximizing the total tokens generated per millisecond across the entire simulation.
* **Latency (Waiting Time):** Minimizing the Time to Decode Ready (TDR) for initial processing and the Time Per Output Token (TPOT) gaps between consecutive generated tokens.

$R$ itself is not announced during the run and is only implicitly revealed when the stream of arrivals stops; consequently any online estimate built from $R$ (see §7 note) is provisional until the run ends.

## Request Lifecycle (State Machine)

Every request travels through an initial token-free input stage followed by an output stage repeated $L_{\text{out}}[i]$ times.

* **Input Stage (One-Time Preparation):**
  * **ARR:** Request arrives with a known input length $L_{\text{in}}[i]$.
  * **P PRE (Local):** Assigns the request to a specific remote computer in $[0, K)$ and queues an uplink transfer.
  * **P PROC (Remote):** Computes $\textit{num\_layers}$ parts on the assigned remote. Can be processed as one full piece or split into ascending, gap-free integer ranges. The final piece queues a downlink transfer.
  * **P POST (Local):** Completes the input stage, stopping the TDR timer and readying the request for output generation.

* **Output Stage (Repeated per Token):**
  * **D PRE (Local):** Batches multiple ready requests across different remote computers. Queues one uplink transfer per remote computer represented in the batch.
  * **D PROC (Remote):** Processes the batch on a specific remote computer (all members must be assigned to this remote) and queues a downlink transfer.
  * **D POST (Local):** Generates exactly one token per grouped member.
  * **FIN:** Issued simultaneously with the final **D POST** completion, permanently retiring the request.

## Strict Execution Constraints

* **Resource Exclusivity:** A computer can execute at most one task at a time and is occupied for $[t, \, t + S + \text{dur}]$ where $S$ is a fixed schedule cost paid once per task (input-stage piece or output group); transfers never pay $S$.
* **Predecessor Rules:** A task cannot be scheduled until all prerequisite event completions (**TDN**, **XDN**, or **ARR**) have explicitly arrived in the event frame.
* **Network Queues:** The shared link operates two independent, one-at-a-time FIFO queues (Uplink and Downlink); a transfer's position in its queue is determined by enqueue order, not by which transfer would finish first.
* **FIN Penalty:** Grouping a request into a **D PRE** (or any) task after it has received a **FIN** event, or after it is already mid-flight in another task, results in a zero-score protocol violation.
* **Liveness / No-Stuck-State Constraint:** At every decision point, if there exists at least one unfinished request, then at least one of the following must remain possible: a currently-assignable legal task, a transfer already in flight, or a future **ARR**. Formally, letting $U(t)$ be the set of unfinished requests at time $t$:

$$U(t) \neq \emptyset \;\Rightarrow\; \Big(\exists\, \text{assignable task at } t\Big) \;\vee\; \Big(\exists\, \text{transfer in flight}\Big) \;\vee\; \Big(\exists\, \text{future ARR}\Big)$$

  Violating this (an idle system with $U(t)\neq\emptyset$ and none of the three disjuncts holding) is a fatal stuck state scored $0$. This constraint dominates every optimization below: a heuristic that maximizes throughput or minimizes wait but ever produces a state failing this condition is invalid regardless of its score contribution elsewhere.

## Network, Resource & Graph Mathematical Models

The request lifecycle is naturally represented as a graph, but the complete scheduling problem is more accurately modeled as a **dynamic disjunctive graph with shared unary resources, two FIFO communication resources, and dynamically-created batch/fork-join nodes**.

The request dependency graph describes what must happen before what. Resource and FIFO constraints describe which otherwise-independent tasks must be ordered by the scheduler.

### 1. System State

At time $t$, define the scheduling state as

$$
X(t)=
\left\{
R_E,\,R_0,\ldots,R_{K-1},\,Q_{\mathrm{UP}},\,Q_{\mathrm{DOWN}},\,state_i,\,r_i
\right\}.
$$

Where $R_E$ is local availability (the time at which the local computer next becomes free), $R_k$ is remote $C_k$'s availability time, $Q_{\mathrm{UP}}$ and $Q_{\mathrm{DOWN}}$ are the FIFO queues' current tail-completion times, $state_i$ is the current request state (which lifecycle node it is waiting on or executing), and $r_i$ is the fixed remote assignment (undefined until that request's **P PRE** completes).

### 2. General Max-Plus Completion Equation

For any **compute task** node $v$ assigned to resource $\rho(v)$, with mandatory event predecessors $Pred(v)$ (the DAG dependencies — **ARR**, **TDN**, or **XDN** events that must have already arrived):

$$
\boxed{C_v=\max\left(A_v,\; \max_{u\in Pred(v)}C_u,\; R_{\rho(v)}\right)+S_v+d_v}
$$

where $A_v$ is the scheduler-chosen decision time (the frame timestamp at which the assignment is issued; $A_v \ge \max_{u \in Pred(v)} C_u$ is required for legality, since the task cannot be issued before its predecessors have arrived), $d_v$ is its duration, and $S_v=S$ for compute tasks, $S_v=0$ for transfer nodes (transfers have no resource term $R_{\rho(v)}$ either, since the FIFO queue's own state — $Q_{\mathrm{UP}}$ or $Q_{\mathrm{DOWN}}$ — plays that role instead; see §4). After scheduling a compute task, the corresponding resource's availability is updated: $R_{\rho(v)} \leftarrow C_v$.

This separates two distinct kinds of constraint that the original single-`max`-over-`Pred(v)` formulation conflated: (a) **DAG predecessor completions**, which are events that must have already been *delivered* to the scheduler before the task can legally be issued, and (b) **resource availability**, which is a *scheduling* constraint (the resource must be physically free) rather than a dependency the interactor checks via delivered events. Both must hold, but only (a) can cause a protocol violation if ignored — issuing to a busy resource is a separate zero-score violation, not merely a later completion time.

### 3. Network Transfer Duration

$$
\boxed{T(\textit{len})=\text{latency\_in\_ms}+\frac{8\,\textit{len}\,\text{bytes\_per\_token}}{\text{bandwidth\_gbps}\times10^6}}
$$

For input-stage transfers, $\textit{len}=L_{\text{in}}[i]$ (regardless of how many pieces the input stage was split into — only the final piece triggers this transfer, once, at full length). For output transfers, $\textit{len}$ is the number of requests carried by that specific per-remote transfer ($m_k$ for a D-PRE-triggered uplink branch, or the D-PROC group size for the downlink). $T$ is strictly positive since $\text{latency\_in\_ms} > 0$ is guaranteed.

### 4. FIFO Network Constraints

UP and DOWN are independent one-at-a-time FIFO queues. For transfer $j$ entering a queue at availability time $A(\cdot_j)$ (i.e., as soon as its triggering task completes):

$$C(UP_j)=\max(A(UP_j),Q_{\mathrm{UP}})+T(\textit{len}_j),\qquad Q_{\mathrm{UP}}\leftarrow C(UP_j)$$

$$C(DOWN_j)=\max(A(DOWN_j),Q_{\mathrm{DOWN}})+T(\textit{len}_j),\qquad Q_{\mathrm{DOWN}}\leftarrow C(DOWN_j).$$

The enqueue order is therefore part of the scheduling dynamics: two transfers ready at the same instant do not execute in parallel — one strictly delays the other's start by occupying the queue's tail-time first. When multiple transfers become ready simultaneously (e.g., a single D-PRE's per-remote branches, §13), the enqueue order is not a free choice: the protocol fixes it (increasing remote index for a D-PRE fork; otherwise interactor event order). The recursion above must therefore be applied **sequentially in that fixed order**, not simultaneously, even though all these transfers are "queued at once" in wall-clock terms.

## DAG / Disjunctive-Graph Representation

### 5. Request Dependency Graph

For one request:

$$ARR\rightarrow P\_PRE\rightarrow UP_P\rightarrow P\_PROC\rightarrow DOWN_P\rightarrow P\_POST$$
$$P\_POST\rightarrow D\_PRE\rightarrow UP_D\rightarrow D\_PROC\rightarrow DOWN_D\rightarrow D\_POST$$

and each $D\_POST_j$ precedes the next $D\_PRE_{j+1}$ until the $L_{\text{out}}[i]$-th $D\_POST$, which coincides with **FIN**. When $P\_PROC$ is split into $\gamma_i > 1$ pieces, the middle edge expands to a chain $P\_PROC_{i,1} \to P\_PROC_{i,2} \to \dots \to P\_PROC_{i,\gamma_i}$, with only the last node feeding $DOWN_P$ (§9).

### 6. Resource / Disjunctive Edges

For two compute tasks $a,b$ contending for the same resource (both requiring the local computer, or both requiring the same remote $C_k$):

$$a\prec b\quad\lor\quad b\prec a.$$

Exactly one ordering is chosen by the scheduler's assignment order (a resource cannot run both at once, and once started a task cannot be pre-empted). The same ordering concept applies to transfers sharing a FIFO queue, but there the *actual* order is not a scheduler choice — it is fixed by enqueue order (protocol-determined for simultaneous ties, interactor-order otherwise), making transfer-queue disjunctive edges resolved automatically rather than by explicit assignment. Thus the scheduler is only free to select **resource orderings for compute tasks**; FIFO orderings are an emergent consequence of *when* the scheduler triggers the tasks that produce transfers.

## Input Stage

### 7. P-PRE

$$\boxed{C(P\_PRE_i)=\max(ARR_i,R_E)+S+p_{\mathrm{pre}}(L_{\text{in}}[i])}$$

Completion of $P\_PRE_i$ also fixes $r_i \in [0,K)$ and immediately enqueues $UP_{P,i}$ (§8) regardless of whether remote $r_i$ is busy — the uplink transfer is not gated on remote availability, only on the local $P\_PRE$ task finishing.

*(Online-TDR note: since each request's TDR contribution is known the instant its own $P\_POST_i$ completes, a running mean $\widehat{TDR}(t) = \frac{1}{n(t)}\sum_{i \text{ finished prefill by } t} TDR_i$ over the $n(t)$ requests that have reached $P\_POST$ so far is computable online and is a legitimate real-time proxy — unlike $TPOT$, whose global mean is not fixable until every request's remaining gaps are known.)*

### 8. Input UP

$$C(UP_{P,i})=\max(C(P\_PRE_i),Q_{\mathrm{UP}})+T(L_{\text{in}}[i]),\qquad Q_{\mathrm{UP}} \leftarrow C(UP_{P,i})$$

### 9. P-PROC and Chunking

**Piece legality constraints.** A chosen decomposition of request $i$'s input stage into pieces $\{[ls_1,le_1), \dots, [ls_{\gamma_i}, le_{\gamma_i})\}$ is legal iff:

$$ls_1 = 0, \qquad le_{\gamma_i} = \textit{num\_layers}, \qquad ls_{j+1} = le_j \ \ \forall j, \qquad le_j > ls_j \ \ \forall j, \qquad 1 \le \gamma_i \le \textit{num\_layers}.$$

(When $\textit{num\_layers}=1$, only $\gamma_i = 1$ is possible.) Any violation — empty range, range outside $[0,\textit{num\_layers}]$, out-of-order, or a gap — is a zero-score protocol error.

For a piece $[ls,le)$:

$$\boxed{d_{\mathrm{piece}}=\frac{le-ls}{\textit{num\_layers}}\,p_{\mathrm{proc}}(L_{\text{in}}[i])}$$

For the first piece:

$$C(P\_PROC_{i,1})=\max(C(UP_{P,i}),R_{r_i})+S+d_{\mathrm{piece},1}.$$

For piece $j > 1$:

$$\boxed{C(P\_PROC_{i,j})=\max\big(C(P\_PROC_{i,j-1}),\,R_{r_i}\big)+S+d_{\mathrm{piece},j}}$$

i.e. each later piece's only DAG predecessor is the *previous piece's own TDN* (no transfer in between — the pieces run back-to-back on the same remote, so the resource-availability term $R_{r_i}$ and the predecessor term coincide whenever the remote is not stolen for other work in between). Only the final piece ($le_{\gamma_i} = \textit{num\_layers}$) queues $DOWN_{P,i}$.

If request $i$ uses $\gamma_i$ pieces, total compute work remains $p_{\mathrm{proc}}(L_{\text{in}}[i])$ since $\sum_j d_{\mathrm{piece},j} = \sum_j \frac{le_j - ls_j}{\textit{num\_layers}} p_{\mathrm{proc}}(L_{\text{in}}[i]) = p_{\mathrm{proc}}(L_{\text{in}}[i])$ (the pieces telescope to cover $[0,\textit{num\_layers})$ exactly once), while schedule-cost overhead grows to

$$\boxed{\gamma_i S}$$

so splitting adds $(\gamma_i-1)S$ of pure overhead relative to $\gamma_i=1$, entirely on the local wall-clock critical path of *this* request (it strictly delays this request's own $DOWN_{P,i}$ and hence its $TDR_i$, by exactly $(\gamma_i - 1)S$, assuming the remote never idles waiting on other work in either case). A useful heuristic criterion is therefore

$$\boxed{\text{Interleaving benefit on remote } r_i>(\gamma_i-1)S}$$

where "interleaving benefit" should be measured as the reduction in *other* requests' completion delays achieved by inserting their tasks into the gaps opened between this request's pieces — not as a benefit to request $i$ itself, which is always net-delayed by splitting.

### 10. Input DOWN and P-POST

$$C(DOWN_{P,i})=\max(C(P\_PROC_{i,\mathrm{last}}),Q_{\mathrm{DOWN}})+T(L_{\text{in}}[i]),\qquad Q_{\mathrm{DOWN}} \leftarrow C(DOWN_{P,i})$$

$$\boxed{C(P\_POST_i)=\max(C(DOWN_{P,i}),R_E)+S+p_{\mathrm{post}}(L_{\text{in}}[i])}$$

Therefore:

$$\boxed{TDR_i=C(P\_POST_i)-ARR_i},\qquad TDR=\frac1R\sum_iTDR_i.$$

($R$ here is the *final*, scoring-time count of all requests in the test — this exact quantity is only computable once the run is known to be complete, i.e. retrospectively; see the online proxy in §7.)

## Output Stage and Dynamic Batching

### 11. Dynamic Batch Definition

For output group $G$, let $m=|G|$ and

$$G_k=\{i\in G:r_i=k\},\qquad m_k=|G_k|,\qquad m=\sum_km_k.$$

A D-PRE group may therefore fork into multiple remote branches. A member $i \in G$ is legal only if $Prev_i$ (its previous final step; see §12) has already completed and $i \notin FIN$-set — checked independently per member, so a group may freely mix requests at different $r_i$ and arbitrary $L_{out}$ progress, as long as each individually satisfies its own predecessor rule.

### 12. D-PRE

Let $Prev_i$ be $P\_POST_i$ for the first output step of request $i$, or its previous $D\_POST$ thereafter:

$$\boxed{C(D\_PRE_G)=\max\left(R_E,\max_{i\in G}C(Prev_i)\right)+S+d_{\mathrm{pre}}(m)}$$

Note $d_{\mathrm{pre}}(m)$ depends **only on the total group size $m$**, not on the per-remote split $\{m_k\}$ — this is the stated reason cross-remote grouping is always efficiency-neutral-or-better for the local D-PRE cost: combining ready requests regardless of their assigned remote never increases $d_{\mathrm{pre}}$ beyond what a single-remote group of the same size $m$ would cost.

### 13. D-PRE Fork into Per-Remote Transfers

For each $k$ with $m_k>0$, in **increasing order of $k$** (the protocol-fixed enqueue order for a single D-PRE's branches — see §4):

$$C(UP_{G,k})=\max(C(D\_PRE_G),Q_{\mathrm{UP}})+T(m_k),\qquad Q_{\mathrm{UP}} \leftarrow C(UP_{G,k})$$

applied sequentially for $k = 0, 1, \dots, K-1$ (skipping empty branches), so that a later branch's uplink transfer time already reflects the queueing delay imposed by earlier (lower-index) branches from the *same* D-PRE — even though all branches become "ready" at the identical instant $C(D\_PRE_G)$.

### 14. Per-Remote D-PROC

$$\boxed{C(D\_PROC_{G,k})=\max(C(UP_{G,k}),R_k)+S+d_{\mathrm{proc}}(m_k)}$$

checked independently per request within $G_k$: two members of $G_k$ may in principle have arrived via different D-PRE groups' UP transfers (§ Dependency Summary), but since all members of $G_k$ by definition belong to the *same* D-PROC task here, the relevant predecessor is the max over each member's own triggering XDN — which coincides with $C(UP_{G,k})$ only when all of $G_k$ came from the same D-PRE. The general form is $\max_i(\text{that member's own uplink XDN})$; §14's boxed form is the common case where the whole D-PROC group was produced by one D-PRE.

### 15. Per-Remote DOWN

$$C(DOWN_{G,k})=\max(C(D\_PROC_{G,k}),Q_{\mathrm{DOWN}})+T(m_k),\qquad Q_{\mathrm{DOWN}} \leftarrow C(DOWN_{G,k})$$

### 16. D-POST Join

D-POST requires all remote branches represented in the group to have returned:

$$\boxed{C(D\_POST_G)=\max\left(R_E,\max_{k:m_k>0}C(DOWN_{G,k})\right)+S+d_{\mathrm{post}}(m)}$$

Thus the output graph has a fork-join structure:

$$D\_PRE_G\rightarrow\{UP_{G,k}\rightarrow D\_PROC_{G,k}\rightarrow DOWN_{G,k}\}_{k:m_k>0}\rightarrow D\_POST_G.$$

The join is a **max**, not a sum: $D\_POST_G$'s completion is bottlenecked by whichever remote branch is slowest, so a group spanning remotes with very different current loads inherits the worst branch's finish time. This is the key fact that §18's additive cost proxy does not capture (see note there).

## Remote Assignment

### 17. Fixed Remote Assignment

A baseline is

$$\boxed{r_i=\arg\min_kR_k}$$

although this ignores future batching and transfer effects, and $R_k$ as tracked by the scheduler is only the time the remote becomes free of *already-assigned* tasks — it does not account for work assigned-but-not-yet-issued, nor for how many other in-flight requests already share remote $k$ (which affects future batch fragmentation, §19). A richer heuristic can use

$$\boxed{Cost(i,k)=\alpha R_k+\beta ExpectedDPROCWait(i,k)+\gamma BatchFragmentation(i,k)}$$

This is a heuristic, not an exact optimality equation — $ExpectedDPROCWait$ and $BatchFragmentation$ are themselves estimates (e.g., $BatchFragmentation(i,k)$ could be modeled as the marginal increase in the number of distinct nonempty $\{m_k\}$ branches across *future* batches that assigning $i \to k$ would induce, discouraging spreading requests thinly across many remotes when consolidating onto fewer, larger per-remote branches reduces total fork-join overhead per §18).

## Batch Efficiency

### 18. Approximate Per-Member Cost

All of $d_{\mathrm{pre}}, d_{\mathrm{proc}}, d_{\mathrm{post}}, p_{\mathrm{pre}}, p_{\mathrm{proc}}, p_{\mathrm{post}}$ below are **not closed-form functions**: each is obtained from the task-time table via piecewise-linear interpolation over $batch\_size$ (§0, added below), so no assumption of linearity, concavity, or monotonicity in $m$ should be baked into a heuristic without checking the actual table.

### 0. Task-Time Table Interpolation (prerequisite for all $d(\cdot), p(\cdot)$ terms above)

Let a step's table rows give durations $y_1, \dots, y_N$ at distinct listed sizes $x_1 < x_2 < \dots < x_N$ (restricted to that step's non-missing entries). For query size $x$:

$$
f(x) = \begin{cases}
y_1 & x \le x_1 \\
y_j + \dfrac{y_{j+1}-y_j}{x_{j+1}-x_j}(x - x_j) & x_j \le x \le x_{j+1} \\
y_N & x \ge x_N
\end{cases}
$$

$f$ instantiates whichever of $p_{\mathrm{pre}}, p_{\mathrm{proc}}, p_{\mathrm{post}}$ (queried at $x = L_{\text{in}}[i]$) or $d_{\mathrm{pre}}, d_{\mathrm{proc}}, d_{\mathrm{post}}$ (queried at $x = $ group/member count $m$) is needed. Every step is guaranteed at least one non-missing entry, and every resulting legal task duration is guaranteed strictly positive.

For batch size $m$:

$$Cost_{\mathrm{local}}(m)=\frac{2S+d_{\mathrm{pre}}(m)+d_{\mathrm{post}}(m)}m.$$

For remote branch $k$:

$$Cost_{\mathrm{remote}}(m_k)=\frac{S+d_{\mathrm{proc}}(m_k)}{m_k},\qquad Cost_{\mathrm{net}}(m_k)=\frac{2T(m_k)}{m_k}.$$

A rough comparison metric is

$$\boxed{Cost_{\mathrm{batch}}(G)\approx\frac{2S+d_{\mathrm{pre}}(m)+d_{\mathrm{post}}(m)+\sum_{k:m_k>0}[S+d_{\mathrm{proc}}(m_k)+2T(m_k)]}{m}.}$$

**Important caveat (strengthened):** this is a **total-work-per-member** proxy — it sums cost across all remote branches — and is useful for comparing the *throughput efficiency* (resource-seconds consumed per token) of different groupings. It is explicitly **not** an estimate of $C(D\_POST_G)$, which is a **max**, not a sum, over branches (§16). A group with one large, slow branch and several tiny fast branches can have a *low* average $Cost_{\mathrm{batch}}$ while still having *high* wall-clock latency, because the slow branch alone determines the join time. For latency-sensitive decisions (TDR/TPOT), the relevant quantity is the **critical-path estimate**:

$$\boxed{Latency_{\mathrm{batch}}(G) \approx S + d_{\mathrm{pre}}(m) + \max_{k:m_k>0}\Big[T(m_k) + S + d_{\mathrm{proc}}(m_k) + T(m_k)\Big] + S + d_{\mathrm{post}}(m)}$$

(ignoring queueing delay from $Q_{UP}, Q_{DOWN}, R_k$ contention with other concurrent work, which the exact recursions in §12–16 do capture). $Cost_{\mathrm{batch}}$ should drive throughput-oriented grouping decisions; $Latency_{\mathrm{batch}}$ should drive decisions gated by SLO1/SLO2 pressure.

### 19. Marginal Batch Decision

For a candidate request $i$:

$$\boxed{\Delta Cost_i=Cost_{\mathrm{batch}}(G\cup\{i\})-Cost_{\mathrm{batch}}(G)}$$

$$\boxed{\Delta Latency_i = Latency_{\mathrm{batch}}(G \cup \{i\}) - Latency_{\mathrm{batch}}(G)}$$

A dynamic batching policy can compare the throughput gain from including $i$ (via $\Delta Cost_i$, typically negative/beneficial when $i$'s remote branch already has slack or $i$ shares $r_i$ with an existing member) against the latency penalty caused by delaying $i$ and every current member of $G$ by $\Delta Latency_i$, rather than relying only on a fixed threshold $\beta$. Note that including $i$ delays **every** member of $G$ already waiting in the not-yet-issued $D\_PRE_G$, not just $i$ itself — so the true latency cost of admitting $i$ late is $|G| \cdot (\text{delay incurred by waiting for } i)$, not a single member's delay; this asymmetry is why $\tau$ (time-to-live, below) exists as a hard cutoff independent of $\Delta Cost$.

## Token Timing and TPOT

### 20. Token Production Times

Let $e_{i,j}$ be the timestamp at which request $i$ produces token $j$. If it belongs to group $G_j$ for iteration $j$:

$$\boxed{e_{i,j}=C(D\_POST_{G_j})}$$

The pooled TPOT is

$$\boxed{TPOT=\frac{\sum_i\sum_{j=1}^{L_{\mathrm{out}}[i]-1}(e_{i,j+1}-e_{i,j})}{\sum_i(L_{\mathrm{out}}[i]-1)}}.$$

Requests with one output token contribute no TPOT gap; if **no** request contributes a gap (every $L_{out}[i]=1$), $TPOT$ is defined as $0$ (matching the problem statement's explicit fallback, not left undefined by a $0/0$ division).

## Waiting-Time Objective

### 21. TDR and TPOT Penalties

$$excess_{\mathrm{TDR}}=\max\left(0,\frac{TDR-SLO1}{SLO1}\right)$$

$$excess_{\mathrm{TPOT}}=\max\left(0,\frac{TPOT-SLO2}{SLO2}\right)$$

$$\boxed{dist=\sqrt{excess_{\mathrm{TDR}}^2+excess_{\mathrm{TPOT}}^2}}$$

The waiting component is

$$WaitingScore=\begin{cases}\max(0,1-dist/dist_{\mathrm{base}}),&dist_{\mathrm{base}}>0,\\1,&dist_{\mathrm{base}}=0\land dist=0,\\0,&dist_{\mathrm{base}}=0\land dist>0.\end{cases}$$

## Throughput Objective

### 22. Output Rate

$$T_{\mathrm{elapsed}}=T_{\mathrm{last\ token}}-T_{\mathrm{first\ arrival}}$$

$$\boxed{tp=\frac{\sum_iL_{\mathrm{out}}[i]}{T_{\mathrm{elapsed}}}}$$

$$ThroughputScore=clamp(tp;tp_{\mathrm{base}},tp_{\mathrm{UB}}), \qquad clamp(x;base,target) = \max\!\big(0,\min(1,\tfrac{x-base}{target-base})\big).$$

## Final Optimization Problem

The scheduling decisions are

$$\boxed{\mathcal{D}=\{r_i,\text{resource ordering},\text{chunk boundaries},\text{batch membership},\text{batch timing}\}.}$$

The objective is

$$\boxed{\max_{\mathcal{D}}\left[w_{\mathrm{tp}}\,ThroughputScore+w_c\,WaitingScore\right]}$$

subject to:

* lifecycle dependencies (§5, the DAG order per request),
* compute-resource exclusivity (§6, one task at a time per computer),
* UP/DOWN FIFO ordering (§4, enqueue-order-determined completion times),
* fixed remote assignment (§17, $r_i$ immutable once $P\_PRE_i$ completes),
* valid chunk boundaries (§9, the piece legality constraints),
* legal batching ($m \ge 1$, distinct ids, each member's own predecessor satisfied — §11),
* the liveness / no-stuck-state constraint (Strict Execution Constraints, above) — a **hard feasibility constraint**, not a soft objective term; any $\mathcal{D}$ violating it scores $0$ regardless of the bracketed objective value,
* and eventual completion of every request.

This formulation explains why the environment is non-differentiable: a scheduling decision can change resource order, FIFO order, batch composition, or the resulting graph topology. Derivative-free optimization is therefore appropriate for tuning higher-level scheduling heuristics — but note the objective is only evaluated over *feasible* $\mathcal{D}$ (those satisfying every constraint above); infeasible points do not have a well-defined "low score" on this landscape, they are simply excluded (scored $0$ by fiat, not by the formula), which matters for how a derivative-free search should treat constraint violations (as a hard penalty/rejection, not as a smoothly-worse objective value).

## Algorithmic Workflow Parameters

The scheduling logic requires tuning specific thresholds to balance the scoring objectives.

| Parameter | Function | Tuning Impact |
| --- | --- | --- |
| **$\beta$ (Batch Threshold)** | Target request count to trigger a **D PRE** group. | Higher values dilute the schedule cost $S$ across more requests to maximize throughput, but heavily penalize TPOT and TDR by forcing early requests to wait.|
| **$\tau$ (Time-to-Live)** | Maximum wait time before forcing execution of an under-filled batch. | Lower values optimize the wait-time penalty ($dist$) by ensuring quick token generation, but risk congesting the network with small payloads and repeatedly triggering the $S$ cost penalty.|
| **$\gamma$ (Input Chunking)** | Number of ranges a **P PROC** task is divided into. | $\gamma = 1$ secures the fastest TDR for a request, while $\gamma > 1$ allows interleaving other output tasks to keep the remote worker highly utilized at the cost of delaying the chunked request.|

We can model $\beta$, $\tau$, and $\gamma$ as parametric heuristic functions, transforming the scheduling problem into a black-box optimization task where we tune weights and biases to maximize the total score. By parameterizing these thresholds, we map the static system variables provided in the startup configuration directly to real-time scheduling decisions.

**Batch Threshold ($\beta$)**
Model $\beta$ as a function of the schedule cost $S$, the throughput weight $w_{tp}$, and the wait-time target $SLO2$. Higher $S$ necessitates larger batches to amortize the setup penalty, while a strict $SLO2$ forces smaller batches to prevent bottlenecking.

$$\beta(S, w_{tp}, SLO2) = \max\left(1, \left\lfloor w_1 S + w_2 \left(\frac{w_{tp}}{1 - w_{tp} + \epsilon}\right) - w_3 SLO2 + b_1 \right\rfloor\right)$$

The outer $\max(1, \cdot)$ correctly guarantees $\beta \ge 1$ even when the inner expression is very negative (e.g. tiny $S$, small $w_{tp}$, huge $SLO2$), which is necessary since $m=1$ must always remain a legal, achievable fallback batch size.

**Time-to-Live ($\tau$)**
Model $\tau$ based on the waiting-time tolerance $SLO2$ and the network parameters. It acts as a pressure valve to prevent the wait-time penalty $dist$ from exploding when a batch is taking too long to fill.

$$\tau(SLO2, \text{latency}) = \max\left(0, w_4 SLO2 + w_5\, \text{latency} + b_2\right)$$

Because the scheduler is purely reactive (§ Interaction: "you act only when a frame arrives"), $\tau$ cannot be enforced as a wall-clock timer — there is no self-wake mechanism. In practice $\tau$ is only *checkable* at the next event frame: a batch should be flushed with whatever is ready as soon as an arriving frame's timestamp shows the oldest waiting member has exceeded $\tau$, rather than being flushed proactively at an unobserved future instant.

**Input Chunking ($\gamma$)**
Model $\gamma$ based on the input length $L_{\text{in}}$ and the total available parts $\textit{num\_layers}$. If the input is massive, chunking allows you to interleave other tasks, keeping the remote computer utilized.

$$\gamma(L_{\text{in}}, \textit{num\_layers}) = \min\left(\textit{num\_layers}, \max\left(1, \left\lfloor w_6 \frac{L_{\text{in}}}{1000} + b_3 \right\rfloor\right)\right)$$

The outer $\min(\textit{num\_layers}, \cdot)$ is required for legality (§9: $\gamma_i \le \textit{num\_layers}$ always, with equality forced when $\textit{num\_layers}=1$), and the inner $\max(1, \cdot)$ mirrors $\beta$'s guarantee that the unsplit single-piece case remains reachable.

**Optimization Strategy**
Because the scoring system and the directed acyclic graph (DAG) of the request lifecycle form a non-differentiable environment, standard gradient descent cannot backpropagate through the simulation. You must treat this as a derivative-free optimization problem over **feasible** parameter settings only — any $(w_1,\dots,w_6,b_1,\dots,b_3)$ that induces a stuck state or protocol violation on some offline-simulated input must be rejected outright (treated as $-\infty$ / score $0$), not merely penalized, since the real interactor's verdict is identically all-or-nothing on those failure modes.

* **Bayesian Optimization:** Use Gaussian processes to efficiently explore the parameter space $(w_1 \dots w_6, b_1 \dots b_3)$. This builds a surrogate model of your scoring function, predicting which weight combinations will yield the highest total score without running exhaustive simulations.
* **Simulated Annealing:** Implement a local search that randomly perturbs the weights. Accepting worse solutions with a decaying probability allows the model to escape local optima, which is highly effective for discrete scheduling topologies.
* **Grid/Random Search:** For rapid baseline testing, evaluate random combinations of weights against an offline simulator. Since you only have $22$ preliminary tests for feedback, running thousands of combinations locally ensures your final parameters are robust against hidden variations in $R$ or $L_{\text{out}}[i]$.