// =============================================================================
// Mathematically Grounded Edge-Cloud Scheduler
// =============================================================================
//
// Implements the mathematical formulations, max-plus completion equations,
// FIFO link dynamics, intelligent remote assignment, adaptive prefill chunking,
// dynamic decode batching (beta/tau), and SLO-aware priority dispatch
// as formally specified in Mathematics.md.
//
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

using namespace std;

// -----------------------------------------------------------------------------
// System & Scoring Configuration Models
// -----------------------------------------------------------------------------

struct SystemParams {
    int K = 1;
    double S = 1.0;
    double latency_in_ms = 1.0;
    double bandwidth_gbps = 1.0;
    long long bytes_per_token = 125000;
    int num_layers = 1;
};

struct ScoringParams {
    double SLO1 = 30.0;
    double SLO2 = 15.0;
    double tp_UB = 0.0625;
    double tp_base = 0.0222;
    double dist_base = 0.0;
    double w_tp = 0.5;
    double w_c = 0.5;
};

// -----------------------------------------------------------------------------
// Tuning Knobs per Mathematics.md (§Algorithmic Workflow Parameters)
// -----------------------------------------------------------------------------

struct TuningKnobs {
    double W1 = 1.477513243077123;  // weight of S in beta
    double W2 = 2.0618695279290793;  // weight of normalized throughput weight in beta
    double W3 = 0.04881820865819875;  // weight of SLO2 in beta
    double B1 = 2.7398213369682245;  // bias in beta
    double W4 = 0.35403860334134957;  // weight of SLO2 in tau
    double W5 = 0.3732723085347498;  // weight of latency in tau
    double B2 = 1.103002753683954;  // bias in tau
    double W6 = 5.911434444900584;  // weight of (L_in / 1000) in gamma
    double B3 = 1.8621518934750698;  // bias in gamma
    double URG_SCALE = 2.099102751723999; // urgency multiplier
};


static TuningKnobs g_knobs;

