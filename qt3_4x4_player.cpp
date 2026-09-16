// Heuristic player for the 4x4 GQT3 variant (m=4, n=2) -- see
// QT3_Algorithm_Design.md section 12.2 and QT3_4x4_Findings.md.
//
// This is deliberately NOT an exact solver. QT3_4x4_Findings.md documents why
// a certified full/bounded solve of the 4x4 game is out of reach with
// full-width search (exact minimax, or threshold/proof-number search) on a
// single machine: the raw tree is ~16!/9! times bigger than the 3x3 game
// solved exactly by qt3_solver.cpp, and QT3 has no early-termination
// structure (a line can only be scored once the ENTIRE 16-cell board is
// classical), so proof-based methods get essentially no leverage over plain
// alpha-beta.
//
// What this program *does* do:
//   - Reuses the exact game mechanics from qt3_solver.cpp (hypergraph
//     entanglement test, edge->cell bijection collapse enumeration, exact
//     harmonic scoring with the section 2.6 tie-break), generalised to the
//     4x4 board (16 cells, 10 lines, SCALE = 2520 = lcm(1..10)).
//   - Replaces the exhaustive solve with depth-limited alpha-beta plus a
//     hand-written positional evaluation function, so it can pick strong
//     moves in bounded time.
//   - Keeps a transposition table (keyed on canonical D4-symmetric state +
//     remaining depth) purely for search speed / move ordering -- it is not
//     claimed to make the result exact.
//   - Plays real games to full completion (using the exact rules, not the
//     heuristic) so the ACTUAL final margin printed at the end of a game is
//     exact and correct -- only the *move choices* during play are heuristic.
//
// Usage: qt3_4x4_player [selfplay|vsrandom N|depth]

#include <vector>
#include <array>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <random>

using namespace std;

static constexpr int BOARD_M = 4;
static constexpr int BOARD_S = 16;
static constexpr int NUM_LINES = 10;
static constexpr int EMPTY = 0, P1 = 1, P2 = 2;
static constexpr int MOVE = 0, COLLAPSE = 1;
static constexpr int64_t SCALE = 2520; // lcm(1..10)
static constexpr int MAXN = 2;
static int DEPTH_LIMIT = 4; // plies (MOVE and COLLAPSE nodes both count); overridable via argv

// ---------------------------------------------------------------------
// Lines (4 rows + 4 cols + 2 diagonals) and the D4 permutations of a 4x4
// grid, generated from (r,c) transforms (identity, 3 rotations, 4
// reflections) with a startup self-check against the line set.
// ---------------------------------------------------------------------

static const int LINES[NUM_LINES][BOARD_M] = {
    {0,1,2,3}, {4,5,6,7}, {8,9,10,11}, {12,13,14,15},
    {0,4,8,12}, {1,5,9,13}, {2,6,10,14}, {3,7,11,15},
    {0,5,10,15}, {3,6,9,12}
};

static const int8_t PERMS[8][16] = {
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},       // identity
    {3,7,11,15,2,6,10,14,1,5,9,13,0,4,8,12},       // rot90
    {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0},       // rot180
    {12,8,4,0,13,9,5,1,14,10,6,2,15,11,7,3},       // rot270
    {12,13,14,15,8,9,10,11,4,5,6,7,0,1,2,3},       // flip vertical axis
    {3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12},       // flip horizontal axis
    {0,4,8,12,1,5,9,13,2,6,10,14,3,7,11,15},       // transpose
    {15,11,7,3,14,10,6,2,13,9,5,1,12,8,4,0},       // anti-transpose
};

static void verifySymmetry() {
    for (int g = 0; g < 8; g++) {
        bool seen[16] = {false};
        for (int i = 0; i < 16; i++) {
            int8_t p = PERMS[g][i];
            if (p < 0 || p >= 16 || seen[p]) {
                cerr << "INVARIANT VIOLATION: PERMS[" << g << "] is not a valid permutation\n";
                exit(1);
            }
            seen[p] = true;
        }
        for (int L = 0; L < NUM_LINES; L++) {
            array<int,BOARD_M> img;
            for (int k = 0; k < BOARD_M; k++) img[k] = PERMS[g][LINES[L][k]];
            sort(img.begin(), img.end());
            bool found = false;
            for (int L2 = 0; L2 < NUM_LINES; L2++) {
                array<int,BOARD_M> orig;
                for (int k = 0; k < BOARD_M; k++) orig[k] = LINES[L2][k];
                sort(orig.begin(), orig.end());
                if (orig == img) { found = true; break; }
            }
            if (!found) {
                cerr << "INVARIANT VIOLATION: PERMS[" << g << "] does not map line " << L
                     << " onto another line -- D4 table is wrong\n";
                exit(1);
            }
        }
    }
}

