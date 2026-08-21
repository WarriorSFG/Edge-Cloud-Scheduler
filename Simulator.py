"""Part 2 -- interactor / judge for ./scheduler, plus the analytic system model.

Two independent things live here:

1. ``Table`` / ``link_ms`` / ``reference_schedule`` / ``ideal_throughput`` -- a
   pure-python model of the machine described in the problem statement. The
   generator uses it to *calibrate* tp_base / dist_base / tp_UB the way the real
   judge does (from a one-request-at-a-time reference schedule), so that scores
   land in the interesting part of the [0, 1000] range instead of being pinned
   at 0 or 1000 by arbitrary constants.

2. ``run_one`` -- a faithful interactor: it drives the scheduler over
   stdin/stdout exactly as the contest judge does, enforces every rule from
   "Mistakes That Give Zero Points on a Test", and scores the run with the
   verbatim "Scoring" formula.

Fidelity notes (each of these was wrong or missing in the first draft and each
one changes the verdict):
  * FIN is emitted in the *same frame* as the final D POST TDN. Delivering it a
    frame later makes any correct scheduler re-issue work for a finished
    request and take a spurious zero.
  * A transfer enters its FIFO link queue when the triggering task *completes*,
    not when it is assigned. Enqueueing at assignment time reorders the queue
    whenever a later-assigned task finishes earlier.
  * Task specs are echoed in canonical form (rebuilt from parsed fields), and
    responses are read as a token stream, since the statement allows arbitrary
    whitespace between tokens.
  * Reads are deadline-bounded with select(2): a scheduler that stops printing
    is reported as a timeout instead of hanging the tuner forever.

Self-check:  python3 Simulator.py --validate-sample
"""
from __future__ import annotations

import argparse
import heapq
import json
import math
import os
import select
import subprocess
import time
from bisect import bisect_left
from dataclasses import dataclass
from typing import Any, Iterable, Sequence

import Knobs

MAX_FRAMES = 2_000_000            # "At most 2e6 frames per test"
_POSIX = os.name == "posix"

# Column indices into a task-time-table row (row[0] is batch_size).
C_PRE_PRE, C_PRE_PROC, C_PRE_POST, C_DEC_PRE, C_DEC_PROC, C_DEC_POST = range(6)
STAGE_COL = {"P_PRE": C_PRE_PRE, "P_PROC": C_PRE_PROC, "P_POST": C_PRE_POST,
             "D_PRE": C_DEC_PRE, "D_PROC": C_DEC_PROC, "D_POST": C_DEC_POST}


# =============================================================== system model
class Table:
    """The task-time table: piecewise linear between listed batch sizes, flat
    outside the listed range, skipping -1 (missing) cells per column.

    Mirrors ``Table::at()`` in scheduler.cpp so the scheduler and the judge
    never disagree about a duration.
    """

    __slots__ = ("xs", "ys")

    def __init__(self, rows: Sequence[Sequence[float]]) -> None:
        self.xs: list[list[float]] = []
        self.ys: list[list[float]] = []
        for c in range(1, 7):
            pts = sorted((float(r[0]), float(r[c])) for r in rows if float(r[c]) >= 0.0)
            if not pts:
                raise ValueError(f"column {c} has no non-missing entry "
                                 "(problem statement guarantees at least one)")
            self.xs.append([p[0] for p in pts])
            self.ys.append([p[1] for p in pts])

    def at(self, col: int, b: float) -> float:
        xs, ys = self.xs[col], self.ys[col]
        if b <= xs[0]:
            return ys[0]
        if b >= xs[-1]:
            return ys[-1]
        i = bisect_left(xs, b)
        if xs[i] == b:
            return ys[i]
        x0, x1, y0, y1 = xs[i - 1], xs[i], ys[i - 1], ys[i]
        return y0 + (y1 - y0) * (b - x0) / (x1 - x0)

    def sizes(self) -> list[float]:
        out: set[float] = set()
        for xs in self.xs:
            out.update(xs)
        return sorted(out)


def link_ms(lat: float, bw_gbps: float, bpt: int, length: float) -> float:
    """Transfer time = latency + 8*data_bytes/(bandwidth_gbps * 1e6) ms."""
    return lat + 8.0 * length * bpt / (bw_gbps * 1e6)


