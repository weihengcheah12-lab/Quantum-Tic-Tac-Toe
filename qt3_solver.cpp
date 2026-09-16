// Quantum Tic-Tac-Toe (QT3) exact solver -- driver around qt3_engine.h.
// See qt3_engine.h for the model/algorithm notes (sections 2-8, 12.1, 13 of
// QT3_Algorithm_Design.md).
//
// Usage: qt3_solver [baseline|n1|regression|subpos|narrow|save|all]
//
// "save" solves the baseline and writes the full transposition table to
// qt3_3x3_tt.bin -- this is the "data from the solver" that qt3_perfect_ai.cpp
// loads as a warm-start cache for instant, provably-optimal move choice.

#include "qt3_engine.h"
#include <chrono>

static State sampleDeep(State s, int targetEmptiesOrLess, int seedChoice, int N) {
    while (true) {
        int emptyCount = 0;
        for (int i = 0; i < 9; i++) if (s.owner[i] == EMPTY) emptyCount++;
        if (emptyCount <= targetEmptiesOrLess || terminal(s)) return s;
        vector<State> kids = successors(s, N);
        if (kids.empty()) return s;
        int idx = seedChoice % (int)kids.size();
        s = kids[idx];
    }
}

static void runBaseline() {
    State root;
    TT_t TT;
    TT.reserve(1 << 20);
    long long nodes = 0;
    auto t0 = chrono::steady_clock::now();
    int64_t v = alphabeta(root, INT64_MIN / 2, INT64_MAX / 2, TT, true, true, nodes, 2);
    auto t1 = chrono::steady_clock::now();
    double secs = chrono::duration<double>(t1 - t0).count();
    cout << "QT3 baseline (m=3, n=2), TT+D4:\n";
    cout << "  V*(root) = " << v << "/" << SCALE << " = " << (double)v / SCALE
         << (v == SCALE/2 ? "  [matches theorem +0.5]" : "  [UNEXPECTED]") << "\n";
    cout << "  nodes visited: " << nodes << ", distinct canonical states cached: " << TT.size()
         << ", time: " << secs << "s\n";
}

static void runN1() {
    State root;
    TT_t TT;
    long long nodes = 0;
    int64_t v = alphabeta(root, INT64_MIN / 2, INT64_MAX / 2, TT, true, true, nodes, 1);
    cout << "n=1 sanity check (reduces to ordinary tic-tac-toe, expect draw = 0):\n";
    cout << "  V* = " << v << (v == 0 ? "  [PASS]" : "  [FAIL]") << ", nodes=" << nodes << "\n";
}

static void runRegression() {
    cout << "WARNING: this re-solves the root without D4 folding; the reachable\n"
         << "state count is large enough that the transposition table can exceed\n"
         << "available RAM (observed >13GB and still growing on a 16GB machine).\n"
         << "Run only with ample memory headroom.\n";
    State root;
    TT_t TT1;
    TT1.reserve(1 << 20);
    long long n1 = 0;
    auto t0 = chrono::steady_clock::now();
    int64_t v1 = alphabeta(root, INT64_MIN / 2, INT64_MAX / 2, TT1, true, true, n1, 2);
    auto t1 = chrono::steady_clock::now();

    TT_t TT2;
    TT2.reserve(1 << 22);
    long long n2 = 0;
    auto t2 = chrono::steady_clock::now();
    int64_t v2 = alphabeta(root, INT64_MIN / 2, INT64_MAX / 2, TT2, true, false, n2, 2);
    auto t3 = chrono::steady_clock::now();

    cout << "Regression (section 13.3): TT+D4 vs TT-only on the full root:\n";
    cout << "  TT+D4:    V=" << v1 << " nodes=" << n1 << " time="
         << chrono::duration<double>(t1 - t0).count() << "s\n";
    cout << "  TT-only:  V=" << v2 << " nodes=" << n2 << " time="
         << chrono::duration<double>(t3 - t2).count() << "s\n";
    cout << "  " << (v1 == v2 ? "[MATCH]" : "[MISMATCH!]") << "\n";
}

