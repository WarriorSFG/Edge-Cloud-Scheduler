<<<<<<< Updated upstream
// =============================================================================
// Schedulers/Scheduler.cpp — Implementation of Mathematical Scheduler
// =============================================================================

#include "Scheduler.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cmath>

using namespace std;

// =============================================================================
// KnobSet Environment Loader
// =============================================================================

static double envd(const char *n, double d) { const char *s = getenv(n); return s ? atof(s) : d; }

KnobSet KnobSet::fromEnvironment() {
    KnobSet k;
    k.W1        = envd("V4_W1", envd("W1", k.W1));
    k.W2        = envd("V4_W2", envd("W2", k.W2));
    k.W3        = envd("V4_W3", envd("W3", k.W3));
    k.B1        = envd("V4_B1", envd("B1", k.B1));
    k.W4        = envd("V4_W4", envd("W4", k.W4));
    k.W5        = envd("V4_W5", envd("W5", k.W5));
    k.B2        = envd("V4_B2", envd("B2", k.B2));
    k.W6        = envd("V4_W6", envd("W6", k.W6));
    k.B3        = envd("V4_B3", envd("B3", k.B3));
    k.URG_SCALE = envd("V4_URG_SCALE", envd("URG_SCALE", k.URG_SCALE));
    return k;
}

// =============================================================================
// SchedulerEnv Constructor & Configuration
// =============================================================================

SchedulerEnv::SchedulerEnv() {
    kn_ = KnobSet::fromEnvironment();
}

=======
// Scheduler.cpp — Implementation of the encapsulated scheduler environment.

#include "Scheduler.h"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

using namespace std;

// ===========================================================================
// KnobSet — load from environment variables
// ===========================================================================
KnobSet KnobSet::fromEnvironment() {
    KnobSet k;
    auto getEnvD = [](const char *n1, const char *n2, double def) {
        const char *s = getenv(n1);
        if (s) return atof(s);
        s = getenv(n2);
        return s ? atof(s) : def;
    };
    k.W1        = getEnvD("V4_W1", "W1", k.W1);
    k.W2        = getEnvD("V4_W2", "W2", k.W2);
    k.W3        = getEnvD("V4_W3", "W3", k.W3);
    k.B1        = getEnvD("V4_B1", "B1", k.B1);
    k.W4        = getEnvD("V4_W4", "W4", k.W4);
    k.W5        = getEnvD("V4_W5", "W5", k.W5);
    k.B2        = getEnvD("V4_B2", "B2", k.B2);
    k.W6        = getEnvD("V4_W6", "W6", k.W6);
    k.B3        = getEnvD("V4_B3", "B3", k.B3);
    k.URG_SCALE = getEnvD("V4_URG_SCALE", "URG_SCALE", k.URG_SCALE);
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
      na_(0), finalScore_(0)
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
>>>>>>> Stashed changes
void SchedulerEnv::loadKnobs(const KnobSet &kn) {
    kn_ = kn;
}

void SchedulerEnv::loadKnobsFromEnv() {
<<<<<<< Updated upstream
    kn_ = KnobSet::fromEnvironment();
}

Req &SchedulerEnv::reqAt(int rid) {
    if (rid >= (int)reqs_.size()) reqs_.resize(rid + 1);
    return reqs_[rid];
=======
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
    R_dpre_.build(T_decode_pre_, S_, 0.15);
    R_dproc_.build(T_decode_proc_, S_, 0.15);
    R_dpost_.build(T_decode_post_, S_, 0.15);

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
>>>>>>> Stashed changes
}

bool SchedulerEnv::setupParams(int K, int numLayers, double S, double latMs, double bwGbps,
                               int bytesPerToken, double SLO1, double SLO2, double tpUB,
                               double tpBase, double distBase, double wTp, double wC,
<<<<<<< Updated upstream
                               const std::vector<std::vector<double>> &tableRows) {
    K_ = K;
=======
                               const std::vector<std::vector<double>> &tableRows)
{
    K_ = max(1, min(8, K));
>>>>>>> Stashed changes
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
<<<<<<< Updated upstream

    for (int i = 0; i < TaskTimeTable::NUM_STEPS; i++) {
        table_.table[i].sizes.clear();
        table_.table[i].durs.clear();
    }

    for (const auto &row : tableRows) {
        if (row.size() >= 7) {
            long long bs = (long long)row[0];
            table_.table[TaskTimeTable::PREFILL_PRE].add(bs, row[1]);
            table_.table[TaskTimeTable::PREFILL_PROC].add(bs, row[2]);
            table_.table[TaskTimeTable::PREFILL_POST].add(bs, row[3]);
            table_.table[TaskTimeTable::DECODE_PRE].add(bs, row[4]);
            table_.table[TaskTimeTable::DECODE_PROC].add(bs, row[5]);
            table_.table[TaskTimeTable::DECODE_POST].add(bs, row[6]);
        }
    }
    table_.finalize();

    reqs_.clear();
    local_busy_ = false;
    remote_busy_.assign(K_, 0);
    remote_busy_until_.assign(K_, 0.0);
    remote_prefill_backlog_.assign(K_, 0.0);
    remote_active_dec_.assign(K_, 0);
    remote_last_was_decode_.assign(K_, 0);
    total_active_dec_ = 0;
    up_inflight_ = 0;
    down_inflight_ = 0;

    tdr_sum_ = 0.0;
    tdr_count_ = 0;
    arr_count_ = 0;
    not_ppost_count_ = 0;
    sum_arr_not_ppost_ = 0.0;
    tpot_gap_sum_ = 0.0;
    tpot_gap_count_ = 0;
    finalScore_ = 0.0;
    outActions_.clear();
    return true;
}

bool SchedulerEnv::setupInitialState(std::istream &in) {
    int K, numLayers, bytesPerToken;
    double S, latMs, bwGbps;
    if (!(in >> K >> S >> latMs >> bwGbps >> bytesPerToken >> numLayers)) return false;

    double SLO1, SLO2, tpUB, tpBase, distBase, wTp, wC;
    if (!(in >> SLO1 >> SLO2 >> tpUB >> tpBase >> distBase >> wTp >> wC)) return false;

    int N;
    if (!(in >> N)) return false;

    vector<vector<double>> rows(N, vector<double>(7));
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < 7; j++) {
            in >> rows[i][j];
        }
    }

    return setupParams(K, numLayers, S, latMs, bwGbps, bytesPerToken,
                       SLO1, SLO2, tpUB, tpBase, distBase, wTp, wC, rows);
}

