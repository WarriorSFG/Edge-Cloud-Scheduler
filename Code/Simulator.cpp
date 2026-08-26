// Simulator.cpp — High-performance, parallel simulation engine for the Edge-Cloud Scheduler.
// Implements the faithful interactive Judge protocol and scoring model from ProblemStatement.md.

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <queue>
#include <string>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "../Schedulers/Scheduler.h"

using namespace std;

// ===========================================================================
// Testcase and Result Data Structures
// ===========================================================================

struct RequestData {
    int rid;
    int L_in;
    int L_out;
    double arrival_ms;
};

struct TestCase {
    int testcase_id = 0;
    string profile = "unknown";

    // System parameters
    int K = 1;
    double S = 1.0;
    double latency_in_ms = 1.0;
    double bandwidth_gbps = 1.0;
    int bytes_per_token = 10000;
    int num_layers = 1;

    // Scoring parameters
    double SLO1 = 100.0;
    double SLO2 = 50.0;
    double tp_UB = 1.0;
    double tp_base = 0.0;
    double dist_base = 10.0;
    double w_tp = 0.5;
    double w_c = 0.5;

    // Requests
    int R = 0;
    vector<RequestData> requests;

    // Task-time table
    int N = 0;
    vector<vector<double>> raw_table_rows; // [batch_size, prefill_pre, ...]
    Table T_prefill_pre;
    Table T_prefill_proc;
    Table T_prefill_post;
    Table T_decode_pre;
    Table T_decode_proc;
    Table T_decode_post;

    void buildTables() {
        T_prefill_pre.pts.clear();
        T_prefill_proc.pts.clear();
        T_prefill_post.pts.clear();
        T_decode_pre.pts.clear();
        T_decode_proc.pts.clear();
        T_decode_post.pts.clear();

        for (const auto &row : raw_table_rows) {
            if (row.size() >= 7) {
                T_prefill_pre.add(row[0], row[1]);
                T_prefill_proc.add(row[0], row[2]);
                T_prefill_post.add(row[0], row[3]);
                T_decode_pre.add(row[0], row[4]);
                T_decode_proc.add(row[0], row[5]);
                T_decode_post.add(row[0], row[6]);
            }
        }
        T_prefill_pre.finalize();
        T_prefill_proc.finalize();
        T_prefill_post.finalize();
        T_decode_pre.finalize();
        T_decode_proc.finalize();
        T_decode_post.finalize();
    }
};

struct SimResult {
    int testcase_id = 0;
    string profile = "";
    bool valid = false;
    double score = 0.0;
    double tp = 0.0;
    double rate_score = 0.0;
    double tdr = 0.0;
    double tpot = 0.0;
    double excess_tdr = 0.0;
    double excess_tpot = 0.0;
    double dist = 0.0;
    double wait_score = 0.0;
    double total_time_ms = 0.0;
    int total_tokens = 0;
    string error_msg = "";
};

// ===========================================================================
// FastJudge — Discrete-Event Simulator Engine
// ===========================================================================

enum EventType : uint8_t {
    EV_ARR = 0,
    EV_TDN_PPRE,
    EV_TDN_PPROC,
    EV_TDN_PPOST,
    EV_TDN_DPRE,
    EV_TDN_DPROC,
    EV_TDN_DPOST,
    EV_XDN_UP,
    EV_XDN_DOWN
};

struct SimEvent {
    double time = 0.0;
    uint64_t seq = 0;
    EventType type = EV_ARR;
    int rem = 0;
    int rid = 0;
    int ls = 0, le = 0;
    double dur = 0.0;
    int m = 0;
    vector<int> rids;
    int size_bytes = 0;
    bool is_prefill = false;
};

struct EventComparator {
    bool operator()(const SimEvent &a, const SimEvent &b) const {
        if (a.time != b.time) return a.time > b.time;
        return a.seq > b.seq;
    }
};

