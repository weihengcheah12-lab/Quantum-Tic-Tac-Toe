// Shared exact QT3 (m=3, n<=2) game engine, factored out of qt3_solver.cpp so
// qt3_perfect_ai.cpp can reuse the identical, already-validated rules and
// search code rather than risking drift from a copy-pasted duplicate.
//
// Implements the model of QT3_Algorithm_Design.md sections 2-8, 12.1:
//   - hypergraph model of spooky marks (union-find / component rigidity test
//     for entanglement, general enough to cover n=1 and n=2 with one code path)
//   - collapse = bijection between component edges and component cells,
//     enumerated by backtracking (matches the "system of distinct
//     representatives" description in section 12.1)
//   - harmonic scoring with the section 2.6 total order on won lines
//   - explicit max/min minimax (section 3's node-ownership table) with
//     alpha-beta pruning, a three-flag transposition table (section 7.2),
//     and D4 symmetry canonicalisation (section 7.3)
//   - all payoffs kept as exact integers scaled by SCALE = 840 = lcm(1..8)
//
// Performance note: State is a fixed-size POD (no heap allocation anywhere
// in the search), and the transposition-table key is a fixed 48-byte record
// hashed with FNV-1a -- this is what makes the search fast, not any change
// to the algorithm itself.
//
// This header is included into exactly one translation unit per program (it
// is never linked across multiple .cpp files in the same binary), so plain
// `static` linkage on everything below is intentional and safe.

#pragma once

#include <vector>
#include <array>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <utility>
#include <iostream>

using namespace std;

static constexpr int EMPTY = 0, P1 = 1, P2 = 2;
static constexpr int MOVE = 0, COLLAPSE = 1;
static constexpr int64_t SCALE = 840; // lcm(1..8)
static constexpr int MAXN = 2;        // max marks per turn we ever run (QT3 baseline)

// ---------------------------------------------------------------------
// State representation (section 4) -- fixed-size POD, no heap allocation.
// ---------------------------------------------------------------------

struct Edge {
    int8_t t = 0;       // 0 = unused slot
    int8_t player = 0;
    int8_t n = 0;        // number of cells this turn placed (1 or 2)
    int8_t cells[MAXN] = {0, 0}; // sorted ascending among the first n entries
};

struct State {
    int8_t owner[9] = {0};
    int8_t sub[9] = {0};      // meaningful only where owner != 0
    Edge edges[9];             // at most 9 turns total; live (uncollapsed) subset
    int8_t edgeCount = 0;
    int8_t t = 1;              // next subscript to be issued
    int8_t mode = MOVE;

    bool operator==(const State &o) const { return memcmp(this, &o, sizeof(State)) == 0; }
};

static inline int mover(int t) { return (t % 2 == 1) ? P1 : P2; }
static inline int opponent(int p) { return 3 - p; }
static inline int decider(const State &S) {
    return (S.mode == MOVE) ? mover(S.t) : opponent(mover(S.t));
}
static inline bool terminal(const State &S) {
    for (int i = 0; i < 9; i++) if (S.owner[i] == EMPTY) return false;
    return true;
}

// ---------------------------------------------------------------------
// Section 2.2 / 12.1: union-find over cells touched by live edges, and the
// rigidity (entanglement) test #edges_in_component >= #vertices_in_component.
// ---------------------------------------------------------------------

struct DSU {
    int8_t parent[9];
    DSU() { for (int i = 0; i < 9; i++) parent[i] = (int8_t)i; }
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

// For a given cell, returns (vertexCountOfItsComponent, edgeCountOfItsComponent).
static void componentStats(const State &S, DSU &dsu, int cell, int &vCount, int &eCount) {
    int root = dsu.find(cell);
    bool touched[9] = {false};
    for (int i = 0; i < S.edgeCount; i++)
        for (int k = 0; k < S.edges[i].n; k++) touched[(int)S.edges[i].cells[k]] = true;
    vCount = 0;
    for (int i = 0; i < 9; i++) if (touched[i] && dsu.find(i) == root) vCount++;
    eCount = 0;
    for (int i = 0; i < S.edgeCount; i++) if (dsu.find(S.edges[i].cells[0]) == root) eCount++;
}

// ---------------------------------------------------------------------
// Section 5.1: move generation / placement
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
        S2.mode = COLLAPSE; // t unchanged: entanglement resolved by the opponent next
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
    int empties[9], k = 0;
    for (int i = 0; i < 9; i++) if (S.owner[i] == EMPTY) empties[k++] = i;

