# QT3 4×4 — heuristic player for the 4×4 generalisation

A depth-limited alpha-beta player for the 4×4 generalisation of Quantum
Tic-Tac-Toe (16 cells, 10 scoring lines, same entangle/collapse rules as the
3×3 game). Unlike the 3×3 solver, this board size is **not exactly solved**
— a full-width search is computationally infeasible at this size — so this
is a strong heuristic player, not a proof of optimal play.

Self-contained: `qt3_4x4_player.cpp` reimplements its own entanglement,
collapse, and scoring logic for the 16-cell board (it does not depend on
`qt3_engine.h`, which is hardcoded to the 3×3/9-cell case) plus a hand-written
static evaluation function for its depth-limited search.

## Files

- `qt3_4x4_player.cpp` / `qt3_4x4_player.exe` — the player.

## Build (optional — a prebuilt `.exe` is included)

```
g++ -O3 -std=c++17 -march=native -o qt3_4x4_player.exe qt3_4x4_player.cpp
```

## Run

```
./qt3_4x4_player.exe selfplay [depth]          # AI vs AI, both sides at the given search depth (default depth as compiled in)
./qt3_4x4_player.exe vsrandom [games] [depth]  # AI vs a uniform-random opponent, both seats, N games each
./qt3_4x4_player.exe depth [depth_limit]       # timing probe: node count and time for one root move at a given depth
```
