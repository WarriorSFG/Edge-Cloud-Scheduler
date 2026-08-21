#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <vector>
#include <map>
#include <iostream>

using namespace std;

// ------------------------------------------------------------------ knobs
static double envd(const char *n, double d) { const char *s = getenv(n); return s ? atof(s) : d; }
static int    envi(const char *n, int d)    { const char *s = getenv(n); return s ? atoi(s) : d; }

static int    KN_SPT;         // shortest-prefill-first admission + SRPT at remotes
static int    KN_CHUNK;       // split P PROC into pieces when decode is active
static double KN_CHUNK_SMULT; // piece duration target = CHUNK_SMULT * S
static int    KN_HOLD;        // hold a forming D PRE batch (bounded)
static double KN_WAVES;       // target number of in-flight decode waves
static double KN_HOLD_WFRAC;  // hold cap as fraction of SLO2
static double KN_HOLD_SMULT;  // hold cap in multiples of S
static double KN_UPGATE_FRAC; // uplink backlog gate for P PRE (SLO mode)
static int    KN_UPPRE_MAX;   // max in-flight prefill uploads (SLO mode)
static int    KN_UPPRE_MAX_TP;// max in-flight prefill uploads (tp mode)
static double KN_RATE_EFF;    // min fraction of best rate a wave-capped batch may drop to
static double KN_DECW;        // decode-load weight in remote assignment
static int    KN_WAVES_PROC;  // also wave-cap D PROC (default off)
static int    KN_LATHOLD;     // lockstep hold + big waves when link latency dominates
static double KN_LATFRAC;     // fraction of active decodes to gather per latency wave
static int    KN_CONS;        // consolidate assignments onto few clouds when latency dominates
static double KN_CONS_PEN;    // spill penalty (multiples of 2*lat) for out-of-set clouds
static double KN_CHUNK_MINS;  // only chunk prefills longer than this many S
static double KN_CHUNK_TPP;   // minimum tpot pressure before chunking engages

static void loadKnobs() {
    KN_SPT          = envi("V4_SPT", 1);
    KN_CHUNK        = envi("V4_CHUNK", 1);
    KN_CHUNK_SMULT  = envd("V4_CHUNK_SMULT", 20.0);
    KN_HOLD         = envi("V4_HOLD", -1);     // -1: auto (on iff throughput dominates)
    KN_WAVES        = envd("V4_WAVES", 3.0);   // wave target when a hold is active; <=0: drain fully
    KN_HOLD_WFRAC   = envd("V4_HOLD_WFRAC", 0.25);
    KN_HOLD_SMULT   = envd("V4_HOLD_SMULT", 10.0);
    KN_UPGATE_FRAC  = envd("V4_UPGATE_FRAC", 0.5);
    KN_UPPRE_MAX    = envi("V4_UPPRE_MAX", 3);
    KN_UPPRE_MAX_TP = envi("V4_UPPRE_MAX_TP", 6);
    KN_RATE_EFF     = envd("V4_RATE_EFF", 0.7);
    KN_DECW         = envd("V4_DECW", 1.0);
    KN_WAVES_PROC   = envi("V4_WAVES_PROC", 0);
    KN_LATHOLD      = envi("V4_LATHOLD", 1);
    KN_LATFRAC      = envd("V4_LATFRAC", 0.5);
    KN_CONS         = envi("V4_CONS", 1);
    KN_CONS_PEN     = envd("V4_CONS_PEN", 20.0);
    KN_CHUNK_MINS   = envd("V4_CHUNK_MINS", 30.0);
    KN_CHUNK_TPP    = envd("V4_CHUNK_TPP", 0.8);
}

// ------------------------------------------------------------------ tables
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
    vector<pair<double,double>> bp;  // (g, g/(S+T(g))) at each table breakpoint, g ascending
    vector<int> prefixBestIdx;
    int gMinEff = 1;                 // smallest g achieving RATE_EFF of the best rate

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
        for (auto &p : bp) { if (p.second >= KN_RATE_EFF * maxRate) { gMinEff = max(1, (int)p.first); break; } }
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