// =============================================================================
// Event Handlers
// =============================================================================

void SchedulerEnv::onArrival(double t, int rid, int lin) {
    Req &r = reqAt(rid);
    r.id = rid;
    r.L_in = lin;
    r.state = ST_ARRIVED;
    r.arr_time = t;
    r.state_entry_time = t;
    r.exists = true;

    arr_count_++;
    not_ppost_count_++;
    sum_arr_not_ppost_ += t;
}

void SchedulerEnv::onTaskDonePPre(double t, int rem, int rid, double dur) {
    (void)dur;
    local_busy_ = false;
    Req &r = reqAt(rid);
    r.remote = rem;
    r.state = ST_PPRE_INFLIGHT;
    r.state_entry_time = t;
    remote_prefill_backlog_[rem] += table_.prefill_proc((double)r.L_in);
    up_inflight_++;
}

void SchedulerEnv::onTaskDonePProc(double t, int ls, int le, int rem, int rid, double dur) {
    (void)ls;
    remote_busy_[rem] = 0;
    Req &r = reqAt(rid);
    r.layers_done = le;
    remote_prefill_backlog_[rem] = max(0.0, remote_prefill_backlog_[rem] - dur);
    if (le == numLayers_) {
        r.state = ST_PPROC_INFLIGHT;
        r.state_entry_time = t;
        down_inflight_++;
    } else {
        r.state = ST_WAIT_PPROC;
        r.state_entry_time = t;
=======
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
    R_dpre_.build(T_decode_pre_, S_, 0.15);
    R_dproc_.build(T_decode_proc_, S_, 0.15);
    R_dpost_.build(T_decode_post_, S_, 0.15);

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
>>>>>>> Stashed changes
    }
}

void SchedulerEnv::onTaskDonePPost(double t, int rem, int rid, double dur) {
<<<<<<< Updated upstream
    (void)dur;
    (void)rem;
    local_busy_ = false;
    Req &r = reqAt(rid);
    r.state = ST_READY_FOR_DPRE;
    r.state_entry_time = t;

    double tdr = t - r.arr_time;
    tdr_sum_ += tdr;
    tdr_count_++;
    not_ppost_count_--;
    sum_arr_not_ppost_ -= r.arr_time;

    remote_active_dec_[r.remote]++;
    total_active_dec_++;
}