def reference_schedule(case: dict) -> dict[str, float]:
    """The one-request-at-a-time reference schedule the scoring section refers to.

    Nothing overlaps: each request runs its full input stage and then all of its
    output steps at group size 1 before the next request starts. Returns the
    reference tp / tdr / tpot, which is exactly what tp_base and dist_base are
    defined from.
    """
    tbl = Table(case["table"]["rows"])
    S = case["S"]
    lat, bw, bpt = case["latency_in_ms"], case["bandwidth_gbps"], case["bytes_per_token"]
    reqs = case["requests"]
    if not reqs:
        return dict(tp=0.0, tdr=0.0, tpot=0.0, span=0.0, total_out=0)

    step = (3.0 * S + tbl.at(C_DEC_PRE, 1) + tbl.at(C_DEC_PROC, 1)
            + tbl.at(C_DEC_POST, 1) + 2.0 * link_ms(lat, bw, bpt, 1))

    now = 0.0
    tdr_sum = 0.0
    gap_sum = 0.0
    gap_n = 0
    total_out = 0
    last_tok = 0.0
    first_arr = min(r["arrival"] for r in reqs)
    for r in reqs:
        lin, lout = r["L_in"], r["L_out"]
        t = max(r["arrival"], now)
        t += S + tbl.at(C_PRE_PRE, lin)
        t += link_ms(lat, bw, bpt, lin)
        t += S + tbl.at(C_PRE_PROC, lin)
        t += link_ms(lat, bw, bpt, lin)
        t += S + tbl.at(C_PRE_POST, lin)
        tdr_sum += t - r["arrival"]
        t += step * lout
        if lout >= 2:
            gap_sum += step * (lout - 1)
            gap_n += lout - 1
        total_out += lout
        last_tok = t
        now = t

    span = max(last_tok - first_arr, 1e-12)
    return dict(tp=total_out / span,
                tdr=tdr_sum / len(reqs),
                tpot=(gap_sum / gap_n) if gap_n else 0.0,
                step=step, span=span, total_out=total_out)


def ideal_throughput(case: dict) -> float:
    """A work-conservation upper bound on achievable tokens/ms.

    For each candidate output group size g, no schedule can beat the busiest
    resource: the local computer must run every prefill PRE/POST plus one
    D PRE/D POST per wave, the K remotes share every P PROC plus one D PROC per
    wave, and each link direction carries every prefill transfer plus one
    per-wave decode transfer. The elapsed time also cannot be shorter than the
    arrival spread. Maximising over g gives a bound no scheduler can exceed --
    the right shape for tp_UB ("an estimated high rate").
    """
    tbl = Table(case["table"]["rows"])
    S, K = case["S"], case["K"]
    lat, bw, bpt = case["latency_in_ms"], case["bandwidth_gbps"], case["bytes_per_token"]
    reqs = case["requests"]
    if not reqs:
        return 1e-9

    total_out = sum(r["L_out"] for r in reqs)
    pre_local = sum(2.0 * S + tbl.at(C_PRE_PRE, r["L_in"]) + tbl.at(C_PRE_POST, r["L_in"])
                    for r in reqs)
    pre_remote = sum(S + tbl.at(C_PRE_PROC, r["L_in"]) for r in reqs)
    pre_link = sum(link_ms(lat, bw, bpt, r["L_in"]) for r in reqs)
    arr_span = max(r["arrival"] for r in reqs) - min(r["arrival"] for r in reqs)

    cand = {1.0, float(total_out)}
    cand.update(tbl.sizes())
    g = 2.0
    while g <= 4096.0:
        cand.add(g)
        g *= 2.0
    R = len(reqs)

    best = 0.0
    for g in sorted(cand):
        g = min(g, float(max(1, R)))          # a wave cannot exceed live requests
        if g < 1.0:
            continue
        waves = total_out / g
        local = pre_local + waves * (2.0 * S + tbl.at(C_DEC_PRE, g) + tbl.at(C_DEC_POST, g))
        remote = (pre_remote + waves * (S + tbl.at(C_DEC_PROC, g))) / K
        link = pre_link + waves * link_ms(lat, bw, bpt, g)
        elapsed = max(local, remote, link, arr_span, 1e-12)
        best = max(best, total_out / elapsed)
    return max(best, 1e-9)


# ==================================================================== scoring
@dataclass
class SimResult:
    ok: bool
    score: float
    tp: float = 0.0
    mean_tdr: float = 0.0
    mean_tpot: float = 0.0
    dist: float = 0.0
    norm_tp: float = 0.0
    norm_c: float = 0.0
    violation: str | None = None
    frames: int = 0
    wall_s: float = 0.0
    profile: str = ""
    total_out: int = 0
    span: float = 0.0


def _clamp01(x: float, base: float, target: float) -> float:
    if target == base:
        return 0.0
    return max(0.0, min(1.0, (x - base) / (target - base)))


