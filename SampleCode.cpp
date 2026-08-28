#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <vector>
#include <map>
#include <iostream>
<<<<<<< Updated upstream
=======
#include <string>
>>>>>>> Stashed changes

using namespace std;

// ==============================================================================
<<<<<<< Updated upstream
// Configuration & Knobs
// ==============================================================================
static double envd(const char *n, double d) { const char *s = getenv(n); return s ? atof(s) : d; }
static int    envi(const char *n, int d)    { const char *s = getenv(n); return s ? atoi(s) : d; }

static int    KN_SPT, KN_CHUNK, KN_HOLD, KN_UPPRE_MAX, KN_UPPRE_MAX_TP;
static int    KN_WAVES_PROC, KN_LATHOLD, KN_CONS, KN_AGE_NORM;
static int    KN_CHUNK_PRED, KN_HOLD_ACT, KN_WAVE_CAPS_BATCH;
static double KN_CHUNK_SMULT, KN_WAVES, KN_HOLD_WFRAC, KN_HOLD_SMULT;
static double KN_UPGATE_FRAC, KN_RATE_EFF, KN_DECW, KN_LATFRAC, KN_CONS_PEN;
static double KN_CHUNK_MINS, KN_CHUNK_TPP, KN_BASE_W;
static double KN_B_DPOST, KN_B_PPOST, KN_B_DPRE, KN_B_PPRE, KN_B_DPROC, KN_B_PPROC;
static double B_DPOST, B_PPOST, B_DPRE, B_PPRE, B_DPROC, B_PPROC;
static double KN_AGE_FLOOR, KN_AGE_AW, KN_AGE_PRESS, KN_AGE_SLO_W;
static double KN_PPRE_AGECAP, KN_DECQ, KN_CHUNK_RATIO, KN_LAT_MULT;
static double KN_GATE_TDR, KN_HOLD_AW;

