#pragma once
// =============================================================================
// Schedulers/Scheduler.h — Mathematically Grounded Edge-Cloud Scheduler
// =============================================================================
//
// Implements the mathematical formulations from Mathematics.md:
// - Piecewise-linear interpolation with boundary clamping (§0)
// - Max-plus completion & FIFO network transfer dynamics (§3, §4)
// - Cost-based remote worker assignment (§17, §18)
// - Adaptive prefill chunking gamma(L_in, num_layers) (§9)
// - Dynamic decode batching beta(S, w_tp, SLO2) and time-to-live tau (§11-16, §18-19)
// - Online metric pressure & SLO-aware priority dispatch (§21, §22)
//
// Encapsulated in a thread-safe, re-entrant SchedulerEnv class for fast parallel
// evaluation via Simulator.cpp and hyperparameter optimization via Trainer.py.
// =============================================================================

#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <cstdlib>

static const int MAXR = 8192;

// =============================================================================
// Knob values — Mathematical Parameterization from Mathematics.md
// =============================================================================


struct KnobSet {
    // Dynamic batch threshold beta parameters
    double W1 = 1.477513243077123;  // weight of S in beta
    double W2 = 2.0618695279290793;  // weight of throughput pressure in beta
    double W3 = 0.04881820865819875;  // weight of SLO2 in beta
    double B1 = 2.7398213369682245;  // bias in beta

    // Time-to-live tau parameters
    double W4 = 0.35403860334134957;  // weight of SLO2 in tau
    double W5 = 0.3732723085347498;  // weight of latency in tau
    double B2 = 1.103002753683954;  // bias in tau

    // Input chunking gamma parameters
    double W6 = 5.911434444900584;  // weight of L_in/1000 in gamma
    double B3 = 1.8621518934750698;  // bias in gamma

    // Priority balance knobs
    double URG_SCALE = 2.099102751723999; // DPRE vs PPRE urgency multiplier

    static KnobSet fromEnvironment();
};


// =============================================================================
// Task-Time Table Piecewise-Linear Interpolation (§0)
// =============================================================================

struct StepTable {
    std::vector<long long> sizes;
    std::vector<double> durs;

    void add(long long bs, double v) {
        if (v >= 0.0) {
            sizes.push_back(bs);
            durs.push_back(v);
        }
    }

    void finalize() {
        std::vector<std::pair<long long, double>> pairs;
        pairs.reserve(sizes.size());
        for (size_t i = 0; i < sizes.size(); i++) {
            pairs.push_back({sizes[i], durs[i]});
        }
        std::sort(pairs.begin(), pairs.end());
        sizes.clear(); durs.clear();
        for (auto &p : pairs) {
            sizes.push_back(p.first);
            durs.push_back(p.second);
        }
    }

    double lookup(double x) const {
        if (sizes.empty()) return 0.0;
        if (x <= (double)sizes.front()) return durs.front();
        if (x >= (double)sizes.back())  return durs.back();

        int lo = 0, hi = (int)sizes.size() - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if ((double)sizes[mid] <= x) lo = mid;
            else hi = mid - 1;
        }
        int j = lo;
        if (j + 1 >= (int)sizes.size()) return durs[j];
        if ((double)sizes[j] == x) return durs[j];

        double x0 = (double)sizes[j],   y0 = durs[j];
        double x1 = (double)sizes[j+1], y1 = durs[j+1];
        return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
    }
};

struct TaskTimeTable {
    enum Step {
        PREFILL_PRE = 0, PREFILL_PROC, PREFILL_POST,
        DECODE_PRE, DECODE_PROC, DECODE_POST, NUM_STEPS
    };
    StepTable table[NUM_STEPS];

    void finalize() {
        for (auto &t : table) t.finalize();
    }

    double prefill_pre(double L_in)  const { return table[PREFILL_PRE].lookup(L_in); }
    double prefill_proc(double L_in) const { return table[PREFILL_PROC].lookup(L_in); }
    double prefill_post(double L_in) const { return table[PREFILL_POST].lookup(L_in); }
    double decode_pre(double m)      const { return table[DECODE_PRE].lookup(m); }
    double decode_proc(double m)     const { return table[DECODE_PROC].lookup(m); }
    double decode_post(double m)     const { return table[DECODE_POST].lookup(m); }
};

