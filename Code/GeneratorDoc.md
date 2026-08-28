# Generator Mathematics — Stress-Coefficient Test-Case Model

## 0. Why This Exists

The current `Generator.py` samples every physical field ($K, S,$ latency, bandwidth,
$L_{\text{in}}, L_{\text{out}}$, task-time shape, arrivals) **independently and uniformly**
inside a per-profile range, then derives SLO1/SLO2/$tp_{UB}$/$tp_{base}$/$dist_{base}$
*afterward* from whatever combination happened to land (`_generate_scoring`). Two draws from
the same profile can therefore differ in whether the network or the compute is the actual
bottleneck, purely by chance — the profile controls a *box* in parameter space, not a
*mechanic*.

This document replaces that with a **7-dimensional stress vector** $\boldsymbol{\theta}\in[0,1]^7$.
Each coordinate is a direct, named dial on one scheduling mechanic from `Mathematics.md`. Every
physical field is a **deterministic function of $\boldsymbol{\theta}$ plus small independent
multiplicative jitter** — so a test case's *character* (what it's designed to stress) is chosen
on purpose, while its *exact numbers* are still unique every draw, which is what prevents a
scheduler from overfitting to fixed constants.

**Read this top-to-bottom once before implementing** — later sections consume values derived in
earlier ones, and the dependency order below is load-bearing (see §10 for the fixed evaluation
order and the circularity that has to be broken).

---

## 1. The Stress Vector

$$
\boldsymbol{\theta} = (\theta_{\text{net}},\ \theta_{\text{comp}},\ \theta_{\text{batch}},\
\theta_{\text{chunk}},\ \theta_{\text{arr}},\ \theta_{\text{scale}},\ \theta_{\text{slo}})
\in [0,1]^7
$$

| Coefficient | Mechanic (see `Mathematics.md`) | $\theta=0$ | $\theta=1$ |
|---|---|---|---|
| $\theta_{\text{net}}$ | Network dominance (§3–4) | link ≈ free | link dominates every transfer |
| $\theta_{\text{comp}}$ | Compute dominance (task-time table) | tasks ≈ free | tasks dominate transfer |
| $\theta_{\text{batch}}$ | Batching economics (§18–19) | $S$ tiny, grouping worthless | $S$ huge, grouping mandatory |
| $\theta_{\text{chunk}}$ | Chunking pressure (§9) | 1 layer, no splitting | many layers, large $L_{\text{in}}$ |
| $\theta_{\text{arr}}$ | Arrival concurrency | near-serial streaming | full burst at $t=0$ |
| $\theta_{\text{scale}}$ | Problem size | few, short requests | near-constraint-max $R$, $\sum L_{\text{out}}$ |
| $\theta_{\text{slo}}$ | SLO tightness vs. achievable frontier | very loose | pinned at the frontier |

Each coordinate is drawn from a **profile-specific Beta distribution**:

$$
\boxed{\theta_j \sim \text{Beta}(a_j, b_j)}
$$

$\text{Beta}(1,1)$ = uniform ("this profile doesn't care about this axis" — the default).
$\text{Beta}(4,1)$ concentrates near 1 ("this profile is *about* stressing this mechanic").
$\text{Beta}(1,4)$ concentrates near 0. A profile is now a table of 7 $(a_j,b_j)$ pairs (§9)
instead of ~15 independent min/max ranges.

This is the **only** place randomness enters the top-level character of a test case. Everything
below is derived, then jittered.

---

## 2. Compute Scale & Task-Time Table

Order matters: compute the reference compute time **first**, since the network side (§3) is
defined relative to it.

### 2.1 Reference decode-compute time

$$
D_{\text{tok}} = D_0 \cdot \big(1 + 4\,\theta_{\text{comp}}\big)\cdot \eta_3, \qquad
D_0 = 0.5\text{ ms},\quad \eta_3 \sim \text{LogU}(0.7, 1.4)
$$

$\theta_{\text{comp}}=0 \Rightarrow D_{\text{tok}}\approx 0.5$ ms (light);
$\theta_{\text{comp}}=1 \Rightarrow D_{\text{tok}}\approx 2.5$ ms (heavy), before jitter.
Deliberately modest at $m=1$ — the **curve shape** below, not the base value, is what decides
whether compute or network wins at realistic batch/chunk sizes.

### 2.2 Curve-shape exponents