// ---------------------------------------------------------------------
// State representation (fixed-size POD, same design as qt3_solver.cpp)
// ---------------------------------------------------------------------

struct Edge {
    int8_t t = 0;
    int8_t player = 0;
    int8_t n = 0;
    int8_t cells[MAXN] = {0, 0};
};

struct State {
    int8_t owner[BOARD_S] = {0};
    int8_t sub[BOARD_S] = {0};
    Edge edges[BOARD_S];
    int8_t edgeCount = 0;
    int8_t t = 1;
    int8_t mode = MOVE;
};

static inline int mover(int t) { return (t % 2 == 1) ? P1 : P2; }
static inline int opponent(int p) { return 3 - p; }
static inline int decider(const State &S) {
    return (S.mode == MOVE) ? mover(S.t) : opponent(mover(S.t));
}
static inline bool terminal(const State &S) {
    for (int i = 0; i < BOARD_S; i++) if (S.owner[i] == EMPTY) return false;
    return true;
}

// ---------------------------------------------------------------------
// Union-find + rigidity (entanglement) test
// ---------------------------------------------------------------------

struct DSU {
    int8_t parent[BOARD_S];
    DSU() { for (int i = 0; i < BOARD_S; i++) parent[i] = (int8_t)i; }
    int find(int x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    }
    void uni(int a, int b) { a = find(a); b = find(b); if (a != b) parent[a] = b; }
};

static DSU buildDSU(const State &S) {
    DSU dsu;
    for (int i = 0; i < S.edgeCount; i++) {
        const Edge &e = S.edges[i];
        for (int k = 1; k < e.n; k++) dsu.uni(e.cells[0], e.cells[k]);
    }
    return dsu;
}

static void componentStats(const State &S, DSU &dsu, int cell, int &vCount, int &eCount) {
    int root = dsu.find(cell);
    bool touched[BOARD_S] = {false};
    for (int i = 0; i < S.edgeCount; i++)
        for (int k = 0; k < S.edges[i].n; k++) touched[(int)S.edges[i].cells[k]] = true;
    vCount = 0;
    for (int i = 0; i < BOARD_S; i++) if (touched[i] && dsu.find(i) == root) vCount++;
    eCount = 0;
    for (int i = 0; i < S.edgeCount; i++) if (dsu.find(S.edges[i].cells[0]) == root) eCount++;
}

// ---------------------------------------------------------------------
// Move generation / placement
// ---------------------------------------------------------------------

static State place(const State &S, const int *cellsIn, int N) {
    State S2 = S;
    int t = S.t;
    int pl = mover(t);
    Edge e;
    e.t = (int8_t)t;
    e.player = (int8_t)pl;
    e.n = (int8_t)N;
    e.cells[0] = (int8_t)cellsIn[0];
    if (N == 2) {
        e.cells[1] = (int8_t)cellsIn[1];
        if (e.cells[0] > e.cells[1]) swap(e.cells[0], e.cells[1]);
    }
    S2.edges[S2.edgeCount++] = e;

    DSU dsu = buildDSU(S2);
    int vCount, eCount;
    componentStats(S2, dsu, e.cells[0], vCount, eCount);
    if (eCount >= vCount) {
        S2.mode = COLLAPSE;
    } else {
        S2.mode = MOVE;
        S2.t = (int8_t)(t + 1);
    }
    return S2;
}

static void genCombosRec(const int *empties, int k, int N, int start, int *cur, int depth,
                          vector<array<int,MAXN>> &out) {
    if (depth == N) {
        array<int,MAXN> c{};
        for (int i = 0; i < N; i++) c[i] = cur[i];
        out.push_back(c);
        return;
    }
    for (int i = start; i < k; i++) {
        cur[depth] = empties[i];
        genCombosRec(empties, k, N, i + 1, cur, depth + 1, out);
    }
}

