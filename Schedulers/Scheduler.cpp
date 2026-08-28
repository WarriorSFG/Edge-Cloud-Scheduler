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

void SchedulerEnv::loadKnobs(const KnobSet &kn) {
    kn_ = kn;
}

void SchedulerEnv::loadKnobsFromEnv() {
    kn_ = KnobSet::fromEnvironment();
}

Req &SchedulerEnv::reqAt(int rid) {
    if (rid >= (int)reqs_.size()) reqs_.resize(rid + 1);
    return reqs_[rid];
}

bool SchedulerEnv::setupParams(int K, int numLayers, double S, double latMs, double bwGbps,
                               int bytesPerToken, double SLO1, double SLO2, double tpUB,
                               double tpBase, double distBase, double wTp, double wC,
                               const std::vector<std::vector<double>> &tableRows) {
    K_ = K;
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
    }
}

void SchedulerEnv::onTaskDonePPost(double t, int rem, int rid, double dur) {
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
    }
}

void SchedulerEnv::onTaskDoneDProc(double t, int rem, int m, const int* rids, double dur) {
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
    }
}

void SchedulerEnv::onTransferDone(double t, bool up, int rem, bool prefill, int m, const int* rids) {
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
        if (dproc > 0.0 && S_ + dproc > 0.95 * SLO2_) {
            maxFeasibleBatch = max(1, m - 1);
            break;
        }
    }

    int target = min(readyCount, min(beta, maxFeasibleBatch));
    return max(1, target);
}


double SchedulerEnv::computeTauTimeToLive(double excess_tpot) {
    double tau = kn_.W4 * SLO2_ + kn_.W5 * latMs_ + kn_.B2;
    if (excess_tpot > 0.1) {
        tau *= 0.5;
    }
    return max(0.0, tau);
}

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
        runTick(t, e, input, actionsOut);
    }
}