$$
\boxed{p_{\text{decode\_proc}} = 0.4 + 0.9\,\theta_{\text{comp}}, \qquad
p_{\text{prefill\_proc}} = 0.5 + 0.7\,\theta_{\text{comp}}}
$$

$$
p_{\text{pre}} = p_{\text{post}} = 0.15 + 0.25\,\theta_{\text{comp}} \qquad \text{(flatter — fixed local bookkeeping, not model compute)}
$$

$$
d(m) = D_{\text{tok}}\cdot m^{\,p}\cdot \zeta(m), \qquad \zeta(m)\sim\mathcal{U}(0.9,1.1)\ \text{i.i.d. per table row}
$$

$p<1$: sublinear, batching amortizes cost. $p=1$: batching is cost-neutral. $p>1$: superlinear,
large batches actively hurt (a compute bottleneck that *punishes* naive large-$m$ batching).
Driving the exponent (not just the base) off $\theta_{\text{comp}}$ is what makes one coefficient
produce qualitatively different regimes instead of uniformly scaling every duration.

### 2.3 Row placement (deferred — needs $R_{\max\_group}$ from §5.2)

$$
N \sim \mathcal{U}\{2,\dots,\ 20+60\,\theta_{\text{scale}}\}
$$

$$
\boxed{x_i = \Big\lceil x_{\max}^{\,i/(N-1)}\Big\rceil,\quad i=0,\dots,N-1,\qquad
x_{\max}=\min\big(4096,\ \lceil 4\,R_{\max\_group}\rceil\big)}
$$

Log-spaced placement so small **and** large batch sizes are both represented, and the top of the
range tracks batch sizes the run can actually reach (§5.2) instead of always spanning up to 4096
regardless of feasibility. Each of the 6 columns is independently missing ($-1$) with probability
$0.12$, subject to "every step needs $\ge 1$ non-missing entry" (unchanged from the current
generator — not a stress lever).

**Dependency note:** $R_{\max\_group}$ (§5.2) needs $D_{\text{tok}}, T_{\text{tok}}, S, K$, which
in turn need pieces of this section. See §10 for the exact evaluation order that avoids a cycle.

---

## 3. Network Parameters

### 3.1 Network dominance ratio

$$
\boxed{\rho_{\text{net}}(\theta_{\text{net}}) = \rho_{\min}\Big(\frac{\rho_{\max}}{\rho_{\min}}\Big)^{\theta_{\text{net}}}},
\qquad \rho_{\min}=0.05,\ \rho_{\max}=20
$$

Log-interpolates between "network 20× cheaper than compute" and "network 20× more expensive."
Used once to *set* latency/bandwidth (below) and again in §8 as a post-hoc sanity check.

### 3.2 Reference transfer time and its split

$$
T_{\text{tok}} = \rho_{\text{net}}\cdot D_{\text{tok}}
$$

$$
\lambda \sim \mathcal{U}(0.1, 0.9) \qquad \text{(latency-vs-bandwidth mix; a nuisance parameter, stays uniform)}
$$

$$
latency\_in\_ms = \lambda\, T_{\text{tok}}\cdot \eta_1, \qquad \eta_1\sim\text{LogU}(0.85,1.18)
$$

$$
bytes\_per\_token \sim \text{LogU}(10^2,10^6) \qquad \text{(drawn once, independent — a billing unit, not a stress dial)}
$$

$$
\boxed{bandwidth\_gbps = \frac{8\cdot bytes\_per\_token}{10^6\cdot(1-\lambda)\,T_{\text{tok}}\cdot\eta_2}},
\qquad \eta_2\sim\text{LogU}(0.85,1.18)
$$

This inverts $T(1) = latency + 8\cdot bpt/(bw\times10^6)$ (`Mathematics.md` §3) at 1 token,
solving for bandwidth given the target bandwidth-limited share. **Clip after computing:**
latency to $[0.001, 50]$ ms, bandwidth to $[0.001, 100]$ Gb/s. If clipping binds,
$\rho_{\text{net}}$'s realized value drifts from target — exactly what §8's check catches.

---

## 4. Schedule Cost $S$ (Batching Economics)

$$
\boxed{S = D_{\text{tok}}\cdot\big(0.2+12\,\theta_{\text{batch}}^{1.5}\big)\cdot\eta_4},
\qquad \eta_4\sim\text{LogU}(0.8,1.25)
$$