void SchedulerEnv::onTaskDoneDPre(double t, int m, const int* rids, double dur) {
    (void)dur;
    local_busy_ = false;
    for (int j = 0; j < m; j++) {
        int rid = rids[j];
        reqAt(rid).state = ST_DPRE_INFLIGHT;
        reqAt(rid).state_entry_time = t;
    }
    vector<int> remotes_seen(K_, 0);
    for (int j = 0; j < m; j++) remotes_seen[reqAt(rids[j]).remote] = 1;
    for (int rem = 0; rem < K_; rem++) {
        if (remotes_seen[rem]) up_inflight_++;
=======
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
>>>>>>> Stashed changes
    }
}

void SchedulerEnv::onTaskDoneDProc(double t, int rem, int m, const int* rids, double dur) {
<<<<<<< Updated upstream
    (void)dur;
    remote_busy_[rem] = 0;
    for (int j = 0; j < m; j++) {
        int rid = rids[j];
        reqAt(rid).state = ST_DPROC_INFLIGHT;
        reqAt(rid).state_entry_time = t;
    }
    down_inflight_++;
}

void SchedulerEnv::onTaskDoneDPost(double t, int m, const int* rids, double dur) {
    (void)dur;
    local_busy_ = false;
    for (int j = 0; j < m; j++) {
        int rid = rids[j];
        Req &r = reqAt(rid);
        r.tokens_produced++;
        if (r.tokens_produced == 1) {
            r.first_token_time = t;
        } else {
            double gap = t - r.last_token_time;
            r.total_gap_time += gap;
            r.gap_count++;
            tpot_gap_sum_ += gap;
            tpot_gap_count_++;
        }

        r.last_token_time = t;
        r.state = ST_READY_FOR_DPRE;
        r.state_entry_time = t;
=======
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
>>>>>>> Stashed changes
    }
}

void SchedulerEnv::onTransferDone(double t, bool up, int rem, bool prefill, int m, const int* rids) {
<<<<<<< Updated upstream
    (void)rem;
    if (up) {
        up_inflight_ = max(0, up_inflight_ - 1);
        if (prefill) {
            reqAt(rids[0]).state = ST_WAIT_PPROC;
            reqAt(rids[0]).state_entry_time = t;
        } else {
            for (int j = 0; j < m; j++) {
                reqAt(rids[j]).state = ST_WAIT_DPROC;
                reqAt(rids[j]).state_entry_time = t;
            }
        }
    } else {
        down_inflight_ = max(0, down_inflight_ - 1);
        if (prefill) {
            reqAt(rids[0]).state = ST_WAIT_PPOST;
            reqAt(rids[0]).state_entry_time = t;
        } else {
            for (int j = 0; j < m; j++) {
                reqAt(rids[j]).state = ST_WAIT_DPOST;
                reqAt(rids[j]).state_entry_time = t;
            }
        }
    }
}


void SchedulerEnv::onFinish(double t, int rid) {
    (void)t;
    Req &r = reqAt(rid);
    r.state = ST_FINISHED;
    if (r.remote >= 0 && r.remote < K_) {
        remote_active_dec_[r.remote] = max(0, remote_active_dec_[r.remote] - 1);
    }
    total_active_dec_ = max(0, total_active_dec_ - 1);
}


// =============================================================================
// Mathematical Decision Functions (§17, §9, §11-16, §18-19, §21-22)
// =============================================================================

int SchedulerEnv::pickBestRemote(int rid, double t) {
    if (K_ <= 1) return 0;

    double dproc_1 = table_.decode_proc(1.0);
    bool netDominant = (latMs_ > S_ + dproc_1);

    int bestRemote = 0;
    double minCost = 1e300;

    for (int k = 0; k < K_; k++) {
        double availTime = max(t, remote_busy_until_[k]);
        double backlog = remote_prefill_backlog_[k];
        double decLoad = (double)remote_active_dec_[k] * (S_ + dproc_1);

        double fragPenalty = 0.0;
        if (netDominant && total_active_dec_ > 0 && remote_active_dec_[k] == 0) {
            fragPenalty = latMs_ * 1.5;
        }

        double pproc_this = table_.prefill_proc((double)reqs_[rid].L_in);
        double cost = (availTime - t) + backlog + 0.8 * decLoad + fragPenalty + 0.1 * pproc_this;
        if (cost < minCost) {
            minCost = cost;
            bestRemote = k;
        }
    }
    return bestRemote;
}

