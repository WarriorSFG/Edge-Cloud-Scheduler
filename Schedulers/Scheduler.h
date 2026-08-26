#pragma once
// Scheduler.h — All scheduler state encapsulated in one re-entrant class.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <vector>
#include <map>
#include <string>
#include <sstream>

// ===========================================================================
// Knob values — a plain struct so Python can build one and hand it over.
// ===========================================================================
struct KnobSet {
    int    SPT           = 1;
    int    CHUNK         = 1;
    double CHUNK_SMULT   = 117.072478194454;
    int    HOLD          = 1;
    double WAVES         = 2.6067767581806;
    double HOLD_WFRAC    = 0.311336180161816;
    double HOLD_SMULT    = 35.7840349513665;
    double UPGATE_FRAC   = 0.133183223107648;
    int    UPPRE_MAX     = 9;
    int    UPPRE_MAX_TP  = 13;
    double RATE_EFF      = 0.785281063030896;
    double DECW          = 1.56316454998452;
    int    WAVES_PROC    = 0;
    int    LATHOLD       = 0;
    double LATFRAC       = 0.784885482322511;
    int    CONS          = 1;
    double CONS_PEN      = 4.38521352191839;
    double CHUNK_MINS    = 83.0861287103681;
    double CHUNK_TPP     = 0.709332551776164;
    double BASE_W        = 0.0688174470439218;
    double B_DPOST       = 4.77937996680665;
    double B_PPOST       = 6.659109487552;
    double B_DPRE        = 2.11491371566828;
    double B_PPRE        = 3.59042054650082;
    double B_DPROC       = 5.46600299914945;
    double B_PPROC       = 0.210235165695655;
    double AGE_FLOOR     = 0.345735897111258;
    double AGE_AW        = 2.16669410499286;
    double AGE_PRESS     = 3.24529577274236;
    double AGE_SLO_W     = 20.5097192909652;
    int    AGE_NORM      = 0;
    double PPRE_AGECAP   = 58607547317.7586;
    double DECQ          = 1.48621187428654;
    double CHUNK_RATIO   = 7.06079186759801;
    int    CHUNK_PRED    = 0;
    double LAT_MULT      = 7.54176800349664;
    double GATE_TDR      = 1.27796897979818;
    int    HOLD_ACT      = 39;
    double HOLD_AW       = 0.691000184000935;
    int    WAVE_CAPS_BATCH = 0;

    // Build from environment variables (matches loadKnobs in original code)
    static KnobSet fromEnvironment();
};

// ===========================================================================
// Core data structures (same as original, but not global)
// ===========================================================================

struct Table {
    std::vector<std::pair<double,double>> pts;
    void add(double bs, double v) { if (v >= 0.0) pts.push_back({bs, v}); }
    void finalize() { std::sort(pts.begin(), pts.end()); }
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
    std::vector<std::pair<double,double>> bp;
    std::vector<int> prefixBestIdx;
    int gMinEff = 1;

    void build(const Table &t, double sVal, double rateEff);
    int  bestSize(int n) const;
    double perItemCost() const;
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
    inline void reset() { head = tail = 0; }
};

// Lookahead types
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


// ===========================================================================
// SchedulerEnv — the entire scheduler as a reusable, re-entrant object.
// ===========================================================================
class SchedulerEnv {
public:
    SchedulerEnv();

    // ---- configuration ---------------------------------------------------
    void loadKnobs(const KnobSet &kn);
    void loadKnobsFromEnv();

    // ---- setup -----------------------------------------------------------
    // Parse header lines from `in` and prepare all tables.
    // Returns false if parsing fails (EOF / malformed).
    bool setupInitialState(std::istream &in);

    // Direct in-memory fast setup (defined with FastJudge/TestCaseData)
    bool setupParams(int K, int numLayers, double S, double latMs, double bwGbps,
                     int bytesPerToken, double SLO1, double SLO2, double tpUB,
                     double tpBase, double distBase, double wTp, double wC,
                     const std::vector<std::vector<double>> &tableRows);

    // Direct zero-copy event handlers for FastJudge
    void onArrival(double t, int rid, int lin);
    void onTaskDonePPre(double t, int rem, int rid, double dur);
    void onTaskDonePProc(double t, int ls, int le, int rem, int rid, double dur);
    void onTaskDonePPost(double t, int rem, int rid, double dur);
    void onTaskDoneDPre(double t, int m, const int* rids, double dur);
    void onTaskDoneDProc(double t, int rem, int m, const int* rids, double dur);
    void onTaskDoneDPost(double t, int m, const int* rids, double dur);
    void onTransferDone(double t, bool up, int rem, bool prefill, int m, const int* rids);
    void onFinish(double t, int rid);
    int  stepTick(double t);
    const char* getAction(int idx) const { return outBuf_[idx]; }

    // ---- per-frame step --------------------------------------------------
    // Read one frame's events from `in`, run the engine, and append zero or
    // more action lines to `out`.  Returns the number of actions emitted.
    int runTick(double t, int numEvents, std::istream &in, std::string &out);

    // ---- full interaction -------------------------------------------------
    // Drive the entire stdin-like interaction in one call.
    // Reads the header + all frames from `input`, writes all outputs to
    // `actionsOut`.  Returns when input is exhausted.
    void runFull(std::istream &input, std::string &actionsOut);

