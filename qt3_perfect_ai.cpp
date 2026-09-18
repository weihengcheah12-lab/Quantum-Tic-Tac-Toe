// Perfect-play AI for 3x3 Quantum Tic-Tac-Toe, built on the "data from the
// solver": the full transposition table qt3_solver.cpp dumps to
// qt3_3x3_tt.bin (via `qt3_solver save`) after exactly solving the game.
//
// How this guarantees ACTUAL perfect play, not just "usually correct cached
// play": the root-only solve does not necessarily leave an EXACT value for
// every state reachable under arbitrary (e.g. human) play -- alpha-beta
// pruning discards some subtrees whose exact value is irrelevant to the
// root's value, leaving only a bound there, not a computed value. This
// program does not rely on all-cached-EXACT. Instead, chooseMove() calls the
// SAME alphabeta() search from qt3_engine.h, seeded with the loaded table.
// Any state with a cached EXACT entry returns instantly (O(1)); anything
// else triggers a real (but now warm-started, hence fast) live solve of that
// specific sub-position, which is exact by construction and gets cached for
// the rest of the session. Every move this program makes is therefore
// game-theoretically optimal, unconditionally -- the loaded file is a speed
// optimisation, not a correctness dependency.
//
// Usage:
//   qt3_perfect_ai selfplay [tt_file]           -- AI vs AI, both sides perfect
//   qt3_perfect_ai verify N [tt_file]           -- AI vs random, N games each side
//   qt3_perfect_ai play [1|2] [tt_file]         -- interactive: human is P<side> (default 1)

#include "qt3_engine.h"
#include <chrono>
#include <random>
#include <sstream>

static const char *DEFAULT_TT_PATH = "qt3_3x3_tt.bin";

static string cellName(int c) {
    string s;
    s += (char)('A' + c / 3);
    s += (char)('1' + c % 3);
    return s;
}
static int cellFromName(const string &s) {
    if (s.size() != 2) return -1;
    char r = toupper(s[0]), c = s[1];
    if (r < 'A' || r > 'C' || c < '1' || c > '3') return -1;
    return (r - 'A') * 3 + (c - '1');
}

static void printBoard(const State &S) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int i = r * 3 + c;
            if (S.owner[i] == EMPTY) cout << " .   ";
            else cout << (S.owner[i] == P1 ? "X" : "O") << (int)S.sub[i]
                       << (S.sub[i] >= 10 ? "" : " ") << "  ";
        }
        cout << "\n";
    }
}

// Picks the best child of S under EXACT game-theoretic play, using the
// warm-started TT (loaded data + on-demand live solving, both via the exact
// alphabeta from qt3_engine.h -- no heuristics anywhere in this function).
static State chooseMove(const State &S, TT_t &TT, long long &nodesOut) {
    vector<State> children = successors(S, 2);
    int dec = decider(S);
    long long nodes = 0;
    // Root-level alpha-beta across the candidate children, exactly like the
    // loop inside alphabeta() itself -- without tightening alpha/beta across
    // siblings here, each child would be searched with a full (-inf,+inf)
    // window and lose all cross-sibling pruning, making this far slower than
    // it needs to be (a real perf bug, not a correctness one: each child's
    // value would still come out exact, just expensively).
    int64_t alpha = INT64_MIN / 2, beta = INT64_MAX / 2;
    int64_t best = (dec == P1) ? INT64_MIN : INT64_MAX;
    int bestIdx = 0;
    for (size_t i = 0; i < children.size(); i++) {
        int64_t v = alphabeta(children[i], alpha, beta, TT, true, true, nodes, 2);
        if (dec == P1) {
            if (v > best) { best = v; bestIdx = (int)i; }
            if (v > alpha) alpha = v;
        } else {
            if (v < best) { best = v; bestIdx = (int)i; }
            if (v < beta) beta = v;
        }
    }
    nodesOut = nodes;
    return children[bestIdx];
}

static void printFinal(const State &S) {
    cout << "\nFinal board:\n";
    printBoard(S);
    int64_t v = scoreMargin(S);
    cout << "\nFinal EXACT margin: " << v << "/" << SCALE << " = " << (double)v / SCALE << "\n";
    auto won = wonLinesRanked(S);
    if (won.empty()) cout << "No lines won -- draw by exhaustion.\n";
    else {
        cout << "Won lines, in scoring order:\n";
        for (auto &w : won) {
            int L = w[0], owner = w[1], rank = w[2];
            cout << "  rank " << rank << ": line {" << cellName(LINES[L][0]) << ","
                 << cellName(LINES[L][1]) << "," << cellName(LINES[L][2]) << "} -> "
                 << (owner == P1 ? "P1" : "P2") << ", award " << (SCALE / rank) << "/" << SCALE << "\n";
        }
    }
}