int SchedulerEnv::computePieceEnd(int rid, int k, double excess_tdr, double excess_tpot) {
    (void)k;
    (void)excess_tdr;
    (void)excess_tpot;
    int ls = reqs_[rid].layers_done;
    int L = numLayers_;
    int lrem = L - ls;

    if (L <= 1 || lrem <= 1) return L;

    double pproc_full = table_.prefill_proc((double)reqs_[rid].L_in);
    double remainingCompute = ((double)lrem / (double)L) * pproc_full;

    if (remainingCompute <= 2.0 * S_ + 0.5 * SLO2_) return L;

    double gamma_val = kn_.W6 * ((double)reqs_[rid].L_in / 1000.0) + kn_.B3;
    int targetPieces = min(L, max(1, (int)floor(gamma_val)));
    int pieceLayers = max(1, (int)ceil((double)L / (double)targetPieces));

    int le = min(L, ls + pieceLayers);
    return le;
}

int SchedulerEnv::computeBetaBatchTarget(int readyCount, double excess_tpot) {
    if (readyCount <= 1) return 1;

    double tp_ratio = wTp_ / (1.0 - wTp_ + 0.05);
    double b_target = kn_.W1 * S_ + kn_.W2 * tp_ratio - kn_.W3 * min(100.0, SLO2_) + kn_.B1;
    if (excess_tpot > 0.2) {
        b_target = max(1.0, b_target - 2.0 * excess_tpot);
    }

    int beta = max(1, (int)floor(b_target));

    // Physical feasibility check: do not batch so many that S + decode_proc(m) exceeds SLO2
    int maxFeasibleBatch = readyCount;
    for (int m = 1; m <= readyCount; m++) {
        double dproc = table_.decode_proc((double)m);
=======
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
    tdrPressure_  = (cOff || slo1Free_) ? 0.0 : min(1.5, max(0.0, (gProjTdr_ - SLO1_) / SLO1_));
    tpotPressure_ = (cOff || slo2Free_) ? 0.0 : min(1.5, max(0.0, (gProjTpot_ - SLO2_) / SLO2_));
}

double SchedulerEnv::awEff() const { return cHopeless_ ? 1.0 : aw_; }
bool SchedulerEnv::latDominant() const {
    return 3.0 * latMs_ > (S_ + T_decode_proc_.at((double)max(1, activeDecTotal_)));
}
int SchedulerEnv::cloudsInUse() const {
    int u = 0; for (int k = 0; k < K_; ++k) if (activeDec_[k] > 0) u++; return max(1, u);
}

double SchedulerEnv::ageD(double w) const {
    double r = w / max(1e-6, SLO2_);
    // Super-linear convex penalty: steep acceleration when latency exceeds SLO
    double penalty = (r > 1.0) ? (r + 2.0 * (r - 1.0) * (r - 1.0)) : r;
    return penalty * (1.0 + tpotPressure_);
}

double SchedulerEnv::ageP(double w) const {
    double r = w / max(1e-6, SLO1_);
    // Super-linear convex penalty: steep acceleration when latency exceeds SLO
    double penalty = (r > 1.0) ? (r + 2.0 * (r - 1.0) * (r - 1.0)) : r;
    return penalty * (1.0 + tdrPressure_);
}

int SchedulerEnv::oldestPend() {
    trim(pendRing_, PEND_PPRE); return pendRing_.empty() ? -1 : pendRing_.front();
}

int SchedulerEnv::pickPPRE(double t) {
    int old = oldestPend(); if (old < 0) return -1;
    if (t - arrOf_[old] > max(3.0 * SLO1_, 200.0 * S_)) return old;
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

int SchedulerEnv::computeBetaBatchTarget(int readyCount, const RateOptimizer &ro) const {
    if (readyCount <= 1) return 1;

    int rBest = ro.bestSize(readyCount);
    if (rBest <= 1) return 1;

    double tp_ratio = wTp_ / (1.0 - wTp_ + 0.05);

    double b_target = 1.0 + tp_ratio * max(0.0, kn_.W1 * S_ + kn_.W2 - kn_.W3 * min(100.0, SLO2_) + kn_.B1);
    if (tpotPressure_ > 0.2) {
        b_target = max(1.0, b_target - 2.0 * tpotPressure_);
    }

    int beta = max(1, (int)floor(b_target));
    beta = min(beta, rBest);

    int maxFeasibleBatch = readyCount;
    for (int m = 1; m <= readyCount; m++) {
        double dproc = T_decode_proc_.at((double)m);
>>>>>>> Stashed changes
        if (dproc > 0.0 && S_ + dproc > 0.95 * SLO2_) {
            maxFeasibleBatch = max(1, m - 1);
            break;
        }
    }

    int target = min(readyCount, min(beta, maxFeasibleBatch));
    return max(1, target);
}

<<<<<<< Updated upstream

double SchedulerEnv::computeTauTimeToLive(double excess_tpot) {
    double tau = kn_.W4 * SLO2_ + kn_.W5 * latMs_ + kn_.B2;
    if (excess_tpot > 0.1) {
=======
double SchedulerEnv::computeTauTimeToLive() const {
    double tau = wTp_ * max(0.0, kn_.W4 * SLO2_ + kn_.W5 * latMs_ + kn_.B2);
    if (tpotPressure_ > 0.1) {
>>>>>>> Stashed changes
        tau *= 0.5;
    }
    return max(0.0, tau);
}

<<<<<<< Updated upstream
bool SchedulerEnv::hasInFlightEvents() const {
    if (local_busy_) return true;
    for (int k = 0; k < K_; k++) {
        if (remote_busy_[k]) return true;
    }
    if (up_inflight_ > 0 || down_inflight_ > 0) return true;
    return false;
}

// =============================================================================
// Frame Step Dispatcher
// =============================================================================

int SchedulerEnv::stepTick(double t) {
    outActions_.clear();

    // Compute online metric pressures
    double proj_tdr = (arr_count_ > 0)
        ? (tdr_sum_ + (not_ppost_count_ * t - sum_arr_not_ppost_)) / (double)arr_count_
        : 0.0;
    double proj_tpot = (tpot_gap_count_ > 0)
        ? tpot_gap_sum_ / (double)tpot_gap_count_
        : 0.0;

    double excess_tdr = max(0.0, (proj_tdr - SLO1_) / max(1e-6, SLO1_));
    double excess_tpot = max(0.0, (proj_tpot - SLO2_) / max(1e-6, SLO2_));

    // Partition all active candidates in a single pass
    vector<int> ppostCandidates;
    vector<int> dpostCandidates;
    vector<int> dpreCandidates;
    vector<int> ppreCandidates;
    vector<vector<int>> pprocCandidates(K_);
    vector<vector<int>> dprocCandidates(K_);

    for (int i = 0; i < (int)reqs_.size(); i++) {
        if (!reqs_[i].exists) continue;
        switch (reqs_[i].state) {
            case ST_WAIT_PPOST:     ppostCandidates.push_back(i); break;
            case ST_WAIT_DPOST:     dpostCandidates.push_back(i); break;
            case ST_READY_FOR_DPRE: dpreCandidates.push_back(i); break;
            case ST_ARRIVED:        ppreCandidates.push_back(i); break;
            case ST_WAIT_PPROC:     if (reqs_[i].remote >= 0 && reqs_[i].remote < K_) pprocCandidates[reqs_[i].remote].push_back(i); break;
            case ST_WAIT_DPROC:     if (reqs_[i].remote >= 0 && reqs_[i].remote < K_) dprocCandidates[reqs_[i].remote].push_back(i); break;
            default: break;
        }
    }

    // 1. Dispatch Local Computer
    if (!local_busy_) {
        bool dpreTrigger = false;
        int betaTarget = computeBetaBatchTarget((int)dpreCandidates.size(), excess_tpot);
        double tau = computeTauTimeToLive(excess_tpot);

        double oldestDpreWait = 0.0;
        if (!dpreCandidates.empty()) {
            for (int rid : dpreCandidates) {
                oldestDpreWait = max(oldestDpreWait, t - reqs_[rid].state_entry_time);
            }
            bool anyRemoteIdle = false;
            for (int rid : dpreCandidates) {
                int rem = reqs_[rid].remote;
                if (rem >= 0 && rem < K_ && !remote_busy_[rem]) {
                    anyRemoteIdle = true;
                    break;
                }
            }
            if ((int)dpreCandidates.size() >= betaTarget || oldestDpreWait >= tau || anyRemoteIdle || !hasInFlightEvents()) {
                dpreTrigger = true;
            }
        }

        if (!dpostCandidates.empty()) {
            string s = "E D POST -1 " + to_string(dpostCandidates.size());
            for (int rid : dpostCandidates) {
                s += " " + to_string(rid);
                reqs_[rid].state = ST_DPOST_INFLIGHT;
            }
            outActions_.push_back(s);
            local_busy_ = true;
        }
        else if (dpreTrigger && !dpreCandidates.empty()) {
            string s = "E D PRE -1 " + to_string(dpreCandidates.size());
            for (int rid : dpreCandidates) {
                s += " " + to_string(rid);
                reqs_[rid].state = ST_DPRE_INFLIGHT;
            }
            outActions_.push_back(s);
            local_busy_ = true;
        }
        else if (!ppostCandidates.empty()) {
            int bestRid = ppostCandidates[0];
            for (int rid : ppostCandidates) {
                if (reqs_[rid].arr_time < reqs_[bestRid].arr_time) {
                    bestRid = rid;
                }
            }
            string s = "E P POST " + to_string(reqs_[bestRid].remote) + " " + to_string(bestRid);
            outActions_.push_back(s);
            reqs_[bestRid].state = ST_PPOST_INFLIGHT;
            local_busy_ = true;
        }
        else if (!ppreCandidates.empty()) {
            int bestRid = ppreCandidates[0];
            for (int rid : ppreCandidates) {
                if (reqs_[rid].arr_time < reqs_[bestRid].arr_time) {
                    bestRid = rid;
                }
            }
            int assignedRemote = pickBestRemote(bestRid, t);
            string s = "E P PRE " + to_string(assignedRemote) + " " + to_string(bestRid);
            outActions_.push_back(s);
            reqs_[bestRid].state = ST_PPRE_INFLIGHT;
            reqs_[bestRid].remote = assignedRemote;
            local_busy_ = true;
        }
        else if (!dpreCandidates.empty()) {
            string s = "E D PRE -1 " + to_string(dpreCandidates.size());
            for (int rid : dpreCandidates) {
                s += " " + to_string(rid);
                reqs_[rid].state = ST_DPRE_INFLIGHT;
            }
            outActions_.push_back(s);
            local_busy_ = true;
        }
    }

    // 2. Dispatch Remote Computers
    for (int rem = 0; rem < K_; rem++) {
        if (remote_busy_[rem]) continue;
        if (pprocCandidates[rem].empty() && dprocCandidates[rem].empty()) continue;

        bool runDproc = false;
        if (!dprocCandidates[rem].empty() && pprocCandidates[rem].empty()) {
            runDproc = true;
        } else if (!dprocCandidates[rem].empty() && !pprocCandidates[rem].empty()) {
            if (remote_last_was_decode_[rem]) {
                runDproc = false;
            } else {
                runDproc = true;
            }
        }

        if (runDproc && !dprocCandidates[rem].empty()) {
            int batchCount = (int)dprocCandidates[rem].size();
            string s = "C" + to_string(rem) + " D PROC " + to_string(rem) + " " + to_string(batchCount);
            for (int rid : dprocCandidates[rem]) {
                s += " " + to_string(rid);
                reqs_[rid].state = ST_DPROC_INFLIGHT;
            }
            outActions_.push_back(s);
            remote_busy_[rem] = 1;
            remote_last_was_decode_[rem] = 1;
            double dur = table_.decode_proc((double)batchCount);
            remote_busy_until_[rem] = t + S_ + dur;
        } else if (!pprocCandidates[rem].empty()) {
            int bestRid = pprocCandidates[rem][0];
            for (int rid : pprocCandidates[rem]) {
                if (reqs_[rid].arr_time < reqs_[bestRid].arr_time) {
                    bestRid = rid;
                }
            }

            int ls = reqs_[bestRid].layers_done;
            int le = computePieceEnd(bestRid, rem, excess_tdr, excess_tpot);

            string s = "C" + to_string(rem) + " P PROC " + to_string(ls) + " " + to_string(le) +
                       " " + to_string(rem) + " " + to_string(bestRid);
            outActions_.push_back(s);

            reqs_[bestRid].state = ST_PPROC_INFLIGHT;
            remote_busy_[rem] = 1;
            remote_last_was_decode_[rem] = 0;
            double dur = table_.prefill_proc((double)reqs_[bestRid].L_in) * ((double)(le - ls) / (double)numLayers_);
            remote_busy_until_[rem] = t + S_ + dur;
        }
    }


    return (int)outActions_.size();
}

int SchedulerEnv::runTick(double t, int numEvents, std::istream &in, std::string &out) {
    for (int k = 0; k < numEvents; k++) {
        string tag;
        in >> tag;
        if (tag == "ARR") {
            int rid; int lin;
            in >> rid >> lin;
            onArrival(t, rid, lin);
        } else if (tag == "TDN") {
            string server;
            in >> server;
            if (server == "E") {
                string w1, w2;
                in >> w1 >> w2;
                if (w1 == "P" && w2 == "PRE") {
                    int rem, rid; double dur;
                    in >> rem >> rid >> dur;
                    onTaskDonePPre(t, rem, rid, dur);
                } else if (w1 == "P" && w2 == "POST") {
                    int rem, rid; double dur;
                    in >> rem >> rid >> dur;
                    onTaskDonePPost(t, rem, rid, dur);
                } else if (w1 == "D" && w2 == "PRE") {
                    int minus1, m;
                    in >> minus1 >> m;
                    vector<int> ids(m);
                    for (int &x : ids) in >> x;
                    double dur; in >> dur;
                    onTaskDoneDPre(t, m, ids.data(), dur);
                } else if (w1 == "D" && w2 == "POST") {
                    int minus1, m;
                    in >> minus1 >> m;
                    vector<int> ids(m);
                    for (int &x : ids) in >> x;
                    double dur; in >> dur;
                    onTaskDoneDPost(t, m, ids.data(), dur);
                }
            } else {
                int remoteIdx = stoi(server.substr(1));
                string w1, w2;
                in >> w1 >> w2;
                if (w1 == "P") {
                    int ls, le, remote, rid; double dur;
                    in >> ls >> le >> remote >> rid >> dur;
                    onTaskDonePProc(t, ls, le, remoteIdx, rid, dur);
                } else {
                    int remote, m;
                    in >> remote >> m;
                    vector<int> ids(m);
                    for (int &x : ids) in >> x;
                    double dur; in >> dur;
                    onTaskDoneDProc(t, remoteIdx, m, ids.data(), dur);
                }
            }
        } else if (tag == "XDN") {
            string dir; in >> dir;
            int remote; long long size; string kind; int m;
            in >> remote >> size >> kind >> m;
            vector<int> ids(m);
            for (int &x : ids) in >> x;
            onTransferDone(t, (dir == "UP"), remote, (kind == "PRE"), m, ids.data());
        } else if (tag == "FIN") {
            int rid; in >> rid;
            onFinish(t, rid);
        }
    }

    int na = stepTick(t);
    ostringstream oss;
    oss << na << "\n";
    for (int j = 0; j < na; j++) {
        oss << outActions_[j] << "\n";
    }
    out += oss.str();
    return na;
}

void SchedulerEnv::runFull(std::istream &input, std::string &actionsOut) {
    if (!setupInitialState(input)) return;

    string line;
    while (input >> line) {
        if (line == "END") break;
        double t = stod(line);
        int e;
        input >> e;
=======
bool SchedulerEnv::holdSatisfied(double t, Ring &q, St want, int nReady, int target, double tau) {
    if (nReady >= target) return true;
    trim(q, want);
    if (!q.empty()) {
        if (t - q.frontTs() >= tau) return true;
        return false;
    }
    return false;
}

bool SchedulerEnv::gateOK(double t) {
    if (activeDecTotal_ == 0) return true;
    int old = oldestPend();
    if (old >= 0 && t - arrOf_[old] > 1.5 * SLO1_ && !slo1Free_) return true;
    if (awEff() >= 0.7 || (slo2Free_ && slo1Free_)) return prefUpQueued_ < 16;
    if (prefUpQueued_ >= 8) return false;
    return max(0.0, upFreeAt_ - t) <= max(0.35 * SLO2_, 4.0 * latMs_);
}

int SchedulerEnv::bestRemote(double t) {
    double decCost = R_dproc_.perItemCost();
    int best = 0; double bestSc = 1e300;
    for (int k = 0; k < K_; ++k) {
        double avail = max(0.0, busyUntil_[k] - t);
        double decBacklog = (double)(dprocReady_[k].size() + decUpInflight_[k]) * (S_ + T_decode_proc_.at(1.0));
        double sc = prefBacklogMs_[k] + avail + 2.0 * (double)activeDec_[k] * decCost + decBacklog;
        if (sc < bestSc) { bestSc = sc; best = k; }
    }
    return best;
}

int SchedulerEnv::pieceEnd(int rid, int k) {
    int ls = layersDone_[rid], L = numLayers_, lrem = L - ls;
    if (L <= 1 || lrem <= 1) return L;

    bool decodeBlocked = dprocReady_[k].size() > 0 || decUpInflight_[k] > 0 || (activeDec_[k] > 0);
    if (!decodeBlocked || wC_ <= 1e-12) return L;

    double tpotRel = max(0.0, gProjTpot_ / max(1e-6, SLO2_) - 1.0);
    double tdrRel  = max(0.0, gProjTdr_  / max(1e-6, SLO1_) - 1.0);
    if (tpotRel <= tdrRel) return L;

    double remaining = (double)lrem / (double)L * fullProcDur_[rid];
    if (remaining <= max(4.0 * S_, 0.5 * SLO2_)) return L;

    double G = max(kn_.W6 * S_, 0.25 * SLO2_);
    if (remaining <= 1.5 * G) return L;

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
    return (tdrExcess + 2.0 * tdrExcess * tdrExcess) + (tpotExcess + 2.0 * tpotExcess * tpotExcess);
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
    return evaluate_sim_state(s) - (0.001 * a.bid_score);
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
    if (s1[0] == 'P') {
        int rid; in >> rid;
        bool isPost = (s2[1] == 'O');
        double dur; in >> dur;
        if (!isPost) {
            enqUp(linOf_[rid], t); prefUpQueued_++;
            int k = cloudOf_[rid]; prefBacklogMs_[k] += fullProcDur_[rid];
        } else {
            enqDown(1.0, t);
        }
    } else if (s1[0] == 'D') {
        int hdr, m; in >> hdr >> m;
        bool isPost = (s2[1] == 'O'), isPre = (!isPost && s2[2] == 'E');
        static int cloudCnt[8];
        if (isPre) for (int k = 0; k < 8; ++k) cloudCnt[k] = 0;
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
    char dir[8], szS[24], step[8]; int rem, m;
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
// Core Engine Execution
// ===========================================================================
void SchedulerEnv::runLocalEngine(double t, bool force) {
    if (!localFree_) return;
    trim(dpostReady_, PEND_DPOST); trim(qPPOST_, PEND_PPOST); trim(dpreReady_, PEND_DPRE);

    int nDPost = dpostReady_.size(), nDPre  = dpreReady_.size();
    int ppost  = qPPOST_.empty() ? -1 : qPPOST_.front();
    int ppre   = pickPPRE(t), oldest = oldestPend();

    int betaTargetDPre = computeBetaBatchTarget(nDPre, R_dpre_);
    int betaTargetDPost = computeBetaBatchTarget(nDPost, R_dpost_);
    double tau = computeTauTimeToLive();

    if (!dpreReady_.empty() && (t - dpreReady_.frontTs() >= tau)) {
        int cap = min(R_dpre_.bestSize(nDPre), betaTargetDPre);
        int n = drain(dpreReady_, PEND_DPRE, max(1, cap));
        if (n > 0) {
            int len = sprintf(outBuf_[na_], "E D PRE -1 %d", n);
            for (int j = 0; j < n; ++j) { len += sprintf(outBuf_[na_] + len, " %d", batchBuf_[j]); st_[batchBuf_[j]] = IN_DPRE; }
            ++na_; localFree_ = false;
            return;
        }
    }

    bool dpreGo  = nDPre > 0 && (force || holdSatisfied(t, dpreReady_, PEND_DPRE, nDPre, betaTargetDPre, tau));
    bool dpostGo = nDPost > 0 && (force || holdSatisfied(t, dpostReady_, PEND_DPOST, nDPost, betaTargetDPost, tau));
    bool ppreGo = ppre >= 0 && (force || gateOK(t));

    vector<Action> legal_moves;

    if (dpostGo) {
        int dpostCap = min(R_dpost_.bestSize(nDPost), betaTargetDPost);
        legal_moves.push_back({A_DPOST, max(1, dpostCap), -1, -1, kn_.URG_SCALE * ageD(t - dpostReady_.frontTs())});
    }
    if (ppost >= 0) {
        legal_moves.push_back({A_PPOST, 1, ppost, -1, kn_.URG_SCALE * ageP(t - qPPOST_.frontTs())});
    }
    if (dpreGo) {
        int dpreCap = min(R_dpre_.bestSize(nDPre), betaTargetDPre);
        legal_moves.push_back({A_DPRE, max(1, dpreCap), -1, -1, kn_.URG_SCALE * ageD(t - dpreReady_.frontTs())});
    }
    if (ppreGo) {
        double ppreWait = min(t - arrOf_[oldest >= 0 ? oldest : ppre], 318000.0);
        legal_moves.push_back({A_PPRE, 1, ppre, bestRemote(t), kn_.URG_SCALE * ageP(ppreWait)});
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
        double sD = nD ? kn_.URG_SCALE * ageD(t - dprocReady_[k].frontTs()) : -1e300;
        double sP = pr >= 0 ? kn_.URG_SCALE * ageP(t - arrOf_[pr]) : -1e300;

        if (sD <= -1e299 && sP <= -1e299) continue;

        if (sD >= sP) {
            int maxFeasibleDProc = nD;
            for (int m = 1; m <= nD; m++) {
                double dproc = T_decode_proc_.at((double)m);
                if (dproc > 0.0 && S_ + dproc > 0.95 * SLO2_) {
                    maxFeasibleDProc = max(1, m - 1);
                    break;
                }
            }
            int cap = min(R_dproc_.bestSize(nD), maxFeasibleDProc);
            if (wTp_ <= 0.05) cap = 1;
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
>>>>>>> Stashed changes
        runTick(t, e, input, actionsOut);
    }
}
