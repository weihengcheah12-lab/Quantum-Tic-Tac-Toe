# QT3 — Quantum Tic-Tac-Toe perfect-play solver

An exact solver and perfect-play AI for 3×3 Quantum Tic-Tac-Toe (each move
places a "spooky" mark in two cells at once; when marks form a loop, they
collapse into one real, classical mark per cell), plus a terminal client to
play against it.

## Files

- `qt3_engine.h` — game engine: rules, entanglement/collapse, scoring, alpha-beta search.
- `qt3_solver.cpp` / `qt3_solver.exe` — exactly solves the game; `save` mode writes the solved table to disk.
- `qt3_perfect_ai.cpp` / `qt3_perfect_ai.exe` — loads that table and plays perfectly.
- `play_cli.py` — terminal client to play against it.

## Build (optional — prebuilt `.exe` files are included)

```
g++ -O3 -std=c++17 -march=native -o qt3_solver.exe qt3_solver.cpp
g++ -O3 -std=c++17 -march=native -o qt3_perfect_ai.exe qt3_perfect_ai.cpp
```

## Generate the solved table (required once, not included in this repo)

```
./qt3_solver.exe save
```

Takes ~3 minutes and writes `qt3_3x3_tt.bin` (~1.9GB) into this folder. This
file is listed in `.gitignore` — don't commit it (GitHub blocks files over
100MB without Git LFS).

## Play

```
python play_cli.py        # you play P1 (X, moves first)
python play_cli.py 2      # you play P2 (O, moves second)
```

- Cells are numbered 1–9, left-to-right / top-to-bottom (`1 2 3 / 4 5 6 / 7 8 9`).
- Enter moves as two cell numbers, e.g. `[2,7]` (`2 7` also works).
- If your move closes an entanglement loop, you'll be shown the possible
  collapse resolutions by number and asked to pick one.
- Type `quit` or `exit` to leave; answer `y`/`n` to play again after a game ends.

This also writes `qt3_position.html` next to the script after every move — a
read-only page that auto-refreshes once a second, showing the live board. It
opens automatically the first time; no server needed.

## 4×4 add-on

The 3×3 solver above is the main project. [`4x4/`](4x4/) is a bonus
extension to the 4×4 board (16 cells, 10 scoring lines, same
entangle/collapse rules) — that size is **not exactly solved** (full-width
search is computationally infeasible there), so `4x4/qt3_4x4_player.cpp`
is a strong heuristic player, not a proof of optimal play. It's
self-contained and doesn't depend on `qt3_engine.h` above, since that engine
is hardcoded to the 3×3/9-cell board.

```
g++ -O3 -std=c++17 -march=native -o 4x4/qt3_4x4_player.exe 4x4/qt3_4x4_player.cpp

./4x4/qt3_4x4_player.exe selfplay [depth]          # AI vs AI, both sides at the given search depth
./4x4/qt3_4x4_player.exe vsrandom [games] [depth]  # AI vs a uniform-random opponent, both seats, N games each
./4x4/qt3_4x4_player.exe depth [depth_limit]       # timing probe: node count and time for one root move at a given depth
```