class FastJudge {
public:
    static SimResult run(const TestCase &tc, const KnobSet &knobs, SchedulerEnv &env) {
        SimResult res;
        res.testcase_id = tc.testcase_id;
        res.profile = tc.profile;

        env.loadKnobs(knobs);
        if (!env.setupParams(tc.K, tc.num_layers, tc.S, tc.latency_in_ms, tc.bandwidth_gbps,
                            tc.bytes_per_token, tc.SLO1, tc.SLO2, tc.tp_UB,
                            tc.tp_base, tc.dist_base, tc.w_tp, tc.w_c,
                            tc.raw_table_rows)) {
            res.valid = false;
            res.score = 0.0;
            res.error_msg = "Failed to initialize SchedulerEnv";
            return res;
        }

        priority_queue<SimEvent, vector<SimEvent>, EventComparator> pq;
        uint64_t seq_counter = 0;

        // Seed ARR events
        for (const auto &req : tc.requests) {
            SimEvent ev;
            ev.time = req.arrival_ms;
            ev.seq = seq_counter++;
            ev.type = EV_ARR;
            ev.rid = req.rid;
            ev.ls = req.L_in; // Store L_in in ls
            pq.push(ev);
        }

        double up_link_free_at = 0.0;
        double down_link_free_at = 0.0;
        int finished_requests = 0;
        int total_requests = (int)tc.requests.size();

        int tokens_produced[MAXR] = {0};
        double first_tok_t[MAXR] = {0};
        double last_tok_t[MAXR] = {0};
        double ppost_done_t[MAXR] = {0};
        int req_assigned_remote[MAXR] = {0};

        double earliest_arr = 1e300;
        double latest_finish = 0.0;
        for (const auto &req : tc.requests) {
            earliest_arr = min(earliest_arr, req.arrival_ms);
        }

        vector<SimEvent> current_frame;

        while (!pq.empty()) {
            double current_t = pq.top().time;
            current_frame.clear();

            // Collect all events occurring at current_t
            while (!pq.empty() && abs(pq.top().time - current_t) < 1e-9) {
                current_frame.push_back(pq.top());
                pq.pop();
            }

            // Step 1: Dispatch events to SchedulerEnv
            vector<int> fins_to_emit;

            for (const auto &ev : current_frame) {
                switch (ev.type) {
                    case EV_ARR:
                        env.onArrival(current_t, ev.rid, ev.ls /*L_in*/);
                        break;
                    case EV_TDN_PPRE:
                        env.onTaskDonePPre(current_t, ev.rem, ev.rid, ev.dur);
                        // Side effect: Queue UP transfer
                        {
                            int lin = tc.requests[ev.rid].L_in;
                            int size_bytes = lin * tc.bytes_per_token;
                            double xfer_time = tc.latency_in_ms + 8.0 * (double)size_bytes / (tc.bandwidth_gbps * 1e6);
                            double finish_t = max(current_t, up_link_free_at) + xfer_time;
                            up_link_free_at = finish_t;

                            SimEvent xdn;
                            xdn.time = finish_t;
                            xdn.seq = seq_counter++;
                            xdn.type = EV_XDN_UP;
                            xdn.rem = ev.rem;
                            xdn.is_prefill = true;
                            xdn.size_bytes = size_bytes;
                            xdn.m = 1;
                            xdn.rids = {ev.rid};
                            pq.push(xdn);
                        }
                        break;
                    case EV_TDN_PPROC:
                        env.onTaskDonePProc(current_t, ev.ls, ev.le, ev.rem, ev.rid, ev.dur);
                        // Side effect: only if le == num_layers, queue DOWN transfer
                        if (ev.le == tc.num_layers) {
                            int lin = tc.requests[ev.rid].L_in;
                            int size_bytes = lin * tc.bytes_per_token;
                            double xfer_time = tc.latency_in_ms + 8.0 * (double)size_bytes / (tc.bandwidth_gbps * 1e6);
                            double finish_t = max(current_t, down_link_free_at) + xfer_time;
                            down_link_free_at = finish_t;

                            SimEvent xdn;
                            xdn.time = finish_t;
                            xdn.seq = seq_counter++;
                            xdn.type = EV_XDN_DOWN;
                            xdn.rem = ev.rem;
                            xdn.is_prefill = true;
                            xdn.size_bytes = size_bytes;
                            xdn.m = 1;
                            xdn.rids = {ev.rid};
                            pq.push(xdn);
                        }
                        break;
                    case EV_TDN_PPOST:
                        env.onTaskDonePPost(current_t, ev.rem, ev.rid, ev.dur);
                        ppost_done_t[ev.rid] = current_t;
                        break;
                    case EV_TDN_DPRE:
                        env.onTaskDoneDPre(current_t, ev.m, ev.rids.data(), ev.dur);
                        // Side effect: Queue one UP transfer per remote in increasing remote index order
                        {
                            vector<int> rem_members[8];
                            for (int r : ev.rids) {
                                int k = req_assigned_remote[r];
                                if (k >= 0 && k < tc.K) {
                                    rem_members[k].push_back(r);
                                }
                            }
                            for (int k = 0; k < tc.K; ++k) {
                                if (!rem_members[k].empty()) {
                                    int sub_m = (int)rem_members[k].size();
                                    int size_bytes = sub_m * tc.bytes_per_token;
                                    double xfer_time = tc.latency_in_ms + 8.0 * (double)size_bytes / (tc.bandwidth_gbps * 1e6);
                                    double finish_t = max(current_t, up_link_free_at) + xfer_time;
                                    up_link_free_at = finish_t;

                                    SimEvent xdn;
                                    xdn.time = finish_t;
                                    xdn.seq = seq_counter++;
                                    xdn.type = EV_XDN_UP;
                                    xdn.rem = k;
                                    xdn.is_prefill = false;
                                    xdn.size_bytes = size_bytes;
                                    xdn.m = sub_m;
                                    xdn.rids = rem_members[k];
                                    pq.push(xdn);
                                }
                            }
                        }
                        break;
                    case EV_TDN_DPROC:
                        env.onTaskDoneDProc(current_t, ev.rem, ev.m, ev.rids.data(), ev.dur);
                        // Side effect: Queue DOWN transfer
                        {
                            int size_bytes = ev.m * tc.bytes_per_token;
                            double xfer_time = tc.latency_in_ms + 8.0 * (double)size_bytes / (tc.bandwidth_gbps * 1e6);
                            double finish_t = max(current_t, down_link_free_at) + xfer_time;
                            down_link_free_at = finish_t;

                            SimEvent xdn;
                            xdn.time = finish_t;
                            xdn.seq = seq_counter++;
                            xdn.type = EV_XDN_DOWN;
                            xdn.rem = ev.rem;
                            xdn.is_prefill = false;
                            xdn.size_bytes = size_bytes;
                            xdn.m = ev.m;
                            xdn.rids = ev.rids;
                            pq.push(xdn);
                        }
                        break;
                    case EV_TDN_DPOST:
                        env.onTaskDoneDPost(current_t, ev.m, ev.rids.data(), ev.dur);
                        // Side effect: produces tokens and FINs
                        for (int r : ev.rids) {
                            tokens_produced[r]++;
                            if (tokens_produced[r] == 1) {
                                first_tok_t[r] = current_t;
                            }
                            last_tok_t[r] = current_t;
                            if (tokens_produced[r] == tc.requests[r].L_out) {
                                fins_to_emit.push_back(r);
                            }
                        }
                        break;
                    case EV_XDN_UP:
                        env.onTransferDone(current_t, true /*up*/, ev.rem, ev.is_prefill, ev.m, ev.rids.data());
                        break;
                    case EV_XDN_DOWN:
                        env.onTransferDone(current_t, false /*up*/, ev.rem, ev.is_prefill, ev.m, ev.rids.data());
                        break;
                }
            }

            // Process FIN events in the same frame
            for (int fin_rid : fins_to_emit) {
                env.onFinish(current_t, fin_rid);
                finished_requests++;
                latest_finish = max(latest_finish, current_t);
            }

            if (finished_requests == total_requests) {
                break;
            }

            // Step 2: Scheduler step
            int na = env.stepTick(current_t);

            // Step 3: Parse scheduler actions and schedule future TDN events
            for (int j = 0; j < na; ++j) {
                const char *p = env.getAction(j);
                while (*p == ' ') p++;
                char srv = *p;
                int remote_srv = -1;
                if (srv == 'C') {
                    p++;
                    remote_srv = 0;
                    while (*p >= '0' && *p <= '9') {
                        remote_srv = remote_srv * 10 + (*p - '0');
                        p++;
                    }
                } else {
                    p++; // 'E'
                }
                while (*p == ' ') p++;
                char step_kind = *p; // 'P' or 'D'
                p++;
                while (*p == ' ') p++;
                char sub_kind[8];
                int sk_len = 0;
                while (*p > ' ' && sk_len < 7) {
                    sub_kind[sk_len++] = *p++;
                }
                sub_kind[sk_len] = '\0';

                if (step_kind == 'P') {
                    if (strcmp(sub_kind, "PRE") == 0) {
                        int rem = (int)strtol(p, (char**)&p, 10);
                        int rid = (int)strtol(p, (char**)&p, 10);
                        req_assigned_remote[rid] = rem;
                        double dur = tc.T_prefill_pre.at((double)tc.requests[rid].L_in);
                        SimEvent ev;
                        ev.time = current_t + tc.S + dur;
                        ev.seq = seq_counter++;
                        ev.type = EV_TDN_PPRE;
                        ev.rem = rem;
                        ev.rid = rid;
                        ev.dur = dur;
                        pq.push(ev);
                    } else if (strcmp(sub_kind, "PROC") == 0) {
                        int ls = (int)strtol(p, (char**)&p, 10);
                        int le = (int)strtol(p, (char**)&p, 10);
                        int rem = (int)strtol(p, (char**)&p, 10);
                        int rid = (int)strtol(p, (char**)&p, 10);
                        double base_dur = tc.T_prefill_proc.at((double)tc.requests[rid].L_in);
                        double dur = ((double)(le - ls) / tc.num_layers) * base_dur;
                        SimEvent ev;
                        ev.time = current_t + tc.S + dur;
                        ev.seq = seq_counter++;
                        ev.type = EV_TDN_PPROC;
                        ev.ls = ls;
                        ev.le = le;
                        ev.rem = rem;
                        ev.rid = rid;
                        ev.dur = dur;
                        pq.push(ev);
                    } else if (strcmp(sub_kind, "POST") == 0) {
                        int rem = (int)strtol(p, (char**)&p, 10);
                        int rid = (int)strtol(p, (char**)&p, 10);
                        double dur = tc.T_prefill_post.at((double)tc.requests[rid].L_in);
                        SimEvent ev;
                        ev.time = current_t + tc.S + dur;
                        ev.seq = seq_counter++;
                        ev.type = EV_TDN_PPOST;
                        ev.rem = rem;
                        ev.rid = rid;
                        ev.dur = dur;
                        pq.push(ev);
                    }
                } else if (step_kind == 'D') {
                    if (strcmp(sub_kind, "PRE") == 0) {
                        int hdr = (int)strtol(p, (char**)&p, 10); (void)hdr;
                        int m = (int)strtol(p, (char**)&p, 10);
                        SimEvent ev;
                        ev.rids.resize(m);
                        for (int i = 0; i < m; ++i) {
                            ev.rids[i] = (int)strtol(p, (char**)&p, 10);
                        }
                        double dur = tc.T_decode_pre.at((double)m);
                        ev.time = current_t + tc.S + dur;
                        ev.seq = seq_counter++;
                        ev.type = EV_TDN_DPRE;
                        ev.m = m;
                        ev.dur = dur;
                        pq.push(ev);
                    } else if (strcmp(sub_kind, "PROC") == 0) {
                        int rem = (int)strtol(p, (char**)&p, 10);
                        int m = (int)strtol(p, (char**)&p, 10);
                        SimEvent ev;
                        ev.rids.resize(m);
                        for (int i = 0; i < m; ++i) {
                            ev.rids[i] = (int)strtol(p, (char**)&p, 10);
                        }
                        double dur = tc.T_decode_proc.at((double)m);
                        ev.time = current_t + tc.S + dur;
                        ev.seq = seq_counter++;
                        ev.type = EV_TDN_DPROC;
                        ev.rem = rem;
                        ev.m = m;
                        ev.dur = dur;
                        pq.push(ev);
                    } else if (strcmp(sub_kind, "POST") == 0) {
                        int hdr = (int)strtol(p, (char**)&p, 10); (void)hdr;
                        int m = (int)strtol(p, (char**)&p, 10);
                        SimEvent ev;
                        ev.rids.resize(m);
                        for (int i = 0; i < m; ++i) {
                            ev.rids[i] = (int)strtol(p, (char**)&p, 10);
                        }
                        double dur = tc.T_decode_post.at((double)m);
                        ev.time = current_t + tc.S + dur;
                        ev.seq = seq_counter++;
                        ev.type = EV_TDN_DPOST;
                        ev.m = m;
                        ev.dur = dur;
                        pq.push(ev);
                    }
                }
            }
        }

        // Check completion status
        if (finished_requests < total_requests) {
            res.valid = false;
            res.score = 0.0;
            res.error_msg = "Scheduler got stuck: unfinished requests remain but event queue is empty.";
            return res;
        }

        // Faithful score computation
        res.valid = true;
        int total_tokens = 0;
        for (const auto &req : tc.requests) {
            total_tokens += req.L_out;
        }
        res.total_tokens = total_tokens;

        double total_elapsed = latest_finish - earliest_arr;
        res.total_time_ms = total_elapsed;

        double tp = (total_elapsed > 0.0) ? ((double)total_tokens / total_elapsed) : 0.0;
        res.tp = tp;

        double tp_norm = 0.0;
        if (tc.tp_UB > tc.tp_base) {
            tp_norm = max(0.0, min(1.0, (tp - tc.tp_base) / (tc.tp_UB - tc.tp_base)));
        }
        res.rate_score = tp_norm;

        // TDR
        double tdr_sum = 0.0;
        for (const auto &req : tc.requests) {
            tdr_sum += (ppost_done_t[req.rid] - req.arrival_ms);
        }
        double tdr = (total_requests > 0) ? (tdr_sum / total_requests) : 0.0;
        res.tdr = tdr;

        // TPOT
        double gap_sum = 0.0;
        long total_gaps = 0;
        for (const auto &req : tc.requests) {
            if (req.L_out > 1) {
                gap_sum += (last_tok_t[req.rid] - first_tok_t[req.rid]);
                total_gaps += (req.L_out - 1);
            }
        }
        double tpot = (total_gaps > 0) ? (gap_sum / (double)total_gaps) : 0.0;
        res.tpot = tpot;

        double excess_tdr = max(0.0, (tdr - tc.SLO1) / tc.SLO1);
        double excess_tpot = max(0.0, (tpot - tc.SLO2) / tc.SLO2);
        res.excess_tdr = excess_tdr;
        res.excess_tpot = excess_tpot;

        double dist = sqrt(excess_tdr * excess_tdr + excess_tpot * excess_tpot);
        res.dist = dist;

        double wait_norm = 0.0;
        if (tc.dist_base > 0.0) {
            wait_norm = max(0.0, 1.0 - dist / tc.dist_base);
        } else if (tc.dist_base == 0.0 && dist == 0.0) {
            wait_norm = 1.0;
        } else {
            wait_norm = 0.0;
        }
        res.wait_score = wait_norm;

        double norm_score = tc.w_tp * tp_norm + tc.w_c * wait_norm;
        res.score = 1000.0 * norm_score;

        return res;
    }
};