// ------------------------------------------------------------------ state
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

static St      st[MAXR];
static int8_t  cloudOf[MAXR];
static int32_t linOf[MAXR];
static double  arrOf[MAXR];
static int32_t layersDone[MAXR];   // prefill pieces completed up to this layer
static double  fullProcDur[MAXR];  // prefill_proc(Lin)
static int32_t tokCnt[MAXR];
static double  firstTok[MAXR], lastTokT[MAXR], curTpot[MAXR];

// local queues
static Ring qPPOST, dpreReady, dpostReady;
static Ring pendRing;                      // pending P PRE in arrival order (aging/starvation)
static multimap<int,int> pendByLin;        // pending P PRE keyed by Lin (SPT)
// per-remote queues
static Ring dprocReady[8];
static Ring prefReadyRing[8];              // prefill items ready at k, arrival order (aging)
static multimap<double,int> prefQ[8];      // start/resume queue keyed by REMAINING proc ms (SRPT)

static bool   localFree = true;
static bool   remoteFree[8];
static double busyUntil[8];

// link model (exact: transfers enqueue at TDN times we observe)
static double upFreeAt = 0, downFreeAt = 0;
static int    upQLen = 0, downQLen = 0;
static int    prefUpQueued = 0;            // prefill uploads currently in the UP queue
static double latMs, bwGbps; static int bytesPerToken;
static inline double xferMs(double lenTokens) { return latMs + 8.0 * lenTokens * bytesPerToken / (bwGbps * 1e6); }
static inline void enqUp(double lenTokens, double t)   { upFreeAt   = max(upFreeAt, t)   + xferMs(lenTokens); upQLen++; }
static inline void enqDown(double lenTokens, double t) { downFreeAt = max(downFreeAt, t) + xferMs(lenTokens); downQLen++; }

// load accounting
static double prefBacklogMs[8];             // remaining prefill proc ms assigned to k
static int    activeDec[8];                 // decode-phase (post P POST, pre FIN) requests on k
static int    activeDecTotal = 0;
static int    decUpInflight[8];             // decode members in UP transfers headed to k

// score bookkeeping (live projections)
static double tdrSum = 0; static long tdrCnt = 0;
static long   cntArr = 0, cntNotPPost = 0; static double sumArrNotPPost = 0;
static double sumTpotDone = 0; static long cntTpotDone = 0;
static double sumTpotAct = 0;  static long cntTpotAct = 0;
static double sumLoutDone = 0; static long cntLoutDone = 0;

static int K, numLayers;
static double S;
static double wTp = 0.5, wC = 0.5, aw = 0.5;
static double SLO1 = 1, SLO2 = 1, tpUB, tpBase, distBase;

static Table T_prefill_pre, T_prefill_proc, T_prefill_post;
static Table T_decode_pre, T_decode_proc, T_decode_post;
static RateOptimizer R_dpre, R_dproc, R_dpost;

static char outBuf[16][1 << 16];
static int32_t batchBuf[QCAP];

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
        int32_t r = q.front(); 
        q.pop(); 
        if (st[r] == want) {
            batchBuf[n++] = r; 
        }
    }
    return n;
}
// ------------------------------------------------------------------ pressures
static bool slo1Free, slo2Free;   // SLO so large it cannot be violated in practice
static inline double projTdrMean(double t) {
    if (cntArr == 0) return 0;
    return (tdrSum + (cntNotPPost * t - sumArrNotPPost)) / (double)cntArr;
}
static inline double projTpotMean() {
    long c = cntTpotDone + cntTpotAct;
    return c ? (sumTpotDone + sumTpotAct) / (double)c : 0;
}
static double tdrPressure, tpotPressure; static bool cHopeless;
static double gProjTdr, gProjTpot;
static inline void recomputePressures(double t) {
    gProjTdr = projTdrMean(t); gProjTpot = projTpotMean();
    cHopeless = (distBase <= 0.0) && (gProjTdr > 2.0 * SLO1 || gProjTpot > 2.0 * SLO2);
    bool cOff = (wC <= 1e-12) || cHopeless;
    tdrPressure  = (cOff || slo1Free) ? 0.0 : min(1.5, gProjTdr / SLO1);
    tpotPressure = (cOff || slo2Free) ? 0.0 : min(1.5, gProjTpot / SLO2);
}
static inline double awEff() { return cHopeless ? 1.0 : aw; }