static vector<State> generateMoves(const State &S, int N) {
    int empties[BOARD_S], k = 0;
    for (int i = 0; i < BOARD_S; i++) if (S.owner[i] == EMPTY) empties[k++] = i;

    if (k < N) {
        // Forced last classical move (section 2.4). Note this is NOT limited to
        // odd s: an entangled component's size equals however many cells its
        // spooky tree accumulated before the cycle-closing edge arrived, which
        // can be odd even on this even (s=16) board -- a collapse can
        // classicalise 3, 5, ... cells at once, so a single leftover
        // non-classical cell is reachable here too. k==1 is the only possible
        // case for N=2 (k==0 is caught by the terminal test before this runs).
        if (k != 1) {
            cerr << "INVARIANT VIOLATION: forced-move case with " << k << " empties (expected 1)\n";
            exit(1);
        }
        State S2 = S;
        int c = empties[0];
        int t = S.t;
        S2.owner[c] = (int8_t)mover(t);
        S2.sub[c] = (int8_t)t;
        S2.t = (int8_t)(t + 1);
        return {S2};
    }

    vector<array<int,MAXN>> combos;
    int cur[MAXN];
    genCombosRec(empties, k, N, 0, cur, 0, combos);
    vector<State> moves;
    moves.reserve(combos.size());
    for (auto &combo : combos) moves.push_back(place(S, combo.data(), N));
    return moves;
}

// ---------------------------------------------------------------------
// Collapse enumeration (edge -> cell bijection)
// ---------------------------------------------------------------------

static vector<State> generateCollapses(const State &S, int N) {
    DSU dsu = buildDSU(S);
    bool touched[BOARD_S] = {false};
    for (int i = 0; i < S.edgeCount; i++)
        for (int k = 0; k < S.edges[i].n; k++) touched[(int)S.edges[i].cells[k]] = true;

    int violatingRoot = -1, violations = 0;
    for (int i = 0; i < BOARD_S; i++) if (touched[i]) {
        int root = dsu.find(i);
        if (root != i) continue;
        int vCount = 0, eCount = 0;
        for (int j = 0; j < BOARD_S; j++) if (touched[j] && dsu.find(j) == root) vCount++;
        for (int j = 0; j < S.edgeCount; j++) if (dsu.find(S.edges[j].cells[0]) == root) eCount++;
        if (eCount >= vCount) { violatingRoot = root; violations++; }
    }
    if (violations != 1) {
        cerr << "INVARIANT VIOLATION: expected exactly one entangled component at a "
             << "COLLAPSE node, found " << violations << "\n";
        exit(1);
    }

    vector<int> Eidx;
    for (int i = 0; i < S.edgeCount; i++) if (dsu.find(S.edges[i].cells[0]) == violatingRoot) Eidx.push_back(i);

    vector<vector<pair<int,int>>> assignments;
    bool usedMask[BOARD_S] = {false};
    vector<pair<int,int>> current;
    std::function<void(size_t)> bt = [&](size_t idx) {
        if (idx == Eidx.size()) { assignments.push_back(current); return; }
        int ei = Eidx[idx];
        for (int c = 0; c < S.edges[ei].n; c++) {
            int cell = S.edges[ei].cells[c];
            if (!usedMask[cell]) {
                usedMask[cell] = true;
                current.push_back({ei, cell});
                bt(idx + 1);
                current.pop_back();
                usedMask[cell] = false;
            }
        }
    };
    bt(0);

    if (assignments.empty()) {
        cerr << "INVARIANT VIOLATION: no collapse bijection found\n";
        exit(1);
    }
    if (N == 2 && assignments.size() != 2) {
        cerr << "INVARIANT VIOLATION: n=2 collapse must have exactly 2 raw bijections, got "
             << assignments.size() << "\n";
        exit(1);
    }

    bool removeMask[BOARD_S] = {false};
    for (int ei : Eidx) removeMask[ei] = true;

    vector<State> rawChildren;
    for (auto &assign : assignments) {
        State child = S;
        for (auto &pr : assign) {
            int ei = pr.first, cell = pr.second;
            child.owner[cell] = S.edges[ei].player;
            child.sub[cell] = S.edges[ei].t;
        }
        int8_t newCount = 0;
        Edge kept[BOARD_S];
        for (int i = 0; i < S.edgeCount; i++) if (!removeMask[i]) kept[newCount++] = S.edges[i];
        for (int i = 0; i < newCount; i++) child.edges[i] = kept[i];
        child.edgeCount = newCount;
        child.mode = MOVE;
        child.t = (int8_t)(S.t + 1);
        rawChildren.push_back(child);
    }

    vector<State> result;
    for (auto &c : rawChildren) {
        bool dup = false;
        for (auto &r : result) if (memcmp(&r, &c, sizeof(State)) == 0) { dup = true; break; }
        if (!dup) result.push_back(c);
    }
    return result;
}