static void runSubpos() {
    State root;
    vector<State> samples = {
        sampleDeep(root, 4, 0, 2),
        sampleDeep(root, 4, 7, 2),
        sampleDeep(root, 4, 13, 2),
    };
    cout << "Sub-position regression (section 13.3): TT+D4 vs raw (TT off, sym off):\n";
    for (size_t i = 0; i < samples.size(); i++) {
        TT_t TTfull;
        long long nf = 0;
        int64_t vfull = alphabeta(samples[i], INT64_MIN / 2, INT64_MAX / 2, TTfull, true, true, nf, 2);
        TT_t dummy;
        long long nr = 0;
        int64_t vraw = alphabeta(samples[i], INT64_MIN / 2, INT64_MAX / 2, dummy, false, false, nr, 2);
        cout << "  sample " << i << ": TT+D4=" << vfull << " raw=" << vraw
             << (vfull == vraw ? "  [MATCH]" : "  [MISMATCH!]")
             << " (full nodes=" << nf << ", raw nodes=" << nr << ")\n";
    }
}

static void runNarrow() {
    // Diagnostic (not part of the proof pipeline): does a null-window /
    // threshold query ("is V*(root) >= T?") cost meaningfully less than the
    // full (-inf,+inf) search? This tells us whether MTD(f)-style repeated
    // narrow searches (or DF-PN) are worth building for the 4x4 port.
    State root;
    TT_t TT;
    TT.reserve(1 << 20);
    long long nodes = 0;
    auto t0 = chrono::steady_clock::now();
    // True root value is 420 (scaled by 840). Ask "is V* >= 420?" via a
    // 1-unit window, the canonical null-window probe.
    int64_t v = alphabeta(root, 419, 420, TT, true, true, nodes, 2);
    auto t1 = chrono::steady_clock::now();
    double secs = chrono::duration<double>(t1 - t0).count();
    cout << "Narrow-window probe \"V* >= 420/840?\" (alpha=419, beta=420):\n";
    cout << "  result = " << v << " (>=420 means YES)\n";
    cout << "  nodes visited: " << nodes << ", TT size: " << TT.size() << ", time: " << secs << "s\n";
    cout << "  (compare to full-window baseline: 77235388 nodes, 168s)\n";
}

static void runSave(const string &path) {
    State root;
    TT_t TT;
    TT.reserve(1 << 20);
    long long nodes = 0;
    cout << "Solving baseline (m=3, n=2) to build the full transposition table...\n";
    auto t0 = chrono::steady_clock::now();
    int64_t v = alphabeta(root, INT64_MIN / 2, INT64_MAX / 2, TT, true, true, nodes, 2);
    auto t1 = chrono::steady_clock::now();
    cout << "  V*(root) = " << v << "/" << SCALE << ", nodes=" << nodes
         << ", entries=" << TT.size()
         << ", solve time=" << chrono::duration<double>(t1 - t0).count() << "s\n";

    long long exactCount = 0;
    for (auto &kv : TT) if (kv.second.flag == F_EXACT) exactCount++;
    cout << "  of which EXACT: " << exactCount << ", bound-only: " << (TT.size() - exactCount) << "\n";

    cout << "Writing " << path << " (" << TT.size() << " entries, "
         << (TT.size() * 57ULL) / (1024 * 1024) << " MiB)...\n";
    auto t2 = chrono::steady_clock::now();
    saveTT(TT, path);
    auto t3 = chrono::steady_clock::now();
    cout << "  done in " << chrono::duration<double>(t3 - t2).count() << "s\n";
}

int main(int argc, char **argv) {
    // Note: "regression" (TT-only vs TT+D4 at the full root) is deliberately
    // NOT part of "all" -- without the 8x D4 folding the transposition table
    // can grow past available RAM. Run it explicitly, with memory headroom,
    // if you want that specific cross-check; the "subpos" checks already
    // validate TT+D4 against a from-scratch raw search on real sub-positions.
    string mode = (argc > 1) ? argv[1] : "all";
    if (mode == "baseline" || mode == "all") runBaseline();
    if (mode == "n1" || mode == "all") runN1();
    if (mode == "subpos" || mode == "all") runSubpos();
    if (mode == "regression") runRegression();
    if (mode == "narrow") runNarrow();
    if (mode == "save") runSave(argc > 2 ? argv[2] : "qt3_3x3_tt.bin");
    return 0;
}