    if (k < N) {
        // Forced last classical move (section 2.4): the only possible case for
        // N=2 is k==1 (k==0 is caught by the terminal test before this runs).
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
// Section 2.3 / 5.3 / 12.1: collapse enumeration (edge -> cell bijection)
// ---------------------------------------------------------------------

static vector<State> generateCollapses(const State &S, int N) {
    DSU dsu = buildDSU(S);
    bool touched[9] = {false};
    for (int i = 0; i < S.edgeCount; i++)
        for (int k = 0; k < S.edges[i].n; k++) touched[(int)S.edges[i].cells[k]] = true;

    // Find the unique violating component (section 2.2: before a move G is a
    // forest, so exactly one component becomes unicyclic/rigid-violating).
    int violatingRoot = -1, violations = 0;
    for (int i = 0; i < 9; i++) if (touched[i]) {
        int root = dsu.find(i);
        if (root != i) continue; // only test each root once
        int vCount = 0, eCount = 0;
        for (int j = 0; j < 9; j++) if (touched[j] && dsu.find(j) == root) vCount++;
        for (int j = 0; j < S.edgeCount; j++) if (dsu.find(S.edges[j].cells[0]) == root) eCount++;
        if (eCount >= vCount) { violatingRoot = root; violations++; }
    }
    if (violations != 1) {
        cerr << "INVARIANT VIOLATION: expected exactly one entangled component at a "
             << "COLLAPSE node, found " << violations << "\n";
        exit(1);
    }

    vector<int> Vcells, Eidx;
    for (int i = 0; i < 9; i++) if (touched[i] && dsu.find(i) == violatingRoot) Vcells.push_back(i);
    for (int i = 0; i < S.edgeCount; i++) if (dsu.find(S.edges[i].cells[0]) == violatingRoot) Eidx.push_back(i);

    // Backtracking enumeration of all bijections E_X -> V_X (Eidx already in
    // ascending-t order because S.edges preserves insertion order under removal).
    vector<vector<pair<int,int>>> assignments;
    bool usedMask[9] = {false};
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

    bool removeMask[9] = {false};
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
        Edge kept[9];
        for (int i = 0; i < S.edgeCount; i++) if (!removeMask[i]) kept[newCount++] = S.edges[i];
        for (int i = 0; i < newCount; i++) child.edges[i] = kept[i];
        child.edgeCount = newCount;
        child.mode = MOVE;
        child.t = (int8_t)(S.t + 1);
        rawChildren.push_back(child);
    }

    // Deduplicate (section 5.3: "if the two produce identical classical boards
    // ... deduplicate to one child. Never more than two.")
    vector<State> result;
    for (auto &c : rawChildren) {
        bool dup = false;
        for (auto &r : result) if (r == c) { dup = true; break; }
        if (!dup) result.push_back(c);
    }
    return result;
}

static vector<State> successors(const State &S, int N) {
    return (S.mode == MOVE) ? generateMoves(S, N) : generateCollapses(S, N);
}

// ---------------------------------------------------------------------
// Section 5.4 / 2.5-2.6: terminal test and scoring
// ---------------------------------------------------------------------

static const int LINES[8][3] = {
    {0,1,2},{3,4,5},{6,7,8},
    {0,3,6},{1,4,7},{2,5,8},
    {0,4,8},{2,4,6}
};

static int64_t scoreMargin(const State &S) {
    struct Won { int idx; int owner; array<int,3> subsDesc; };
    vector<Won> won;
    for (int L = 0; L < 8; L++) {
        int a = LINES[L][0], b = LINES[L][1], c = LINES[L][2];
        if (S.owner[a] != EMPTY && S.owner[a] == S.owner[b] && S.owner[b] == S.owner[c]) {
            array<int,3> subs = {S.sub[a], S.sub[b], S.sub[c]};
            sort(subs.rbegin(), subs.rend()); // descending, per section 2.6 step 1
            won.push_back({L, (int)S.owner[a], subs});
        }
    }
    sort(won.begin(), won.end(), [](const Won &x, const Won &y) {
        if (x.subsDesc != y.subsDesc) return x.subsDesc < y.subsDesc; // smaller = earlier
        return x.idx < y.idx; // section 2.6 step 2: canonical line index tiebreak
    });
    int64_t v = 0;
    for (size_t i = 0; i < won.size(); i++) {
        int64_t award = SCALE / (int64_t)(i + 1);
        v += (won[i].owner == P1) ? award : -award;
    }
    return v;
}

// Returns the list of won lines (index, owner, rank starting at 1) in scoring
// order, for display purposes (e.g. explaining a finished game).
static vector<array<int,3>> wonLinesRanked(const State &S) {
    struct Won { int idx; int owner; array<int,3> subsDesc; };
    vector<Won> won;
    for (int L = 0; L < 8; L++) {
        int a = LINES[L][0], b = LINES[L][1], c = LINES[L][2];
        if (S.owner[a] != EMPTY && S.owner[a] == S.owner[b] && S.owner[b] == S.owner[c]) {
            array<int,3> subs = {S.sub[a], S.sub[b], S.sub[c]};
            sort(subs.rbegin(), subs.rend());
            won.push_back({L, (int)S.owner[a], subs});
        }
    }
    sort(won.begin(), won.end(), [](const Won &x, const Won &y) {
        if (x.subsDesc != y.subsDesc) return x.subsDesc < y.subsDesc;
        return x.idx < y.idx;
    });
    vector<array<int,3>> out; // {lineIdx, owner, rank}
    for (size_t i = 0; i < won.size(); i++) out.push_back({won[i].idx, won[i].owner, (int)i + 1});
    return out;
}

// ---------------------------------------------------------------------
// Section 7.3: D4 symmetry canonicalisation -- written directly into a fixed
// 48-byte record with no intermediate heap allocation.
// ---------------------------------------------------------------------

static const int8_t PERMS[8][9] = {
    {0,1,2,3,4,5,6,7,8},   // identity
    {2,5,8,1,4,7,0,3,6},   // rot90
    {8,7,6,5,4,3,2,1,0},   // rot180
    {6,3,0,7,4,1,8,5,2},   // rot270
    {6,7,8,3,4,5,0,1,2},   // flip vertical axis (rows mirrored)
    {2,1,0,5,4,3,8,7,6},   // flip horizontal axis (cols mirrored)
    {0,3,6,1,4,7,2,5,8},   // transpose (main diagonal)
    {8,5,2,7,4,1,6,3,0},   // anti-transpose
};

static constexpr int KEYLEN = 9 + 9 * 4 + 2; // = 47, padded to 48 in the struct
struct Key48 {
    uint8_t b[48] = {0};
    bool operator==(const Key48 &o) const { return memcmp(b, o.b, KEYLEN) == 0; }
};
struct Key48Hash {
    size_t operator()(const Key48 &k) const {
        uint64_t h = 1469598103934665603ULL;
        for (int i = 0; i < KEYLEN; i++) { h ^= k.b[i]; h *= 1099511628211ULL; }
        return (size_t)h;
    }
};

// Writes the record for state S under permutation perm[9] (identity allowed).
static void writeRecord(const State &S, const int8_t *perm, uint8_t *buf) {
    uint8_t board[9];
    for (int i = 0; i < 9; i++)
        board[perm[i]] = (uint8_t)(((int)S.owner[i] << 4) | ((int)S.sub[i] & 0xF));
    memcpy(buf, board, 9);

    uint8_t *ep = buf + 9;
    for (int i = 0; i < 9; i++) {
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
    buf[9 + 9 * 4] = (uint8_t)S.t;
    buf[9 + 9 * 4 + 1] = (uint8_t)S.mode;
}

static Key48 canonKey(const State &S) {
    Key48 best, cur;
    writeRecord(S, PERMS[0], best.b);
    for (int g = 1; g < 8; g++) {
        writeRecord(S, PERMS[g], cur.b);
        if (memcmp(cur.b, best.b, KEYLEN) < 0) best = cur;
    }
    return best;
}
static Key48 rawKey(const State &S) {
    Key48 k;
    writeRecord(S, PERMS[0], k.b);
    return k;
}

// ---------------------------------------------------------------------
// Section 6-7: alpha-beta with 3-flag transposition table
// ---------------------------------------------------------------------

enum Flag : int8_t { F_EXACT = 0, F_LOWER = 1, F_UPPER = 2 };
struct TTEntry { int64_t value; int8_t flag; };
using TT_t = unordered_map<Key48, TTEntry, Key48Hash>;

static int64_t alphabeta(const State &S, int64_t alpha, int64_t beta,
                          TT_t &TT,
                          bool useTT, bool useSym, long long &nodes, int N) {
    nodes++;
    Key48 key;
    if (useTT) {
        key = useSym ? canonKey(S) : rawKey(S);
        auto it = TT.find(key);
        if (it != TT.end()) {
            const TTEntry &e = it->second;
            if (e.flag == F_EXACT) return e.value;
            if (e.flag == F_LOWER) alpha = max(alpha, e.value);
            else if (e.flag == F_UPPER) beta = min(beta, e.value);
            if (alpha >= beta) return e.value;
        }
    }

    if (terminal(S)) {
        int64_t v = scoreMargin(S);
        if (useTT) TT[key] = {v, F_EXACT};
        return v;
    }

    vector<State> children = successors(S, N);
    int64_t a0 = alpha, b0 = beta, v;
    int dec = decider(S);
    if (dec == P1) {
        v = INT64_MIN;
        for (auto &c : children) {
            int64_t cv = alphabeta(c, alpha, beta, TT, useTT, useSym, nodes, N);
            if (cv > v) v = cv;
            if (v > alpha) alpha = v;
            if (alpha >= beta) break;
        }
    } else {
        v = INT64_MAX;
        for (auto &c : children) {
            int64_t cv = alphabeta(c, alpha, beta, TT, useTT, useSym, nodes, N);
            if (cv < v) v = cv;
            if (v < beta) beta = v;
            if (alpha >= beta) break;
        }
    }

    if (useTT) {
        int8_t flag = (v <= a0) ? F_UPPER : (v >= b0) ? F_LOWER : F_EXACT;
        TT[key] = {v, flag};
    }
    return v;
}

// ---------------------------------------------------------------------
// Binary persistence of a TT_t, used to save the solved game and reload it
// as a warm-start cache for the perfect-play AI. Format: 8-byte entry count,
// then for each entry: 48-byte key + 8-byte value (little-endian int64) +
// 1-byte flag = 57 bytes/entry.
// ---------------------------------------------------------------------

static void saveTT(const TT_t &TT, const string &path) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { cerr << "ERROR: cannot open " << path << " for writing\n"; exit(1); }
    uint64_t count = TT.size();
    fwrite(&count, sizeof(count), 1, f);
    for (auto &kv : TT) {
        fwrite(kv.first.b, 1, 48, f);
        fwrite(&kv.second.value, sizeof(int64_t), 1, f);
        fwrite(&kv.second.flag, sizeof(int8_t), 1, f);
    }
    fclose(f);
}

static bool loadTT(TT_t &TT, const string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint64_t count = 0;
    if (fread(&count, sizeof(count), 1, f) != 1) { fclose(f); return false; }
    TT.reserve((size_t)count * 2);
    for (uint64_t i = 0; i < count; i++) {
        Key48 key;
        int64_t value;
        int8_t flag;
        if (fread(key.b, 1, 48, f) != 48) break;
        if (fread(&value, sizeof(value), 1, f) != 1) break;
        if (fread(&flag, sizeof(flag), 1, f) != 1) break;
        TT[key] = {value, flag};
    }
    fclose(f);
    return true;
}