// ---------------------------------------------------------------------
// Exact terminal scoring (section 5.4 / 2.5-2.6) -- used only at true
// terminal states, never approximated.
// ---------------------------------------------------------------------

static int64_t scoreMargin(const State &S) {
    struct Won { int idx; int owner; array<int,BOARD_M> subsDesc; };
    vector<Won> won;
    for (int L = 0; L < NUM_LINES; L++) {
        int o = S.owner[LINES[L][0]];
        bool mono = (o != EMPTY);
        for (int k = 1; k < BOARD_M && mono; k++) if (S.owner[LINES[L][k]] != o) mono = false;
        if (mono) {
            array<int,BOARD_M> subs;
            for (int k = 0; k < BOARD_M; k++) subs[k] = S.sub[LINES[L][k]];
            sort(subs.rbegin(), subs.rend());
            won.push_back({L, o, subs});
        }
    }
    sort(won.begin(), won.end(), [](const Won &x, const Won &y) {
        if (x.subsDesc != y.subsDesc) return x.subsDesc < y.subsDesc;
        return x.idx < y.idx;
    });
    int64_t v = 0;
    for (size_t i = 0; i < won.size(); i++) {
        int64_t award = SCALE / (int64_t)(i + 1);
        v += (won[i].owner == P1) ? award : -award;
    }
    return v;
}

// ---------------------------------------------------------------------
// D4 canonicalisation for the transposition table (depth-tagged: a node's
// heuristic value depends on how much search remains below it, so entries
// from different remaining-depth are kept separate rather than compared).
// ---------------------------------------------------------------------

static constexpr int KEYLEN = BOARD_S + BOARD_S * 4 + 3; // board + edges + t + mode + depth
struct KeyBuf {
    uint8_t b[KEYLEN + 16] = {0}; // padded
    bool operator==(const KeyBuf &o) const { return memcmp(b, o.b, KEYLEN) == 0; }
};
struct KeyHash {
    size_t operator()(const KeyBuf &k) const {
        uint64_t h = 1469598103934665603ULL;
        for (int i = 0; i < KEYLEN; i++) { h ^= k.b[i]; h *= 1099511628211ULL; }
        return (size_t)h;
    }
};

static void writeRecord(const State &S, const int8_t *perm, int depthRemaining, uint8_t *buf) {
    uint8_t board[BOARD_S];
    for (int i = 0; i < BOARD_S; i++)
        board[perm[i]] = (uint8_t)(((int)S.owner[i] << 4) | ((int)S.sub[i] & 0xF));
    memcpy(buf, board, BOARD_S);

    uint8_t *ep = buf + BOARD_S;
    for (int i = 0; i < BOARD_S; i++) {
        if (i < S.edgeCount) {
            const Edge &e = S.edges[i];
            int8_t c0 = perm[e.cells[0]];
            int8_t c1 = (e.n == 2) ? perm[e.cells[1]] : (int8_t)-1;
            if (e.n == 2 && c0 > c1) swap(c0, c1);
            ep[0] = (uint8_t)e.t;
            ep[1] = (uint8_t)e.player;
            ep[2] = (uint8_t)c0;
            ep[3] = (uint8_t)(c1 < 0 ? 0xFF : c1);
        } else {
            ep[0] = ep[1] = ep[2] = ep[3] = 0;
        }
        ep += 4;
    }
    buf[BOARD_S + BOARD_S * 4] = (uint8_t)S.t;
    buf[BOARD_S + BOARD_S * 4 + 1] = (uint8_t)S.mode;
    buf[BOARD_S + BOARD_S * 4 + 2] = (uint8_t)depthRemaining;
}