// --- link-latency dominance ------------------------------------------------
static inline bool latDominant() {
    int A = max(1, activeDecTotal);
    return 2.0 * latMs > (S + T_decode_proc.at((double)A));
}
static inline int cloudsInUse() {
    int u = 0;
    for (int k = 0; k < K; ++k) if (activeDec[k] > 0) u++;
    return max(1, u);
}
static inline int bestCloudCount() {
    int A = max(4, activeDecTotal);
    int bestU = 1; double bestF = 1e300;
    for (int U = 1; U <= K; ++U) {
        double f = 2.0 * latMs * U + S + T_decode_proc.at(ceil((double)A / U));
        if (f < bestF) { bestF = f; bestU = U; }
    }
    return bestU;
}

static inline double ageD(double w) {
    double r = w / SLO2; if (r > 5) r = 5 + log1p(r - 5);
    return w * (0.5 + awEff() + tpotPressure) + 3.0 * r;
}
static inline double ageP(double w) {
    double r = w / SLO1; if (r > 5) r = 5 + log1p(r - 5);
    return w * (0.5 + (1.0 - awEff()) + tdrPressure) + 3.0 * r;
}
static const double B_DPOST = 3.2, B_PPOST = 2.4, B_DPRE = 2.0, B_PPRE = 1.2, B_DPROC = 2.0, B_PPROC = 1.2;