static void loadOrDie(TT_t &TT, const string &path) {
    cout << "Loading solved-game data from " << path << " ...\n";
    auto t0 = chrono::steady_clock::now();
    if (!loadTT(TT, path)) {
        cerr << "Could not open " << path << ". Run `qt3_solver save " << path
             << "` first to generate it (takes ~3 minutes).\n";
        exit(1);
    }
    auto t1 = chrono::steady_clock::now();
    cout << "  loaded " << TT.size() << " entries in "
         << chrono::duration<double>(t1 - t0).count() << "s\n";
}

static void runSelfplay(const string &ttPath) {
    TT_t TT;
    loadOrDie(TT, ttPath);
    State S;
    cout << "\nPerfect AI vs perfect AI (both sides play the exact game-theoretic optimum):\n\n";
    long long totalNodes = 0;
    auto t0 = chrono::steady_clock::now();
    while (!terminal(S)) {
        State before = S;
        long long nodes;
        S = chooseMove(S, TT, nodes);
        totalNodes += nodes;
        if (before.mode == MOVE) {
            cout << "  turn " << (int)before.t << " (" << (mover(before.t) == P1 ? "P1" : "P2") << "): ";
            if (S.edgeCount > before.edgeCount) {
                Edge &e = S.edges[S.edgeCount - 1];
                cout << "places on {" << cellName(e.cells[0]) << ", " << cellName(e.cells[1]) << "}";
                if (S.mode == COLLAPSE) cout << " -> entangled, collapse pending";
            } else {
                cout << "forced classical placement";
            }
            cout << " (" << nodes << " search calls)\n";
        } else {
            cout << "    collapse resolved by " << (opponent(mover(before.t)) == P1 ? "P1" : "P2")
                 << " (" << nodes << " search calls)\n";
        }
    }
    auto t1 = chrono::steady_clock::now();
    printFinal(S);
    cout << "Total search calls: " << totalNodes << " (mostly O(1) cache hits), wall time: "
         << chrono::duration<double>(t1 - t0).count() << "s\n";
    if (scoreMargin(S) == SCALE / 2) cout << "[PASS] matches the proven game value +0.5\n";
    else cout << "[FAIL] expected +0.5 -- something is wrong\n";
}

static State randomChild(const State &S, mt19937 &rng) {
    vector<State> children = successors(S, 2);
    uniform_int_distribution<size_t> dist(0, children.size() - 1);
    return children[dist(rng)];
}

static void runVerify(int games, const string &ttPath) {
    TT_t TT;
    loadOrDie(TT, ttPath);
    mt19937 rng(42);
    cout << "\nPerfect AI vs uniform-random opponent, " << games << " games each side:\n";

    int failP1 = 0, failP2 = 0;
    double sumP1 = 0, sumP2 = 0;
    for (int g = 0; g < games; g++) {
        State S;
        while (!terminal(S)) {
            long long nodes;
            if (decider(S) == P1) S = chooseMove(S, TT, nodes);
            else S = randomChild(S, rng);
        }
        int64_t v = scoreMargin(S);
        sumP1 += (double)v / SCALE;
        if (v < SCALE / 2) failP1++; // AI as P1 must always get >= the game value
    }
    for (int g = 0; g < games; g++) {
        State S;
        while (!terminal(S)) {
            long long nodes;
            if (decider(S) == P2) S = chooseMove(S, TT, nodes);
            else S = randomChild(S, rng);
        }
        int64_t v = scoreMargin(S);
        sumP2 += (double)v / SCALE;
        if (v > SCALE / 2) failP2++; // AI as P2 must always hold P1 to <= the game value
    }
    cout << "  AI as P1 vs random P2: avg margin " << (sumP1 / games)
         << " (guarantee: always >= 0.5) -- violations: " << failP1 << "/" << games
         << (failP1 == 0 ? "  [PASS]" : "  [FAIL]") << "\n";
    cout << "  AI as P2 vs random P1: avg margin " << (sumP2 / games)
         << " (guarantee: always <= 0.5) -- violations: " << failP2 << "/" << games
         << (failP2 == 0 ? "  [PASS]" : "  [FAIL]") << "\n";
}