// ===========================================================================
// Fast JSONL Parser
// ===========================================================================

static string extractJsonString(const string &s, const string &key) {
    string pattern = "\"" + key + "\":";
    size_t pos = s.find(pattern);
    if (pos == string::npos) return "";
    pos += pattern.size();
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\"')) pos++;
    size_t end = pos;
    while (end < s.size() && s[end] != '\"' && s[end] != ',' && s[end] != '}') end++;
    return s.substr(pos, end - pos);
}

static double extractJsonDouble(const string &s, const string &key, double defVal = 0.0) {
    string pattern = "\"" + key + "\":";
    size_t pos = s.find(pattern);
    if (pos == string::npos) return defVal;
    pos += pattern.size();
    while (pos < s.size() && s[pos] == ' ') pos++;
    char *endPtr;
    return strtod(s.c_str() + pos, &endPtr);
}

static int extractJsonInt(const string &s, const string &key, int defVal = 0) {
    return (int)extractJsonDouble(s, key, (double)defVal);
}

bool parseTestCaseLine(const string &line, TestCase &tc) {
    if (line.empty() || line[0] != '{') return false;

    tc.testcase_id = extractJsonInt(line, "testcase_id", 0);
    tc.profile = extractJsonString(line, "profile");
    if (tc.profile.empty()) tc.profile = "default";

    tc.K = extractJsonInt(line, "K", 1);
    tc.S = extractJsonDouble(line, "S", 1.0);
    tc.latency_in_ms = extractJsonDouble(line, "latency_in_ms", 1.0);
    tc.bandwidth_gbps = extractJsonDouble(line, "bandwidth_gbps", 1.0);
    tc.bytes_per_token = extractJsonInt(line, "bytes_per_token", 10000);
    tc.num_layers = extractJsonInt(line, "num_layers", 1);

    tc.SLO1 = extractJsonDouble(line, "SLO1", 100.0);
    tc.SLO2 = extractJsonDouble(line, "SLO2", 50.0);
    tc.tp_UB = extractJsonDouble(line, "tp_UB", 1.0);
    tc.tp_base = extractJsonDouble(line, "tp_base", 0.0);
    tc.dist_base = extractJsonDouble(line, "dist_base", 10.0);
    tc.w_tp = extractJsonDouble(line, "w_tp", 0.5);
    tc.w_c = extractJsonDouble(line, "w_c", 0.5);
    tc.R = extractJsonInt(line, "R", 0);
    tc.N = extractJsonInt(line, "N", 0);

    // Parse requests array
    tc.requests.clear();
    size_t reqPos = line.find("\"requests\":");
    if (reqPos != string::npos) {
        size_t arrStart = line.find('[', reqPos);
        size_t arrEnd = line.find(']', arrStart);
        if (arrStart != string::npos && arrEnd != string::npos) {
            size_t p = arrStart;
            while (p < arrEnd) {
                size_t objStart = line.find('{', p);
                if (objStart == string::npos || objStart >= arrEnd) break;
                size_t objEnd = line.find('}', objStart);
                if (objEnd == string::npos || objEnd > arrEnd) break;

                string reqStr = line.substr(objStart, objEnd - objStart + 1);
                RequestData r;
                r.rid = extractJsonInt(reqStr, "rid", (int)tc.requests.size());
                r.L_in = extractJsonInt(reqStr, "L_in", 1);
                r.L_out = extractJsonInt(reqStr, "L_out", 1);
                r.arrival_ms = extractJsonDouble(reqStr, "arrival_ms", 0.0);
                tc.requests.push_back(r);

                p = objEnd + 1;
            }
        }
    }
    if (tc.R <= 0) tc.R = (int)tc.requests.size();

    // Parse task_time_table array
    tc.raw_table_rows.clear();
    size_t ttPos = line.find("\"task_time_table\":");
    if (ttPos != string::npos) {
        size_t arrStart = line.find('[', ttPos);
        size_t arrEnd = line.find(']', arrStart);
        if (arrStart != string::npos && arrEnd != string::npos) {
            size_t p = arrStart;
            while (p < arrEnd) {
                size_t objStart = line.find('{', p);
                if (objStart == string::npos || objStart >= arrEnd) break;
                size_t objEnd = line.find('}', objStart);
                if (objEnd == string::npos || objEnd > arrEnd) break;

                string rowStr = line.substr(objStart, objEnd - objStart + 1);
                double bs = extractJsonDouble(rowStr, "batch_size", 1.0);
                double pp = extractJsonDouble(rowStr, "prefill_pre", -1.0);
                double ppr = extractJsonDouble(rowStr, "prefill_proc", -1.0);
                double ppo = extractJsonDouble(rowStr, "prefill_post", -1.0);
                double dp = extractJsonDouble(rowStr, "decode_pre", -1.0);
                double dpr = extractJsonDouble(rowStr, "decode_proc", -1.0);
                double dpo = extractJsonDouble(rowStr, "decode_post", -1.0);

                tc.raw_table_rows.push_back({bs, pp, ppr, ppo, dp, dpr, dpo});
                p = objEnd + 1;
            }
        }
    }
    tc.N = (int)tc.raw_table_rows.size();

    tc.buildTables();
    return true;
}