static void parseTuningArgs(int argc, char* argv[]) {
    // 1. Check environment variables
    auto getEnvD = [](const char* name, double def) {
        const char* val = getenv(name);
        return val ? atof(val) : def;
    };
    auto envd = [](const char* name, double def) {
        const char* val = getenv(name);
        return val ? atof(val) : def;
    };
    g_knobs.W1 = getEnvD("W1", envd("V4_W1", 1.477513243077123));
    g_knobs.W2 = getEnvD("W2", envd("V4_W2", 2.0618695279290793));
    g_knobs.W3 = getEnvD("W3", envd("V4_W3", 0.04881820865819875));
    g_knobs.B1 = getEnvD("B1", envd("V4_B1", 2.7398213369682245));
    g_knobs.W4 = getEnvD("W4", envd("V4_W4", 0.35403860334134957));
    g_knobs.W5 = getEnvD("W5", envd("V4_W5", 0.3732723085347498));
    g_knobs.B2 = getEnvD("B2", envd("V4_B2", 1.103002753683954));
    g_knobs.W6 = getEnvD("W6", envd("V4_W6", 5.911434444900584));
    g_knobs.B3 = getEnvD("B3", envd("V4_B3", 1.8621518934750698));
    g_knobs.URG_SCALE = getEnvD("URG_SCALE", envd("V4_URG_SCALE", 2.099102751723999));

    if (argc >= 10) {
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
    } else {
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
}


// -----------------------------------------------------------------------------
// Task-Time Table Piecewise-Linear Interpolation (§0)
// -----------------------------------------------------------------------------

struct StepTable {
    vector<long long> sizes;
    vector<double> durs;

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

    void finalize() {
        vector<pair<long long, double>> pairs;
        pairs.reserve(sizes.size());
        for (size_t i = 0; i < sizes.size(); i++) {
            pairs.push_back({sizes[i], durs[i]});
        }
        sort(pairs.begin(), pairs.end());
        sizes.clear(); durs.clear();
        for (auto &p : pairs) {
            sizes.push_back(p.first);
            durs.push_back(p.second);
        }
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

static SystemParams  g_sys;
static ScoringParams g_score;
static TaskTimeTable g_table;

// -----------------------------------------------------------------------------
// Network Transfer Model (§3, §4)
// -----------------------------------------------------------------------------

inline double transferTime(double lenTokens) {
    return g_sys.latency_in_ms + (8.0 * lenTokens * (double)g_sys.bytes_per_token) / (g_sys.bandwidth_gbps * 1e6);
}

// -----------------------------------------------------------------------------
// Request Lifecycle & State Machine (§5, §10, §11)
// -----------------------------------------------------------------------------

enum ReqState {
    ST_ARRIVED = 0,         // ARR seen; P PRE pending
    ST_PPRE_INFLIGHT,       // P PRE running on local
    ST_WAIT_PPROC,          // input UP XDN received (or previous chunk TDN); waiting for P PROC
    ST_PPROC_INFLIGHT,      // P PROC chunk running on remote
    ST_WAIT_PPOST,          // input DOWN XDN received; waiting for P POST
    ST_PPOST_INFLIGHT,      // P POST running on local
    ST_READY_FOR_DPRE,      // ready for D PRE (output iteration ready)
    ST_DPRE_INFLIGHT,       // member of running D PRE
    ST_WAIT_DPROC,          // decode UP XDN received; waiting for D PROC
    ST_DPROC_INFLIGHT,      // member of running D PROC on remote
    ST_WAIT_DPOST,          // decode DOWN XDN received; waiting for D POST
    ST_DPOST_INFLIGHT,      // member of running D POST on local
    ST_FINISHED             // FIN received; retired
};

struct Req {
    int id = -1;
    long long L_in = 0;
    int remote = -1;                // fixed remote assignment in [0, K)
    ReqState state = ST_ARRIVED;
    bool exists = false;

    double arr_time = 0.0;
    double state_entry_time = 0.0;

    int layers_done = 0;            // number of P PROC layers completed so far
    int tokens_produced = 0;

    double first_token_time = 0.0;
    double last_token_time = 0.0;
    double total_gap_time = 0.0;
    int gap_count = 0;
};

static vector<Req> g_reqs;

static Req &reqAt(int rid) {
    if (rid >= (int)g_reqs.size()) g_reqs.resize(rid + 1);
    return g_reqs[rid];
}

// Resource availability tracking (§1, §2)
static bool g_local_busy = false;
static vector<char> g_remote_busy;
static vector<double> g_remote_busy_until;
static vector<double> g_remote_prefill_backlog; // estimated compute ms of prefill assigned
static vector<int> g_remote_active_dec;         // active decodes assigned to remote k
static vector<char> g_remote_last_was_decode;   // tracks alternating execution on remote k
static int g_total_active_dec = 0;

// Network FIFO queues tracking (§4)
static int g_up_inflight = 0;
static int g_down_inflight = 0;



// Online metrics tracking (§7, §20, §21, §22)
static double g_tdr_sum = 0.0;
static long long g_tdr_count = 0;
static long long g_arr_count = 0;
static long long g_not_ppost_count = 0;
static double g_sum_arr_not_ppost = 0.0;

static double g_tpot_gap_sum = 0.0;
static long long g_tpot_gap_count = 0;

// -----------------------------------------------------------------------------
// Online Pressure & Metric Estimates (§7, §21, §22)
// -----------------------------------------------------------------------------

struct SystemPressures {
    double proj_tdr = 0.0;
    double proj_tpot = 0.0;
    double excess_tdr = 0.0;
    double excess_tpot = 0.0;
    double dist = 0.0;
    double aw = 0.5;
};

static SystemPressures computePressures(double t) {
    SystemPressures sp;
    sp.proj_tdr = (g_arr_count > 0)
        ? (g_tdr_sum + (g_not_ppost_count * t - g_sum_arr_not_ppost)) / (double)g_arr_count
        : 0.0;

    sp.proj_tpot = (g_tpot_gap_count > 0)
        ? g_tpot_gap_sum / (double)g_tpot_gap_count
        : 0.0;

    sp.excess_tdr = max(0.0, (sp.proj_tdr - g_score.SLO1) / max(1e-6, g_score.SLO1));
    sp.excess_tpot = max(0.0, (sp.proj_tpot - g_score.SLO2) / max(1e-6, g_score.SLO2));
    sp.dist = sqrt(sp.excess_tdr * sp.excess_tdr + sp.excess_tpot * sp.excess_tpot);

    double sumW = g_score.w_tp + g_score.w_c;
    sp.aw = (sumW > 1e-9) ? g_score.w_tp / sumW : 0.5;
    return sp;
}

// -----------------------------------------------------------------------------
// Intelligent Remote Assignment (§17, §18)
// -----------------------------------------------------------------------------

static int pickBestRemote(int rid, double t) {
    if (g_sys.K <= 1) return 0;

    double dproc_1 = g_table.decode_proc(1.0);
    bool netDominant = (g_sys.latency_in_ms > g_sys.S + dproc_1);

    int bestRemote = 0;
    double minCost = 1e300;

    for (int k = 0; k < g_sys.K; k++) {
        double availTime = max(t, g_remote_busy_until[k]);
        double backlog = g_remote_prefill_backlog[k];
        double decLoad = (double)g_remote_active_dec[k] * (g_sys.S + dproc_1);

        double fragPenalty = 0.0;
        if (netDominant && g_total_active_dec > 0 && g_remote_active_dec[k] == 0) {
            fragPenalty = g_sys.latency_in_ms * 1.5;
        }

        double pproc_this = g_table.prefill_proc((double)g_reqs[rid].L_in);
        double cost = (availTime - t) + backlog + 0.8 * decLoad + fragPenalty + 0.1 * pproc_this;
        if (cost < minCost) {
            minCost = cost;
            bestRemote = k;
        }
    }
    return bestRemote;
}

// -----------------------------------------------------------------------------
// Adaptive Input Chunking Decision (§9, §Algorithmic Workflow Parameters)
// -----------------------------------------------------------------------------

static int computePieceEnd(int rid, int k, const SystemPressures &pressures) {
    (void)k;
    (void)pressures;
    int ls = g_reqs[rid].layers_done;
    int L = g_sys.num_layers;
    int lrem = L - ls;

    if (L <= 1 || lrem <= 1) return L;

    double pproc_full = g_table.prefill_proc((double)g_reqs[rid].L_in);
    double remainingCompute = ((double)lrem / (double)L) * pproc_full;

    if (remainingCompute <= 2.0 * g_sys.S + 0.5 * g_score.SLO2) return L;

    // gamma(L_in, num_layers) = min(num_layers, max(1, floor(W6 * L_in / 1000 + B3)))
    double gamma_val = g_knobs.W6 * ((double)g_reqs[rid].L_in / 1000.0) + g_knobs.B3;
    int targetPieces = min(L, max(1, (int)floor(gamma_val)));
    int pieceLayers = max(1, (int)ceil((double)L / (double)targetPieces));

    int le = min(L, ls + pieceLayers);
    return le;
}

// -----------------------------------------------------------------------------
// Dynamic Output Batching Thresholds (§11–16, §18–19, §Algorithmic Workflow Parameters)
// -----------------------------------------------------------------------------

static int computeBetaBatchTarget(int readyCount, const SystemPressures &pressures) {
    if (readyCount <= 1) return 1;

    double w_tp = g_score.w_tp;
    double tp_ratio = w_tp / (1.0 - w_tp + 0.05);

    // beta(S, w_tp, SLO2) = max(1, floor(W1 * S + W2 * (w_tp / (1 - w_tp + eps)) - W3 * SLO2 + B1))
    double b_target = g_knobs.W1 * g_sys.S + g_knobs.W2 * tp_ratio - g_knobs.W3 * min(100.0, g_score.SLO2) + g_knobs.B1;
    if (pressures.excess_tpot > 0.2) {
        b_target = max(1.0, b_target - 2.0 * pressures.excess_tpot);
    }

    int beta = max(1, (int)floor(b_target));

    // Physical feasibility check: do not batch so many that S + decode_proc(m) exceeds SLO2
    int maxFeasibleBatch = readyCount;
    for (int m = 1; m <= readyCount; m++) {
        double dproc = g_table.decode_proc((double)m);
        if (dproc > 0.0 && g_sys.S + dproc > 0.95 * g_score.SLO2) {
            maxFeasibleBatch = max(1, m - 1);
            break;
        }
    }

    int target = min(readyCount, min(beta, maxFeasibleBatch));
    return max(1, target);
}


static double computeTauTimeToLive(const SystemPressures &pressures) {
    // tau(SLO2, latency) = max(0, W4 * SLO2 + W5 * latency + B2)
    double tau = g_knobs.W4 * g_score.SLO2 + g_knobs.W5 * g_sys.latency_in_ms + g_knobs.B2;

    if (pressures.excess_tpot > 0.1) {
        tau *= 0.5;
    }
    return max(0.0, tau);
}

// -----------------------------------------------------------------------------
// Event Formatter Helpers
// -----------------------------------------------------------------------------

struct Assignment { string line; };

static string fmtPPre(int remote, int rid) {
    return "E P PRE " + to_string(remote) + " " + to_string(rid);
}
static string fmtPProc(int remote, int rid, int ls, int le) {
    return "C" + to_string(remote) + " P PROC " + to_string(ls) + " " + to_string(le) +
           " " + to_string(remote) + " " + to_string(rid);
}
static string fmtPPost(int remote, int rid) {
    return "E P POST " + to_string(remote) + " " + to_string(rid);
}
static string fmtDPre(const vector<int> &ids) {
    string s = "E D PRE -1 " + to_string(ids.size());
    for (int id : ids) s += " " + to_string(id);
    return s;
}
static string fmtDProc(int remote, const vector<int> &ids) {
    string s = "C" + to_string(remote) + " D PROC " + to_string(remote) + " " + to_string(ids.size());
    for (int id : ids) s += " " + to_string(id);
    return s;
}
static string fmtDPost(const vector<int> &ids) {
    string s = "E D POST -1 " + to_string(ids.size());
    for (int id : ids) s += " " + to_string(id);
    return s;
}

static bool hasInFlightEvents() {
    if (g_local_busy) return true;
    for (int k = 0; k < g_sys.K; k++) {
        if (g_remote_busy[k]) return true;
    }
    if (g_up_inflight > 0 || g_down_inflight > 0) return true;
    return false;
}

// -----------------------------------------------------------------------------
// Startup Configuration Reader
// -----------------------------------------------------------------------------

void readStartupConfig(istream &in) {
    in >> g_sys.K >> g_sys.S >> g_sys.latency_in_ms >> g_sys.bandwidth_gbps
       >> g_sys.bytes_per_token >> g_sys.num_layers;

    in >> g_score.SLO1 >> g_score.SLO2 >> g_score.tp_UB >> g_score.tp_base
       >> g_score.dist_base >> g_score.w_tp >> g_score.w_c;

    int N;
    in >> N;
    for (int i = 0; i < N; i++) {
        long long batch_size;
        double pp, pproc, ppost, dp, dproc, dpost;
        in >> batch_size >> pp >> pproc >> ppost >> dp >> dproc >> dpost;
        auto add = [&](TaskTimeTable::Step s, double v) {
            if (v >= 0) {
                g_table.table[s].sizes.push_back(batch_size);
                g_table.table[s].durs.push_back(v);
            }
        };
        add(TaskTimeTable::PREFILL_PRE,  pp);
        add(TaskTimeTable::PREFILL_PROC, pproc);
        add(TaskTimeTable::PREFILL_POST, ppost);
        add(TaskTimeTable::DECODE_PRE,   dp);
        add(TaskTimeTable::DECODE_PROC,  dproc);
        add(TaskTimeTable::DECODE_POST,  dpost);
    }
    g_table.finalize();

    g_remote_busy.assign(g_sys.K, 0);
    g_remote_busy_until.assign(g_sys.K, 0.0);
    g_remote_prefill_backlog.assign(g_sys.K, 0.0);
    g_remote_active_dec.assign(g_sys.K, 0);
    g_remote_last_was_decode.assign(g_sys.K, 0);
}


// -----------------------------------------------------------------------------
// Main Scheduler Loop
// -----------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    parseTuningArgs(argc, argv);
    readStartupConfig(cin);

    string line;
    while (true) {
        if (!(cin >> line)) break;
        if (line == "END") break;

        double t = stod(line);
        int e;
        cin >> e;

        // Process all events in this frame
        for (int k = 0; k < e; k++) {
            string tag;
            cin >> tag;

            if (tag == "ARR") {
                int rid; long long Lin;
                cin >> rid >> Lin;
                Req &r = reqAt(rid);
                r.id = rid;
                r.L_in = Lin;
                r.state = ST_ARRIVED;
                r.arr_time = t;
                r.state_entry_time = t;
                r.exists = true;

                g_arr_count++;
                g_not_ppost_count++;
                g_sum_arr_not_ppost += t;
            }
            else if (tag == "TDN") {
                string server;
                cin >> server;
                if (server == "E") {
                    string w1, w2;
                    cin >> w1 >> w2;
                    if (w1 == "P" && w2 == "PRE") {
                        int remote, rid; double dur;
                        cin >> remote >> rid >> dur;
                        g_local_busy = false;
                        Req &r = reqAt(rid);
                        r.remote = remote;
                        r.state = ST_PPRE_INFLIGHT;
                        r.state_entry_time = t;
                        g_remote_prefill_backlog[remote] += g_table.prefill_proc((double)r.L_in);
                        g_up_inflight++;
                    } else if (w1 == "P" && w2 == "POST") {
                        int remote, rid; double dur;
                        cin >> remote >> rid >> dur;
                        g_local_busy = false;
                        Req &r = reqAt(rid);
                        r.state = ST_READY_FOR_DPRE;
                        r.state_entry_time = t;

                        double tdr = t - r.arr_time;
                        g_tdr_sum += tdr;
                        g_tdr_count++;
                        g_not_ppost_count--;
                        g_sum_arr_not_ppost -= r.arr_time;

                        g_remote_active_dec[r.remote]++;
                        g_total_active_dec++;
                    } else if (w1 == "D" && w2 == "PRE") {
                        int minus1, m;
                        cin >> minus1 >> m;
                        vector<int> ids(m);
                        for (auto &x : ids) cin >> x;
                        double dur; cin >> dur;
                        g_local_busy = false;
                        for (int rid : ids) {
                            reqAt(rid).state = ST_DPRE_INFLIGHT;
                            reqAt(rid).state_entry_time = t;
                        }
                        vector<int> remotes_seen(g_sys.K, 0);
                        for (int rid : ids) remotes_seen[reqAt(rid).remote] = 1;
                        for (int rem = 0; rem < g_sys.K; rem++) {
                            if (remotes_seen[rem]) g_up_inflight++;
                        }
                    } else if (w1 == "D" && w2 == "POST") {
                        int minus1, m;
                        cin >> minus1 >> m;
                        vector<int> ids(m);
                        for (auto &x : ids) cin >> x;
                        double dur; cin >> dur;
                        g_local_busy = false;
                        for (int rid : ids) {
                            Req &r = reqAt(rid);
                            r.tokens_produced++;
                            if (r.tokens_produced == 1) {
                                r.first_token_time = t;
                            } else {
                                double gap = t - r.last_token_time;
                                r.total_gap_time += gap;
                                r.gap_count++;
                                g_tpot_gap_sum += gap;
                                g_tpot_gap_count++;
                            }
                            r.last_token_time = t;
                            r.state = ST_READY_FOR_DPRE;
                            r.state_entry_time = t;
                        }
                    }
                } else {
                    int remoteIdx = stoi(server.substr(1));
                    string w1, w2;
                    cin >> w1 >> w2;
                    if (w1 == "P") {
                        int ls, le, remote, rid; double dur;
                        cin >> ls >> le >> remote >> rid >> dur;
                        g_remote_busy[remoteIdx] = 0;
                        Req &r = reqAt(rid);
                        r.layers_done = le;
                        g_remote_prefill_backlog[remoteIdx] = max(0.0, g_remote_prefill_backlog[remoteIdx] - dur);
                        if (le == g_sys.num_layers) {
                            r.state = ST_PPROC_INFLIGHT;
                            r.state_entry_time = t;
                            g_down_inflight++;
                        } else {
                            r.state = ST_WAIT_PPROC;
                            r.state_entry_time = t;
                        }
                    } else {
                        int remote, m;
                        cin >> remote >> m;
                        vector<int> ids(m);
                        for (auto &x : ids) cin >> x;
                        double dur; cin >> dur;
                        g_remote_busy[remoteIdx] = 0;
                        for (int rid : ids) {
                            reqAt(rid).state = ST_DPROC_INFLIGHT;
                            reqAt(rid).state_entry_time = t;
                        }
                        g_down_inflight++;
                    }
                }
            }
            else if (tag == "XDN") {
                string dir; cin >> dir;
                int remote; long long size; string kind; int m;
                cin >> remote >> size >> kind >> m;
                vector<int> ids(m);
                for (auto &x : ids) cin >> x;

                if (dir == "UP") {
                    g_up_inflight = max(0, g_up_inflight - 1);
                    if (kind == "PRE") {
                        reqAt(ids[0]).state = ST_WAIT_PPROC;
                        reqAt(ids[0]).state_entry_time = t;
                    } else {
                        for (int rid : ids) {
                            reqAt(rid).state = ST_WAIT_DPROC;
                            reqAt(rid).state_entry_time = t;
                        }
                    }
                } else {
                    g_down_inflight = max(0, g_down_inflight - 1);
                    if (kind == "PRE") {
                        reqAt(ids[0]).state = ST_WAIT_PPOST;
                        reqAt(ids[0]).state_entry_time = t;
                    } else {
                        for (int rid : ids) {
                            reqAt(rid).state = ST_WAIT_DPOST;
                            reqAt(rid).state_entry_time = t;
                        }
                    }
                }
            }
            else if (tag == "FIN") {
                int rid; cin >> rid;
                Req &r = reqAt(rid);
                r.state = ST_FINISHED;
                if (r.remote >= 0 && r.remote < g_sys.K) {
                    g_remote_active_dec[r.remote] = max(0, g_remote_active_dec[r.remote] - 1);
                }
                g_total_active_dec = max(0, g_total_active_dec - 1);
            }
        }

        // ---------------------------------------------------------------------
        // Compute Pressures & State Analysis (§7, §21, §22)
        // ---------------------------------------------------------------------
        SystemPressures pressures = computePressures(t);
        vector<Assignment> out;

        // Partition all active candidates in a single pass
        vector<int> ppostCandidates;
        vector<int> dpostCandidates;
        vector<int> dpreCandidates;
        vector<int> ppreCandidates;
        vector<vector<int>> pprocCandidates(g_sys.K);
        vector<vector<int>> dprocCandidates(g_sys.K);

        for (int i = 0; i < (int)g_reqs.size(); i++) {
            if (!g_reqs[i].exists) continue;
            switch (g_reqs[i].state) {
                case ST_WAIT_PPOST:     ppostCandidates.push_back(i); break;
                case ST_WAIT_DPOST:     dpostCandidates.push_back(i); break;
                case ST_READY_FOR_DPRE: dpreCandidates.push_back(i); break;
                case ST_ARRIVED:        ppreCandidates.push_back(i); break;
                case ST_WAIT_PPROC:     if (g_reqs[i].remote >= 0 && g_reqs[i].remote < g_sys.K) pprocCandidates[g_reqs[i].remote].push_back(i); break;
                case ST_WAIT_DPROC:     if (g_reqs[i].remote >= 0 && g_reqs[i].remote < g_sys.K) dprocCandidates[g_reqs[i].remote].push_back(i); break;
                default: break;
            }
        }

        // ---------------------------------------------------------------------
        // Dispatch Local Computer (at most 1 task)
        // ---------------------------------------------------------------------
        if (!g_local_busy) {
            bool dpreTrigger = false;
            int betaTarget = computeBetaBatchTarget((int)dpreCandidates.size(), pressures);
            double tau = computeTauTimeToLive(pressures);

            double oldestDpreWait = 0.0;
            if (!dpreCandidates.empty()) {
                for (int rid : dpreCandidates) {
                    oldestDpreWait = max(oldestDpreWait, t - g_reqs[rid].state_entry_time);
                }
                bool anyRemoteIdle = false;
                for (int rid : dpreCandidates) {
                    int rem = g_reqs[rid].remote;
                    if (rem >= 0 && rem < g_sys.K && !g_remote_busy[rem]) {
                        anyRemoteIdle = true;
                        break;
                    }
                }
                if ((int)dpreCandidates.size() >= betaTarget || oldestDpreWait >= tau || anyRemoteIdle || !hasInFlightEvents()) {
                    dpreTrigger = true;
                }
            }

            if (!dpostCandidates.empty()) {
                out.push_back({fmtDPost(dpostCandidates)});
                for (int rid : dpostCandidates) {
                    g_reqs[rid].state = ST_DPOST_INFLIGHT;
                }
                g_local_busy = true;
            }
            else if (dpreTrigger && !dpreCandidates.empty()) {
                out.push_back({fmtDPre(dpreCandidates)});
                for (int rid : dpreCandidates) {
                    g_reqs[rid].state = ST_DPRE_INFLIGHT;
                }
                g_local_busy = true;
            }
            else if (!ppostCandidates.empty()) {
                int bestRid = ppostCandidates[0];
                for (int rid : ppostCandidates) {
                    if (g_reqs[rid].arr_time < g_reqs[bestRid].arr_time) {
                        bestRid = rid;
                    }
                }
                out.push_back({fmtPPost(g_reqs[bestRid].remote, bestRid)});
                g_reqs[bestRid].state = ST_PPOST_INFLIGHT;
                g_local_busy = true;
            }
            else if (!ppreCandidates.empty()) {
                int bestRid = ppreCandidates[0];
                for (int rid : ppreCandidates) {
                    if (g_reqs[rid].arr_time < g_reqs[bestRid].arr_time) {
                        bestRid = rid;
                    }
                }
                int assignedRemote = pickBestRemote(bestRid, t);
                out.push_back({fmtPPre(assignedRemote, bestRid)});
                g_reqs[bestRid].state = ST_PPRE_INFLIGHT;
                g_reqs[bestRid].remote = assignedRemote;
                g_local_busy = true;
            }
            else if (!dpreCandidates.empty()) {
                out.push_back({fmtDPre(dpreCandidates)});
                for (int rid : dpreCandidates) {
                    g_reqs[rid].state = ST_DPRE_INFLIGHT;
                }
                g_local_busy = true;
            }
        }

        // ---------------------------------------------------------------------
        // Dispatch Remote Computers (at most 1 task per remote Ck)
        // ---------------------------------------------------------------------
        for (int rem = 0; rem < g_sys.K; rem++) {
            if (g_remote_busy[rem]) continue;
            if (pprocCandidates[rem].empty() && dprocCandidates[rem].empty()) continue;

            bool runDproc = false;
            if (!dprocCandidates[rem].empty() && pprocCandidates[rem].empty()) {
                runDproc = true;
            } else if (!dprocCandidates[rem].empty() && !pprocCandidates[rem].empty()) {
                if (g_remote_last_was_decode[rem]) {
                    runDproc = false;
                } else {
                    runDproc = true;
                }
            }

            if (runDproc && !dprocCandidates[rem].empty()) {
                int batchCount = (int)dprocCandidates[rem].size();
                out.push_back({fmtDProc(rem, dprocCandidates[rem])});
                for (int rid : dprocCandidates[rem]) {
                    g_reqs[rid].state = ST_DPROC_INFLIGHT;
                }
                g_remote_busy[rem] = 1;
                g_remote_last_was_decode[rem] = 1;
                double dur = g_table.decode_proc((double)batchCount);
                g_remote_busy_until[rem] = t + g_sys.S + dur;
            } else if (!pprocCandidates[rem].empty()) {
                int bestRid = pprocCandidates[rem][0];
                for (int rid : pprocCandidates[rem]) {
                    if (g_reqs[rid].arr_time < g_reqs[bestRid].arr_time) {
                        bestRid = rid;
                    }
                }
                int ls = g_reqs[bestRid].layers_done;
                int le = computePieceEnd(bestRid, rem, pressures);

                out.push_back({fmtPProc(rem, bestRid, ls, le)});
                g_reqs[bestRid].state = ST_PPROC_INFLIGHT;
                g_remote_busy[rem] = 1;
                g_remote_last_was_decode[rem] = 0;

                double fullDur = g_table.prefill_proc((double)g_reqs[bestRid].L_in);
                double dur = ((double)(le - ls) / (double)g_sys.num_layers) * fullDur;
                g_remote_busy_until[rem] = t + g_sys.S + dur;
            }
        }

        // ---------------------------------------------------------------------
        // Output Assignments & Flush Stream
        // ---------------------------------------------------------------------
        cout << out.size() << "\n";
        for (auto &a : out) {
            cout << a.line << "\n";
        }
        cout.flush();
    }

    return 0;
}