static void runPlay(int humanSide, const string &ttPath) {
    TT_t TT;
    loadOrDie(TT, ttPath);
    State S;
    cout << "\nYou are " << (humanSide == P1 ? "P1 (X, odd turns)" : "P2 (O, even turns)")
         << ". The AI plays perfectly -- the proven game value is +0.5 (P1 wins by half a "
         << "point), so as P2 the best you can do is lose by exactly that margin.\n"
         << "Move format: two cell names like 'A1 B2' (rows A-C, cols 1-3). "
         << "Collapse choices: type 1 or 2.\n\n";

    while (!terminal(S)) {
        printBoard(S);
        cout << "\n";
        int dec = decider(S);
        if (dec != humanSide) {
            long long nodes;
            State before = S;
            S = chooseMove(S, TT, nodes);
            cout << "AI (" << (dec == P1 ? "P1" : "P2") << "): ";
            if (before.mode == MOVE) {
                if (S.edgeCount > before.edgeCount) {
                    Edge &e = S.edges[S.edgeCount - 1];
                    cout << "places on {" << cellName(e.cells[0]) << ", " << cellName(e.cells[1]) << "}";
                    if (S.mode == COLLAPSE) cout << " -> entangled";
                } else cout << "forced classical placement";
            } else cout << "resolves the collapse";
            cout << "\n\n";
            continue;
        }

        vector<State> children = successors(S, 2);
        if (S.mode == MOVE) {
            int empties = 0;
            for (int i = 0; i < 9; i++) if (S.owner[i] == EMPTY) empties++;
            if (empties == 1) {
                cout << "(Only one cell left -- forced placement.)\n\n";
                S = children[0];
                continue;
            }
            cout << "Your move (e.g. A1 B2): ";
            string line;
            if (!getline(cin, line)) return;
            istringstream iss(line);
            string a, b;
            iss >> a >> b;
            int ca = cellFromName(a), cb = cellFromName(b);
            if (ca < 0 || cb < 0 || ca == cb || S.owner[ca] != EMPTY || S.owner[cb] != EMPTY) {
                cout << "Invalid move, try again.\n\n";
                continue;
            }
            int lo = min(ca, cb), hi = max(ca, cb);
            State *match = nullptr;
            for (auto &c : children) {
                Edge &e = c.edges[c.edgeCount - 1];
                if (e.cells[0] == lo && e.cells[1] == hi) { match = &c; break; }
            }
            if (!match) { cout << "Invalid move, try again.\n\n"; continue; }
            S = *match;
        } else {
            cout << "Collapse pending -- choose a resolution:\n";
            for (size_t i = 0; i < children.size(); i++) {
                cout << "  " << (i + 1) << ": ";
                for (int c = 0; c < 9; c++)
                    if (S.owner[c] == EMPTY && children[i].owner[c] != EMPTY)
                        cout << cellName(c) << "->" << (children[i].owner[c] == P1 ? "X" : "O")
                             << (int)children[i].sub[c] << " ";
                cout << "\n";
            }
            cout << "Choice: ";
            int choice;
            if (!(cin >> choice)) return;
            cin.ignore();
            if (choice < 1 || choice > (int)children.size()) { cout << "Invalid, try again.\n\n"; continue; }
            S = children[choice - 1];
        }
        cout << "\n";
    }
    printFinal(S);
}

// ---------------------------------------------------------------------
// "serve" mode: a line-based JSON protocol over stdin/stdout, so a thin web
// server can keep this process (and its loaded 1.9GB table) resident and
// forward one command per HTTP request instead of reloading per move. stdout
// carries ONLY JSON state lines in this mode -- all status/errors go to
// stderr, and every command that changes state ends by emitting the new
// state as one JSON line.
//
// Commands (read one per line from stdin):
//   NEW                 -- reset to the empty board
//   STATE               -- re-emit the current state
//   MOVE <a> <b>        -- human places on cells a,b (0-8); only valid in MOVE mode
//   FORCED              -- apply the forced single-cell placement (section 2.4)
//   COLLAPSE <1|2>      -- human resolves a pending collapse by choice index
//   AIMOVE              -- let the perfect AI decide (a placement, forced move,
//                          or collapse resolution, whichever the current node needs)
//   QUIT                -- exit
// ---------------------------------------------------------------------