def compute_score(case: dict, total_out: int, span: float,
                  tdrs: Sequence[float], gaps: Sequence[float]) -> SimResult:
    """Verbatim "Scoring" section formula -- no approximation."""
    tp = (total_out / span) if span > 0 else 0.0
    tdr = sum(tdrs) / len(tdrs) if tdrs else 0.0
    tpot = sum(gaps) / len(gaps) if gaps else 0.0
    ex_tdr = max(0.0, (tdr - case["SLO1"]) / case["SLO1"])
    ex_tpot = max(0.0, (tpot - case["SLO2"]) / case["SLO2"])
    dist = math.sqrt(ex_tdr ** 2 + ex_tpot ** 2)

    norm_tp = _clamp01(tp, case["tp_base"], case["tp_UB"])
    db = case["dist_base"]
    if db > 0:
        norm_c = max(0.0, min(1.0, 1.0 - dist / db))
    else:
        norm_c = 1.0 if dist == 0.0 else 0.0

    ns = case["w_tp"] * norm_tp + case["w_c"] * norm_c
    return SimResult(True, 1000.0 * ns, tp, tdr, tpot, dist, norm_tp, norm_c,
                     total_out=total_out, span=span, profile=case.get("profile", ""))


# ================================================================= child proc
class Violation(Exception):
    """Any rule from "Mistakes That Give Zero Points on a Test"."""