    // ---- scoring helpers (exposed for Python) ----------------------------
    double getScore() const { return finalScore_; }

private:
    // ---- knobs (instance copies) -----------------------------------------
    KnobSet kn_;
    // Derived from knobs
    double B_DPOST_, B_PPOST_, B_DPRE_, B_PPRE_, B_DPROC_, B_PPROC_;

    // ---- problem parameters ----------------------------------------------
    int K_, numLayers_;
    double S_, wTp_, wC_, aw_;
    double SLO1_, SLO2_, tpUB_, tpBase_, distBase_;
    bool slo1Free_, slo2Free_;
    double tdrPressure_, tpotPressure_;
    bool cHopeless_;
    double gProjTdr_, gProjTpot_;
    double latMs_, bwGbps_;
    int bytesPerToken_;

    // ---- tables & optimizers ---------------------------------------------
    Table T_prefill_pre_, T_prefill_proc_, T_prefill_post_;
    Table T_decode_pre_, T_decode_proc_, T_decode_post_;
    RateOptimizer R_dpre_, R_dproc_, R_dpost_;

    // ---- per-request state -----------------------------------------------
    St      st_[MAXR];
    int8_t  cloudOf_[MAXR];
    int32_t linOf_[MAXR];
    double  arrOf_[MAXR];
    int32_t layersDone_[MAXR];
    double  fullProcDur_[MAXR];
    int32_t tokCnt_[MAXR];
    double  firstTok_[MAXR], lastTokT_[MAXR], curTpot_[MAXR];

    // ---- queues ----------------------------------------------------------
    Ring qPPOST_, dpreReady_, dpostReady_, pendRing_;
    std::multimap<int,int> pendByLin_;
    Ring dprocReady_[8], prefReadyRing_[8];
    std::multimap<double,int> prefQ_[8];

    // ---- server state ----------------------------------------------------
    bool   localFree_;
    bool   remoteFree_[8];
    double busyUntil_[8];

    // ---- link state ------------------------------------------------------
    double upFreeAt_, downFreeAt_;
    int    upQLen_, downQLen_, prefUpQueued_;

    // ---- decode tracking -------------------------------------------------
    double prefBacklogMs_[8];
    int    activeDec_[8];
    int    activeDecTotal_;
    int    decUpInflight_[8];

    // ---- metrics accumulators --------------------------------------------
    double tdrSum_;  long tdrCnt_;
    long   cntArr_, cntNotPPost_;  double sumArrNotPPost_;
    double sumTpotDone_; long cntTpotDone_;
    double sumTpotAct_;  long cntTpotAct_;
    double sumLoutDone_; long cntLoutDone_;

    // ---- scratch buffers -------------------------------------------------
    char outBuf_[16][1 << 16];
    int32_t batchBuf_[QCAP];
    int na_;

    double finalScore_;

    // ---- inline helpers --------------------------------------------------
    inline double xferMs(double lenTokens) const {
        return latMs_ + 8.0 * lenTokens * bytesPerToken_ / (bwGbps_ * 1e6);
    }
    inline void enqUp(double lenTokens, double t) {
        upFreeAt_ = std::max(upFreeAt_, t) + xferMs(lenTokens); upQLen_++;
    }
    inline void enqDown(double lenTokens, double t) {
        downFreeAt_ = std::max(downFreeAt_, t) + xferMs(lenTokens); downQLen_++;
    }
    inline double avgLoutEst() const {
        return cntLoutDone_ > 0 ? std::max(1.0, sumLoutDone_ / cntLoutDone_) : 32.0;
    }
    inline int pendingEvents() const {
        int busy = localFree_ ? 0 : 1;
        for (int k = 0; k < K_; ++k) if (!remoteFree_[k]) busy++;
        return busy + upQLen_ + downQLen_;
    }
    inline void trim(Ring &q, St want) {
        while (!q.empty() && st_[q.front()] != want) q.pop();
    }
    inline int drain(Ring &q, St want, int cap) {
        int n = 0;
        while (!q.empty() && n < cap) {
            int32_t r = q.front(); q.pop();
            if (st_[r] == want) batchBuf_[n++] = r;
        }
        return n;
    }

    // ---- pressure & heuristic helpers ------------------------------------
    double projTdrMean(double t) const;
    double projTpotMean() const;
    void   recomputePressures(double t);
    double awEff() const;
    bool   latDominant() const;
    int    cloudsInUse() const;
    int    bestCloudCount() const;
    double ageD(double w) const;
    double ageP(double w) const;
    int    oldestPend();
    int    pickPPRE(double t);
    int    pickPref(int k);
    bool   holdActive() const;
    int    waveCap(const RateOptimizer &R, int active) const;
    bool   holdSatisfied(double t, Ring &q, St want, int nReady, int target, bool isDpre);
    bool   gateOK(double t);
    int    bestRemote(double t);
    int    pieceEnd(int rid, int k);

    // ---- lookahead -------------------------------------------------------
    double evaluate_sim_state(const LocalSimState &s) const;
    double simulate_and_evaluate(LocalSimState s, const Action &a) const;

    // ---- event handlers --------------------------------------------------
    void processArrival(double t, std::istream &in);
    void processTransfer(double t, std::istream &in);
    void processCross(double t, std::istream &in);
    void processFinish(double t, std::istream &in);

    // ---- engine ----------------------------------------------------------
    void runLocalEngine(double t, bool force);
    void runRemoteEngines(double t);
    bool hasPendingWork() const;
};