static string jsonBoard(const State &S) {
    string s = "[";
    for (int i = 0; i < 9; i++) {
        if (i) s += ",";
        s += "{\"owner\":" + to_string((int)S.owner[i]) + ",\"sub\":" + to_string((int)S.sub[i]) + "}";
    }
    return s + "]";
}
static string jsonSpooky(const State &S) {
    string s = "[";
    bool first = true;
    for (int i = 0; i < S.edgeCount; i++) {
        const Edge &e = S.edges[i];
        for (int k = 0; k < e.n; k++) {
            if (!first) s += ",";
            first = false;
            s += "{\"cell\":" + to_string((int)e.cells[k]) + ",\"player\":" + to_string((int)e.player)
               + ",\"t\":" + to_string((int)e.t) + "}";
        }
    }
    return s + "]";
}
static string jsonWonLines(const State &S) {
    auto won = wonLinesRanked(S);
    string s = "[";
    for (size_t i = 0; i < won.size(); i++) {
        if (i) s += ",";
        int L = won[i][0], owner = won[i][1], rank = won[i][2];
        s += "{\"line\":[" + to_string(LINES[L][0]) + "," + to_string(LINES[L][1]) + ","
           + to_string(LINES[L][2]) + "],\"owner\":" + to_string(owner) + ",\"rank\":" + to_string(rank)
           + ",\"award\":" + to_string(SCALE / rank) + "}";
    }
    return s + "]";
}
static string jsonLegalPairs(const State &S, bool &isForced, int &forcedCell) {
    isForced = false; forcedCell = -1;
    int empties = 0, onlyCell = -1;
    for (int i = 0; i < 9; i++) if (S.owner[i] == EMPTY) { empties++; onlyCell = i; }
    if (empties == 1) { isForced = true; forcedCell = onlyCell; return "[]"; }
    string s = "[";
    bool first = true;
    for (int a = 0; a < 9; a++) if (S.owner[a] == EMPTY)
        for (int b = a + 1; b < 9; b++) if (S.owner[b] == EMPTY) {
            if (!first) s += ","; first = false;
            s += "[" + to_string(a) + "," + to_string(b) + "]";
        }
    return s + "]";
}
static string jsonCollapseOptions(const State &S) {
    vector<State> opts = successors(S, 2);
    string s = "[";
    for (size_t i = 0; i < opts.size(); i++) {
        if (i) s += ",";
        s += "[";
        bool first = true;
        for (int c = 0; c < 9; c++) if (S.owner[c] == EMPTY && opts[i].owner[c] != EMPTY) {
            if (!first) s += ","; first = false;
            s += "{\"cell\":" + to_string(c) + ",\"owner\":" + to_string((int)opts[i].owner[c])
               + ",\"sub\":" + to_string((int)opts[i].sub[c]) + "}";
        }
        s += "]";
    }
    return s + "]";
}

static void emitState(const State &S, const string &lastActionJSON, const string &errorMsg) {
    bool isForced; int forcedCell;
    string legalPairs = (S.mode == MOVE) ? jsonLegalPairs(S, isForced, forcedCell) : "[]";
    string collapseOpts = (S.mode == COLLAPSE) ? jsonCollapseOptions(S) : "[]";
    bool term = terminal(S);
    cout << "{"
         << "\"board\":" << jsonBoard(S) << ","
         << "\"spooky\":" << jsonSpooky(S) << ","
         << "\"mode\":\"" << (S.mode == MOVE ? "MOVE" : "COLLAPSE") << "\","
         << "\"decider\":" << decider(S) << ","
         << "\"terminal\":" << (term ? "true" : "false") << ","
         << "\"score\":" << (term ? to_string(scoreMargin(S)) : string("null")) << ","
         << "\"wonLines\":" << (term ? jsonWonLines(S) : string("[]")) << ","
         << "\"legalPairs\":" << legalPairs << ","
         << "\"forcedCell\":" << ((S.mode == MOVE && isForced) ? to_string(forcedCell) : string("null")) << ","
         << "\"collapseOptions\":" << collapseOpts << ","
         << "\"lastAction\":" << lastActionJSON << ","
         << "\"error\":" << (errorMsg.empty() ? string("null") : ("\"" + errorMsg + "\""))
         << "}" << endl; // endl flushes -- the parent process reads exactly one line per command
}