static void loadKnobs() {
    B_DPOST = KN_BASE_W * KN_B_DPOST;  B_PPOST = KN_BASE_W * KN_B_PPOST;
    B_DPRE  = KN_BASE_W * KN_B_DPRE;   B_PPRE  = KN_BASE_W * KN_B_PPRE;
    B_DPROC = KN_BASE_W * KN_B_DPROC;  B_PPROC = KN_BASE_W * KN_B_PPROC;
    KN_SPT            = envi("V4_SPT", 1);
    KN_CHUNK          = envi("V4_CHUNK", 0);
    KN_CHUNK_SMULT    = envd("V4_CHUNK_SMULT", 400.0);
    KN_HOLD           = envi("V4_HOLD", -1);
    KN_WAVES          = envd("V4_WAVES", 11.62305927);
    KN_HOLD_WFRAC     = envd("V4_HOLD_WFRAC", 1.0);
    KN_HOLD_SMULT     = envd("V4_HOLD_SMULT", 0.5);
    KN_UPGATE_FRAC    = envd("V4_UPGATE_FRAC", 0.9658870697);
    KN_UPPRE_MAX      = envi("V4_UPPRE_MAX", 2);
    KN_UPPRE_MAX_TP   = envi("V4_UPPRE_MAX_TP", 2);
    KN_RATE_EFF       = envd("V4_RATE_EFF", 0.1480865479);
    KN_DECW           = envd("V4_DECW", 4.0);
    KN_WAVES_PROC     = envi("V4_WAVES_PROC", 1);
    KN_LATHOLD        = envi("V4_LATHOLD", 1);
    KN_LATFRAC        = envd("V4_LATFRAC", 0.1282438934);
    KN_CONS           = envi("V4_CONS", 1);
    KN_CONS_PEN       = envd("V4_CONS_PEN", 1.0);
    KN_CHUNK_MINS     = envd("V4_CHUNK_MINS", 200.0);
    KN_CHUNK_TPP      = envd("V4_CHUNK_TPP", 0.06087353826);
    KN_BASE_W         = envd("V4_BASE_W", 462.743042);
    KN_B_DPOST        = envd("V4_B_DPOST", 8.0);
    KN_B_PPOST        = envd("V4_B_PPOST", 7.886381149);
    KN_B_DPRE         = envd("V4_B_DPRE", 0.0);
    KN_B_PPRE         = envd("V4_B_PPRE", 0.2446048111);
    KN_B_DPROC        = envd("V4_B_DPROC", 0.0);
    KN_B_PPROC        = envd("V4_B_PPROC", 7.45443821);
    KN_AGE_FLOOR      = envd("V4_AGE_FLOOR", 2.895564795);
    KN_AGE_AW         = envd("V4_AGE_AW", 0.3142893314);
    KN_AGE_PRESS      = envd("V4_AGE_PRESS", 0.0);
    KN_AGE_SLO_W      = envd("V4_AGE_SLO_W", 0.7868204713);
    KN_AGE_NORM       = envi("V4_AGE_NORM", 1);
    KN_PPRE_AGECAP    = envd("V4_PPRE_AGECAP", 9.999999959e+11);
    KN_DECQ           = envd("V4_DECQ", 0.1527011245);
    KN_CHUNK_RATIO    = envd("V4_CHUNK_RATIO", 0.02044871822);
    KN_CHUNK_PRED     = envi("V4_CHUNK_PRED", 0);
    KN_LAT_MULT       = envd("V4_LAT_MULT", 0.5145295858);
    KN_GATE_TDR       = envd("V4_GATE_TDR", 2.947692394);
    KN_HOLD_ACT       = envi("V4_HOLD_ACT", 1);
    KN_HOLD_AW        = envd("V4_HOLD_AW", 0.9263480306);
    KN_WAVE_CAPS_BATCH = envi("V4_WAVE_CAPS_BATCH", 0);
=======
// Tuning Knobs per Mathematics.md (§Algorithmic Workflow Parameters)
// ==============================================================================
struct TuningKnobs {
    double W1 = 6.806004019032381;  // weight of S in beta
    double W2 = 9.91138845650093;  // weight of normalized throughput weight in beta
    double W3 = 0.10888201654352386;  // weight of SLO2 in beta
    double B1 = 14.552964640809979;  // bias in beta
    double W4 = 1.6025810155559634;  // weight of SLO2 in tau
    double W5 = 1.9209793002118332;  // weight of latency in tau
    double B2 = 6.35782882757335;  // bias in tau
    double W6 = 16.125831260654476;  // weight of (L_in / 1000) in gamma
    double B3 = 4.201778407952444;  // bias in gamma
    double URG_SCALE = 16.116441930697253; // urgency multiplier
};

static TuningKnobs g_knobs;

static void parseTuningArgs(int argc = 0, char* argv[] = nullptr) {
    auto getEnvD = [](const char* name, double def) {
        const char* val = getenv(name);
        return val ? atof(val) : def;
    };
    auto envd = [](const char* name, double def) {
        const char* val = getenv(name);
        return val ? atof(val) : def;
    };
    g_knobs.W1 = getEnvD("W1", envd("V4_W1", 6.806004019032381));
    g_knobs.W2 = getEnvD("W2", envd("V4_W2", 9.91138845650093));
    g_knobs.W3 = getEnvD("W3", envd("V4_W3", 0.10888201654352386));
    g_knobs.B1 = getEnvD("B1", envd("V4_B1", 14.552964640809979));
    g_knobs.W4 = getEnvD("W4", envd("V4_W4", 1.6025810155559634));
    g_knobs.W5 = getEnvD("W5", envd("V4_W5", 1.9209793002118332));
    g_knobs.B2 = getEnvD("B2", envd("V4_B2", 6.35782882757335));
    g_knobs.W6 = getEnvD("W6", envd("V4_W6", 16.125831260654476));
    g_knobs.B3 = getEnvD("B3", envd("V4_B3", 4.201778407952444));
    g_knobs.URG_SCALE = getEnvD("URG_SCALE", envd("V4_URG_SCALE", 16.116441930697253));

    if (argv && argc >= 10) {
        g_knobs.W1 = atof(argv[1]);
        g_knobs.W2 = atof(argv[2]);
        g_knobs.W3 = atof(argv[3]);
        g_knobs.B1 = atof(argv[4]);
        g_knobs.W4 = atof(argv[5]);
        g_knobs.W5 = atof(argv[6]);
        g_knobs.B2 = atof(argv[7]);
        g_knobs.W6 = atof(argv[8]);
        g_knobs.B3 = atof(argv[9]);
        if (argc >= 11) g_knobs.URG_SCALE = atof(argv[10]);
    } else if (argv) {
        for (int i = 1; i + 1 < argc; i += 2) {
            string flag = argv[i];
            double val = atof(argv[i + 1]);
            if (flag == "--w1" || flag == "--W1") g_knobs.W1 = val;
            else if (flag == "--w2" || flag == "--W2") g_knobs.W2 = val;
            else if (flag == "--w3" || flag == "--W3") g_knobs.W3 = val;
            else if (flag == "--b1" || flag == "--B1") g_knobs.B1 = val;
            else if (flag == "--w4" || flag == "--W4") g_knobs.W4 = val;
            else if (flag == "--w5" || flag == "--W5") g_knobs.W5 = val;
            else if (flag == "--b2" || flag == "--B2") g_knobs.B2 = val;
            else if (flag == "--w6" || flag == "--W6") g_knobs.W6 = val;
            else if (flag == "--b3" || flag == "--B3") g_knobs.B3 = val;
            else if (flag == "--urg" || flag == "--URG_SCALE") g_knobs.URG_SCALE = val;
        }
    }
>>>>>>> Stashed changes
}

// ==============================================================================
// Core Structures (Tables, Optimizers, Rings)
// ==============================================================================
struct Table {
    vector<pair<double,double>> pts;
    void add(double bs, double v) { if (v >= 0.0) pts.push_back({bs, v}); }
    void finalize() { sort(pts.begin(), pts.end()); }
    double at(double bs) const {
        if (pts.empty()) return 0.0;
        if (bs <= pts.front().first) return pts.front().second;
        if (bs >= pts.back().first)  return pts.back().second;
        int lo = 0, hi = (int)pts.size() - 1;
        while (lo < hi) { int mid = (lo + hi) / 2; if (pts[mid].first < bs) lo = mid + 1; else hi = mid; }
        if (pts[lo].first == bs) return pts[lo].second;
        auto &p1 = pts[lo]; auto &p0 = pts[lo - 1];
        return p0.second + (bs - p0.first) / (p1.first - p0.first) * (p1.second - p0.second);
    }
};

struct RateOptimizer {
    const Table *tab = nullptr;
    double S = 0;
    vector<pair<double,double>> bp; 
    vector<int> prefixBestIdx;
    int gMinEff = 1;

    void build(const Table &t, double sVal) {
        tab = &t; S = sVal; bp.clear();
        for (auto &p : t.pts) bp.push_back({p.first, p.first / (S + p.second)});
        prefixBestIdx.assign(bp.size(), 0);
        int best = 0;
        double maxRate = 0;
        for (size_t i = 0; i < bp.size(); ++i) {
            if (bp[i].second > bp[best].second) best = (int)i;
            prefixBestIdx[i] = best;
            maxRate = max(maxRate, bp[i].second);
        }
        gMinEff = 1;
<<<<<<< Updated upstream
        for (auto &p : bp) { if (p.second >= KN_RATE_EFF * maxRate) { gMinEff = max(1, (int)p.first); break; } }
=======
        for (auto &p : bp) { if (p.second >= 0.15 * maxRate) { gMinEff = max(1, (int)p.first); break; } }
>>>>>>> Stashed changes
    }

    int bestSize(int n) const {
        if (n <= 1) return max(1, n);
        double bestRatio = -1; int bestG = n;
        if (!bp.empty()) {
            int lo = 0, hi = (int)bp.size() - 1, idx = -1;
            while (lo <= hi) { int mid = (lo + hi) / 2; if (bp[mid].first <= n) { idx = mid; lo = mid + 1; } else hi = mid - 1; }
            if (idx >= 0) { int bi = prefixBestIdx[idx]; bestRatio = bp[bi].second; bestG = (int)bp[bi].first; }
        }
        double ratioN = n / (S + tab->at((double)n));
        if (ratioN >= bestRatio) bestG = n;
        return min(max(bestG, 1), n);
    }

    double perItemCost() const {
        double bestRatio = 0;
        for (auto &pr : bp) bestRatio = max(bestRatio, pr.second);
        if (bestRatio <= 1e-12) return S + (tab && !tab->pts.empty() ? tab->pts.back().second : 0.0);
        return 1.0 / bestRatio;
    }
};

enum St : uint8_t {
    S_FIN = 0,
    PEND_PPRE, IN_PPRE, WAIT_UP_PRE,
    PEND_PROC, IN_PROC, PEND_PROC_RES, WAIT_DOWN_PRE,
    PEND_PPOST, IN_PPOST,
    PEND_DPRE, IN_DPRE, WAIT_UP_DEC,
    PEND_DPROC, IN_DPROC, WAIT_DOWN_DEC,
    PEND_DPOST, IN_DPOST
};

static const int MAXR = 2048 + 8;
static const int QCAP = 1 << 16;
static const int QMASK = QCAP - 1;

struct Ring {
    int32_t d[QCAP]; double ts[QCAP];
    int head = 0, tail = 0;
    inline void push(int32_t v, double t) { d[tail] = v; ts[tail] = t; tail = (tail + 1) & QMASK; }
    inline int32_t front() const { return d[head]; }
    inline double frontTs() const { return ts[head]; }
    inline void pop() { head = (head + 1) & QMASK; }
    inline bool empty() const { return head == tail; }
    inline int size() const { return (tail - head) & QMASK; }
};

// ==============================================================================
// Global Simulator State
// ==============================================================================
static St      st[MAXR];
static int8_t  cloudOf[MAXR];
static int32_t linOf[MAXR];
static double  arrOf[MAXR];
static int32_t layersDone[MAXR];
static double  fullProcDur[MAXR];
static int32_t tokCnt[MAXR];
static double  firstTok[MAXR], lastTokT[MAXR], curTpot[MAXR];

static Ring qPPOST, dpreReady, dpostReady, pendRing;
static multimap<int,int> pendByLin;
static Ring dprocReady[8], prefReadyRing[8];
static multimap<double,int> prefQ[8];

static bool   localFree = true;
static bool   remoteFree[8];
static double busyUntil[8];

static double upFreeAt = 0, downFreeAt = 0;
static int    upQLen = 0, downQLen = 0, prefUpQueued = 0;
static double latMs, bwGbps; static int bytesPerToken;

static double prefBacklogMs[8];
static int    activeDec[8];
static int    activeDecTotal = 0;
static int    decUpInflight[8];

static double tdrSum = 0; static long tdrCnt = 0;
static long   cntArr = 0, cntNotPPost = 0; static double sumArrNotPPost = 0;
static double sumTpotDone = 0; static long cntTpotDone = 0;
static double sumTpotAct = 0;  static long cntTpotAct = 0;
static double sumLoutDone = 0; static long cntLoutDone = 0;

static int K, numLayers;
static double S, wTp = 0.5, wC = 0.5, aw = 0.5;
static double SLO1 = 1, SLO2 = 1, tpUB, tpBase, distBase;
static bool slo1Free, slo2Free;
static double tdrPressure, tpotPressure; static bool cHopeless;
static double gProjTdr, gProjTpot;

static Table T_prefill_pre, T_prefill_proc, T_prefill_post;
static Table T_decode_pre, T_decode_proc, T_decode_post;
static RateOptimizer R_dpre, R_dproc, R_dpost;

static char outBuf[16][1 << 16];
static int32_t batchBuf[QCAP];
static int na = 0; // Actions generated in the current tick

// ==============================================================================
// Inline Link & Utility Models
// ==============================================================================
static inline double xferMs(double lenTokens) { return latMs + 8.0 * lenTokens * bytesPerToken / (bwGbps * 1e6); }
static inline void enqUp(double lenTokens, double t)   { upFreeAt   = max(upFreeAt, t)   + xferMs(lenTokens); upQLen++; }
static inline void enqDown(double lenTokens, double t) { downFreeAt = max(downFreeAt, t) + xferMs(lenTokens); downQLen++; }
static inline double avgLoutEst() { return cntLoutDone > 0 ? max(1.0, sumLoutDone / cntLoutDone) : 32.0; }
static inline int pendingEvents() {
    int busy = localFree ? 0 : 1;
    for (int k = 0; k < K; ++k) if (!remoteFree[k]) busy++;
    return busy + upQLen + downQLen;
}
static inline void trim(Ring &q, St want) { while (!q.empty() && st[q.front()] != want) q.pop(); }
static inline int drain(Ring &q, St want, int cap) {
    int n = 0;
    while (!q.empty() && n < cap) { 
        int32_t r = q.front(); q.pop(); 
        if (st[r] == want) batchBuf[n++] = r; 
    }
    return n;
}

// ==============================================================================
<<<<<<< Updated upstream
// Pressures & Heuristics
=======
// Pressures & Mathematical Helpers (§Algorithmic Workflow Parameters)
>>>>>>> Stashed changes
// ==============================================================================
static inline double projTdrMean(double t) { return (cntArr == 0) ? 0 : (tdrSum + (cntNotPPost * t - sumArrNotPPost)) / (double)cntArr; }
static inline double projTpotMean() { long c = cntTpotDone + cntTpotAct; return c ? (sumTpotDone + sumTpotAct) / (double)c : 0; }
static inline void recomputePressures(double t) {
    gProjTdr = projTdrMean(t); gProjTpot = projTpotMean();
    cHopeless = (distBase <= 0.0) && (gProjTdr > 2.0 * SLO1 || gProjTpot > 2.0 * SLO2);
    bool cOff = (wC <= 1e-12) || cHopeless;
<<<<<<< Updated upstream
    tdrPressure  = (cOff || slo1Free) ? 0.0 : min(1.5, gProjTdr / SLO1);
    tpotPressure = (cOff || slo2Free) ? 0.0 : min(1.5, gProjTpot / SLO2);
}
static inline double awEff() { return cHopeless ? 1.0 : aw; }
static inline bool latDominant() { return KN_LAT_MULT * latMs > (S + T_decode_proc.at((double)max(1, activeDecTotal))); }
static inline int cloudsInUse() { int u = 0; for (int k = 0; k < K; ++k) if (activeDec[k] > 0) u++; return max(1, u); }

static inline int bestCloudCount() {
    int A = max(4, activeDecTotal);
    int bestU = 1; double bestF = 1e300;
    for (int U = 1; U <= K; ++U) {
        double f = KN_LAT_MULT * latMs * U + S + T_decode_proc.at(ceil((double)A / U));
        if (f < bestF) { bestF = f; bestU = U; }
    }
    return bestU;
}

static inline double ageD(double w) {
    double r = w / SLO2; if (r > 5) r = 5 + log1p(r - 5);
    return (KN_AGE_NORM ? (w / SLO2) : w) * (KN_AGE_FLOOR + KN_AGE_AW * awEff() + KN_AGE_PRESS * tpotPressure) + KN_AGE_SLO_W * r;
}
static inline double ageP(double w) {
    double r = w / SLO1; if (r > 5) r = 5 + log1p(r - 5);
    return (KN_AGE_NORM ? (w / SLO1) : w) * (KN_AGE_FLOOR + KN_AGE_AW * (1.0 - awEff()) + KN_AGE_PRESS * tdrPressure) + KN_AGE_SLO_W * r;
=======
    tdrPressure  = (cOff || slo1Free) ? 0.0 : min(1.5, max(0.0, (gProjTdr - SLO1) / SLO1));
    tpotPressure = (cOff || slo2Free) ? 0.0 : min(1.5, max(0.0, (gProjTpot - SLO2) / SLO2));
}
static inline double awEff() { return cHopeless ? 1.0 : aw; }
static inline bool latDominant() { return 3.0 * latMs > (S + T_decode_proc.at((double)max(1, activeDecTotal))); }
static inline int cloudsInUse() { int u = 0; for (int k = 0; k < K; ++k) if (activeDec[k] > 0) u++; return max(1, u); }

static inline double ageD(double w) {
    double r = w / max(1e-6, SLO2);
    // Super-linear convex penalty: steep acceleration when latency exceeds SLO
    double penalty = (r > 1.0) ? (r + 2.0 * (r - 1.0) * (r - 1.0)) : r;
    return penalty * (1.0 + tpotPressure);
}
static inline double ageP(double w) {
    double r = w / max(1e-6, SLO1);
    // Super-linear convex penalty: steep acceleration when latency exceeds SLO
    double penalty = (r > 1.0) ? (r + 2.0 * (r - 1.0) * (r - 1.0)) : r;
    return penalty * (1.0 + tdrPressure);
>>>>>>> Stashed changes
}

static inline int oldestPend() { trim(pendRing, PEND_PPRE); return pendRing.empty() ? -1 : pendRing.front(); }
static inline int pickPPRE(double t) {
    int old = oldestPend(); if (old < 0) return -1;
<<<<<<< Updated upstream
    if (!KN_SPT || t - arrOf[old] > max(3.0 * SLO1, 200.0 * S)) return old;
=======
    if (t - arrOf[old] > max(3.0 * SLO1, 200.0 * S)) return old;
>>>>>>> Stashed changes
    while (!pendByLin.empty()) {
        auto it = pendByLin.begin();
        if (st[it->second] == PEND_PPRE) return it->second;
        pendByLin.erase(it);
    }
    return old;
}
static inline int pickPref(int k) {
    while (!prefQ[k].empty()) {
        auto it = prefQ[k].begin();
        int rid = it->second;
        if (st[rid] == PEND_PROC || st[rid] == PEND_PROC_RES) return rid;
        prefQ[k].erase(it);
    }
    return -1;
}

<<<<<<< Updated upstream
static inline bool holdActive() {
    if (KN_HOLD == 0) return false;
    if (KN_HOLD == 1) return true;
    if (KN_HOLD_ACT > 0 && activeDecTotal >= KN_HOLD_ACT) return true;
    return awEff() >= KN_HOLD_AW || (slo1Free && slo2Free);
}

static inline int waveCap(const RateOptimizer &R, int active) {
    if (KN_WAVES <= 0.0) return 1 << 28;
    int cap = (int)ceil((double)max(1, active) / KN_WAVES);
    return max(cap, min(R.gMinEff, max(1, active / 2)));
}

static inline bool holdSatisfied(double t, Ring &q, St want, int nReady, int target, bool isDpre) {
    if (isDpre && KN_LATHOLD && latDominant() && activeDecTotal >= 2) {
        if (nReady >= max(1, (int)(KN_LATFRAC * activeDecTotal))) return true;
        trim(q, want);
        if (!q.empty() && t - q.frontTs() >= max(slo2Free ? KN_LAT_MULT * latMs * cloudsInUse() : min(KN_LAT_MULT * latMs * cloudsInUse(), 0.5 * SLO2), 4.0 * S)) return true;
        return false;
    }
    if (!holdActive() || nReady >= target) return true;
    trim(q, want);
    if (!q.empty()) {
        double wcap = (slo2Free || awEff() > 0.9) ? KN_HOLD_SMULT * S : max(2.0 * S, min(KN_HOLD_WFRAC * SLO2, KN_HOLD_SMULT * S));
        if (t - q.frontTs() >= wcap) return true;
=======
// -----------------------------------------------------------------------------
// Dynamic Beta Batch Target & Tau Time-To-Live Functions (§Algorithmic Workflow)
// -----------------------------------------------------------------------------
static inline int computeBetaBatchTarget(int readyCount, const RateOptimizer& ro) {
    if (readyCount <= 1) return 1;

    int rBest = ro.bestSize(readyCount);
    if (rBest <= 1) return 1;

    double tp_ratio = wTp / (1.0 - wTp + 0.05);

    // Continuous formulation: when w_tp = 0, batching incentive is zero (beta = 1)
    // As w_tp increases, beta scales smoothly with schedule cost S and throughput demand
    double b_target = 1.0 + tp_ratio * max(0.0, g_knobs.W1 * S + g_knobs.W2 - g_knobs.W3 * min(100.0, SLO2) + g_knobs.B1);
    if (tpotPressure > 0.2) {
        b_target = max(1.0, b_target - 2.0 * tpotPressure);
    }

    int beta = max(1, (int)floor(b_target));
    beta = min(beta, rBest);

    // Physical feasibility check: do not batch so many that S + decode_proc(m) exceeds SLO2
    int maxFeasibleBatch = readyCount;
    for (int m = 1; m <= readyCount; m++) {
        double dproc = T_decode_proc.at((double)m);
        if (dproc > 0.0 && S + dproc > 0.95 * SLO2) {
            maxFeasibleBatch = max(1, m - 1);
            break;
        }
    }

    int target = min(readyCount, min(beta, maxFeasibleBatch));
    return max(1, target);
}

static inline double computeTauTimeToLive() {
    // Continuous formulation: when w_tp = 0, timeout tau = 0 (immediate dispatch, zero artificial delay)
    // As w_tp increases, tau scales smoothly with latency and deadline
    double tau = wTp * max(0.0, g_knobs.W4 * SLO2 + g_knobs.W5 * latMs + g_knobs.B2);
    if (tpotPressure > 0.1) {
        tau *= 0.5;
    }
    return max(0.0, tau);
}

static inline bool holdSatisfied(double t, Ring &q, St want, int nReady, int target, double tau) {
    if (nReady >= target) return true;
    trim(q, want);
    if (!q.empty()) {
        if (t - q.frontTs() >= tau) return true;
        return false;
>>>>>>> Stashed changes
    }
    return false;
}

static inline bool gateOK(double t) {
    if (activeDecTotal == 0) return true;
    int old = oldestPend();
<<<<<<< Updated upstream
    if (old >= 0 && t - arrOf[old] > KN_GATE_TDR * SLO1 && !slo1Free) return true;
    if (awEff() >= 0.7 || (slo2Free && slo1Free)) return prefUpQueued < KN_UPPRE_MAX_TP;
    if (prefUpQueued >= KN_UPPRE_MAX) return false;
    return max(0.0, upFreeAt - t) <= KN_UPGATE_FRAC * max(SLO2, 8.0 * latMs);
=======
    if (old >= 0 && t - arrOf[old] > 1.5 * SLO1 && !slo1Free) return true;
    if (awEff() >= 0.7 || (slo2Free && slo1Free)) return prefUpQueued < 16;
    if (prefUpQueued >= 8) return false;
    return max(0.0, upFreeAt - t) <= max(0.35 * SLO2, 4.0 * latMs);
>>>>>>> Stashed changes
}

static inline int bestRemote(double t) {
    double decCost = R_dproc.perItemCost();
<<<<<<< Updated upstream
    double consPen[8] = {0};
    
    if (KN_CONS && K > 1 && latDominant()) {
        int U = bestCloudCount();
        if (U < K) {
            int order[8]; for (int k = 0; k < K; ++k) order[k] = k;
            sort(order, order + K, [](int a, int b) {
                if (activeDec[a] != activeDec[b]) return activeDec[a] > activeDec[b];
                if (prefBacklogMs[a] != prefBacklogMs[b]) return prefBacklogMs[a] < prefBacklogMs[b];
                return a < b;
            });
            for (int i = U; i < K; ++i) consPen[order[i]] = KN_CONS_PEN * KN_LAT_MULT * latMs;
        }
    }

    double expectedDropTime[8];
    for (int k = 0; k < K; ++k) expectedDropTime[k] = prefBacklogMs[k] + max(0.0, busyUntil[k] - t);

    int best = 0; double bestSc = 1e300;
    for (int k = 0; k < K; ++k) {
        double decQ = KN_DECQ * (double)(dprocReady[k].size() + decUpInflight[k]) * (S + T_decode_proc.at(1.0));
        double netCollisionPenalty = 0.0;
        for (int j = 0; j < K; ++j) {
            if (k == j) continue;
            double diff = abs(expectedDropTime[k] - expectedDropTime[j]);
            double collisionWindow = latMs * 4.0; 
            if (diff < collisionWindow) netCollisionPenalty += (collisionWindow - diff) * 1.5; 
        }

        double sc = prefBacklogMs[k] + max(0.0, busyUntil[k] - t)
                  + KN_DECW * activeDec[k] * decCost * max(4.0, 0.5 * avgLoutEst())
                  + decQ + consPen[k] + netCollisionPenalty;
                  
=======
    int best = 0; double bestSc = 1e300;
    for (int k = 0; k < K; ++k) {
        double avail = max(0.0, busyUntil[k] - t);
        double decBacklog = (double)(dprocReady[k].size() + decUpInflight[k]) * (S + T_decode_proc.at(1.0));
        double sc = prefBacklogMs[k] + avail + 2.0 * (double)activeDec[k] * decCost + decBacklog;
>>>>>>> Stashed changes
        if (sc < bestSc) { bestSc = sc; best = k; }
    }
    return best;
}

static inline int pieceEnd(int rid, int k) {
    int ls = layersDone[rid], L = numLayers, lrem = L - ls;
<<<<<<< Updated upstream
    if (!KN_CHUNK || L <= 1 || lrem <= 1) return L;
    bool decodeBlocked = dprocReady[k].size() > 0 || decUpInflight[k] > 0 || (KN_CHUNK_PRED && activeDec[k] > 0);
    if (!decodeBlocked || slo2Free || wC <= 1e-12 || cHopeless) return L;
    
    double tpotRel = max(0.0, gProjTpot / SLO2 - 1.0);
    double tdrRel  = max(0.0, gProjTdr  / SLO1 - 1.0);
    if (gProjTpot < KN_CHUNK_TPP * SLO2 || tpotRel <= KN_CHUNK_RATIO * tdrRel) return L;
    
    double remaining = (double)lrem / L * fullProcDur[rid];
    if (remaining <= max(KN_CHUNK_MINS * S, 0.5 * SLO2)) return L;
    double G = max(KN_CHUNK_SMULT * S, 0.25 * SLO2);
    if (remaining <= 1.6 * G) return L;
    
=======
    if (L <= 1 || lrem <= 1) return L;

    // Only chunk if decode is actually waiting or active on this specific cloud
    bool decodeBlocked = dprocReady[k].size() > 0 || decUpInflight[k] > 0 || (activeDec[k] > 0);
    if (!decodeBlocked || wC <= 1e-12) return L;

    // If prefill delay (TDR) is worse than decode delay (TPOT), do not chunk prefill
    double tpotRel = max(0.0, gProjTpot / max(1e-6, SLO2) - 1.0);
    double tdrRel  = max(0.0, gProjTdr  / max(1e-6, SLO1) - 1.0);
    if (tpotRel <= tdrRel) return L;

    double remaining = (double)lrem / (double)L * fullProcDur[rid];
    if (remaining <= max(4.0 * S, 0.5 * SLO2)) return L;

    // Chunk size G based on mathematical knob formulation
    double G = max(g_knobs.W6 * S, 0.25 * SLO2);
    if (remaining <= 1.5 * G) return L;

>>>>>>> Stashed changes
    int p = max(1, min((int)llround((double)lrem * G / remaining), lrem));
    return ls + p;
}

// ==============================================================================
// Lookahead Engine 
// ==============================================================================
enum ActionType { A_DPOST, A_PPOST, A_DPRE, A_PPRE, A_WAIT };

struct Action {
    ActionType type; int cap; int target_rid; int target_cloud; double bid_score;
};

struct LocalSimState {
    double t, tdrSum; long tdrCnt, cntArr, cntNotPPost;
    double sumArrNotPPost, sumTpotAct, sumTpotDone;
    long cntTpotAct, cntTpotDone;
    double upFreeAt, downFreeAt;
};

double evaluate_sim_state(const LocalSimState& s) {
    double projTdr = (s.cntArr > 0) ? (s.tdrSum + (s.cntNotPPost * s.t - s.sumArrNotPPost)) / (double)s.cntArr : 0;
    long c = s.cntTpotDone + s.cntTpotAct;
    double projTpot = c ? (s.sumTpotDone + s.sumTpotAct) / (double)c : 0;
    double tdrExcess = max(0.0, (projTdr - SLO1) / SLO1);
    double tpotExcess = max(0.0, (projTpot - SLO2) / SLO2);
<<<<<<< Updated upstream
    return sqrt(tdrExcess * tdrExcess + tpotExcess * tpotExcess);
=======
    // Super-linear convex penalty: steep punishment on large deviations beyond SLO
    return (tdrExcess + 2.0 * tdrExcess * tdrExcess) + (tpotExcess + 2.0 * tpotExcess * tpotExcess);
>>>>>>> Stashed changes
}

double simulate_and_evaluate(LocalSimState s, const Action& a) {
    if (a.type == A_WAIT) return evaluate_sim_state(s);
    double dur = 0;
    if (a.type == A_DPOST) {
        dur = S + T_decode_post.at((double)a.cap); s.t += dur;
        s.cntTpotAct += a.cap; s.sumTpotAct += a.cap * (dur / SLO2); 
    } 
    else if (a.type == A_DPRE) {
        dur = S + T_decode_pre.at((double)a.cap); s.t += dur;
        s.upFreeAt = max(s.upFreeAt, s.t) + xferMs(1.0);
    } 
    else if (a.type == A_PPOST) {
        dur = S + T_prefill_post.at(1.0); s.t += dur;
        s.tdrSum += s.t - arrOf[a.target_rid]; s.tdrCnt++; s.cntNotPPost--; s.sumArrNotPPost -= arrOf[a.target_rid];
    } 
    else if (a.type == A_PPRE) {
        dur = S + T_prefill_pre.at(1.0); s.t += dur;
        s.upFreeAt = max(s.upFreeAt, s.t) + xferMs(linOf[a.target_rid]);
    }
<<<<<<< Updated upstream
    return evaluate_sim_state(s) - (0.00001 * a.bid_score); 
=======
    return evaluate_sim_state(s) - (0.001 * a.bid_score); 
>>>>>>> Stashed changes
}

// ==============================================================================
// Event Handlers
// ==============================================================================
void process_arrival(double t) {
    int rid, lin; cin >> rid >> lin;
    linOf[rid] = lin; arrOf[rid] = t;
    layersDone[rid] = 0; tokCnt[rid] = 0; curTpot[rid] = 0;
    fullProcDur[rid] = T_prefill_proc.at((double)lin);
    st[rid] = PEND_PPRE; pendRing.push(rid, t); pendByLin.insert({lin, rid});
    cntArr++; cntNotPPost++; sumArrNotPPost += t;
}

void process_transfer(double t) {
    char srv[16], s1[8], s2[8]; cin >> srv >> s1 >> s2;
    if (srv[0] == 'E') localFree = true;
    else { int k = srv[1] - '0'; if (k >= 0 && k < 8) remoteFree[k] = true; }

    if (s1[0] == 'P') {
        if (s2[1] == 'O') {
            int rem, rid; double dur; cin >> rem >> rid >> dur;
            if (st[rid] != S_FIN) {
                st[rid] = PEND_DPRE; dpreReady.push(rid, t);
                int k = cloudOf[rid]; activeDec[k]++; activeDecTotal++;
                tdrSum += t - arrOf[rid]; tdrCnt++;
                cntNotPPost--; sumArrNotPPost -= arrOf[rid];
            }
        } else if (s2[2] == 'E') {
            int rem, rid; double dur; cin >> rem >> rid >> dur;
            if (st[rid] != S_FIN) st[rid] = WAIT_UP_PRE;
            enqUp((double)linOf[rid], t); prefUpQueued++;
        } else {
            int ls, le, rem, rid; double dur; cin >> ls >> le >> rem >> rid >> dur;
            prefBacklogMs[rem] = max(0.0, prefBacklogMs[rem] - dur);
            if (st[rid] != S_FIN) {
                layersDone[rid] = le;
                if (le >= numLayers) { st[rid] = WAIT_DOWN_PRE; enqDown((double)linOf[rid], t); }
                else {
                    st[rid] = PEND_PROC_RES;
                    double remaining = (double)(numLayers - le) / numLayers * fullProcDur[rid];
                    prefQ[rem].insert({remaining, rid}); prefReadyRing[rem].push(rid, t);
                }
            }
        }
    } else if (s1[0] == 'D') {
        int hdr, m; cin >> hdr >> m;
        bool isPost = (s2[1] == 'O'), isPre = (!isPost && s2[2] == 'E');
        static int cloudCnt[8];
        if (isPre) for (int k = 0; k < 8; ++k) cloudCnt[k] = 0;
        for (int j = 0; j < m; ++j) {
            int rid; cin >> rid;
            if (isPre) cloudCnt[(int)cloudOf[rid]]++;
            if (st[rid] == S_FIN) continue;
            if (isPost) {
                tokCnt[rid]++;
                if (tokCnt[rid] == 1) firstTok[rid] = t;
                else {
                    double nt = (t - firstTok[rid]) / (tokCnt[rid] - 1);
                    if (tokCnt[rid] == 2) cntTpotAct++;
                    sumTpotAct += nt - curTpot[rid]; curTpot[rid] = nt;
                }
                lastTokT[rid] = t;
                st[rid] = PEND_DPRE; dpreReady.push(rid, t);
            }
            else if (isPre) st[rid] = WAIT_UP_DEC;
            else st[rid] = WAIT_DOWN_DEC;
        }
        double dur; cin >> dur;
        if (isPre) { for (int k = 0; k < K; ++k) if (cloudCnt[k] > 0) { enqUp((double)cloudCnt[k], t); decUpInflight[k] += cloudCnt[k]; } }
        else if (!isPost) enqDown((double)m, t);
    }
}

void process_cross(double t) {
    char dir[8], szS[24], step[8]; int rem, m;
    cin >> dir >> rem >> szS >> step >> m;
    bool up = (dir[0] == 'U'), prefill = (step[0] == 'P');
    if (up) upQLen--; else downQLen--;
    if (up && prefill) prefUpQueued--;
    if (up && !prefill && rem >= 0 && rem < 8) decUpInflight[rem] = max(0, decUpInflight[rem] - m);
    
    for (int j = 0; j < m; ++j) {
        int rid; cin >> rid;
        if (st[rid] == S_FIN) continue;
        int k = cloudOf[rid];
        if (up && prefill)       { st[rid] = PEND_PROC; prefQ[k].insert({fullProcDur[rid], rid}); prefReadyRing[k].push(rid, t); }
        else if (!up && prefill) { st[rid] = PEND_PPOST; qPPOST.push(rid, t); }
        else if (up)             { st[rid] = PEND_DPROC; dprocReady[k].push(rid, t); }
        else                     { st[rid] = PEND_DPOST; dpostReady.push(rid, t); }
    }
}

void process_finish(double t) {
    int rid; cin >> rid;
    if (st[rid] != S_FIN) {
        int k = cloudOf[rid];
        if (activeDec[k] > 0) activeDec[k]--;
        if (activeDecTotal > 0) activeDecTotal--;
        sumLoutDone += tokCnt[rid]; cntLoutDone++;
        if (tokCnt[rid] >= 2) { sumTpotAct -= curTpot[rid]; cntTpotAct--; sumTpotDone += curTpot[rid]; }
        cntTpotDone++; st[rid] = S_FIN;
    }
}

// ==============================================================================
// Core Engine Logics
// ==============================================================================
void run_local_engine(double t, bool force) {
    if (!localFree) return;
    trim(dpostReady, PEND_DPOST); trim(qPPOST, PEND_PPOST); trim(dpreReady, PEND_DPRE);

    int nDPost = dpostReady.size(), nDPre  = dpreReady.size();
    int ppost  = qPPOST.empty() ? -1 : qPPOST.front();
    int ppre   = pickPPRE(t), oldest = oldestPend();

<<<<<<< Updated upstream
    bool holdOn = holdActive();
    int dpreTarget  = holdOn ? waveCap(R_dpre, activeDecTotal)  : (1 << 28);
    int dpostTarget = holdOn ? waveCap(R_dpost, activeDecTotal) : (1 << 28);
    bool dpreGo  = nDPre > 0 && (force || holdSatisfied(t, dpreReady, PEND_DPRE, nDPre, dpreTarget, true));
    bool dpostGo = nDPost > 0 && (force || holdSatisfied(t, dpostReady, PEND_DPOST, nDPost, dpostTarget, false));
=======
    int betaTargetDPre = computeBetaBatchTarget(nDPre, R_dpre);
    int betaTargetDPost = computeBetaBatchTarget(nDPost, R_dpost);
    double tau = computeTauTimeToLive();

    // Fast-path immediate dispatch: If wait time of oldest item in dpreReady exceeds tau, execute immediately
    if (!dpreReady.empty() && (t - dpreReady.frontTs() >= tau)) {
        int cap = min(R_dpre.bestSize(nDPre), betaTargetDPre);
        int n = drain(dpreReady, PEND_DPRE, max(1, cap));
        if (n > 0) {
            int len = sprintf(outBuf[na], "E D PRE -1 %d", n);
            for (int j = 0; j < n; ++j) { len += sprintf(outBuf[na] + len, " %d", batchBuf[j]); st[batchBuf[j]] = IN_DPRE; }
            ++na; localFree = false;
            return;
        }
    }

    bool dpreGo  = nDPre > 0 && (force || holdSatisfied(t, dpreReady, PEND_DPRE, nDPre, betaTargetDPre, tau));
    bool dpostGo = nDPost > 0 && (force || holdSatisfied(t, dpostReady, PEND_DPOST, nDPost, betaTargetDPost, tau));
>>>>>>> Stashed changes
    bool ppreGo = ppre >= 0 && (force || gateOK(t));

    vector<Action> legal_moves;

    if (dpostGo) {
<<<<<<< Updated upstream
        int dpostCap = KN_WAVE_CAPS_BATCH ? min(R_dpost.bestSize(nDPost), dpostTarget) : R_dpost.bestSize(nDPost);
        legal_moves.push_back({A_DPOST, max(1, dpostCap), -1, -1, B_DPOST + ageD(t - dpostReady.frontTs())});
    }
    if (ppost >= 0) legal_moves.push_back({A_PPOST, 1, ppost, -1, B_PPOST + ageP(t - qPPOST.frontTs())});
    if (dpreGo) {
        int cap = ((KN_LATHOLD && latDominant()) || !KN_WAVE_CAPS_BATCH) ? R_dpre.bestSize(nDPre) : min(R_dpre.bestSize(nDPre), dpreTarget);
        legal_moves.push_back({A_DPRE, max(1, cap), -1, -1, B_DPRE + ageD(t - dpreReady.frontTs())});
    }
    if (ppreGo) {
        double ppreWait = min(t - arrOf[oldest >= 0 ? oldest : ppre], KN_PPRE_AGECAP);
        legal_moves.push_back({A_PPRE, 1, ppre, bestRemote(t), B_PPRE + ageP(ppreWait)});
=======
        int dpostCap = min(R_dpost.bestSize(nDPost), betaTargetDPost);
        legal_moves.push_back({A_DPOST, max(1, dpostCap), -1, -1, g_knobs.URG_SCALE * ageD(t - dpostReady.frontTs())});
    }
    if (ppost >= 0) {
        legal_moves.push_back({A_PPOST, 1, ppost, -1, g_knobs.URG_SCALE * ageP(t - qPPOST.frontTs())});
    }
    if (dpreGo) {
        int dpreCap = min(R_dpre.bestSize(nDPre), betaTargetDPre);
        legal_moves.push_back({A_DPRE, max(1, dpreCap), -1, -1, g_knobs.URG_SCALE * ageD(t - dpreReady.frontTs())});
    }
    if (ppreGo) {
        double ppreWait = min(t - arrOf[oldest >= 0 ? oldest : ppre], 318000.0);
        legal_moves.push_back({A_PPRE, 1, ppre, bestRemote(t), g_knobs.URG_SCALE * ageP(ppreWait)});
>>>>>>> Stashed changes
    }

    if (!legal_moves.empty()) {
        LocalSimState currentState = { t, tdrSum, tdrCnt, cntArr, cntNotPPost, sumArrNotPPost, sumTpotAct, sumTpotDone, cntTpotAct, cntTpotDone, upFreeAt, downFreeAt };
        Action bestMove = legal_moves[0]; double bestScore = 1e300;

        for (const Action& move : legal_moves) {
            double projected_penalty = simulate_and_evaluate(currentState, move);
            if (projected_penalty < bestScore) { bestScore = projected_penalty; bestMove = move; }
        }

        if (bestMove.type == A_DPOST) {
            int n = drain(dpostReady, PEND_DPOST, bestMove.cap);
            if (n > 0) {
                int len = sprintf(outBuf[na], "E D POST -1 %d", n);
                for (int j = 0; j < n; ++j) { len += sprintf(outBuf[na] + len, " %d", batchBuf[j]); st[batchBuf[j]] = IN_DPOST; }
                ++na; localFree = false;
            }
        } else if (bestMove.type == A_PPOST) {
            qPPOST.pop();
            if (st[bestMove.target_rid] == PEND_PPOST) {
                sprintf(outBuf[na++], "E P POST %d %d", (int)cloudOf[bestMove.target_rid], bestMove.target_rid);
                st[bestMove.target_rid] = IN_PPOST; localFree = false;
            }
        } else if (bestMove.type == A_DPRE) {
            int n = drain(dpreReady, PEND_DPRE, bestMove.cap);
            if (n > 0) {
                int len = sprintf(outBuf[na], "E D PRE -1 %d", n);
                for (int j = 0; j < n; ++j) { len += sprintf(outBuf[na] + len, " %d", batchBuf[j]); st[batchBuf[j]] = IN_DPRE; }
                ++na; localFree = false;
            }
        } else if (bestMove.type == A_PPRE) {
            if (st[bestMove.target_rid] == PEND_PPRE) {
                cloudOf[bestMove.target_rid] = (int8_t)bestMove.target_cloud;
                prefBacklogMs[bestMove.target_cloud] += fullProcDur[bestMove.target_rid];
                sprintf(outBuf[na++], "E P PRE %d %d", bestMove.target_cloud, bestMove.target_rid);
                st[bestMove.target_rid] = IN_PPRE; localFree = false;
            }
        }
    }
}

void run_remote_engines(double t) {
    int order[8];
    for (int k = 0; k < K; ++k) order[k] = k;
    
    sort(order, order + K, [](int a, int b) {
        int maxTokA = -1, maxTokB = -1;
        if (!dprocReady[a].empty()) maxTokA = tokCnt[dprocReady[a].front()];
        if (!dprocReady[b].empty()) maxTokB = tokCnt[dprocReady[b].front()];
        if (maxTokA != maxTokB) return maxTokA > maxTokB;
        return prefBacklogMs[a] > prefBacklogMs[b];
    });

    for (int i = 0; i < K; ++i) {
        int k = order[i];
        if (!remoteFree[k]) continue;
        trim(dprocReady[k], PEND_DPROC);
        while (!prefReadyRing[k].empty()) {
            St s = st[prefReadyRing[k].front()];
            if (s == PEND_PROC || s == PEND_PROC_RES) break;
            prefReadyRing[k].pop();
        }

        int nD = dprocReady[k].size(), pr = pickPref(k);
<<<<<<< Updated upstream
        double sD = nD ? B_DPROC + ageD(t - dprocReady[k].frontTs()) : -1e300;
        double sP = pr >= 0 ? B_PPROC + ageP(t - (prefReadyRing[k].empty() ? t : prefReadyRing[k].frontTs())) : -1e300;
=======
        double sD = nD ? g_knobs.URG_SCALE * ageD(t - dprocReady[k].frontTs()) : -1e300;
        double sP = pr >= 0 ? g_knobs.URG_SCALE * ageP(t - arrOf[pr]) : -1e300;
>>>>>>> Stashed changes
        
        if (sD <= -1e299 && sP <= -1e299) continue;

        if (sD >= sP) {
<<<<<<< Updated upstream
            int cap = R_dproc.bestSize(nD);
            if (KN_WAVES_PROC) cap = min(cap, max(waveCap(R_dproc, activeDec[k]), 1));
=======
            int maxFeasibleDProc = nD;
            for (int m = 1; m <= nD; m++) {
                double dproc = T_decode_proc.at((double)m);
                if (dproc > 0.0 && S + dproc > 0.95 * SLO2) {
                    maxFeasibleDProc = max(1, m - 1);
                    break;
                }
            }
            int cap = min(R_dproc.bestSize(nD), maxFeasibleDProc);
            if (wTp <= 0.05) cap = 1;
>>>>>>> Stashed changes
            int n = drain(dprocReady[k], PEND_DPROC, max(1, cap));
            if (n > 0) {
                int len = sprintf(outBuf[na], "C%d D PROC %d %d", k, k, n);
                for (int j = 0; j < n; ++j) { len += sprintf(outBuf[na] + len, " %d", batchBuf[j]); st[batchBuf[j]] = IN_DPROC; }
                ++na; remoteFree[k] = false;
                busyUntil[k] = t + S + T_decode_proc.at((double)n);
            }
        } else {
            prefQ[k].erase(prefQ[k].begin());
            if (pr >= 0 && st[pr] != S_FIN) {
                int ls = layersDone[pr], le = pieceEnd(pr, k);
                sprintf(outBuf[na++], "C%d P PROC %d %d %d %d", k, ls, le, k, pr);
                st[pr] = IN_PROC; remoteFree[k] = false;
                busyUntil[k] = t + S + (double)(le - ls) / numLayers * fullProcDur[pr];
            }
        }
    }
}

bool has_pending_work() {
    bool anyWork = !pendByLin.empty() || !dpreReady.empty() || !dpostReady.empty() || !qPPOST.empty();
    for (int k = 0; k < K && !anyWork; ++k) anyWork = !prefQ[k].empty() || !dprocReady[k].empty();
    return anyWork;
}

bool setup_initial_state() {
    if (!(cin >> K >> S >> latMs >> bwGbps >> bytesPerToken >> numLayers)) return false;
    K = max(1, min(8, K));
    cin >> SLO1 >> SLO2 >> tpUB >> tpBase >> distBase >> wTp >> wC;
    { double s = wTp + wC; aw = (s > 1e-12) ? wTp / s : 0.5; }
    if (SLO1 <= 0) SLO1 = 1e-6;
    if (SLO2 <= 0) SLO2 = 1e-6;
    slo1Free = SLO1 >= 1e8; slo2Free = SLO2 >= 1e8;

    int N; if (!(cin >> N)) return false;
    for (int i = 0; i < N; ++i) {
        double bs, a, b, c, d, e2, f;
        cin >> bs >> a >> b >> c >> d >> e2 >> f;
        T_prefill_pre.add(bs, a);  T_prefill_proc.add(bs, b); T_prefill_post.add(bs, c);
        T_decode_pre.add(bs, d);   T_decode_proc.add(bs, e2); T_decode_post.add(bs, f);
    }
    T_prefill_pre.finalize(); T_prefill_proc.finalize(); T_prefill_post.finalize();
    T_decode_pre.finalize();  T_decode_proc.finalize();  T_decode_post.finalize();
    R_dpre.build(T_decode_pre, S); R_dproc.build(T_decode_proc, S); R_dpost.build(T_decode_post, S);

    for (int i = 0; i < MAXR; ++i) st[i] = S_FIN;
    for (int k = 0; k < 8; ++k) { remoteFree[k] = true; busyUntil[k] = 0; prefBacklogMs[k] = 0; activeDec[k] = 0; }
    return true;
}

// ==============================================================================
// Main Loop
// ==============================================================================
<<<<<<< Updated upstream
int main() {
    ios_base::sync_with_stdio(false);
    cin.tie(nullptr);
    
    loadKnobs();
=======
int main(int argc, char* argv[]) {
    ios_base::sync_with_stdio(false);
    cin.tie(nullptr);
    
    parseTuningArgs(argc, argv);
>>>>>>> Stashed changes
    if (!setup_initial_state()) return 0;

    double t; int e;
    while (cin >> t >> e) {
        for (int i = 0; i < e; ++i) {
            char type[16]; cin >> type;
            if (type[0] == 'A')      process_arrival(t);
            else if (type[0] == 'T') process_transfer(t);
            else if (type[0] == 'X') process_cross(t);
            else if (type[0] == 'F') process_finish(t);
        }

        recomputePressures(t);
        na = 0; 
        
        for (int pass = 0; pass < 2; ++pass) {
            bool force = (pass == 1);
            run_local_engine(t, force);
            run_remote_engines(t);

            if (na > 0 || pendingEvents() > 0) break;
            if (!has_pending_work()) break;
        }

        printf("%d\n", na);
        for (int j = 0; j < na; ++j) puts(outBuf[j]);
        fflush(stdout);
    }
    return 0;
}