// Legacy compatibility Table struct for Simulator.cpp
struct Table {
    std::vector<std::pair<double, double>> pts;
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

// =============================================================================
// Request Lifecycle State
// =============================================================================

enum ReqState {
    ST_ARRIVED = 0,
    ST_PPRE_INFLIGHT,
    ST_WAIT_PPROC,
    ST_PPROC_INFLIGHT,
    ST_WAIT_PPOST,
    ST_PPOST_INFLIGHT,
    ST_READY_FOR_DPRE,
    ST_DPRE_INFLIGHT,
    ST_WAIT_DPROC,
    ST_DPROC_INFLIGHT,
    ST_WAIT_DPOST,
    ST_DPOST_INFLIGHT,
    ST_FINISHED
};

struct Req {
    int id = -1;
    long long L_in = 0;
    int remote = -1;
    ReqState state = ST_ARRIVED;
    bool exists = false;

    double arr_time = 0.0;
    double state_entry_time = 0.0;

    int layers_done = 0;
    int tokens_produced = 0;

    double first_token_time = 0.0;
    double last_token_time = 0.0;
    double total_gap_time = 0.0;
    int gap_count = 0;
};

// =============================================================================
// SchedulerEnv — Re-entrant, thread-safe encapsulated scheduler
// =============================================================================

class SchedulerEnv {
public:
    SchedulerEnv();

    void loadKnobs(const KnobSet &kn);
    void loadKnobsFromEnv();

    bool setupInitialState(std::istream &in);

    bool setupParams(int K, int numLayers, double S, double latMs, double bwGbps,
                     int bytesPerToken, double SLO1, double SLO2, double tpUB,
                     double tpBase, double distBase, double wTp, double wC,
                     const std::vector<std::vector<double>> &tableRows);

    // Direct zero-copy event handlers
    void onArrival(double t, int rid, int lin);
    void onTaskDonePPre(double t, int rem, int rid, double dur);
    void onTaskDonePProc(double t, int ls, int le, int rem, int rid, double dur);
    void onTaskDonePPost(double t, int rem, int rid, double dur);
    void onTaskDoneDPre(double t, int m, const int* rids, double dur);
    void onTaskDoneDProc(double t, int rem, int m, const int* rids, double dur);
    void onTaskDoneDPost(double t, int m, const int* rids, double dur);
    void onTransferDone(double t, bool up, int rem, bool prefill, int m, const int* rids);
    void onFinish(double t, int rid);

    // Frame advancement and action retrieval
    int  stepTick(double t);
    const char* getAction(int idx) const {
        if (idx >= 0 && idx < (int)outActions_.size()) return outActions_[idx].c_str();
        return "";
    }

    int runTick(double t, int numEvents, std::istream &in, std::string &out);
    void runFull(std::istream &input, std::string &actionsOut);

    double getScore() const { return finalScore_; }

private:
    KnobSet kn_;

    // System parameters
    int K_ = 1;
    double S_ = 1.0;
    double latMs_ = 1.0;
    double bwGbps_ = 1.0;
    long long bytesPerToken_ = 125000;
    int numLayers_ = 1;

    // Scoring parameters
    double SLO1_ = 30.0;
    double SLO2_ = 15.0;
    double tpUB_ = 0.0625;
    double tpBase_ = 0.0222;
    double distBase_ = 0.0;
    double wTp_ = 0.5;
    double wC_ = 0.5;

    TaskTimeTable table_;

    // Requests storage
    std::vector<Req> reqs_;
    Req &reqAt(int rid);

    // State tracking
    bool local_busy_ = false;
    std::vector<char> remote_busy_;
    std::vector<double> remote_busy_until_;
    std::vector<double> remote_prefill_backlog_;
    std::vector<int> remote_active_dec_;
    std::vector<char> remote_last_was_decode_;
    int total_active_dec_ = 0;

    int up_inflight_ = 0;
    int down_inflight_ = 0;



    // Online metric tracking
    double tdr_sum_ = 0.0;
    long long tdr_count_ = 0;
    long long arr_count_ = 0;
    long long not_ppost_count_ = 0;
    double sum_arr_not_ppost_ = 0.0;

    double tpot_gap_sum_ = 0.0;
    long long tpot_gap_count_ = 0;

    double finalScore_ = 0.0;

    // Output buffer
    std::vector<std::string> outActions_;

    // Internal algorithms
    int pickBestRemote(int rid, double t);
    int computePieceEnd(int rid, int k, double excess_tdr, double excess_tpot);
    int computeBetaBatchTarget(int readyCount, double excess_tpot);
    double computeTauTimeToLive(double excess_tpot);
    bool hasInFlightEvents() const;
};