static void runServe(const string &ttPath) {
    TT_t TT;
    cerr << "Loading solved-game data from " << ttPath << " ...\n";
    if (!loadTT(TT, ttPath)) {
        cerr << "FATAL: could not open " << ttPath << "\n";
        exit(1);
    }
    cerr << "Loaded " << TT.size() << " entries. Ready.\n";

    State S;
    emitState(S, "{\"type\":\"none\"}", "");

    string line;
    while (getline(cin, line)) {
        istringstream iss(line);
        string cmd; iss >> cmd;

        if (cmd == "NEW") {
            S = State();
            emitState(S, "{\"type\":\"none\"}", "");
        } else if (cmd == "STATE") {
            emitState(S, "{\"type\":\"none\"}", "");
        } else if (cmd == "MOVE") {
            int a = -1, b = -1; iss >> a >> b;
            if (S.mode != MOVE || a < 0 || a > 8 || b < 0 || b > 8 || a == b
                || S.owner[a] != EMPTY || S.owner[b] != EMPTY) {
                emitState(S, "{\"type\":\"none\"}", "invalid move");
                continue;
            }
            int lo = min(a, b), hi = max(a, b);
            vector<State> children = successors(S, 2);
            const State *match = nullptr;
            for (auto &c : children) {
                const Edge &e = c.edges[c.edgeCount - 1];
                if (e.cells[0] == lo && e.cells[1] == hi) { match = &c; break; }
            }
            if (!match) { emitState(S, "{\"type\":\"none\"}", "invalid move"); continue; }
            int who = mover(S.t);
            S = *match;
            emitState(S, "{\"type\":\"place\",\"cells\":[" + to_string(lo) + "," + to_string(hi)
                         + "],\"player\":" + to_string(who) + "}", "");
        } else if (cmd == "FORCED") {
            int empties = 0;
            for (int i = 0; i < 9; i++) if (S.owner[i] == EMPTY) empties++;
            if (S.mode != MOVE || empties != 1) {
                emitState(S, "{\"type\":\"none\"}", "not a forced-move state");
                continue;
            }
            int who = mover(S.t);
            S = successors(S, 2)[0];
            emitState(S, "{\"type\":\"forced\",\"player\":" + to_string(who) + "}", "");
        } else if (cmd == "COLLAPSE") {
            int idx = -1; iss >> idx;
            if (S.mode != COLLAPSE) { emitState(S, "{\"type\":\"none\"}", "not a collapse state"); continue; }
            vector<State> opts = successors(S, 2);
            if (idx < 1 || idx > (int)opts.size()) {
                emitState(S, "{\"type\":\"none\"}", "invalid collapse choice");
                continue;
            }
            int who = opponent(mover(S.t));
            S = opts[idx - 1];
            emitState(S, "{\"type\":\"collapse\",\"choice\":" + to_string(idx)
                         + ",\"player\":" + to_string(who) + "}", "");
        } else if (cmd == "AIMOVE") {
            if (terminal(S)) { emitState(S, "{\"type\":\"none\"}", "game already terminal"); continue; }
            State before = S;
            int dec = decider(before);
            int emptiesBefore = 0;
            for (int i = 0; i < 9; i++) if (before.owner[i] == EMPTY) emptiesBefore++;
            long long nodes;
            S = chooseMove(before, TT, nodes);
            string la;
            if (before.mode == MOVE) {
                if (emptiesBefore == 1) {
                    la = "{\"type\":\"forced\",\"player\":" + to_string(dec) + "}";
                } else {
                    const Edge &e = S.edges[S.edgeCount - 1];
                    la = "{\"type\":\"place\",\"cells\":[" + to_string((int)e.cells[0]) + ","
                       + to_string((int)e.cells[1]) + "],\"player\":" + to_string(dec) + "}";
                }
            } else {
                la = "{\"type\":\"collapse\",\"player\":" + to_string(dec) + "}";
            }
            emitState(S, la, "");
        } else if (cmd == "QUIT") {
            break;
        } else {
            emitState(S, "{\"type\":\"none\"}", "unknown command");
        }
    }
}

int main(int argc, char **argv) {
    string mode = (argc > 1) ? argv[1] : "selfplay";
    if (mode == "selfplay") {
        runSelfplay(argc > 2 ? argv[2] : DEFAULT_TT_PATH);
    } else if (mode == "verify") {
        int games = (argc > 2) ? atoi(argv[2]) : 20;
        runVerify(games, argc > 3 ? argv[3] : DEFAULT_TT_PATH);
    } else if (mode == "play") {
        int side = (argc > 2) ? atoi(argv[2]) : 1;
        runPlay(side == 2 ? P2 : P1, argc > 3 ? argv[3] : DEFAULT_TT_PATH);
    } else if (mode == "serve") {
        runServe(argc > 2 ? argv[2] : DEFAULT_TT_PATH);
    } else {
        cerr << "usage: qt3_perfect_ai [selfplay [tt_file] | verify N [tt_file] | play [1|2] [tt_file] | serve [tt_file]]\n";
        return 1;
    }
    return 0;
}