bool loadTestCases(const string &path, vector<TestCase> &tests) {
    ifstream fin(path);
    if (!fin.is_open()) {
        cerr << "Error: Could not open testcases file: " << path << endl;
        return false;
    }
    string line;
    while (getline(fin, line)) {
        if (line.empty()) continue;
        TestCase tc;
        if (parseTestCaseLine(line, tc)) {
            tests.push_back(tc);
        }
    }
    return !tests.empty();
}

// ===========================================================================
// Evaluation & Output Reporting
// ===========================================================================

void printSimulationSummary(const vector<SimResult> &results, double wall_time_sec, int repeat_count) {
    if (results.empty()) return;

    double total_score = 0.0;
    double min_score = 1000.0, max_score = 0.0;
    int valid_count = 0;
    map<string, pair<double, int>> profile_scores;

    for (const auto &r : results) {
        if (r.valid) {
            total_score += r.score;
            min_score = min(min_score, r.score);
            max_score = max(max_score, r.score);
            valid_count++;
            profile_scores[r.profile].first += r.score;
            profile_scores[r.profile].second++;
        }
    }

    double avg_score = valid_count > 0 ? (total_score / valid_count) : 0.0;
    long total_simulations = (long)results.size() * repeat_count;
    double sims_per_sec = wall_time_sec > 0 ? (total_simulations / wall_time_sec) : 0.0;

    cout << "\n" << string(70, '=') << "\n";
    cout << "                    SIMULATION SUMMARY REPORT\n";
    cout << string(70, '=') << "\n";
    cout << fixed << setprecision(3);
    cout << "  Total Testcases Evaluated : " << results.size() << "\n";
    if (repeat_count > 1) {
        cout << "  Repeat Iterations         : " << repeat_count << " (Total Runs: " << total_simulations << ")\n";
    }
    cout << "  Valid Simulations         : " << valid_count << " / " << results.size() << "\n";
    cout << "  Average Score             : " << avg_score << " / 1000.0\n";
    cout << "  Score Range               : [" << min_score << ", " << max_score << "]\n";
    cout << "  Elapsed Wall Time         : " << setprecision(4) << wall_time_sec << " s\n";
    cout << "  Simulation Throughput     : " << setprecision(1) << sims_per_sec << " sims/sec\n";
    cout << string(70, '-') << "\n";
    cout << "  Profile Breakdown:\n";
    cout << "    " << left << setw(26) << "Profile" << setw(12) << "Avg Score" << "Count\n";
    cout << "    " << string(46, '-') << "\n";
    for (const auto &p : profile_scores) {
        double p_avg = p.second.second > 0 ? (p.second.first / p.second.second) : 0.0;
        cout << "    " << left << setw(26) << p.first
             << fixed << setprecision(3) << setw(12) << p_avg
             << p.second.second << "\n";
    }
    cout << string(70, '=') << "\n\n";
}