static KeyBuf canonKey(const State &S, int depthRemaining) {
    KeyBuf best, cur;
    writeRecord(S, PERMS[0], depthRemaining, best.b);
    for (int g = 1; g < 8; g++) {
        writeRecord(S, PERMS[g], depthRemaining, cur.b);
        if (memcmp(cur.b, best.b, KEYLEN) < 0) best = cur;
    }
    return best;
}

// ---------------------------------------------------------------------
// Heuristic evaluation (used only at non-terminal depth-cutoff nodes).
// Per line: dead if both players have classical cells on it; otherwise a
// weighted combination of classical occupancy (strong) and spooky-mark
// presence (weak, since a spooky mark is not a guaranteed claim). This is a
// hand-tuned static heuristic, not derived from the game's theory -- it
// exists to make the depth-limited player play reasonably, not to bound the
// true game value.
// ---------------------------------------------------------------------

static constexpr int64_t W_CLASSICAL = 1000;
static constexpr int64_t W_SPOOKY = 150;
static constexpr int64_t W_LINE_DONE = 20000; // line already fully classical & monochromatic

static int64_t evalHeuristic(const State &S) {
    int8_t spookyP1[BOARD_S] = {0}, spookyP2[BOARD_S] = {0};
    for (int i = 0; i < S.edgeCount; i++) {
        const Edge &e = S.edges[i];
        for (int k = 0; k < e.n; k++) {
            int c = e.cells[k];
            if (e.player == P1) spookyP1[c]++; else spookyP2[c]++;
        }
    }
    int64_t total = 0;
    for (int L = 0; L < NUM_LINES; L++) {
        int classicalP1 = 0, classicalP2 = 0;
        for (int k = 0; k < BOARD_M; k++) {
            int o = S.owner[LINES[L][k]];
            if (o == P1) classicalP1++;
            else if (o == P2) classicalP2++;
        }
        if (classicalP1 > 0 && classicalP2 > 0) continue; // dead line
        if (classicalP1 == BOARD_M) { total += W_LINE_DONE; continue; }
        if (classicalP2 == BOARD_M) { total -= W_LINE_DONE; continue; }

        int64_t p1 = classicalP1 * W_CLASSICAL, p2 = classicalP2 * W_CLASSICAL;
        for (int k = 0; k < BOARD_M; k++) {
            int c = LINES[L][k];
            if (S.owner[c] != EMPTY) continue;
            if (spookyP1[c] > 0) p1 += W_SPOOKY / spookyP1[c];
            if (spookyP2[c] > 0) p2 += W_SPOOKY / spookyP2[c];
        }
        total += p1 - p2;
    }
    return total;
}

// ---------------------------------------------------------------------
// Depth-limited alpha-beta with heuristic cutoff, D4-symmetric TT, and
// eval-based move ordering.
// ---------------------------------------------------------------------

enum Flag : int8_t { F_EXACT = 0, F_LOWER = 1, F_UPPER = 2 };
struct TTEntry { int64_t value; int8_t flag; };
using TT_t = unordered_map<KeyBuf, TTEntry, KeyHash>;

static long long g_nodes = 0;

