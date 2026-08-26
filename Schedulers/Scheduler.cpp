// Scheduler.cpp — Implementation of the encapsulated scheduler environment.

#include "Scheduler.h"
#include <iostream>
#include <cstring>

using namespace std;

// ===========================================================================
// KnobSet — load from environment variables
// ===========================================================================
static double envd(const char *n, double d) { const char *s = getenv(n); return s ? atof(s) : d; }
static int    envi(const char *n, int d)    { const char *s = getenv(n); return s ? atoi(s) : d; }

KnobSet KnobSet::fromEnvironment() {
    KnobSet k;
    k.SPT          = envi("V4_SPT", k.SPT);
    k.CHUNK        = envi("V4_CHUNK", k.CHUNK);
    k.CHUNK_SMULT  = envd("V4_CHUNK_SMULT", k.CHUNK_SMULT);
    k.HOLD         = envi("V4_HOLD", k.HOLD);
    k.WAVES        = envd("V4_WAVES", k.WAVES);
    k.HOLD_WFRAC   = envd("V4_HOLD_WFRAC", k.HOLD_WFRAC);
    k.HOLD_SMULT   = envd("V4_HOLD_SMULT", k.HOLD_SMULT);
    k.UPGATE_FRAC  = envd("V4_UPGATE_FRAC", k.UPGATE_FRAC);
    k.UPPRE_MAX    = envi("V4_UPPRE_MAX", k.UPPRE_MAX);
    k.UPPRE_MAX_TP = envi("V4_UPPRE_MAX_TP", k.UPPRE_MAX_TP);
    k.RATE_EFF     = envd("V4_RATE_EFF", k.RATE_EFF);
    k.DECW         = envd("V4_DECW", k.DECW);
    k.WAVES_PROC   = envi("V4_WAVES_PROC", k.WAVES_PROC);
    k.LATHOLD      = envi("V4_LATHOLD", k.LATHOLD);
    k.LATFRAC      = envd("V4_LATFRAC", k.LATFRAC);
    k.CONS         = envi("V4_CONS", k.CONS);
    k.CONS_PEN     = envd("V4_CONS_PEN", k.CONS_PEN);
    k.CHUNK_MINS   = envd("V4_CHUNK_MINS", k.CHUNK_MINS);
    k.CHUNK_TPP    = envd("V4_CHUNK_TPP", k.CHUNK_TPP);
    k.BASE_W       = envd("V4_BASE_W", k.BASE_W);
    k.B_DPOST      = envd("V4_B_DPOST", k.B_DPOST);
    k.B_PPOST      = envd("V4_B_PPOST", k.B_PPOST);
    k.B_DPRE       = envd("V4_B_DPRE", k.B_DPRE);
    k.B_PPRE       = envd("V4_B_PPRE", k.B_PPRE);
    k.B_DPROC      = envd("V4_B_DPROC", k.B_DPROC);
    k.B_PPROC      = envd("V4_B_PPROC", k.B_PPROC);
    k.AGE_FLOOR    = envd("V4_AGE_FLOOR", k.AGE_FLOOR);
    k.AGE_AW       = envd("V4_AGE_AW", k.AGE_AW);
    k.AGE_PRESS    = envd("V4_AGE_PRESS", k.AGE_PRESS);
    k.AGE_SLO_W    = envd("V4_AGE_SLO_W", k.AGE_SLO_W);
    k.AGE_NORM     = envi("V4_AGE_NORM", k.AGE_NORM);
    k.PPRE_AGECAP  = envd("V4_PPRE_AGECAP", k.PPRE_AGECAP);
    k.DECQ         = envd("V4_DECQ", k.DECQ);
    k.CHUNK_RATIO  = envd("V4_CHUNK_RATIO", k.CHUNK_RATIO);
    k.CHUNK_PRED   = envi("V4_CHUNK_PRED", k.CHUNK_PRED);
    k.LAT_MULT     = envd("V4_LAT_MULT", k.LAT_MULT);
    k.GATE_TDR     = envd("V4_GATE_TDR", k.GATE_TDR);
    k.HOLD_ACT     = envi("V4_HOLD_ACT", k.HOLD_ACT);
    k.HOLD_AW      = envd("V4_HOLD_AW", k.HOLD_AW);
    k.WAVE_CAPS_BATCH = envi("V4_WAVE_CAPS_BATCH", k.WAVE_CAPS_BATCH);
    return k;
}

// ===========================================================================
// RateOptimizer member implementations
// ===========================================================================
void RateOptimizer::build(const Table &t, double sVal, double rateEff) {
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
    for (auto &p : bp) { if (p.second >= rateEff * maxRate) { gMinEff = max(1, (int)p.first); break; } }
}