// ===========================================================================
// CLI Driver
// ===========================================================================

int main(int argc, char *argv[]) {
    string jsonl_path = "Testcases/Raw/testcases.jsonl";
    int num_threads = 0; // 0 = default (omp_get_max_threads)
    int repeat = 1;
    bool verbose = false;
    string profile_filter = "";
    bool json_output = false;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "-i" || arg == "--input" || arg == "--jsonl") {
            if (i + 1 < argc) jsonl_path = argv[++i];
        } else if (arg == "-t" || arg == "--threads" || arg == "-j") {
            if (i + 1 < argc) num_threads = atoi(argv[++i]);
        } else if (arg == "-r" || arg == "--repeat") {
            if (i + 1 < argc) repeat = max(1, atoi(argv[++i]));
        } else if (arg == "-p" || arg == "--profile") {
            if (i + 1 < argc) profile_filter = argv[++i];
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "--json") {
            json_output = true;
        } else if (arg == "-h" || arg == "--help") {
            cout << "Usage: Simulator [options] [testcases.jsonl]\n"
                 << "Options:\n"
                 << "  -i, --input, --jsonl <file>  Path to JSONL testcases file (default: Testcases/Raw/testcases.jsonl)\n"
                 << "  -t, --threads, -j <N>        Number of OpenMP threads (default: hardware max)\n"
                 << "  -r, --repeat <N>             Repeat evaluation N times for benchmarking (default: 1)\n"
                 << "  -p, --profile <name>         Filter testcases by profile name\n"
                 << "  -v, --verbose                Print per-testcase results\n"
                 << "  --json                       Emit machine-readable JSON summary\n"
                 << "  -h, --help                   Display this help message\n";
            return 0;
        } else if (arg[0] != '-') {
            jsonl_path = arg;
        }
    }