static int64_t search(const State &S, int64_t alpha, int64_t beta, int depthRemaining, TT_t &TT, int N) {
    g_nodes++;
    if (terminal(S)) return scoreMargin(S);

    KeyBuf key = canonKey(S, depthRemaining);
    auto it = TT.find(key);
    if (it != TT.end()) {
        const TTEntry &e = it->second;
        if (e.flag == F_EXACT) return e.value;
        if (e.flag == F_LOWER) alpha = max(alpha, e.value);
        else if (e.flag == F_UPPER) beta = min(beta, e.value);
        if (alpha >= beta) return e.value;
    }

    if (depthRemaining == 0) {
        int64_t v = evalHeuristic(S);
        TT[key] = {v, F_EXACT};
        return v;
    }

    vector<State> children = (S.mode == MOVE) ? generateMoves(S, N) : generateCollapses(S, N);
    int dec = decider(S);

    // Cheap 1-ply-eval move ordering (children of a MOVE node are otherwise
    // in raw combinatorial order; COLLAPSE nodes have <=2 children so it's a
    // no-op there).
    vector<pair<int64_t,int>> order(children.size());
    for (size_t i = 0; i < children.size(); i++) {
        int64_t e = terminal(children[i]) ? scoreMargin(children[i]) : evalHeuristic(children[i]);
        order[i] = {e, (int)i};
    }
    if (dec == P1) sort(order.rbegin(), order.rend());
    else sort(order.begin(), order.end());

    int64_t a0 = alpha, b0 = beta, v;
    if (dec == P1) {
        v = INT64_MIN;
        for (auto &pr : order) {
            int64_t cv = search(children[pr.second], alpha, beta, depthRemaining - 1, TT, N);
            if (cv > v) v = cv;
            if (v > alpha) alpha = v;
            if (alpha >= beta) break;
        }
    } else {
        v = INT64_MAX;
        for (auto &pr : order) {
            int64_t cv = search(children[pr.second], alpha, beta, depthRemaining - 1, TT, N);
            if (cv < v) v = cv;
            if (v < beta) beta = v;
            if (alpha >= beta) break;
        }
    }
    int8_t flag = (v <= a0) ? F_UPPER : (v >= b0) ? F_LOWER : F_EXACT;
    TT[key] = {v, flag};
    return v;
}

// Picks the best child of S (a MOVE or COLLAPSE node) under the depth-limited
// heuristic search, per decider(S).
static State chooseMove(const State &S, int depth, long long &nodesOut) {
    vector<State> children = (S.mode == MOVE) ? generateMoves(S, 2) : generateCollapses(S, 2);
    int dec = decider(S);
    TT_t TT;
    TT.reserve(1 << 16);
    g_nodes = 0;
    int64_t best = (dec == P1) ? INT64_MIN : INT64_MAX;
    int bestIdx = 0;
    for (size_t i = 0; i < children.size(); i++) {
        int64_t v = search(children[i], INT64_MIN / 2, INT64_MAX / 2, depth - 1, TT, 2);
        if ((dec == P1 && v > best) || (dec == P2 && v < best)) { best = v; bestIdx = (int)i; }
    }
    nodesOut = g_nodes;
    return children[bestIdx];
}

// ---------------------------------------------------------------------
// Driver: self-play and AI-vs-random benchmark
// ---------------------------------------------------------------------

static string cellName(int c) {
    string s;
    s += (char)('A' + (c / BOARD_M));
    s += (char)('1' + (c % BOARD_M));
    return s;
}

static void printBoard(const State &S) {
    for (int r = 0; r < BOARD_M; r++) {
        for (int c = 0; c < BOARD_M; c++) {
            int i = r * BOARD_M + c;
            if (S.owner[i] == EMPTY) cout << " .  ";
            else cout << (S.owner[i] == P1 ? "X" : "O") << (int)S.sub[i] << (S.sub[i] >= 10 ? "" : " ") << " ";
        }
        cout << "\n";
    }
}

static State randomChild(const State &S, mt19937 &rng) {
    vector<State> children = (S.mode == MOVE) ? generateMoves(S, 2) : generateCollapses(S, 2);
    uniform_int_distribution<size_t> dist(0, children.size() - 1);
    return children[dist(rng)];
}