class _Child:
    """The scheduler process, with deadline-bounded token-wise reads.

    The statement lets a response put arbitrary whitespace between tokens, so
    the response is consumed as a token stream rather than as fixed lines.
    """

    def __init__(self, binary: str, env: dict[str, str], stderr_to: int | None = None) -> None:
        self.p = subprocess.Popen(
            [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=(subprocess.DEVNULL if stderr_to is None else stderr_to),
            env=env, bufsize=0)
        self.fd = self.p.stdout.fileno()          # type: ignore[union-attr]
        self._buf = b""
        self._tokens: list[bytes] = []
        self._ti = 0

    # -- writing -----------------------------------------------------------
    def write(self, text: str) -> None:
        assert self.p.stdin is not None
        try:
            self.p.stdin.write(text.encode())
            self.p.stdin.flush()
        except (BrokenPipeError, OSError):
            raise Violation("scheduler closed stdin (crashed or exited early)")

    # -- reading -----------------------------------------------------------
    def _readline(self, deadline: float) -> bytes:
        while True:
            nl = self._buf.find(b"\n")
            if nl >= 0:
                line, self._buf = self._buf[:nl], self._buf[nl + 1:]
                return line
            left = deadline - time.monotonic()
            if left <= 0:
                raise Violation("timeout waiting for scheduler response")
            if _POSIX:
                ready, _, _ = select.select([self.fd], [], [], min(left, 0.25))
                if not ready:
                    if self.p.poll() is not None:
                        raise Violation(
                            f"scheduler exited (rc={self.p.returncode}) mid-interaction")
                    continue
            chunk = os.read(self.fd, 1 << 16)
            if not chunk:
                if self._buf:
                    line, self._buf = self._buf, b""
                    return line
                raise Violation("scheduler closed stdout")
            self._buf += chunk

    def token(self, deadline: float) -> str:
        while self._ti >= len(self._tokens):
            self._tokens = self._readline(deadline).split()
            self._ti = 0
        tok = self._tokens[self._ti]
        self._ti += 1
        return tok.decode("ascii", "replace")

    def close(self) -> None:
        try:
            if self.p.stdin:
                self.p.stdin.close()
        except OSError:
            pass
        if self.p.poll() is None:
            self.p.kill()
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass


# =============================================================== judge state
class _Req:
    __slots__ = ("rid", "arrival", "L_in", "L_out", "remote", "stage", "in_flight",
                 "last_piece_end", "n_tokens", "last_tok", "p_post_time", "finished")

    def __init__(self, rid: int, arrival: float, L_in: int, L_out: int) -> None:
        self.rid = rid
        self.arrival = arrival
        self.L_in = L_in
        self.L_out = L_out
        self.remote: int | None = None
        self.stage = "ARR"            # not yet announced
        self.in_flight = False
        self.last_piece_end = 0
        self.n_tokens = 0
        self.last_tok = 0.0
        self.p_post_time: float | None = None
        self.finished = False


class Interactor:
    """Event-driven judge core: a timestamp-ordered heap of ARR / TDN / XDN,
    frame coalescing by identical timestamp, stuck-state detection and full
    legality checking of every assignment."""

    def __init__(self, case: dict) -> None:
        self.c = case
        self.K = int(case["K"])
        self.S = float(case["S"])
        self.layers = int(case["num_layers"])
        self.bpt = int(case["bytes_per_token"])
        self.lat = float(case["latency_in_ms"])
        self.bw = float(case["bandwidth_gbps"])
        self.tbl = Table(case["table"]["rows"])

        rows = sorted(case["requests"], key=lambda r: (r["arrival"], r["rid"]))
        if [r["rid"] for r in rows] != list(range(len(rows))):
            raise ValueError("request ids must be 0..R-1 in arrival order")
        self.reqs = [_Req(r["rid"], float(r["arrival"]), int(r["L_in"]), int(r["L_out"]))
                     for r in rows]
        self.R = len(self.reqs)
        self.remaining = self.R

        self.pq: list[tuple[float, int, tuple]] = []
        self.seq = 0
        for r in self.reqs:
            self._push(r.arrival, ("A", r.rid))

        self.t = 0.0
        self.busy: dict[str, float] = {}
        self.up_free = 0.0
        self.down_free = 0.0

        self.tdrs: list[float] = []
        self.gaps: list[float] = []
        self.total_out = 0
        self.first_arr = min(r.arrival for r in self.reqs)
        self.last_post = self.first_arr
        self.frames = 0
        self.transcript: list[str] | None = None

    # ------------------------------------------------------------- helpers
    def _push(self, t: float, ev: tuple) -> None:
        self.seq += 1
        heapq.heappush(self.pq, (t, self.seq, ev))

    def span(self) -> float:
        return max(1e-12, self.last_post - self.first_arr)

    def _enqueue(self, direction: str, t: float, length: int) -> float:
        """Single-server FIFO per direction; both directions run concurrently."""
        if direction == "UP":
            start = self.up_free if self.up_free > t else t
            self.up_free = start + link_ms(self.lat, self.bw, self.bpt, length)
            return self.up_free
        start = self.down_free if self.down_free > t else t
        self.down_free = start + link_ms(self.lat, self.bw, self.bpt, length)
        return self.down_free

    # -------------------------------------------------------------- frames
    def next_frame(self) -> str | None:
        """Build the next event frame, or None when every request has finished."""
        if self.remaining == 0:
            return None
        if not self.pq:
            raise Violation("stuck state: unfinished requests and no future event")
        self.frames += 1
        if self.frames > MAX_FRAMES:
            raise Violation(f"frame budget exceeded (> {MAX_FRAMES} frames)")

        t = self.pq[0][0]
        self.t = t
        lines: list[str] = []
        fins: list[int] = []
        while self.pq and self.pq[0][0] == t:
            lines.append(self._commit(heapq.heappop(self.pq)[2], fins))
        # "FIN appears beside the TDN of that request's final output final step"
        lines.extend(f"FIN {rid}" for rid in fins)

        frame = f"{t:.9f}\n{len(lines)}\n" + "".join(ln + "\n" for ln in lines)
        if self.transcript is not None:
            self.transcript.append(frame)
        return frame

    def _commit(self, ev: tuple, fins: list[int]) -> str:
        kind = ev[0]
        if kind == "A":
            r = self.reqs[ev[1]]
            r.stage = "P_PRE"
            return f"ARR {r.rid} {r.L_in}"

        if kind == "X":                       # ("X", spec, rids, next_stage)
            _, spec, rids, nxt = ev
            for rid in rids:
                r = self.reqs[rid]
                r.in_flight = False
                r.stage = nxt
            return f"XDN {spec}"

        # ("T", stage, computer, spec, rids, dur, piece_end)
        _, stage, computer, spec, rids, dur, piece_end = ev
        self.busy.pop(computer, None)
        t = self.t

        if stage == "P_PRE":
            rid = rids[0]
            r = self.reqs[rid]
            sz = r.L_in * self.bpt
            done = self._enqueue("UP", t, r.L_in)
            self._push(done, ("X", f"UP {r.remote} {sz} PRE 1 {rid}", rids, "P_PROC"))

        elif stage == "P_PROC":
            rid = rids[0]
            r = self.reqs[rid]
            if piece_end >= self.layers:
                sz = r.L_in * self.bpt
                done = self._enqueue("DOWN", t, r.L_in)
                self._push(done, ("X", f"DOWN {r.remote} {sz} PRE 1 {rid}", rids, "P_POST"))
            else:
                r.in_flight = False           # next piece may start immediately
                r.stage = "P_PROC"

        elif stage == "P_POST":
            r = self.reqs[rids[0]]
            r.in_flight = False
            r.stage = "D_PRE"
            if r.p_post_time is None:
                r.p_post_time = t
                self.tdrs.append(t - r.arrival)

        elif stage == "D_PRE":
            # One UP transfer per distinct remote, enqueued in increasing index.
            per_remote: dict[int, list[int]] = {}
            for rid in rids:
                per_remote.setdefault(self.reqs[rid].remote, []).append(rid)  # type: ignore[arg-type]
            for rm in sorted(per_remote):
                grp = per_remote[rm]
                n = len(grp)
                spec_x = f"UP {rm} {n * self.bpt} DEC {n} " + " ".join(map(str, grp))
                done = self._enqueue("UP", t, n)
                self._push(done, ("X", spec_x, tuple(grp), "D_PROC"))

        elif stage == "D_PROC":
            n = len(rids)
            rm = self.reqs[rids[0]].remote
            spec_x = f"DOWN {rm} {n * self.bpt} DEC {n} " + " ".join(map(str, rids))
            done = self._enqueue("DOWN", t, n)
            self._push(done, ("X", spec_x, rids, "D_POST"))

        elif stage == "D_POST":
            for rid in rids:
                r = self.reqs[rid]
                r.in_flight = False
                if r.n_tokens:
                    self.gaps.append(t - r.last_tok)
                r.n_tokens += 1
                r.last_tok = t
                self.total_out += 1
                self.last_post = t
                if r.n_tokens >= r.L_out:
                    r.finished = True
                    r.stage = "FIN"
                    self.remaining -= 1
                    fins.append(rid)
                else:
                    r.stage = "D_PRE"

        return f"TDN {computer} {spec} {dur:.9f}"

    # ------------------------------------------------------------ responses
    def read_response(self, child: _Child, deadline: float) -> list[dict]:
        head = child.token(deadline)
        try:
            n = int(head, 10)
        except ValueError:
            raise Violation(f"malformed count line: {head!r}")
        if n < 0:
            raise Violation("negative assignment count")
        if n > self.K + 1:
            raise Violation(f"response declares {n} assignments, limit is K+1={self.K + 1}")
        return [self._parse(child, deadline) for _ in range(n)]

    def _int(self, child: _Child, deadline: float, what: str) -> int:
        tok = child.token(deadline)
        try:
            return int(tok, 10)
        except ValueError:
            raise Violation(f"non-integer {what}: {tok!r}")

    def _parse(self, child: _Child, deadline: float) -> dict:
        server = child.token(deadline)
        if server == "E":
            computer = "E"
        elif server.startswith("C") and server[1:].isdigit():
            k = int(server[1:])
            if not 0 <= k < self.K:
                raise Violation(f"server {server} outside [C0, C{self.K - 1}]")
            computer = f"C{k}"
        else:
            raise Violation(f"bad server token: {server!r}")

        p = child.token(deadline)
        s = child.token(deadline)
        stage = f"{p}_{s}"
        if stage not in STAGE_COL:
            raise Violation(f"unknown task shape: {p} {s}")

        spec: dict[str, Any] = {"stage": stage, "computer": computer}

        if stage in ("P_PRE", "P_POST"):
            spec["remote"] = self._int(child, deadline, "remote")
            spec["rids"] = [self._int(child, deadline, "rid")]
            spec["spec"] = f"{p} {s} {spec['remote']} {spec['rids'][0]}"
        elif stage == "P_PROC":
            ls = self._int(child, deadline, "ls")
            le = self._int(child, deadline, "le")
            spec["piece"] = (ls, le)
            spec["remote"] = self._int(child, deadline, "remote")
            spec["rids"] = [self._int(child, deadline, "rid")]
            spec["spec"] = f"P PROC {ls} {le} {spec['remote']} {spec['rids'][0]}"
        else:
            marker = self._int(child, deadline, "group marker")
            m = self._int(child, deadline, "group size")
            if m < 1:
                raise Violation(f"malformed group: m={m}")
            if m > self.R:
                raise Violation(f"group size {m} exceeds the number of requests")
            rids = [self._int(child, deadline, "rid") for _ in range(m)]
            spec["rids"] = rids
            if stage in ("D_PRE", "D_POST"):
                if marker != -1:
                    raise Violation(f"{p} {s} must use the -1 marker, got {marker}")
                spec["remote"] = None
                spec["spec"] = f"{p} {s} -1 {m} " + " ".join(map(str, rids))
            else:
                spec["remote"] = marker
                spec["spec"] = f"D PROC {marker} {m} " + " ".join(map(str, rids))
        return spec

    # ----------------------------------------------------------- legality
    def apply(self, specs: Iterable[dict]) -> None:
        for s in specs:
            self._validate(s)
            self._schedule(s)

    def _validate(self, s: dict) -> None:
        cid = s["computer"]
        if cid in self.busy:
            raise Violation(f"computer {cid} is busy (free at {self.busy[cid]:.9f})")

        rids = s["rids"]
        if not rids:
            raise Violation("group must be non-empty")
        if len(set(rids)) != len(rids):
            raise Violation("duplicate request ids in group")

        stage = s["stage"]
        remote = s["remote"]

        # server / remote agreement
        if stage in ("P_PROC", "D_PROC"):
            if remote is None or not 0 <= remote < self.K:
                raise Violation(f"remote {remote} outside [0,{self.K})")
            if cid != f"C{remote}":
                raise Violation(f"{stage} on {cid} but names remote {remote}")
        elif stage == "P_PRE":
            if not 0 <= remote < self.K:
                raise Violation(f"P PRE remote {remote} outside [0,{self.K})")
            if cid != "E":
                raise Violation("P PRE must run on the local computer E")
        elif cid != "E":
            raise Violation(f"{stage} must run on the local computer E")

        for rid in rids:
            if not 0 <= rid < self.R:
                raise Violation(f"unknown rid {rid}")
            r = self.reqs[rid]
            if r.stage == "ARR":
                raise Violation(f"rid {rid} has not arrived yet")
            if r.finished:
                raise Violation(f"rid {rid} already finished (FIN)")
            if r.in_flight:
                raise Violation(f"rid {rid} is already part of an in-flight task")
            if r.stage != stage:
                raise Violation(f"rid {rid} is at {r.stage}, not ready for {stage}")
            if stage in ("P_PROC", "P_POST", "D_PROC") and r.remote != remote:
                raise Violation(
                    f"rid {rid} is assigned to remote {r.remote}, task names {remote}")

        if stage == "P_PROC":
            ls, le = s["piece"]
            r = self.reqs[rids[0]]
            if le <= ls:
                raise Violation(f"empty piece [{ls},{le})")
            if ls != r.last_piece_end:
                raise Violation(f"piece must start at {r.last_piece_end}, got {ls}")
            if le > self.layers:
                raise Violation(f"piece end {le} exceeds num_layers={self.layers}")

    def _schedule(self, s: dict) -> None:
        stage = s["stage"]
        rids = s["rids"]
        if stage.startswith("P"):
            b: float = self.reqs[rids[0]].L_in
        else:
            b = len(rids)
        dur = self.tbl.at(STAGE_COL[stage], b)

        piece_end = -1
        if stage == "P_PROC":
            ls, le = s["piece"]
            dur *= (le - ls) / self.layers
            piece_end = le

        for rid in rids:
            r = self.reqs[rid]
            r.in_flight = True
            if stage == "P_PRE":
                r.remote = s["remote"]
            elif stage == "P_PROC":
                r.last_piece_end = piece_end

        end = self.t + self.S + dur
        self.busy[s["computer"]] = end
        self._push(end, ("T", stage, s["computer"], s["spec"], tuple(rids), dur, piece_end))


# ==================================================================== runner
def _startup_text(c: dict) -> str:
    lines = [
        f'{c["K"]} {c["S"]:.9f} {c["latency_in_ms"]:.9f} '
        f'{c["bandwidth_gbps"]:.9f} {c["bytes_per_token"]} {c["num_layers"]}',
        f'{c["SLO1"]:.9f} {c["SLO2"]:.9f} {c["tp_UB"]:.9f} '
        f'{c["tp_base"]:.9f} {c["dist_base"]:.9f} {c["w_tp"]:.9f} {c["w_c"]:.9f}',
        str(c["table"]["N"]),
    ]
    for r in c["table"]["rows"]:
        cells = [str(int(r[0]))] + ["-1" if float(x) < 0 else f"{float(x):.9f}" for x in r[1:]]
        lines.append(" ".join(cells))
    return "".join(ln + "\n" for ln in lines)


def run_one(case: dict, env_overrides: dict[str, Any] | None = None,
            timeout_s: float = 60.0, binary: str = "./scheduler",
            keep_transcript: bool = False) -> SimResult:
    """Drive one full interaction and score it."""
    env = {**Knobs.strip_from_environ(), **Knobs.as_env(env_overrides)}
    t0 = time.monotonic()
    deadline = t0 + timeout_s
    profile = case.get("profile", "")

    try:
        state = Interactor(case)
    except (ValueError, KeyError) as exc:
        return SimResult(False, 0.0, violation=f"malformed case: {exc}", profile=profile)
    if keep_transcript:
        state.transcript = []

    try:
        child = _Child(binary, env)
    except FileNotFoundError:
        return SimResult(False, 0.0, violation=f"binary {binary!r} not found", profile=profile)
    except OSError as exc:
        return SimResult(False, 0.0, violation=f"cannot start {binary!r}: {exc}", profile=profile)

    try:
        child.write(_startup_text(case))
        while True:
            if time.monotonic() > deadline:
                raise Violation("timeout (wall clock)")
            frame = state.next_frame()
            if frame is None:
                break
            child.write(frame)
            state.apply(state.read_response(child, deadline))
        child.write("END\n")
        res = compute_score(case, state.total_out, state.span(), state.tdrs, state.gaps)
    except Violation as v:
        res = SimResult(False, 0.0, violation=str(v), profile=profile)
    except (BrokenPipeError, OSError) as v:
        res = SimResult(False, 0.0, violation=f"io error: {v}", profile=profile)
    finally:
        child.close()

    res.frames = state.frames
    res.wall_s = time.monotonic() - t0
    res.profile = profile
    if keep_transcript and state.transcript is not None:
        res_transcript = "".join(state.transcript)
        setattr(res, "transcript", res_transcript)
    return res


def run_many(cases: Sequence[dict], env_overrides: dict[str, Any] | None = None,
             timeout_s: float = 60.0, workers: int = 1,
             binary: str = "./scheduler") -> list[SimResult]:
    """Process-pool fan-out over cases (each run_one spawns its own scheduler)."""
    if workers <= 1 or len(cases) <= 1:
        return [run_one(c, env_overrides, timeout_s, binary) for c in cases]
    from concurrent.futures import ProcessPoolExecutor
    with ProcessPoolExecutor(max_workers=workers) as ex:
        futs = [ex.submit(run_one, c, env_overrides, timeout_s, binary) for c in cases]
        return [f.result() for f in futs]


# ======================================================== sample-trace checks
def _split_sample(path: str) -> tuple[str, list[str]]:
    with open(path) as f:
        content = f.read()
    if "Expected output 1:" not in content:
        raise ValueError("sample file has no 'Expected output 1:' delimiter")
    head, _, tail = content.partition("Expected output 1:")
    head = head.replace("Testcase 1:", "", 1).strip()
    return head, [ln.strip() for ln in tail.strip().splitlines() if ln.strip() != ""]


def case_from_transcript(trace: str, profile: str = "sample") -> dict:
    """Rebuild a runnable case dict from a judge transcript.

    L_out is recovered by counting each request's D POST TDNs, which is exactly
    the information the transcript reveals.
    """
    lines = [ln.strip() for ln in trace.strip().splitlines() if ln.strip()]
    K, S, lat, bw, bpt, layers = lines[0].split()
    slo1, slo2, tp_ub, tp_base, dist_base, w_tp, w_c = map(float, lines[1].split())
    n = int(lines[2])
    rows = [[float(x) for x in lines[3 + i].split()] for i in range(n)]

    arrivals: dict[int, tuple[float, int]] = {}
    tokens: dict[int, int] = {}
    t = 0.0
    i = 3 + n
    while i < len(lines):
        ln = lines[i]
        if ln == "END":
            break
        t = float(ln)
        e = int(lines[i + 1])
        for j in range(e):
            parts = lines[i + 2 + j].split()
            if parts[0] == "ARR":
                rid = int(parts[1])
                arrivals[rid] = (t, int(parts[2]))
                tokens.setdefault(rid, 0)
            elif parts[0] == "TDN" and parts[2] == "D" and parts[3] == "POST":
                m = int(parts[5])
                for rid in map(int, parts[6:6 + m]):
                    tokens[rid] = tokens.get(rid, 0) + 1
        i += 2 + e

    reqs = [dict(rid=rid, arrival=arrivals[rid][0], L_in=arrivals[rid][1],
                 L_out=max(1, tokens.get(rid, 1)))
            for rid in sorted(arrivals)]
    return dict(profile=profile, K=int(K), S=float(S), latency_in_ms=float(lat),
                bandwidth_gbps=float(bw), bytes_per_token=int(bpt),
                num_layers=int(layers), SLO1=slo1, SLO2=slo2, tp_UB=tp_ub,
                tp_base=tp_base, dist_base=dist_base, w_tp=w_tp, w_c=w_c,
                table=dict(N=n, rows=rows), requests=reqs)


def validate_sample(path: str = "Sample Testcase.txt", binary: str = "./scheduler",
                    expect_score: float | None = 500.0000027586,
                    verbose: bool = True) -> bool:
    """Two independent conformance checks against the real judge's own trace.

    (a) replay the fixed transcript and diff the scheduler's lines against
        "Expected output 1";
    (b) rebuild the case and run it through this interactor, checking that the
        score matches the judge's reported points for test #1.
    """
    ok = True
    try:
        trace, expected = _split_sample(path)
    except (OSError, ValueError) as exc:
        print(f"[sample] cannot read {path}: {exc}")
        return False

    # ---- (a) transcript replay -----------------------------------------
    env = {**Knobs.strip_from_environ(), **Knobs.as_env()}
    try:
        child = _Child(binary, env)
    except OSError as exc:
        print(f"[sample] cannot start {binary!r}: {exc}")
        return False
    try:
        child.write(trace + "\n")
        deadline = time.monotonic() + 15.0
        actual: list[str] = []
        try:
            for _ in expected:
                actual.append(" ".join(child._readline(deadline).decode().split()))
        except Violation as v:
            print(f"[sample] replay read failed: {v}")
            ok = False
    finally:
        child.close()

    if len(actual) != len(expected):
        print(f"[sample] line count: expected {len(expected)}, got {len(actual)}")
        ok = False
    for i, (exp, act) in enumerate(zip(expected, actual), 1):
        if " ".join(exp.split()) != act:
            print(f"[sample] mismatch at output line {i}: expected {exp!r}, got {act!r}")
            ok = False
    if ok and verbose:
        print(f"[sample] transcript replay: {len(actual)}/{len(expected)} lines match")

    # ---- (b) full interactor round-trip --------------------------------
    case = case_from_transcript(trace)
    res = run_one(case, {}, timeout_s=30.0, binary=binary)
    if not res.ok:
        print(f"[sample] interactor run failed: {res.violation}")
        return False
    if verbose:
        print(f"[sample] interactor: tp={res.tp:.9f} tdr={res.mean_tdr:.6f} "
              f"tpot={res.mean_tpot:.6f} dist={res.dist:.6f} "
              f"norm_tp={res.norm_tp:.9f} norm_c={res.norm_c:.6f} "
              f"score={res.score:.10f}")
    if expect_score is not None and abs(res.score - expect_score) > 1e-6:
        print(f"[sample] score {res.score:.10f} != judge's {expect_score:.10f}")
        ok = False
    elif verbose and expect_score is not None:
        print(f"[sample] score matches the judge's reported {expect_score} for test #1")

    ref = reference_schedule(case)
    if verbose:
        print(f"[sample] reference schedule: tp={ref['tp']:.9f} (test says "
              f"tp_base={case['tp_base']:.9f}) tdr={ref['tdr']:.6f} tpot={ref['tpot']:.6f}")
    print("[sample] OK" if ok else "[sample] FAILED")
    return ok


# ======================================================================= CLI
def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cases", default="cases.jsonl")
    ap.add_argument("--binary", default="./scheduler")
    ap.add_argument("--timeout_s", type=float, default=60.0)
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--env-file", help="knob values to apply (best_hparams.env)")
    ap.add_argument("--validate-sample", action="store_true")
    ap.add_argument("--sample", default="Sample Testcase.txt")
    ap.add_argument("--limit", type=int, default=0, help="only run the first N cases")
    a = ap.parse_args()

    if a.validate_sample:
        raise SystemExit(0 if validate_sample(a.sample, a.binary) else 1)

    overrides = Knobs.read_env_file(a.env_file) if a.env_file else {}
    with open(a.cases) as f:
        cs = [json.loads(ln) for ln in f if ln.strip()]
    if a.limit:
        cs = cs[:a.limit]
    results = run_many(cs, overrides, a.timeout_s, a.workers, a.binary)

    bad = 0
    for c, r in zip(cs, results):
        flag = "" if r.ok else f"  VIOLATION({r.violation})"
        bad += 0 if r.ok else 1
        print(f'{c["profile"]:18s} R={len(c["requests"]):5d} frames={r.frames:7d} '
              f'{r.wall_s:6.2f}s tp={r.tp:.6f} tdr={r.mean_tdr:.3f} '
              f'tpot={r.mean_tpot:.3f} dist={r.dist:.4f} '
              f'norm_tp={r.norm_tp:.4f} norm_c={r.norm_c:.4f} -> {r.score:8.2f}{flag}')
    scores = [r.score for r in results]
    print(f"\nmean score {sum(scores) / max(1, len(scores)):.2f} over {len(scores)} cases "
          f"({bad} violations, {sum(r.wall_s for r in results):.1f}s cpu)")


if __name__ == "__main__":
    main()