int RateOptimizer::bestSize(int n) const {
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

double RateOptimizer::perItemCost() const {
    double bestRatio = 0;
    for (auto &pr : bp) bestRatio = max(bestRatio, pr.second);
    if (bestRatio <= 1e-12) return S + (tab && !tab->pts.empty() ? tab->pts.back().second : 0.0);
    return 1.0 / bestRatio;
}

// ===========================================================================
// SchedulerEnv — constructor
// ===========================================================================
SchedulerEnv::SchedulerEnv()
    : K_(0), numLayers_(0), S_(0), wTp_(0.5), wC_(0.5), aw_(0.5),
      SLO1_(1), SLO2_(1), tpUB_(0), tpBase_(0), distBase_(0),
      slo1Free_(false), slo2Free_(false),
      tdrPressure_(0), tpotPressure_(0), cHopeless_(false),
      gProjTdr_(0), gProjTpot_(0),
      latMs_(0), bwGbps_(0), bytesPerToken_(0),
      localFree_(true),
      upFreeAt_(0), downFreeAt_(0),
      upQLen_(0), downQLen_(0), prefUpQueued_(0),
      activeDecTotal_(0),
      tdrSum_(0), tdrCnt_(0),
      cntArr_(0), cntNotPPost_(0), sumArrNotPPost_(0),
      sumTpotDone_(0), cntTpotDone_(0),
      sumTpotAct_(0), cntTpotAct_(0),
      sumLoutDone_(0), cntLoutDone_(0),
      na_(0), finalScore_(0),
      B_DPOST_(0), B_PPOST_(0), B_DPRE_(0), B_PPRE_(0), B_DPROC_(0), B_PPROC_(0)
{
    memset(st_, 0, sizeof(st_));
    memset(cloudOf_, 0, sizeof(cloudOf_));
    memset(linOf_, 0, sizeof(linOf_));
    memset(arrOf_, 0, sizeof(arrOf_));
    memset(layersDone_, 0, sizeof(layersDone_));
    memset(fullProcDur_, 0, sizeof(fullProcDur_));
    memset(tokCnt_, 0, sizeof(tokCnt_));
    memset(firstTok_, 0, sizeof(firstTok_));
    memset(lastTokT_, 0, sizeof(lastTokT_));
    memset(curTpot_, 0, sizeof(curTpot_));
    memset(remoteFree_, 0, sizeof(remoteFree_));
    memset(busyUntil_, 0, sizeof(busyUntil_));
    memset(prefBacklogMs_, 0, sizeof(prefBacklogMs_));
    memset(activeDec_, 0, sizeof(activeDec_));
    memset(decUpInflight_, 0, sizeof(decUpInflight_));
}

// ===========================================================================
// loadKnobs
// ===========================================================================
void SchedulerEnv::loadKnobs(const KnobSet &kn) {
    kn_ = kn;
    B_DPOST_ = kn_.BASE_W * kn_.B_DPOST;  B_PPOST_ = kn_.BASE_W * kn_.B_PPOST;
    B_DPRE_  = kn_.BASE_W * kn_.B_DPRE;   B_PPRE_  = kn_.BASE_W * kn_.B_PPRE;
    B_DPROC_ = kn_.BASE_W * kn_.B_DPROC;  B_PPROC_ = kn_.BASE_W * kn_.B_PPROC;
}

void SchedulerEnv::loadKnobsFromEnv() {
    loadKnobs(KnobSet::fromEnvironment());
}

// ===========================================================================
// setupInitialState
// ===========================================================================
bool SchedulerEnv::setupInitialState(std::istream &in) {
    if (!(in >> K_ >> S_ >> latMs_ >> bwGbps_ >> bytesPerToken_ >> numLayers_)) return false;
    K_ = max(1, min(8, K_));
    in >> SLO1_ >> SLO2_ >> tpUB_ >> tpBase_ >> distBase_ >> wTp_ >> wC_;
    { double s = wTp_ + wC_; aw_ = (s > 1e-12) ? wTp_ / s : 0.5; }
    if (SLO1_ <= 0) SLO1_ = 1e-6;
    if (SLO2_ <= 0) SLO2_ = 1e-6;
    slo1Free_ = SLO1_ >= 1e8; slo2Free_ = SLO2_ >= 1e8;

    int N; if (!(in >> N)) return false;
    for (int i = 0; i < N; ++i) {
        double bs, a, b, c, d, e2, f;
        in >> bs >> a >> b >> c >> d >> e2 >> f;
        T_prefill_pre_.add(bs, a);  T_prefill_proc_.add(bs, b); T_prefill_post_.add(bs, c);
        T_decode_pre_.add(bs, d);   T_decode_proc_.add(bs, e2); T_decode_post_.add(bs, f);
    }
    T_prefill_pre_.finalize(); T_prefill_proc_.finalize(); T_prefill_post_.finalize();
    T_decode_pre_.finalize();  T_decode_proc_.finalize();  T_decode_post_.finalize();
    R_dpre_.build(T_decode_pre_, S_, kn_.RATE_EFF);
    R_dproc_.build(T_decode_proc_, S_, kn_.RATE_EFF);
    R_dpost_.build(T_decode_post_, S_, kn_.RATE_EFF);

    for (int i = 0; i < MAXR; ++i) st_[i] = S_FIN;
    for (int k = 0; k < 8; ++k) {
        remoteFree_[k] = true; busyUntil_[k] = 0;
        prefBacklogMs_[k] = 0; activeDec_[k] = 0;
        decUpInflight_[k] = 0;
    }
    localFree_ = true;
    upFreeAt_ = downFreeAt_ = 0;
    upQLen_ = downQLen_ = prefUpQueued_ = 0;
    activeDecTotal_ = 0;
    tdrSum_ = 0; tdrCnt_ = 0;
    cntArr_ = 0; cntNotPPost_ = 0; sumArrNotPPost_ = 0;
    sumTpotDone_ = 0; cntTpotDone_ = 0;
    sumTpotAct_ = 0; cntTpotAct_ = 0;
    sumLoutDone_ = 0; cntLoutDone_ = 0;
    return true;
}

bool SchedulerEnv::setupParams(int K, int numLayers, double S, double latMs, double bwGbps,
                               int bytesPerToken, double SLO1, double SLO2, double tpUB,
                               double tpBase, double distBase, double wTp, double wC,
                               const std::vector<std::vector<double>> &tableRows)
{
    K_ = max(1, min(8, K));
    numLayers_ = numLayers;
    S_ = S;
    latMs_ = latMs;
    bwGbps_ = bwGbps;
    bytesPerToken_ = bytesPerToken;
    SLO1_ = SLO1;
    SLO2_ = SLO2;
    tpUB_ = tpUB;
    tpBase_ = tpBase;
    distBase_ = distBase;
    wTp_ = wTp;
    wC_ = wC;
    { double s = wTp_ + wC_; aw_ = (s > 1e-12) ? wTp_ / s : 0.5; }
    if (SLO1_ <= 0) SLO1_ = 1e-6;
    if (SLO2_ <= 0) SLO2_ = 1e-6;
    slo1Free_ = SLO1_ >= 1e8; slo2Free_ = SLO2_ >= 1e8;

    for (const auto& r : tableRows) {
        if (r.size() >= 7) {
            T_prefill_pre_.add(r[0], r[1]);   T_prefill_proc_.add(r[0], r[2]);  T_prefill_post_.add(r[0], r[3]);
            T_decode_pre_.add(r[0], r[4]);    T_decode_proc_.add(r[0], r[5]);   T_decode_post_.add(r[0], r[6]);
        }
    }
    T_prefill_pre_.finalize(); T_prefill_proc_.finalize(); T_prefill_post_.finalize();
    T_decode_pre_.finalize();  T_decode_proc_.finalize();  T_decode_post_.finalize();
    R_dpre_.build(T_decode_pre_, S_, kn_.RATE_EFF);
    R_dproc_.build(T_decode_proc_, S_, kn_.RATE_EFF);
    R_dpost_.build(T_decode_post_, S_, kn_.RATE_EFF);

    for (int i = 0; i < MAXR; ++i) st_[i] = S_FIN;
    for (int k = 0; k < 8; ++k) {
        remoteFree_[k] = true; busyUntil_[k] = 0;
        prefBacklogMs_[k] = 0; activeDec_[k] = 0;
        decUpInflight_[k] = 0;
    }
    localFree_ = true;
    upFreeAt_ = downFreeAt_ = 0;
    upQLen_ = downQLen_ = prefUpQueued_ = 0;
    activeDecTotal_ = 0;
    tdrSum_ = 0; tdrCnt_ = 0;
    cntArr_ = 0; cntNotPPost_ = 0; sumArrNotPPost_ = 0;
    sumTpotDone_ = 0; cntTpotDone_ = 0;
    sumTpotAct_ = 0; cntTpotAct_ = 0;
    sumLoutDone_ = 0; cntLoutDone_ = 0;
    return true;
}

void SchedulerEnv::onArrival(double t, int rid, int lin) {
    linOf_[rid] = lin; arrOf_[rid] = t;
    layersDone_[rid] = 0; tokCnt_[rid] = 0; curTpot_[rid] = 0;
    fullProcDur_[rid] = T_prefill_proc_.at((double)lin);
    st_[rid] = PEND_PPRE; pendRing_.push(rid, t); pendByLin_.insert({lin, rid});
    cntArr_++; cntNotPPost_++; sumArrNotPPost_ += t;
}

void SchedulerEnv::onTaskDonePPre(double t, int rem, int rid, double dur) {
    localFree_ = true;
    if (st_[rid] != S_FIN) st_[rid] = WAIT_UP_PRE;
    enqUp((double)linOf_[rid], t); prefUpQueued_++;
}

void SchedulerEnv::onTaskDonePProc(double t, int ls, int le, int rem, int rid, double dur) {
    if (rem >= 0 && rem < 8) remoteFree_[rem] = true;
    prefBacklogMs_[rem] = max(0.0, prefBacklogMs_[rem] - dur);
    if (st_[rid] != S_FIN) {
        layersDone_[rid] = le;
        if (le >= numLayers_) { st_[rid] = WAIT_DOWN_PRE; enqDown((double)linOf_[rid], t); }
        else {
            st_[rid] = PEND_PROC_RES;
            double remaining = (double)(numLayers_ - le) / numLayers_ * fullProcDur_[rid];
            prefQ_[rem].insert({remaining, rid}); prefReadyRing_[rem].push(rid, t);
        }
    }
}

void SchedulerEnv::onTaskDonePPost(double t, int rem, int rid, double dur) {
    localFree_ = true;
    if (st_[rid] != S_FIN) {
        st_[rid] = PEND_DPRE; dpreReady_.push(rid, t);
        int k = cloudOf_[rid]; activeDec_[k]++; activeDecTotal_++;
        tdrSum_ += t - arrOf_[rid]; tdrCnt_++;
        cntNotPPost_--; sumArrNotPPost_ -= arrOf_[rid];
    }
}

void SchedulerEnv::onTaskDoneDPre(double t, int m, const int* rids, double dur) {
    localFree_ = true;
    int cloudCnt[8] = {0};
    for (int j = 0; j < m; ++j) {
        int rid = rids[j];
        cloudCnt[(int)cloudOf_[rid]]++;
        if (st_[rid] != S_FIN) st_[rid] = WAIT_UP_DEC;
    }
    for (int k = 0; k < K_; ++k) {
        if (cloudCnt[k] > 0) { enqUp((double)cloudCnt[k], t); decUpInflight_[k] += cloudCnt[k]; }
    }
}

void SchedulerEnv::onTaskDoneDProc(double t, int rem, int m, const int* rids, double dur) {
    if (rem >= 0 && rem < 8) remoteFree_[rem] = true;
    for (int j = 0; j < m; ++j) {
        int rid = rids[j];
        if (st_[rid] != S_FIN) st_[rid] = WAIT_DOWN_DEC;
    }
    enqDown((double)m, t);
}

void SchedulerEnv::onTaskDoneDPost(double t, int m, const int* rids, double dur) {
    localFree_ = true;
    for (int j = 0; j < m; ++j) {
        int rid = rids[j];
        if (st_[rid] == S_FIN) continue;
        tokCnt_[rid]++;
        if (tokCnt_[rid] == 1) firstTok_[rid] = t;
        else {
            double nt = (t - firstTok_[rid]) / (tokCnt_[rid] - 1);
            if (tokCnt_[rid] == 2) cntTpotAct_++;
            sumTpotAct_ += nt - curTpot_[rid]; curTpot_[rid] = nt;
        }
        lastTokT_[rid] = t;
        st_[rid] = PEND_DPRE; dpreReady_.push(rid, t);
    }
}

void SchedulerEnv::onTransferDone(double t, bool up, int rem, bool prefill, int m, const int* rids) {
    if (up) upQLen_--; else downQLen_--;
    if (up && prefill) prefUpQueued_--;
    if (up && !prefill && rem >= 0 && rem < 8) decUpInflight_[rem] = max(0, decUpInflight_[rem] - m);

    for (int j = 0; j < m; ++j) {
        int rid = rids[j];
        if (st_[rid] == S_FIN) continue;
        int k = cloudOf_[rid];
        if (up && prefill)       { st_[rid] = PEND_PROC; prefQ_[k].insert({fullProcDur_[rid], rid}); prefReadyRing_[k].push(rid, t); }
        else if (!up && prefill) { st_[rid] = PEND_PPOST; qPPOST_.push(rid, t); }
        else if (up)             { st_[rid] = PEND_DPROC; dprocReady_[k].push(rid, t); }
        else                     { st_[rid] = PEND_DPOST; dpostReady_.push(rid, t); }
    }
}

void SchedulerEnv::onFinish(double t, int rid) {
    if (st_[rid] != S_FIN) {
        int k = cloudOf_[rid];
        if (activeDec_[k] > 0) activeDec_[k]--;
        if (activeDecTotal_ > 0) activeDecTotal_--;
        sumLoutDone_ += tokCnt_[rid]; cntLoutDone_++;
        if (tokCnt_[rid] >= 2) { sumTpotAct_ -= curTpot_[rid]; cntTpotAct_--; sumTpotDone_ += curTpot_[rid]; }
        cntTpotDone_++; st_[rid] = S_FIN;
    }
}

int SchedulerEnv::stepTick(double t) {
    recomputePressures(t);
    na_ = 0;
    for (int pass = 0; pass < 2; ++pass) {
        bool force = (pass == 1);
        runLocalEngine(t, force);
        runRemoteEngines(t);
        if (na_ > 0 || pendingEvents() > 0) break;
        if (!hasPendingWork()) break;
    }
    return na_;
}

// ===========================================================================
// Pressure & Heuristic helpers
// ===========================================================================
double SchedulerEnv::projTdrMean(double t) const {
    return (cntArr_ == 0) ? 0 : (tdrSum_ + (cntNotPPost_ * t - sumArrNotPPost_)) / (double)cntArr_;
}

double SchedulerEnv::projTpotMean() const {
    long c = cntTpotDone_ + cntTpotAct_;
    return c ? (sumTpotDone_ + sumTpotAct_) / (double)c : 0;
}

void SchedulerEnv::recomputePressures(double t) {
    gProjTdr_ = projTdrMean(t); gProjTpot_ = projTpotMean();
    cHopeless_ = (distBase_ <= 0.0) && (gProjTdr_ > 2.0 * SLO1_ || gProjTpot_ > 2.0 * SLO2_);
    bool cOff = (wC_ <= 1e-12) || cHopeless_;
    tdrPressure_  = (cOff || slo1Free_) ? 0.0 : min(1.5, gProjTdr_ / SLO1_);
    tpotPressure_ = (cOff || slo2Free_) ? 0.0 : min(1.5, gProjTpot_ / SLO2_);
}

double SchedulerEnv::awEff() const { return cHopeless_ ? 1.0 : aw_; }
bool SchedulerEnv::latDominant() const {
    return kn_.LAT_MULT * latMs_ > (S_ + T_decode_proc_.at((double)max(1, activeDecTotal_)));
}
int SchedulerEnv::cloudsInUse() const {
    int u = 0; for (int k = 0; k < K_; ++k) if (activeDec_[k] > 0) u++; return max(1, u);
}

int SchedulerEnv::bestCloudCount() const {
    int A = max(4, activeDecTotal_);
    int bestU = 1; double bestF = 1e300;
    for (int U = 1; U <= K_; ++U) {
        double f = kn_.LAT_MULT * latMs_ * U + S_ + T_decode_proc_.at(ceil((double)A / U));
        if (f < bestF) { bestF = f; bestU = U; }
    }
    return bestU;
}

double SchedulerEnv::ageD(double w) const {
    double r = w / SLO2_; if (r > 5) r = 5 + log1p(r - 5);
    return (kn_.AGE_NORM ? (w / SLO2_) : w) * (kn_.AGE_FLOOR + kn_.AGE_AW * awEff() + kn_.AGE_PRESS * tpotPressure_) + kn_.AGE_SLO_W * r;
}

double SchedulerEnv::ageP(double w) const {
    double r = w / SLO1_; if (r > 5) r = 5 + log1p(r - 5);
    return (kn_.AGE_NORM ? (w / SLO1_) : w) * (kn_.AGE_FLOOR + kn_.AGE_AW * (1.0 - awEff()) + kn_.AGE_PRESS * tdrPressure_) + kn_.AGE_SLO_W * r;
}

int SchedulerEnv::oldestPend() {
    trim(pendRing_, PEND_PPRE); return pendRing_.empty() ? -1 : pendRing_.front();
}

int SchedulerEnv::pickPPRE(double t) {
    int old = oldestPend(); if (old < 0) return -1;
    if (!kn_.SPT || t - arrOf_[old] > max(3.0 * SLO1_, 200.0 * S_)) return old;
    while (!pendByLin_.empty()) {
        auto it = pendByLin_.begin();
        if (st_[it->second] == PEND_PPRE) return it->second;
        pendByLin_.erase(it);
    }
    return old;
}

int SchedulerEnv::pickPref(int k) {
    while (!prefQ_[k].empty()) {
        auto it = prefQ_[k].begin();
        int rid = it->second;
        if (st_[rid] == PEND_PROC || st_[rid] == PEND_PROC_RES) return rid;
        prefQ_[k].erase(it);
    }
    return -1;
}

bool SchedulerEnv::holdActive() const {
    if (kn_.HOLD == 0) return false;
    if (kn_.HOLD == 1) return true;
    if (kn_.HOLD_ACT > 0 && activeDecTotal_ >= kn_.HOLD_ACT) return true;
    return awEff() >= kn_.HOLD_AW || (slo1Free_ && slo2Free_);
}

int SchedulerEnv::waveCap(const RateOptimizer &R, int active) const {
    if (kn_.WAVES <= 0.0) return 1 << 28;
    int cap = (int)ceil((double)max(1, active) / kn_.WAVES);
    return max(cap, min(R.gMinEff, max(1, active / 2)));
}

bool SchedulerEnv::holdSatisfied(double t, Ring &q, St want, int nReady, int target, bool isDpre) {
    if (isDpre && kn_.LATHOLD && latDominant() && activeDecTotal_ >= 2) {
        if (nReady >= max(1, (int)(kn_.LATFRAC * activeDecTotal_))) return true;
        trim(q, want);
        if (!q.empty() && t - q.frontTs() >= max(slo2Free_ ? kn_.LAT_MULT * latMs_ * cloudsInUse() : min(kn_.LAT_MULT * latMs_ * cloudsInUse(), 0.5 * SLO2_), 4.0 * S_)) return true;
        return false;
    }
    if (!holdActive() || nReady >= target) return true;
    trim(q, want);
    if (!q.empty()) {
        double wcap = (slo2Free_ || awEff() > 0.9) ? kn_.HOLD_SMULT * S_ : max(2.0 * S_, min(kn_.HOLD_WFRAC * SLO2_, kn_.HOLD_SMULT * S_));
        if (t - q.frontTs() >= wcap) return true;
    }
    return false;
}

bool SchedulerEnv::gateOK(double t) {
    if (activeDecTotal_ == 0) return true;
    int old = oldestPend();
    if (old >= 0 && t - arrOf_[old] > kn_.GATE_TDR * SLO1_ && !slo1Free_) return true;
    if (awEff() >= 0.7 || (slo2Free_ && slo1Free_)) return prefUpQueued_ < kn_.UPPRE_MAX_TP;
    if (prefUpQueued_ >= kn_.UPPRE_MAX) return false;
    return max(0.0, upFreeAt_ - t) <= kn_.UPGATE_FRAC * max(SLO2_, 8.0 * latMs_);
}

int SchedulerEnv::bestRemote(double t) {
    double decCost = R_dproc_.perItemCost();
    double consPen[8] = {0};

    if (kn_.CONS && K_ > 1 && latDominant()) {
        int U = bestCloudCount();
        if (U < K_) {
            int order[8]; for (int k = 0; k < K_; ++k) order[k] = k;
            sort(order, order + K_, [this](int a, int b) {
                if (activeDec_[a] != activeDec_[b]) return activeDec_[a] > activeDec_[b];
                if (prefBacklogMs_[a] != prefBacklogMs_[b]) return prefBacklogMs_[a] < prefBacklogMs_[b];
                return a < b;
            });
            for (int i = U; i < K_; ++i) consPen[order[i]] = kn_.CONS_PEN * kn_.LAT_MULT * latMs_;
        }
    }

    double expectedDropTime[8];
    for (int k = 0; k < K_; ++k) expectedDropTime[k] = prefBacklogMs_[k] + max(0.0, busyUntil_[k] - t);

    int best = 0; double bestSc = 1e300;
    for (int k = 0; k < K_; ++k) {
        double decQ = kn_.DECQ * (double)(dprocReady_[k].size() + decUpInflight_[k]) * (S_ + T_decode_proc_.at(1.0));
        double netCollisionPenalty = 0.0;
        for (int j = 0; j < K_; ++j) {
            if (k == j) continue;
            double diff = abs(expectedDropTime[k] - expectedDropTime[j]);
            double collisionWindow = latMs_ * 4.0;
            if (diff < collisionWindow) netCollisionPenalty += (collisionWindow - diff) * 1.5;
        }

        double sc = prefBacklogMs_[k] + max(0.0, busyUntil_[k] - t)
                  + kn_.DECW * activeDec_[k] * decCost * max(4.0, 0.5 * avgLoutEst())
                  + decQ + consPen[k] + netCollisionPenalty;

        if (sc < bestSc) { bestSc = sc; best = k; }
    }
    return best;
}

int SchedulerEnv::pieceEnd(int rid, int k) {
    int ls = layersDone_[rid], L = numLayers_, lrem = L - ls;
    if (!kn_.CHUNK || L <= 1 || lrem <= 1) return L;
    bool decodeBlocked = dprocReady_[k].size() > 0 || decUpInflight_[k] > 0 || (kn_.CHUNK_PRED && activeDec_[k] > 0);
    if (!decodeBlocked || slo2Free_ || wC_ <= 1e-12 || cHopeless_) return L;

    double tpotRel = max(0.0, gProjTpot_ / SLO2_ - 1.0);
    double tdrRel  = max(0.0, gProjTdr_  / SLO1_ - 1.0);
    if (gProjTpot_ < kn_.CHUNK_TPP * SLO2_ || tpotRel <= kn_.CHUNK_RATIO * tdrRel) return L;

    double remaining = (double)lrem / L * fullProcDur_[rid];
    if (remaining <= max(kn_.CHUNK_MINS * S_, 0.5 * SLO2_)) return L;
    double G = max(kn_.CHUNK_SMULT * S_, 0.25 * SLO2_);
    if (remaining <= 1.6 * G) return L;

    int p = max(1, min((int)llround((double)lrem * G / remaining), lrem));
    return ls + p;
}

// ===========================================================================
// Lookahead
// ===========================================================================
double SchedulerEnv::evaluate_sim_state(const LocalSimState &s) const {
    double projTdr = (s.cntArr > 0) ? (s.tdrSum + (s.cntNotPPost * s.t - s.sumArrNotPPost)) / (double)s.cntArr : 0;
    long c = s.cntTpotDone + s.cntTpotAct;
    double projTpot = c ? (s.sumTpotDone + s.sumTpotAct) / (double)c : 0;
    double tdrExcess = max(0.0, (projTdr - SLO1_) / SLO1_);
    double tpotExcess = max(0.0, (projTpot - SLO2_) / SLO2_);
    return sqrt(tdrExcess * tdrExcess + tpotExcess * tpotExcess);
}

double SchedulerEnv::simulate_and_evaluate(LocalSimState s, const Action &a) const {
    if (a.type == A_WAIT) return evaluate_sim_state(s);
    double dur = 0;
    if (a.type == A_DPOST) {
        dur = S_ + T_decode_post_.at((double)a.cap); s.t += dur;
        s.cntTpotAct += a.cap; s.sumTpotAct += a.cap * (dur / SLO2_);
    }
    else if (a.type == A_DPRE) {
        dur = S_ + T_decode_pre_.at((double)a.cap); s.t += dur;
        s.upFreeAt = max(s.upFreeAt, s.t) + xferMs(1.0);
    }
    else if (a.type == A_PPOST) {
        dur = S_ + T_prefill_post_.at(1.0); s.t += dur;
        s.tdrSum += s.t - arrOf_[a.target_rid]; s.tdrCnt++; s.cntNotPPost--; s.sumArrNotPPost -= arrOf_[a.target_rid];
    }
    else if (a.type == A_PPRE) {
        dur = S_ + T_prefill_pre_.at(1.0); s.t += dur;
        s.upFreeAt = max(s.upFreeAt, s.t) + xferMs(linOf_[a.target_rid]);
    }
    return evaluate_sim_state(s) - (0.00001 * a.bid_score);
}

// ===========================================================================
// Event handlers
// ===========================================================================
void SchedulerEnv::processArrival(double t, std::istream &in) {
    int rid, lin; in >> rid >> lin;
    linOf_[rid] = lin; arrOf_[rid] = t;
    layersDone_[rid] = 0; tokCnt_[rid] = 0; curTpot_[rid] = 0;
    fullProcDur_[rid] = T_prefill_proc_.at((double)lin);
    st_[rid] = PEND_PPRE; pendRing_.push(rid, t); pendByLin_.insert({lin, rid});
    cntArr_++; cntNotPPost_++; sumArrNotPPost_ += t;
}

void SchedulerEnv::processTransfer(double t, std::istream &in) {
    char srv[16], s1[8], s2[8]; in >> srv >> s1 >> s2;
    if (srv[0] == 'E') localFree_ = true;
    else { int k = srv[1] - '0'; if (k >= 0 && k < 8) remoteFree_[k] = true; }

    if (s1[0] == 'P') {
        if (s2[1] == 'O') {
            int rem, rid; double dur; in >> rem >> rid >> dur;
            if (st_[rid] != S_FIN) {
                st_[rid] = PEND_DPRE; dpreReady_.push(rid, t);
                int k = cloudOf_[rid]; activeDec_[k]++; activeDecTotal_++;
                tdrSum_ += t - arrOf_[rid]; tdrCnt_++;
                cntNotPPost_--; sumArrNotPPost_ -= arrOf_[rid];
            }
        } else if (s2[2] == 'E') {
            int rem, rid; double dur; in >> rem >> rid >> dur;
            if (st_[rid] != S_FIN) st_[rid] = WAIT_UP_PRE;
            enqUp((double)linOf_[rid], t); prefUpQueued_++;
        } else {
            int ls, le, rem, rid; double dur; in >> ls >> le >> rem >> rid >> dur;
            prefBacklogMs_[rem] = max(0.0, prefBacklogMs_[rem] - dur);
            if (st_[rid] != S_FIN) {
                layersDone_[rid] = le;
                if (le >= numLayers_) { st_[rid] = WAIT_DOWN_PRE; enqDown((double)linOf_[rid], t); }
                else {
                    st_[rid] = PEND_PROC_RES;
                    double remaining = (double)(numLayers_ - le) / numLayers_ * fullProcDur_[rid];
                    prefQ_[rem].insert({remaining, rid}); prefReadyRing_[rem].push(rid, t);
                }
            }
        }
    } else if (s1[0] == 'D') {
        int hdr, m; in >> hdr >> m;
        bool isPost = (s2[1] == 'O'), isPre = (!isPost && s2[2] == 'E');
        int cloudCnt[8] = {0};
        for (int j = 0; j < m; ++j) {
            int rid; in >> rid;
            if (isPre) cloudCnt[(int)cloudOf_[rid]]++;
            if (st_[rid] == S_FIN) continue;
            if (isPost) {
                tokCnt_[rid]++;
                if (tokCnt_[rid] == 1) firstTok_[rid] = t;
                else {
                    double nt = (t - firstTok_[rid]) / (tokCnt_[rid] - 1);
                    if (tokCnt_[rid] == 2) cntTpotAct_++;
                    sumTpotAct_ += nt - curTpot_[rid]; curTpot_[rid] = nt;
                }
                lastTokT_[rid] = t;
                st_[rid] = PEND_DPRE; dpreReady_.push(rid, t);
            }
            else if (isPre) st_[rid] = WAIT_UP_DEC;
            else st_[rid] = WAIT_DOWN_DEC;
        }
        double dur; in >> dur;
        if (isPre) { for (int k = 0; k < K_; ++k) if (cloudCnt[k] > 0) { enqUp((double)cloudCnt[k], t); decUpInflight_[k] += cloudCnt[k]; } }
        else if (!isPost) enqDown((double)m, t);
    }
}

void SchedulerEnv::processCross(double t, std::istream &in) {
    char dir[8]; int rem; char szS[24], step[8]; int m;
    in >> dir >> rem >> szS >> step >> m;
    bool up = (dir[0] == 'U'), prefill = (step[0] == 'P');
    if (up) upQLen_--; else downQLen_--;
    if (up && prefill) prefUpQueued_--;
    if (up && !prefill && rem >= 0 && rem < 8) decUpInflight_[rem] = max(0, decUpInflight_[rem] - m);

    for (int j = 0; j < m; ++j) {
        int rid; in >> rid;
        if (st_[rid] == S_FIN) continue;
        int k = cloudOf_[rid];
        if (up && prefill)       { st_[rid] = PEND_PROC; prefQ_[k].insert({fullProcDur_[rid], rid}); prefReadyRing_[k].push(rid, t); }
        else if (!up && prefill) { st_[rid] = PEND_PPOST; qPPOST_.push(rid, t); }
        else if (up)             { st_[rid] = PEND_DPROC; dprocReady_[k].push(rid, t); }
        else                     { st_[rid] = PEND_DPOST; dpostReady_.push(rid, t); }
    }
}

void SchedulerEnv::processFinish(double t, std::istream &in) {
    int rid; in >> rid;
    if (st_[rid] != S_FIN) {
        int k = cloudOf_[rid];
        if (activeDec_[k] > 0) activeDec_[k]--;
        if (activeDecTotal_ > 0) activeDecTotal_--;
        sumLoutDone_ += tokCnt_[rid]; cntLoutDone_++;
        if (tokCnt_[rid] >= 2) { sumTpotAct_ -= curTpot_[rid]; cntTpotAct_--; sumTpotDone_ += curTpot_[rid]; }
        cntTpotDone_++; st_[rid] = S_FIN;
    }
}

// ===========================================================================
// Engines
// ===========================================================================
void SchedulerEnv::runLocalEngine(double t, bool force) {
    if (!localFree_) return;
    trim(dpostReady_, PEND_DPOST); trim(qPPOST_, PEND_PPOST); trim(dpreReady_, PEND_DPRE);

    int nDPost = dpostReady_.size(), nDPre  = dpreReady_.size();
    int ppost  = qPPOST_.empty() ? -1 : qPPOST_.front();
    int ppre   = pickPPRE(t), oldest = oldestPend();

    bool holdOn = holdActive();
    int dpreTarget  = holdOn ? waveCap(R_dpre_, activeDecTotal_)  : (1 << 28);
    int dpostTarget = holdOn ? waveCap(R_dpost_, activeDecTotal_) : (1 << 28);
    bool dpreGo  = nDPre > 0 && (force || holdSatisfied(t, dpreReady_, PEND_DPRE, nDPre, dpreTarget, true));
    bool dpostGo = nDPost > 0 && (force || holdSatisfied(t, dpostReady_, PEND_DPOST, nDPost, dpostTarget, false));
    bool ppreGo = ppre >= 0 && (force || gateOK(t));

    vector<Action> legal_moves;

    if (dpostGo) {
        int dpostCap = kn_.WAVE_CAPS_BATCH ? min(R_dpost_.bestSize(nDPost), dpostTarget) : R_dpost_.bestSize(nDPost);
        legal_moves.push_back({A_DPOST, max(1, dpostCap), -1, -1, B_DPOST_ + ageD(t - dpostReady_.frontTs())});
    }
    if (ppost >= 0) legal_moves.push_back({A_PPOST, 1, ppost, -1, B_PPOST_ + ageP(t - qPPOST_.frontTs())});
    if (dpreGo) {
        int cap = ((kn_.LATHOLD && latDominant()) || !kn_.WAVE_CAPS_BATCH) ? R_dpre_.bestSize(nDPre) : min(R_dpre_.bestSize(nDPre), dpreTarget);
        legal_moves.push_back({A_DPRE, max(1, cap), -1, -1, B_DPRE_ + ageD(t - dpreReady_.frontTs())});
    }
    if (ppreGo) {
        double ppreWait = min(t - arrOf_[oldest >= 0 ? oldest : ppre], kn_.PPRE_AGECAP);
        legal_moves.push_back({A_PPRE, 1, ppre, bestRemote(t), B_PPRE_ + ageP(ppreWait)});
    }

    if (!legal_moves.empty()) {
        LocalSimState currentState = { t, tdrSum_, tdrCnt_, cntArr_, cntNotPPost_, sumArrNotPPost_, sumTpotAct_, sumTpotDone_, cntTpotAct_, cntTpotDone_, upFreeAt_, downFreeAt_ };
        Action bestMove = legal_moves[0]; double bestScore = 1e300;

        for (const Action& move : legal_moves) {
            double projected_penalty = simulate_and_evaluate(currentState, move);
            if (projected_penalty < bestScore) { bestScore = projected_penalty; bestMove = move; }
        }

        if (bestMove.type == A_DPOST) {
            int n = drain(dpostReady_, PEND_DPOST, bestMove.cap);
            if (n > 0) {
                int len = sprintf(outBuf_[na_], "E D POST -1 %d", n);
                for (int j = 0; j < n; ++j) { len += sprintf(outBuf_[na_] + len, " %d", batchBuf_[j]); st_[batchBuf_[j]] = IN_DPOST; }
                ++na_; localFree_ = false;
            }
        } else if (bestMove.type == A_PPOST) {
            qPPOST_.pop();
            if (st_[bestMove.target_rid] == PEND_PPOST) {
                sprintf(outBuf_[na_++], "E P POST %d %d", (int)cloudOf_[bestMove.target_rid], bestMove.target_rid);
                st_[bestMove.target_rid] = IN_PPOST; localFree_ = false;
            }
        } else if (bestMove.type == A_DPRE) {
            int n = drain(dpreReady_, PEND_DPRE, bestMove.cap);
            if (n > 0) {
                int len = sprintf(outBuf_[na_], "E D PRE -1 %d", n);
                for (int j = 0; j < n; ++j) { len += sprintf(outBuf_[na_] + len, " %d", batchBuf_[j]); st_[batchBuf_[j]] = IN_DPRE; }
                ++na_; localFree_ = false;
            }
        } else if (bestMove.type == A_PPRE) {
            if (st_[bestMove.target_rid] == PEND_PPRE) {
                cloudOf_[bestMove.target_rid] = (int8_t)bestMove.target_cloud;
                prefBacklogMs_[bestMove.target_cloud] += fullProcDur_[bestMove.target_rid];
                sprintf(outBuf_[na_++], "E P PRE %d %d", bestMove.target_cloud, bestMove.target_rid);
                st_[bestMove.target_rid] = IN_PPRE; localFree_ = false;
            }
        }
    }
}

void SchedulerEnv::runRemoteEngines(double t) {
    int order[8];
    for (int k = 0; k < K_; ++k) order[k] = k;

    sort(order, order + K_, [this](int a, int b) {
        int maxTokA = -1, maxTokB = -1;
        if (!dprocReady_[a].empty()) maxTokA = tokCnt_[dprocReady_[a].front()];
        if (!dprocReady_[b].empty()) maxTokB = tokCnt_[dprocReady_[b].front()];
        if (maxTokA != maxTokB) return maxTokA > maxTokB;
        return prefBacklogMs_[a] > prefBacklogMs_[b];
    });

    for (int i = 0; i < K_; ++i) {
        int k = order[i];
        if (!remoteFree_[k]) continue;
        trim(dprocReady_[k], PEND_DPROC);
        while (!prefReadyRing_[k].empty()) {
            St s = st_[prefReadyRing_[k].front()];
            if (s == PEND_PROC || s == PEND_PROC_RES) break;
            prefReadyRing_[k].pop();
        }

        int nD = dprocReady_[k].size(), pr = pickPref(k);
        double sD = nD ? B_DPROC_ + ageD(t - dprocReady_[k].frontTs()) : -1e300;
        double sP = pr >= 0 ? B_PPROC_ + ageP(t - (prefReadyRing_[k].empty() ? t : prefReadyRing_[k].frontTs())) : -1e300;

        if (sD <= -1e299 && sP <= -1e299) continue;

        if (sD >= sP) {
            int cap = R_dproc_.bestSize(nD);
            if (kn_.WAVES_PROC) cap = min(cap, max(waveCap(R_dproc_, activeDec_[k]), 1));
            int n = drain(dprocReady_[k], PEND_DPROC, max(1, cap));
            if (n > 0) {
                int len = sprintf(outBuf_[na_], "C%d D PROC %d %d", k, k, n);
                for (int j = 0; j < n; ++j) { len += sprintf(outBuf_[na_] + len, " %d", batchBuf_[j]); st_[batchBuf_[j]] = IN_DPROC; }
                ++na_; remoteFree_[k] = false;
                busyUntil_[k] = t + S_ + T_decode_proc_.at((double)n);
            }
        } else {
            prefQ_[k].erase(prefQ_[k].begin());
            if (pr >= 0 && st_[pr] != S_FIN) {
                int ls = layersDone_[pr], le = pieceEnd(pr, k);
                sprintf(outBuf_[na_++], "C%d P PROC %d %d %d %d", k, ls, le, k, pr);
                st_[pr] = IN_PROC; remoteFree_[k] = false;
                busyUntil_[k] = t + S_ + (double)(le - ls) / numLayers_ * fullProcDur_[pr];
            }
        }
    }
}

bool SchedulerEnv::hasPendingWork() const {
    bool anyWork = !pendByLin_.empty() || !dpreReady_.empty() || !dpostReady_.empty() || !qPPOST_.empty();
    for (int k = 0; k < K_ && !anyWork; ++k) anyWork = !prefQ_[k].empty() || !dprocReady_[k].empty();
    return anyWork;
}

// ===========================================================================
// Per-frame tick
// ===========================================================================
int SchedulerEnv::runTick(double t, int numEvents, std::istream &in, std::string &out) {
    for (int i = 0; i < numEvents; ++i) {
        char type[16]; in >> type;
        if (type[0] == 'A')      processArrival(t, in);
        else if (type[0] == 'T') processTransfer(t, in);
        else if (type[0] == 'X') processCross(t, in);
        else if (type[0] == 'F') processFinish(t, in);
    }

    recomputePressures(t);
    na_ = 0;

    for (int pass = 0; pass < 2; ++pass) {
        bool force = (pass == 1);
        runLocalEngine(t, force);
        runRemoteEngines(t);

        if (na_ > 0 || pendingEvents() > 0) break;
        if (!hasPendingWork()) break;
    }

    // Build output string
    char countBuf[16];
    sprintf(countBuf, "%d\n", na_);
    out += countBuf;
    for (int j = 0; j < na_; ++j) {
        out += outBuf_[j];
        out += '\n';
    }
    return na_;
}

// ===========================================================================
// Full interaction (for standalone mode and PyBind11)
// ===========================================================================
void SchedulerEnv::runFull(std::istream &input, std::string &actionsOut) {
    if (!setupInitialState(input)) return;

    double t; int e;
    while (input >> t >> e) {
        runTick(t, e, input, actionsOut);
    }
}