clipped to $[1,10]$. The exponent $1.5$ pushes most of $[0,1]$ into "batching barely matters"
and reserves the top for "solo dispatch is provably wasteful, a correct scheduler *must* batch."
This gives a direct, checkable dial on `Mathematics.md` §18's
$Cost_{\text{local}}(m)=(2S+d_{pre}(m)+d_{post}(m))/m$.

**Feasibility floor (hard constraint — evaluate after §2's table base values are known):**

$$
S + d_{\text{decode\_proc}}(1) \le 10^4 \qquad\text{and}\qquad S \ge 1
$$

If violated, **re-draw $\eta_4$ only** (not the whole test case) — cheap, local, non-degenerate
rejection sampling.

---

## 5. Layers, Chunking, and $L_{\text{in}}$

$$
\boxed{\textit{num\_layers} = \max\big(1,\ \big\lfloor 64^{\,\theta_{\text{chunk}}}\big\rceil\big)}
$$

$\theta_{\text{chunk}}=0\Rightarrow 1$ layer (chunking impossible — `Mathematics.md` §9's
degenerate case). $\theta_{\text{chunk}}=1\Rightarrow 64$ (max splitting freedom). Log-spacing
keeps mid-range granularities (4, 8, 16) well represented instead of crowded to the extremes.

$$
L_{\text{in}} \sim \text{LogU}\big(L_{\min}(\theta_{\text{chunk}}),\ L_{\max}(\theta_{\text{chunk}})\big)
$$

$$
L_{\min}=16+112\,\theta_{\text{chunk}}, \qquad L_{\max}=256+3200\,\theta_{\text{chunk}}^2
$$

Chunking only matters once a single prefill's compute duration is large enough that splitting
frees the remote in between (§9's "interleaving benefit" criterion) — tying $L_{\text{in}}$'s
range to $\theta_{\text{chunk}}$ keeps that link explicit. Drawn **per request, i.i.d.**, not
once per test case, so even a "high chunking pressure" test case has a realistic mixture of
prefill sizes rather than one repeated value.

---

## 6. Scale, Concurrency, and Arrivals

### 6.1 Request count

$$
\boxed{R = \Big\lceil R_{\min}\cdot(R_{\max}/R_{\min})^{\theta_{\text{scale}}}\Big\rceil\cdot\xi_R},
\qquad R_{\min}=8,\ R_{\max}=2000,\ \xi_R\sim\mathcal{U}(0.85,1.15)
$$

clipped to $[1,2000]$.

### 6.2 Output length ($L_{\text{out}}$) — heavy-tailed

$$
L_{\text{out}} = \Big\lceil 1+(L_{\text{out}}^{\max}-1)\cdot U^{\,\kappa}\Big\rceil,\qquad
U\sim\mathcal{U}(0,1),\ \ \kappa = 2+3\,\theta_{\text{scale}}
$$

with $L_{\text{out}}^{\max}=512$. Running-sum rejection against
$\sum_i L_{\text{out}}[i]\le 2\times10^5$: **truncate the last request's draw to fit**, don't
discard the whole test case. Heavy tails mean a mix of many short "easy" requests and a few long
"expensive" ones in the *same* test case — a scheduler tuned only on uniform $L_{\text{out}}$
mis-tunes $\beta/\tau$ (`Mathematics.md` §Algorithmic Workflow Parameters) for the tail.

### 6.3 Expected concurrent-ready count ($R_{\max\_group}$)

$$
\boxed{R_{\max\_group}\approx \frac{R}{K}\cdot\frac{D_{\text{tok}}+2T_{\text{tok}}+2S}{\overline{IAT}}}
$$

A Little's-law-style estimate of steady-state in-flight decode iterations per remote. **Not**
fed back into $R$ or $K$ — used only to size the table's batch axis (§2.3) so it covers group
sizes the run can realistically reach.

### 6.4 Arrivals (Poisson, rate controlled by $\theta_{\text{arr}}$)

$$
\overline{IAT} = \overline{IAT}_{\text{stream}}\Big(\frac{\overline{IAT}_{\text{burst}}}{\overline{IAT}_{\text{stream}}}\Big)^{\theta_{\text{arr}}},
\qquad \overline{IAT}_{\text{burst}}=0.05\text{ ms},\ \ \overline{IAT}_{\text{stream}}=50\,D_{\text{tok}}
$$

$$
\Delta t_i \sim \text{Exp}(1/\overline{IAT}), \qquad t_i=t_{i-1}+\Delta t_i,\ t_0=0
$$

Replaces the current generator's hand-tuned three-band `if/elif/else` mixture with a single,
principled, controllable-rate family. $\theta_{\text{arr}}=1$ collapses to a near-$t{=}0$ burst
(matching the problem statement's explicit all-at-once edge case); $\theta_{\text{arr}}=0$
spreads requests over ~50× one decode step, forcing survival through long idle stretches
(the liveness constraint in `Mathematics.md`).

### 6.5 Remote count

$$
\boxed{K = \Big\lceil 1+7\cdot\theta_{\text{scale}}^{0.5}\cdot(1-0.4\,\theta_{\text{net}})\Big\rceil}
$$

clipped to $[1,8]$. More remotes help less when the link itself is the bottleneck, so $K$ is
pulled down as $\theta_{\text{net}}\to1$ — deliberately produces "many workers, one narrow shared
pipe" cases that stress the FIFO queue model (`Mathematics.md` §4) rather than only per-remote
compute contention.

---

## 7. SLO Targets and Baselines (Tied to the Achievable Frontier)

### 7.1 Single-request physical floors

Using the max-plus recursions (`Mathematics.md` §7–10, §12–16) for one isolated request:

$$
TDR_{\text{phys}} = (S+p_{pre})+T(L_{\text{in}})+(S+p_{proc})+T(L_{\text{in}})+(S+p_{post})
$$

$$
TPOT_{\text{phys}} = (S+d_{pre}(1))+T(1)+(S+d_{proc}(1))+T(1)+(S+d_{post}(1))
$$

### 7.2 Congestion inflation

$$
\boxed{q = 1+\big(R_{\max\_group}^{0.5}-1\big)^+}, \qquad
TDR_{\text{exp}} = TDR_{\text{phys}}\cdot q, \qquad TPOT_{\text{exp}} = TPOT_{\text{phys}}\cdot q^{0.5}
$$

Replaces the current generator's ad hoc $\sqrt{R/K}$ with a factor derived from the *same*
$R_{\max\_group}$ estimate that already shaped the table and $K$ — internally consistent instead
of a second, unrelated heuristic. TPOT is inflated more gently since batching, once triggered,
reduces marginal per-token cost, partially offsetting congestion; prefill admission has no
equivalent amortization.

### 7.3 SLO placement

$$
\boxed{SLO1 = TDR_{\text{exp}}\cdot(1+4\,\theta_{\text{slo}})\cdot\eta_5}, \qquad
SLO2 = TPOT_{\text{exp}}\cdot(1+4\,\theta_{\text{slo}})\cdot\eta_6, \qquad \eta_{5,6}\sim\text{LogU}(0.8,1.25)
$$

$\theta_{\text{slo}}=0$: target ≈ 4–5× the physical floor (very loose). $\theta_{\text{slo}}=1$:
target sits at the achievable floor (scheduler must be near-optimal to earn any waiting-time
credit). Continuously interpolates the old `tight/moderate/loose` labels.

$$
dist_{base} = (2.5-2\theta_{\text{slo}})\cdot\eta_7, \qquad \eta_7\sim\text{LogU}(0.8,1.25)
$$

$$
tp_{base} = tp_{\text{est}}\cdot(0.5-0.45\,\theta_{\text{slo}})^+\cdot\eta_8, \qquad \eta_8\sim\text{LogU}(0.8,1.25)
$$

$$
\boxed{tp_{UB} = \min\Big(tp_{base}+(tp_{\text{est}}-tp_{base})\cdot(1+\theta_{\text{slo}})\cdot\eta_9,\ \ tp_{base}+\epsilon_{\text{margin}}\cdot tp_{\text{est}}\Big)}
$$

$\eta_9\sim\text{LogU}(0.8,1.25)$. **Fix vs. the original draft:** the outer `min(...)` is new.
Without it, large $\theta_{\text{slo}}$ combined with a large $\eta_9$ draw can push $tp_{UB}$
past $2\times\, tp_{\text{est}}$ while $tp_{base}$ also shrinks toward 0 — legal per the
constraint $tp_{UB}>tp_{base}$, but it silently produces an unreachably generous throughput
ceiling. Cap the spread with $\epsilon_{\text{margin}}=3.0$ (i.e. $tp_{UB}$ never exceeds
$tp_{base}+3\,tp_{\text{est}}$) so the clamp band stays meaningful. If this ever collapses
$tp_{UB}\le tp_{base}$ (only possible under extreme jitter), bump $tp_{UB}\leftarrow tp_{base}+10^{-4}$.

$tp_{\text{est}}$ is computed from $\sum_i L_{\text{out}}[i]$ and $TDR_{\text{exp}}, TPOT_{\text{exp}}$
exactly as the current `est_total_time`/`est_tp` derivation — that part is already
well-grounded and carries over unchanged.

### 7.4 Weight split

$$
w_{tp}\sim\text{Beta}(a_w,b_w), \qquad w_c = 1-w_{tp}
$$

$(a_w,b_w)$ is a profile parameter: "throughput-only" ≈ $\text{Beta}(50,1)$, "latency-only" ≈
$\text{Beta}(1,50)$, "balanced" ≈ $\text{Beta}(6,6)$, "random" ≈ $\text{Beta}(1,1)$. Replaces
literal `w_tp: 0.0`/`1.0` with a smooth, never-exactly-degenerate distribution — closes off one
more axis a scheduler could overfit to.

---

## 8. Post-Hoc Consistency Check (Anti-Degeneracy, Not Anti-Randomness)

After assembling a candidate test case, recompute the **realized** coefficients from the actual
generated numbers and compare to target:

$$
\boxed{\text{accept iff } \forall\,\phi\in\{\rho_{\text{net}},\ S/D_{\text{tok}},\ \textit{num\_layers},\ q\}:\ \ \tfrac12\le \phi_{\text{realized}}/\phi_{\text{target}}\le 2}
$$

If violated, **reject-and-redraw only the jitter terms** ($\eta_1,\dots,\eta_9,\ \xi_R,\ \zeta$),
never $\boldsymbol{\theta}$ itself. This bounds the noise-vs-intent gap without making output
deterministic — every accepted test case still has independent draws for every $\eta_\bullet$,
$\zeta(m)$, per-request $L_{\text{in}}$, arrival gaps, and $L_{\text{out}}$, so two test cases
with identical $\boldsymbol\theta$ are similar in *character* but never identical in *value*.
Complementary to (and should be kept alongside) the existing zero-score calibration rejection
loop, which checks real-judge rank order rather than physical plausibility.

---

## 9. Profiles as Beta-Shape Tables

Every profile becomes 7 $(a_j,b_j)$ pairs plus an optional $(a_w,b_w)$ for the weight split,
instead of ~15 literal min/max ranges. Unlisted axes default to $\text{Beta}(1,1)$ (uniform —
mirrors the current `DEFAULT_PROFILE` fallback).

| profile | $\theta_{\text{net}}$ | $\theta_{\text{comp}}$ | $\theta_{\text{batch}}$ | $\theta_{\text{chunk}}$ | $\theta_{\text{arr}}$ | $\theta_{\text{scale}}$ | $\theta_{\text{slo}}$ | $(a_w,b_w)$ |
|---|---|---|---|---|---|---|---|---|
| `network_bottleneck` | Beta(6,1) | Beta(1,3) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(2,2) | (6,6) |
| `fast_network` | Beta(1,6) | Beta(3,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(3,2) | (6,6) |
| `compute_bottleneck` | Beta(1,6) | Beta(6,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(2,1) | Beta(3,2) | (6,6) |
| `high_schedule_cost` | Beta(1,1) | Beta(1,1) | Beta(6,1) | Beta(1,1) | Beta(1,1) | Beta(2,1) | Beta(2,2) | (6,6) |
| `throughput_only` | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(2,1) | Beta(2,1) | Beta(1,3) | (50,1) |
| `latency_only` | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,2) | Beta(1,1) | Beta(3,1) | (1,50) |
| `balanced` | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(2,2) | (6,6) |
| `single_remote` | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,3) | Beta(1,1) | Beta(1,2) | Beta(2,2) | (6,6) |
| `many_remotes` | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(3,1) | Beta(1,1) | Beta(3,1) | Beta(3,1) | (6,6) |
| `burst_arrivals` | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(6,1) | Beta(2,1) | Beta(3,1) | (6,6) |
| `streaming_arrivals` | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,6) | Beta(2,1) | Beta(2,2) | (6,6) |
| `many_layers` | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(6,1) | Beta(1,1) | Beta(2,1) | Beta(1,1) | (6,6) |
| `single_layer` | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,6) | Beta(1,1) | Beta(1,1) | Beta(1,1) | (6,6) |
| `stress_scale` | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(6,1) | Beta(2,2) | (6,6) |
| `adversarial_mixed` | Beta(2,2) | Beta(2,2) | Beta(4,1) | Beta(1,4) | Beta(4,1) | Beta(3,1) | Beta(4,1) | (2,2) |
| `random` | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | Beta(1,1) | (1,1) |

Add rows for any remaining named profiles from the current `PROFILES` dict following the same
pattern: identify which mechanic the profile's *name* targets, skew that axis's Beta toward 1
(or 0, for an "anti-" profile), leave everything else at Beta(1,1).

---

## 10. Fixed Evaluation Order (breaks the circular dependency)

Several quantities are mutually referential on paper ($D_{\text{tok}}$ needs nothing external;
$T_{\text{tok}}$ needs $D_{\text{tok}}$; $S$ needs $D_{\text{tok}}$; $K$ needs only $\theta$;
$R_{\max\_group}$ needs $R, K, D_{\text{tok}}, T_{\text{tok}}, S, \overline{IAT}$; the table's
row placement needs $R_{\max\_group}$; but the table's *base values* were needed back in §4's
feasibility floor). Compute in exactly this order per test case:

1. Draw $\boldsymbol\theta \sim$ profile's Beta table (§1).
2. $D_{\text{tok}}$, curve exponents $p_\bullet$ (§2.1–2.2) — **table base values only**, not row placement yet.
3. $\rho_{\text{net}} \to T_{\text{tok}} \to$ latency/bandwidth/bytes-per-token (§3).
4. $S$, including the feasibility-floor check against $d_{\text{decode\_proc}}(1)$ from step 2 (§4).
5. $\textit{num\_layers}$, per-request $L_{\text{in}}$ distribution parameters (§5).
6. $R$ (§6.1), $\overline{IAT}$ (§6.4), $K$ (§6.5) — none of these need $R_{\max\_group}$.
7. $R_{\max\_group}$ (§6.3) — now everything it needs ($R,K,D_{\text{tok}},T_{\text{tok}},S,\overline{IAT}$) is available.
8. Task-time table **row placement** using $R_{\max\_group}$ (§2.3), then fill in all 6 columns with jitter $\zeta(m)$.
9. Generate concrete requests: $L_{\text{in}}[i]$ i.i.d. per §5, $L_{\text{out}}[i]$ per §6.2, arrival times per §6.4.
10. $TDR_{\text{phys}}, TPOT_{\text{phys}}$ (§7.1) using the *now-complete* table, then $q$ (§7.2), then SLO1/SLO2/$dist_{base}$/$tp_{base}$/$tp_{UB}$ (§7.3), then $w_{tp}$ (§7.4).
11. Post-hoc consistency check (§8) — reject-and-redraw jitter only, not $\boldsymbol\theta$, if any realized/target ratio falls outside $[\tfrac12,2]$.

---

## 11. Implementation Checklist

- [ ] `Beta(a,b)` sampler (Python: `random.betavariate(a, b)`).
- [ ] `LogU(a,b)` sampler: `math.exp(random.uniform(math.log(a), math.log(b)))`.
- [ ] Profile table replaced: 7 $(a_j,b_j)$ pairs + $(a_w,b_w)$ per profile, defaulting unlisted axes to $(1,1)$.
- [ ] Evaluation pipeline follows §10's fixed order exactly — do not reorder for convenience, the row-placement step genuinely needs $R_{\max\_group}$ computed first.
- [ ] All clip ranges applied *after* their formula, matching the problem's hard constraints: $S\in[1,10]$, latency $\in[0.001,50]$, bandwidth $\in[0.001,100]$, $K\in[1,8]$, $R\in[1,2000]$, $num\_layers\in[1,64]$, $L_{\text{in}}\in[1,4096]$, $L_{\text{out}}\in[1,512]$, $\sum L_{\text{out}}\le 2\times10^5$, table entries $\in[0.001,10^4]$.
- [ ] Feasibility floor re-draw loop (§4) — cap retries (e.g. 20) and fall back to clamping $\eta_4$ directly if it never converges, to guarantee termination.
- [ ] $tp_{UB}$ clip against $tp_{base}+\epsilon_{\text{margin}}\cdot tp_{\text{est}}$ (§7.3) — this is a correction versus the original draft; without it the throughput ceiling can drift arbitrarily far from anything achievable.
- [ ] Post-hoc consistency check (§8) as a bounded retry loop over jitter terms only.
- [ ] Keep the existing real-judge calibration / zero-score rejection loop unchanged — it is complementary, not replaced, by §8.