static void runSelfplay() {
    State S;
    cout << "4x4 GQT3 self-play (heuristic AI vs heuristic AI, depth=" << DEPTH_LIMIT << "):\n\n";
    int plies = 0;
    auto t0 = chrono::steady_clock::now();
    long long totalNodes = 0;
    while (!terminal(S)) {
        long long nodes = 0;
        State before = S;
        S = chooseMove(S, DEPTH_LIMIT, nodes);
        totalNodes += nodes;
        plies++;
        if (before.mode == MOVE) {
            // Report which cell(s) were placed this turn (diff of edges/owner).
            cout << "  turn " << (int)before.t << " (" << (mover(before.t) == P1 ? "P1" : "P2") << "): ";
            if (S.edgeCount > before.edgeCount) {
                Edge &e = S.edges[S.edgeCount - 1];
                cout << "places on {" << cellName(e.cells[0]);
                if (e.n == 2) cout << ", " << cellName(e.cells[1]);
                cout << "}";
                if (S.mode == COLLAPSE) cout << " -> entangled, collapse pending";
            } else {
                // forced classical placement or a collapse just consumed the edges
                cout << "resolves";
            }
            cout << " (searched " << nodes << " nodes)\n";
        } else {
            cout << "    collapse resolved by " << (opponent(mover(before.t)) == P1 ? "P1" : "P2")
                 << " (searched " << nodes << " nodes)\n";
        }
    }
    auto t1 = chrono::steady_clock::now();
    cout << "\nFinal board:\n";
    printBoard(S);
    int64_t v = scoreMargin(S);
    cout << "\nFinal EXACT margin (computed by the real scoring rule, not the heuristic): "
         << v << "/" << SCALE << " = " << (double)v / SCALE << "\n";
    cout << "Total plies: " << plies << ", total search nodes: " << totalNodes
         << ", time: " << chrono::duration<double>(t1 - t0).count() << "s\n";
}

static void runVsRandom(int games) {
    cout << "4x4 GQT3: heuristic AI (depth=" << DEPTH_LIMIT << ") vs uniform-random opponent, "
         << games << " games each side:\n";
    mt19937 rng(12345);
    double aiTotalAsP1 = 0, aiTotalAsP2 = 0;
    int aiWinsAsP1 = 0, aiWinsAsP2 = 0, draws1 = 0, draws2 = 0;
    for (int g = 0; g < games; g++) {
        // AI plays P1, random plays P2.
        State S;
        while (!terminal(S)) {
            long long nodes;
            if (decider(S) == P1) S = chooseMove(S, DEPTH_LIMIT, nodes);
            else S = randomChild(S, rng);
        }
        int64_t v = scoreMargin(S);
        aiTotalAsP1 += (double)v / SCALE;
        if (v > 0) aiWinsAsP1++; else if (v == 0) draws1++;
    }
    for (int g = 0; g < games; g++) {
        // AI plays P2, random plays P1.
        State S;
        while (!terminal(S)) {
            long long nodes;
            if (decider(S) == P2) S = chooseMove(S, DEPTH_LIMIT, nodes);
            else S = randomChild(S, rng);
        }
        int64_t v = scoreMargin(S);
        aiTotalAsP2 += -(double)v / SCALE; // margin is P1-P2; AI is P2 here
        if (v < 0) aiWinsAsP2++; else if (v == 0) draws2++;
    }
    cout << "  AI as P1: won " << aiWinsAsP1 << "/" << games << ", drew " << draws1
         << ", avg margin for AI = " << (aiTotalAsP1 / games) << "\n";
    cout << "  AI as P2: won " << aiWinsAsP2 << "/" << games << ", drew " << draws2
         << ", avg margin for AI = " << (aiTotalAsP2 / games) << "\n";
}

int main(int argc, char **argv) {
    verifySymmetry();
    string mode = (argc > 1) ? argv[1] : "selfplay";

    if (mode == "selfplay") {
        // selfplay [depth]
        if (argc > 2) DEPTH_LIMIT = atoi(argv[2]);
        runSelfplay();
    } else if (mode == "vsrandom") {
        // vsrandom [games] [depth]
        int games = (argc > 2) ? atoi(argv[2]) : 20;
        if (argc > 3) DEPTH_LIMIT = atoi(argv[3]);
        runVsRandom(games);
    } else if (mode == "depth") {
        // depth [depth_limit] -- quick timing probe at the empty board.
        if (argc > 2) DEPTH_LIMIT = atoi(argv[2]);
        State S;
        long long nodes;
        auto t0 = chrono::steady_clock::now();
        chooseMove(S, DEPTH_LIMIT, nodes);
        auto t1 = chrono::steady_clock::now();
        cout << "depth=" << DEPTH_LIMIT << ": " << nodes << " nodes, "
             << chrono::duration<double>(t1 - t0).count() << "s (single root move)\n";
    } else {
        cerr << "usage: qt3_4x4_player [selfplay [depth] | vsrandom [games] [depth] | depth [depth_limit]]\n";
        return 1;
    }
    return 0;
}