// ------------------------------------------------------------------ helpers
static inline int oldestPend() { trim(pendRing, PEND_PPRE); return pendRing.empty() ? -1 : pendRing.front(); }
static inline int pickPPRE(double t) {
    int old = oldestPend(); if (old < 0) return -1;
    if (!KN_SPT) return old;
    if (t - arrOf[old] > max(3.0 * SLO1, 200.0 * S)) return old;
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

static inline bool holdActive() {
    if (KN_HOLD == 0) return false;
    if (KN_HOLD == 1) return true;
    return awEff() >= 0.75 || (slo1Free && slo2Free);
}

static inline int waveCap(const RateOptimizer &R, int active) {
    if (KN_WAVES <= 0.0) return 1 << 28;
    int cap = (int)ceil((double)max(1, active) / KN_WAVES);
    int gme = min(R.gMinEff, max(1, active / 2));
    return max(cap, gme);
}

static inline bool holdSatisfied(double t, Ring &q, St want, int nReady, int target, bool isDpre) {
    bool latDom = isDpre && KN_LATHOLD && latDominant() && activeDecTotal >= 2;
    if (latDom) {
        int latTarget = max(1, (int)(KN_LATFRAC * activeDecTotal));
        if (nReady >= latTarget) return true;
        trim(q, want);
        if (!q.empty()) {
            double wcap = 2.0 * latMs * cloudsInUse();
            if (!slo2Free) wcap = min(wcap, 0.5 * SLO2);
            wcap = max(wcap, 4.0 * S);
            if (t - q.frontTs() >= wcap) return true;
        }
        return false;
    }
    if (!holdActive()) return true;
    if (nReady >= target) return true;
    trim(q, want);
    if (!q.empty()) {
        double wcap = max(2.0 * S, min(KN_HOLD_WFRAC * SLO2, KN_HOLD_SMULT * S));
        if (slo2Free || awEff() > 0.9) wcap = KN_HOLD_SMULT * S;
        if (t - q.frontTs() >= wcap) return true;
    }
    return false;
}

static inline bool gateOK(double t) {
    if (activeDecTotal == 0) return true;
    int old = oldestPend();
    if (old >= 0 && t - arrOf[old] > 0.3 * SLO1 && !slo1Free) return true;
    double backlog = max(0.0, upFreeAt - t);
    bool tpMode = (awEff() >= 0.7) || (slo2Free && slo1Free);
    if (tpMode) return prefUpQueued < KN_UPPRE_MAX_TP;
    if (prefUpQueued >= KN_UPPRE_MAX) return false;
    return backlog <= KN_UPGATE_FRAC * max(SLO2, 8.0 * latMs);
}

static inline int bestRemote(double t) {
    double decCost = R_dproc.perItemCost();
    double consPen[8] = {0,0,0,0,0,0,0,0};
    if (KN_CONS && K > 1 && latDominant()) {
        int U = bestCloudCount();
        if (U < K) {
            int order[8];
            for (int k = 0; k < K; ++k) order[k] = k;
            sort(order, order + K, [](int a, int b) {
                if (activeDec[a] != activeDec[b]) return activeDec[a] > activeDec[b];
                if (prefBacklogMs[a] != prefBacklogMs[b]) return prefBacklogMs[a] < prefBacklogMs[b];
                return a < b;
            });
            for (int i = U; i < K; ++i) consPen[order[i]] = KN_CONS_PEN * 2.0 * latMs;
        }
    }
    int best = 0; double bestSc = 1e300;
    for (int k = 0; k < K; ++k) {
        double sc = prefBacklogMs[k] + max(0.0, busyUntil[k] - t)
                  + KN_DECW * activeDec[k] * decCost * max(4.0, 0.5 * avgLoutEst())
                  + consPen[k];
        if (sc < bestSc) { bestSc = sc; best = k; }
    }
    return best;
}

static inline int pieceEnd(int rid, int k) {
    int ls = layersDone[rid], L = numLayers, lrem = L - ls;
    if (!KN_CHUNK || L <= 1 || lrem <= 1) return L;
    bool decodeBlocked = dprocReady[k].size() > 0 || decUpInflight[k] > 0;
    if (!decodeBlocked) return L;
    if (slo2Free || wC <= 1e-12 || cHopeless) return L;
    double tpotRel = max(0.0, gProjTpot / SLO2 - 1.0);
    double tdrRel  = max(0.0, gProjTdr  / SLO1 - 1.0);
    if (gProjTpot < KN_CHUNK_TPP * SLO2) return L;
    if (tpotRel <= 2.0 * tdrRel) return L;
    double remaining = (double)lrem / L * fullProcDur[rid];
    if (remaining <= max(KN_CHUNK_MINS * S, 0.5 * SLO2)) return L;
    double G = max(KN_CHUNK_SMULT * S, 0.25 * SLO2);
    if (remaining <= 1.6 * G) return L;
    int p = (int)llround((double)lrem * G / remaining);
    p = max(1, min(p, lrem));
    return ls + p;
}

// ------------------------------------------------------------------ main
int main() {
    ios_base::sync_with_stdio(false);
    cin.tie(nullptr);
    loadKnobs();

    if (!(cin >> K >> S >> latMs >> bwGbps >> bytesPerToken >> numLayers)) return 0;
    K = max(1, min(8, K));
    cin >> SLO1 >> SLO2 >> tpUB >> tpBase >> distBase >> wTp >> wC;
    { double s = wTp + wC; aw = (s > 1e-12) ? wTp / s : 0.5; }
    if (SLO1 <= 0) SLO1 = 1e-6;
    if (SLO2 <= 0) SLO2 = 1e-6;
    slo1Free = SLO1 >= 1e8;
    slo2Free = SLO2 >= 1e8;

    int N; if (!(cin >> N)) return 0;
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

    double t; int e;
    while (cin >> t >> e) {
        for (int i = 0; i < e; ++i) {
            char type[16]; cin >> type;

            if (type[0] == 'A') {                            // ARR rid Lin
                int rid, lin; cin >> rid >> lin;
                linOf[rid] = lin; arrOf[rid] = t;
                layersDone[rid] = 0; tokCnt[rid] = 0; curTpot[rid] = 0;
                fullProcDur[rid] = T_prefill_proc.at((double)lin);
                st[rid] = PEND_PPRE;
                pendRing.push(rid, t);
                pendByLin.insert({lin, rid});
                cntArr++; cntNotPPost++; sumArrNotPPost += t;

            } else if (type[0] == 'T') {                            // TDN
                char srv[16], s1[8], s2[8]; cin >> srv >> s1 >> s2;
                if (srv[0] == 'E') localFree = true;
                else { int k = srv[1] - '0'; if (k >= 0 && k < 8) remoteFree[k] = true; }

                if (s1[0] == 'P') {
                    if (s2[1] == 'O') {                             // P POST rem rid dur
                        int rem, rid; double dur; cin >> rem >> rid >> dur;
                        if (st[rid] != S_FIN) {
                            st[rid] = PEND_DPRE; dpreReady.push(rid, t);
                            int k = cloudOf[rid]; activeDec[k]++; activeDecTotal++;
                            tdrSum += t - arrOf[rid]; tdrCnt++;
                            cntNotPPost--; sumArrNotPPost -= arrOf[rid];
                        }
                    } else if (s2[2] == 'E') {                      // P PRE rem rid dur
                        int rem, rid; double dur; cin >> rem >> rid >> dur;
                        if (st[rid] != S_FIN) st[rid] = WAIT_UP_PRE;
                        enqUp((double)linOf[rid], t); prefUpQueued++;
                    } else {                                        // P PROC ls le rem rid dur
                        int ls, le, rem, rid; double dur; cin >> ls >> le >> rem >> rid >> dur;
                        prefBacklogMs[rem] -= dur; if (prefBacklogMs[rem] < 0) prefBacklogMs[rem] = 0;
                        if (st[rid] != S_FIN) {
                            layersDone[rid] = le;
                            if (le >= numLayers) { st[rid] = WAIT_DOWN_PRE; enqDown((double)linOf[rid], t); }
                            else {
                                st[rid] = PEND_PROC_RES;
                                double remaining = (double)(numLayers - le) / numLayers * fullProcDur[rid];
                                prefQ[rem].insert({remaining, rid});
                                prefReadyRing[rem].push(rid, t);
                            }
                        }
                    }
                } else if (s1[0] == 'D') {
                    int hdr, m; cin >> hdr >> m;
                    bool isPost = (s2[1] == 'O');
                    bool isPre  = (!isPost && s2[2] == 'E');
                    static int cloudCnt[8];
                    if (isPre) for (int k = 0; k < 8; ++k) cloudCnt[k] = 0;
                    for (int j = 0; j < m; ++j) {
                        int rid; cin >> rid;
                        if (isPre) cloudCnt[(int)cloudOf[rid]]++;
                        if (st[rid] == S_FIN) continue;
                        if (isPost) {                               // one token
                            tokCnt[rid]++;
                            if (tokCnt[rid] == 1) firstTok[rid] = t;
                            else {
                                double nt = (t - firstTok[rid]) / (tokCnt[rid] - 1);
                                if (tokCnt[rid] == 2) cntTpotAct++;
                                sumTpotAct += nt - curTpot[rid]; curTpot[rid] = nt;
                            }
                            lastTokT[rid] = t;
                        }
                        if (isPost) { st[rid] = PEND_DPRE; dpreReady.push(rid, t); }
                        else if (isPre) st[rid] = WAIT_UP_DEC;
                        else st[rid] = WAIT_DOWN_DEC;
                    }
                    double dur; cin >> dur;
                    if (isPre)      { for (int k = 0; k < K; ++k) if (cloudCnt[k] > 0) { enqUp((double)cloudCnt[k], t); decUpInflight[k] += cloudCnt[k]; } }
                    else if (!isPost) enqDown((double)m, t);        // D PROC
                }

            } else if (type[0] == 'X') {                            // XDN dir rem size PRE|DEC m rids
                char dir[8], szS[24], step[8]; int rem, m;
                cin >> dir >> rem >> szS >> step >> m;
                bool up = (dir[0] == 'U');
                bool prefill = (step[0] == 'P');
                if (up) upQLen--; else downQLen--;
                if (up && prefill) prefUpQueued--;
                if (up && !prefill && rem >= 0 && rem < 8) { decUpInflight[rem] -= m; if (decUpInflight[rem] < 0) decUpInflight[rem] = 0; }
                for (int j = 0; j < m; ++j) {
                    int rid; cin >> rid;
                    if (st[rid] == S_FIN) continue;
                    int k = cloudOf[rid];
                    if (up && prefill)       { st[rid] = PEND_PROC; prefQ[k].insert({fullProcDur[rid], rid}); prefReadyRing[k].push(rid, t); }
                    else if (!up && prefill) { st[rid] = PEND_PPOST; qPPOST.push(rid, t); }
                    else if (up)             { st[rid] = PEND_DPROC; dprocReady[k].push(rid, t); }
                    else                     { st[rid] = PEND_DPOST; dpostReady.push(rid, t); }
                }

            } else if (type[0] == 'F') {                            // FIN rid
                int rid; cin >> rid;
                if (st[rid] != S_FIN) {
                    int k = cloudOf[rid];
                    if (activeDec[k] > 0) activeDec[k]--;
                    if (activeDecTotal > 0) activeDecTotal--;
                    sumLoutDone += tokCnt[rid]; cntLoutDone++;
                    if (tokCnt[rid] >= 2) { sumTpotAct -= curTpot[rid]; cntTpotAct--; sumTpotDone += curTpot[rid]; }
                    cntTpotDone++;
                    st[rid] = S_FIN;
                }
            }
        }

        recomputePressures(t);

        int na = 0;
        for (int pass = 0; pass < 2; ++pass) {
            bool force = (pass == 1);

            // ---------------- local engine ----------------
            if (localFree) {
                trim(dpostReady, PEND_DPOST);
                trim(qPPOST, PEND_PPOST);
                trim(dpreReady, PEND_DPRE);

                int nDPost = dpostReady.size();
                int nDPre  = dpreReady.size();
                int ppost  = qPPOST.empty() ? -1 : qPPOST.front();
                int ppre   = pickPPRE(t);
                int oldest = oldestPend();

                bool holdOn = holdActive();
                int dpreTarget  = holdOn ? waveCap(R_dpre, activeDecTotal)  : (1 << 28);
                int dpostTarget = holdOn ? waveCap(R_dpost, activeDecTotal) : (1 << 28);
                bool dpreGo  = nDPre > 0 && (force || holdSatisfied(t, dpreReady, PEND_DPRE, nDPre, dpreTarget, true));
                bool dpostGo = nDPost > 0 && (force || holdSatisfied(t, dpostReady, PEND_DPOST, nDPost, dpostTarget, false));
                bool ppreGo = ppre >= 0 && (force || gateOK(t));

                double sDPOST = dpostGo ? B_DPOST + ageD(t - dpostReady.frontTs()) : -1e300;
                double sPPOST = ppost >= 0 ? B_PPOST + ageP(t - qPPOST.frontTs()) : -1e300;
                double sDPRE  = dpreGo ? B_DPRE + ageD(t - dpreReady.frontTs()) : -1e300;
                double sPPRE  = ppreGo ? B_PPRE + ageP(t - arrOf[oldest >= 0 ? oldest : ppre]) : -1e300;

                int pick = -1; double best = -1e299;
                if (sDPOST > best) { best = sDPOST; pick = 0; }
                if (sPPOST > best) { best = sPPOST; pick = 1; }
                if (sDPRE  > best) { best = sDPRE;  pick = 2; }
                if (sPPRE  > best) { best = sPPRE;  pick = 3; }

                if (pick == 0) {
                    int n = drain(dpostReady, PEND_DPOST, max(1, min(R_dpost.bestSize(nDPost), dpostTarget)));
                    if (n > 0) {
                        int len = sprintf(outBuf[na], "E D POST -1 %d", n);
                        for (int j = 0; j < n; ++j) { len += sprintf(outBuf[na] + len, " %d", batchBuf[j]); st[batchBuf[j]] = IN_DPOST; }
                        ++na; localFree = false;
                    }
                } else if (pick == 1) {
                    qPPOST.pop();
                    if (st[ppost] == PEND_PPOST) {
                        sprintf(outBuf[na++], "E P POST %d %d", (int)cloudOf[ppost], ppost);
                        st[ppost] = IN_PPOST; localFree = false;
                    }
                } else if (pick == 2) {
                    int cap;
                    if (KN_LATHOLD && latDominant()) cap = R_dpre.bestSize(nDPre);
                    else cap = min(R_dpre.bestSize(nDPre), dpreTarget);
                    int n = drain(dpreReady, PEND_DPRE, max(1, cap));
                    if (n > 0) {
                        int len = sprintf(outBuf[na], "E D PRE -1 %d", n);
                        for (int j = 0; j < n; ++j) { len += sprintf(outBuf[na] + len, " %d", batchBuf[j]); st[batchBuf[j]] = IN_DPRE; }
                        ++na; localFree = false;
                    }
                } else if (pick == 3) {
                    if (st[ppre] == PEND_PPRE) {
                        int k = bestRemote(t);
                        cloudOf[ppre] = (int8_t)k;
                        prefBacklogMs[k] += fullProcDur[ppre];
                        sprintf(outBuf[na++], "E P PRE %d %d", k, ppre);
                        st[ppre] = IN_PPRE; localFree = false;
                    }
                }
            }

            // ---------------- remote engines ----------------
            for (int k = 0; k < K; ++k) {
                if (!remoteFree[k]) continue;
                trim(dprocReady[k], PEND_DPROC);
                while (!prefReadyRing[k].empty()) {
                    St s = st[prefReadyRing[k].front()];
                    if (s == PEND_PROC || s == PEND_PROC_RES) break;
                    prefReadyRing[k].pop();
                }

                int nD = dprocReady[k].size();
                int pr = pickPref(k);

                double sD = nD ? B_DPROC + ageD(t - dprocReady[k].frontTs()) : -1e300;
                double sP = pr >= 0 ? B_PPROC + ageP(t - (prefReadyRing[k].empty() ? t : prefReadyRing[k].frontTs())) : -1e300;
                if (sD <= -1e299 && sP <= -1e299) continue;

                if (sD >= sP) {
                    int cap = R_dproc.bestSize(nD);
                    if (KN_WAVES_PROC) cap = min(cap, max(waveCap(R_dproc, activeDec[k]), 1));
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
                        int ls = layersDone[pr];
                        int le = pieceEnd(pr, k);
                        sprintf(outBuf[na++], "C%d P PROC %d %d %d %d", k, ls, le, k, pr);
                        st[pr] = IN_PROC; remoteFree[k] = false;
                        busyUntil[k] = t + S + (double)(le - ls) / numLayers * fullProcDur[pr];
                    }
                }
            }

            if (na > 0 || pendingEvents() > 0) break;
            bool anyWork = !pendByLin.empty() || !dpreReady.empty() || !dpostReady.empty() || !qPPOST.empty();
            for (int k = 0; k < K && !anyWork; ++k) anyWork = !prefQ[k].empty() || !dprocReady[k].empty();
            if (!anyWork) break;
        }

        printf("%d\n", na);
        for (int j = 0; j < na; ++j) puts(outBuf[j]);
        fflush(stdout);
    }
    return 0;
}