#ifdef _OPENMP
    if (num_threads > 0) {
        omp_set_num_threads(num_threads);
    }
#endif

    vector<TestCase> all_tests;
    if (!loadTestCases(jsonl_path, all_tests)) {
        // Try relative to parent dir
        if (!loadTestCases("../" + jsonl_path, all_tests)) {
            cerr << "Failed to load testcases from: " << jsonl_path << endl;
            return 1;
        }
    }

    // Filter by profile if requested
    vector<TestCase> tests;
    for (const auto &tc : all_tests) {
        if (profile_filter.empty() || tc.profile == profile_filter) {
            tests.push_back(tc);
        }
    }

    if (tests.empty()) {
        cerr << "No testcases matched filter (total loaded: " << all_tests.size() << ")" << endl;
        return 1;
    }

    KnobSet default_knobs = KnobSet::fromEnvironment();

    int n_tests = (int)tests.size();
    vector<SimResult> results(n_tests);

    auto start_time = chrono::high_resolution_clock::now();

    for (int rep = 0; rep < repeat; ++rep) {
#pragma omp parallel
        {
            auto local_env = make_unique<SchedulerEnv>();
#pragma omp for schedule(dynamic)
            for (int i = 0; i < n_tests; ++i) {
                SimResult res = FastJudge::run(tests[i], default_knobs, *local_env);
                if (rep == repeat - 1) {
                    results[i] = res;
                }
            }
        }
    }

    auto end_time = chrono::high_resolution_clock::now();
    double wall_time_sec = chrono::duration<double>(end_time - start_time).count();

    if (verbose) {
        cout << fixed << setprecision(3);
        cout << "\n" << left << setw(6) << "ID" << setw(22) << "Profile" << setw(10) << "Score"
             << setw(10) << "TP" << setw(10) << "TDR" << setw(10) << "TPOT" << "Status\n";
        cout << string(75, '-') << "\n";
        for (const auto &r : results) {
            cout << left << setw(6) << r.testcase_id
                 << setw(22) << r.profile
                 << setw(10) << r.score
                 << setw(10) << r.tp
                 << setw(10) << r.tdr
                 << setw(10) << r.tpot
                 << (r.valid ? "OK" : r.error_msg) << "\n";
        }
    }

    if (json_output) {
        double total_score = 0;
        int valid_cnt = 0;
        for (const auto &r : results) {
            if (r.valid) { total_score += r.score; valid_cnt++; }
        }
        double avg_score = valid_cnt > 0 ? (total_score / valid_cnt) : 0.0;
        long total_sims = (long)n_tests * repeat;
        double sims_per_sec = wall_time_sec > 0 ? (total_sims / wall_time_sec) : 0.0;

        ostringstream oss;
        oss << fixed << setprecision(3);
        oss << "{\n"
            << "  \"total_testcases\": " << n_tests << ",\n"
            << "  \"valid_count\": " << valid_cnt << ",\n"
            << "  \"avg_score\": " << avg_score << ",\n"
            << "  \"wall_time_sec\": " << setprecision(4) << wall_time_sec << ",\n"
            << "  \"sims_per_sec\": " << setprecision(1) << sims_per_sec << ",\n"
            << "  \"results\": [\n";

        for (size_t i = 0; i < results.size(); ++i) {
            const auto &r = results[i];
            oss << "    {\"id\": " << r.testcase_id
                << ", \"profile\": \"" << r.profile << "\""
                << ", \"valid\": " << (r.valid ? "true" : "false")
                << ", \"score\": " << fixed << setprecision(3) << r.score
                << ", \"tp\": " << setprecision(4) << r.tp
                << ", \"tdr\": " << setprecision(3) << r.tdr
                << ", \"tpot\": " << setprecision(3) << r.tpot
                << "}" << (i + 1 < results.size() ? ",\n" : "\n");
        }
        oss << "  ]\n}\n";

        string payload = oss.str();
        cout << payload;
    } else {
        printSimulationSummary(results, wall_time_sec, repeat);
    }

    return 0;
}
