#!/usr/bin/env python3
"""Terminal front-end for the QT3 perfect-play engine.

Reuses the already-solved 3x3 game exactly as-is: it spawns
qt3_perfect_ai.exe in `serve` mode (the same line-based JSON protocol
server.py already drives for the browser UI) and keeps it resident so the
1.9GB transposition table is loaded once. Nothing about the engine, the
solver, or the proof is touched by this file -- it is purely a new way to
talk to it.

Usage:
    python play_cli.py [1|2] [tt_file]
        1|2      -- which side you play (default 1 = P1 = X, moves first)
        tt_file  -- override the transposition-table file (default
                    qt3_3x3_tt.bin next to this script; pass a tiny stub
                    file during development to skip the ~40s/1.9GB load)

Moves are entered as a bracketed pair of 1-9 cell numbers, e.g. [2,7],
meaning "place a spooky mark of your symbol in cells 2 and 7 at once".
Cells are numbered 1-9 left-to-right, top-to-bottom:

    1 2 3
    4 5 6
    7 8 9

Whenever a move closes an entanglement loop, this program shows the
resulting "collapse" choice explicitly (both what each option does, and
-- when the AI is the one resolving it -- which one it picked and what
the road not taken would have looked like). After every ply it rewrites
qt3_position.html next to this script, a self-contained, read-only page
that auto-refreshes once a second so a browser tab can show the actual
board (including the pending collapse, highlighted) without needing any
server -- just open the file once and leave the tab open.
"""

import sys
import os
import re
import json
import tempfile
import pathlib
import subprocess
import webbrowser

HERE = pathlib.Path(__file__).resolve().parent
EXE = HERE / "qt3_perfect_ai.exe"
DEFAULT_TT = HERE / "qt3_3x3_tt.bin"
HTML_FILE = HERE / "qt3_position.html"

MOVE_RE = re.compile(r"\d+")

HOW_TO_PLAY = """\
============================================================
  QUANTUM TIC-TAC-TOE (QT3) -- perfect-play terminal client
============================================================
HOW TO PLAY
  - Cells are numbered 1-9, left-to-right / top-to-bottom:
        1 2 3
        4 5 6
        7 8 9
  - On your turn, place a spooky mark in two cells at once by
    typing their numbers, e.g.  [2,7]
    (brackets/spaces/commas are all optional -- "2 7" also works)
  - If only one empty cell remains, it is placed for you automatically.
  - If your move closes an entanglement loop, you'll see the possible
    collapse resolutions listed by number and be asked to pick one --
    unless there's only one possible resolution, which applies itself.
  - Type "quit" or "exit" at any move prompt to leave.
  - When a game ends, answer y/n to play again.

A live view of the board (with any pending entanglement collapse
highlighted) is kept at qt3_position.html, next to this script. It
opens automatically the first time and refreshes itself once a second
-- just leave that browser tab open while you play in this console.
============================================================
"""


def cell_label(i):
    return str(i + 1)


def owner_mark(owner, sub):
    return ("X" if owner == 1 else "O") + str(sub)


def describe_option(opt):
    return ", ".join(
        f"{cell_label(o['cell'])}->{owner_mark(o['owner'], o['sub'])}" for o in opt
    )


def match_collapse_choice(pre_options, post_state):
    """Given the collapseOptions offered before AIMOVE and the resulting
    state, figure out which option index the engine actually took (the
    `serve` protocol's lastAction for a collapse carries no index)."""
    for idx, opt in enumerate(pre_options, start=1):
        if all(
            post_state["board"][o["cell"]]["owner"] == o["owner"]
            and post_state["board"][o["cell"]]["sub"] == o["sub"]
            for o in opt
        ):
            return idx
    return None


class Engine:
    def __init__(self, tt_path):
        if not EXE.exists():
            sys.exit(f"FATAL: {EXE} not found.")
        if not pathlib.Path(tt_path).exists():
            sys.exit(f"FATAL: {tt_path} not found. Run `qt3_solver save` first.")
        self.proc = subprocess.Popen(
            [str(EXE), "serve", str(tt_path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=sys.stderr,
            text=True,
            bufsize=1,
        )
        greeting = self.proc.stdout.readline()
        if not greeting:
            sys.exit("FATAL: engine process did not start.")
        self.state = json.loads(greeting)

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("engine process exited unexpectedly")
        self.state = json.loads(line)
        return self.state

    def quit(self):
        try:
            self.proc.stdin.write("QUIT\n")
            self.proc.stdin.flush()
        except Exception:
            pass
        try:
            self.proc.terminate()
        except Exception:
            pass


def print_board(state):
    W = 11
    lines = []
    for r in range(3):
        cells = []
        for c in range(3):
            i = r * 3 + c
            cell = state["board"][i]
            if cell["owner"] != 0:
                text = f"[{owner_mark(cell['owner'], cell['sub'])}]"
            else:
                spookies = [s for s in state["spooky"] if s["cell"] == i]
                if spookies:
                    marks = ",".join(owner_mark(s["player"], s["t"]) for s in spookies)
                    text = f"{cell_label(i)}:{marks}"
                else:
                    text = f"({cell_label(i)})"
            cells.append(text.center(W))
        lines.append("|".join(cells))
        if r < 2:
            lines.append("-" * (W * 3 + 2))
    print("\n".join(lines))


def parse_pair(raw):
    nums = [int(x) for x in MOVE_RE.findall(raw)]
    if len(nums) != 2:
        return None
    a, b = nums
    if not (1 <= a <= 9 and 1 <= b <= 9) or a == b:
        return None
    return a - 1, b - 1


def describe_place(pre_state, post_state, who):
    a = post_state["lastAction"]
    cells = a.get("cells")
    pair = f"{cell_label(cells[0])} and {cell_label(cells[1])}" if cells else "?"
    if post_state["mode"] == "COLLAPSE":
        return f"{who} places a spooky mark on {pair} -- that closes a loop! Entanglement collapse is now pending."
    return f"{who} places a spooky mark on {pair} (no collapse yet)."


_browser_opened = False


def write_html(state, human_side, note=None):
    global _browser_opened
    payload = dict(state)
    payload["_note"] = note
    payload["_humanSide"] = human_side
    html = render_html(payload)
    fd, tmp_path = tempfile.mkstemp(dir=str(HERE), suffix=".html")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(html)
        os.replace(tmp_path, HTML_FILE)
    except Exception:
        try:
            os.remove(tmp_path)
        except OSError:
            pass
        raise
    if not _browser_opened:
        _browser_opened = True
        try:
            webbrowser.open(HTML_FILE.as_uri())
        except Exception:
            pass


def render_html(state_with_extras):
    state_json = json.dumps(state_with_extras)
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="refresh" content="1">
<title>QT3 -- live position</title>
<style>
  :root {{
    --bg: #10131a; --panel: #1a1f2b; --panel-2: #222838; --line: #2c3444;
    --text: #e8ecf3; --muted: #8b93a7; --p1: #4fb0ff; --p2: #ff7a6b;
    --accent: #f2c14e; --good: #5ed99b;
  }}
  * {{ box-sizing: border-box; }}
  body {{
    margin: 0; min-height: 100vh; background: var(--bg); color: var(--text);
    font-family: -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    display: flex; align-items: center; justify-content: center; padding: 32px 16px;
  }}
  .app {{ width: 100%; max-width: 560px; }}
  header {{ text-align: center; margin-bottom: 18px; }}
  h1 {{ margin: 0 0 4px; font-size: 24px; font-weight: 700; }}
  .subtitle {{ color: var(--muted); font-size: 13px; }}
  .panel {{ background: var(--panel); border: 1px solid var(--line); border-radius: 14px; padding: 20px; }}
  .board {{ display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; aspect-ratio: 1/1; max-width: 420px; margin: 0 auto; }}
  .cell {{
    position: relative; background: var(--panel-2); border: 1px solid var(--line);
    border-radius: 10px; display: flex; flex-direction: column;
    align-items: center; justify-content: center; overflow: hidden;
  }}
  .cell.entangled {{ border-color: var(--accent); animation: pulse 1.1s ease-in-out infinite; }}
  @keyframes pulse {{ 0%,100% {{ box-shadow: inset 0 0 0 2px var(--accent); }} 50% {{ box-shadow: inset 0 0 0 2px transparent; }} }}
  .num {{ position: absolute; top: 3px; left: 6px; font-size: 10px; color: var(--muted); }}
  .mark {{ font-size: 36px; font-weight: 800; line-height: 1; }}
  .mark.p1 {{ color: var(--p1); }}
  .mark.p2 {{ color: var(--p2); }}
  .sub {{ font-size: 12px; vertical-align: super; opacity: 0.75; }}
  .spookyRow {{ position: absolute; bottom: 6px; left: 0; right: 0; display: flex; gap: 3px; justify-content: center; flex-wrap: wrap; padding: 0 4px; }}
  .chip {{ font-size: 10px; font-weight: 700; padding: 1px 4px; border-radius: 5px; border: 1px dashed currentColor; opacity: 0.85; }}
  .chip.p1 {{ color: var(--p1); }}
  .chip.p2 {{ color: var(--p2); }}
  .status {{ margin-top: 16px; text-align: center; font-size: 14px; color: var(--muted); }}
  .status b {{ color: var(--text); }}
  .status .p1 {{ color: var(--p1); font-weight: 700; }}
  .status .p2 {{ color: var(--p2); font-weight: 700; }}
  .note {{ margin-top: 10px; text-align: center; font-size: 13px; color: var(--accent); }}
  .collapse-options {{ display: flex; gap: 10px; justify-content: center; flex-wrap: wrap; margin-top: 12px; }}
  .collapse-option {{ background: var(--panel-2); border: 1px solid var(--line); border-radius: 10px; padding: 10px 12px; font-size: 12px; text-align: left; }}
  .collapse-option div {{ margin: 2px 0; }}
  .result {{ margin-top: 16px; padding: 14px; border-radius: 10px; background: var(--panel-2); font-size: 13px; }}
  .result .margin {{ font-size: 20px; font-weight: 800; margin-bottom: 6px; }}
  .result .margin.good {{ color: var(--good); }}
  .result .margin.bad {{ color: var(--p2); }}
  .result .margin.even {{ color: var(--accent); }}
  footer {{ text-align: center; color: var(--muted); font-size: 11px; margin-top: 16px; }}
</style>
</head>
<body>
<div class="app">
  <header>
    <h1>Quantum Tic-Tac-Toe</h1>
    <div class="subtitle">Live position -- driven from the terminal, refreshes automatically</div>
  </header>
  <div class="panel">
    <div class="board" id="board"></div>
    <div class="status" id="status"></div>
    <div class="note" id="note" style="display:none"></div>
    <div class="collapse-options" id="collapseOptions" style="display:none"></div>
    <div class="result" id="result" style="display:none"></div>
  </div>
  <footer>Read-only snapshot -- moves and collapse choices are entered in the console window.</footer>
</div>
<script>
const STATE = {state_json};
function cellName(i) {{ return String(i + 1); }}
function playerName(p) {{ return p === 1 ? 'P1' : 'P2'; }}
function playerClass(p) {{ return p === 1 ? 'p1' : 'p2'; }}

function render() {{
  const boardEl = document.getElementById('board');
  boardEl.innerHTML = '';
  const entangledCells = new Set();
  if (STATE.mode === 'COLLAPSE') {{
    for (const opt of STATE.collapseOptions) for (const o of opt) entangledCells.add(o.cell);
  }}
  for (let i = 0; i < 9; i++) {{
    const cellState = STATE.board[i];
    const div = document.createElement('div');
    div.className = 'cell';
    if (entangledCells.has(i)) div.classList.add('entangled');
    const num = document.createElement('div');
    num.className = 'num';
    num.textContent = cellName(i);
    div.appendChild(num);
    if (cellState.owner !== 0) {{
      const mark = document.createElement('div');
      mark.className = 'mark ' + playerClass(cellState.owner);
      mark.innerHTML = (cellState.owner === 1 ? 'X' : 'O') + '<span class="sub">' + cellState.sub + '</span>';
      div.appendChild(mark);
    }} else {{
      const spookies = STATE.spooky.filter(s => s.cell === i);
      if (spookies.length) {{
        const row = document.createElement('div');
        row.className = 'spookyRow';
        for (const s of spookies) {{
          const chip = document.createElement('div');
          chip.className = 'chip ' + playerClass(s.player);
          chip.textContent = (s.player === 1 ? 'X' : 'O') + s.t;
          row.appendChild(chip);
        }}
        div.appendChild(row);
      }}
    }}
    boardEl.appendChild(div);
  }}

  const statusEl = document.getElementById('status');
  const noteEl = document.getElementById('note');
  const collapseEl = document.getElementById('collapseOptions');
  const resultEl = document.getElementById('result');
  collapseEl.style.display = 'none';
  resultEl.style.display = 'none';
  noteEl.style.display = 'none';

  if (STATE._note) {{
    noteEl.style.display = 'block';
    noteEl.textContent = STATE._note;
  }}

  if (STATE.terminal) {{
    statusEl.innerHTML = '<b>Game over</b>';
    showResult();
    return;
  }}

  const turnLabel = '<span class="' + playerClass(STATE.decider) + '">' + playerName(STATE.decider) + '</span>';
  const isHuman = STATE.decider === STATE._humanSide;
  const whoText = isHuman ? '(you)' : '(AI)';
  if (STATE.mode === 'COLLAPSE') {{
    statusEl.innerHTML = turnLabel + ' ' + whoText + ' must resolve a pending entanglement collapse.';
    collapseEl.style.display = 'flex';
    collapseEl.innerHTML = '';
    STATE.collapseOptions.forEach((opt, idx) => {{
      const div = document.createElement('div');
      div.className = 'collapse-option';
      div.innerHTML = '<div><b>Option ' + (idx + 1) + '</b></div>' + opt.map(c =>
        '<div>' + cellName(c.cell) + ' &rarr; ' +
        '<span class="' + playerClass(c.owner) + '">' + (c.owner === 1 ? 'X' : 'O') + c.sub + '</span></div>'
      ).join('');
      collapseEl.appendChild(div);
    }});
  }} else if (STATE.forcedCell !== null) {{
    statusEl.innerHTML = turnLabel + ' ' + whoText + ' places the forced last mark on cell ' + cellName(STATE.forcedCell) + '.';
  }} else {{
    statusEl.innerHTML = turnLabel + "'s turn " + whoText + '.';
  }}
}}

function showResult() {{
  const resultEl = document.getElementById('result');
  resultEl.style.display = 'block';
  const v = STATE.score / 840;
  let cls = 'even', text;
  if (v > 0) {{ cls = STATE._humanSide === 1 ? 'good' : 'bad'; text = 'P1 wins by ' + v; }}
  else if (v < 0) {{ cls = STATE._humanSide === 2 ? 'good' : 'bad'; text = 'P2 wins by ' + (-v); }}
  else {{ text = 'Draw'; }}
  let html = '<div class="margin ' + cls + '">' + text + ' (margin ' + (v > 0 ? '+' : '') + v + ')</div>';
  if (STATE.wonLines.length === 0) {{
    html += 'No lines were completed -- draw by exhaustion.';
  }} else {{
    html += 'Won lines, in scoring order:<br>';
    for (const w of STATE.wonLines) {{
      html += 'rank ' + w.rank + ': {{' + w.line.map(cellName).join(',') + '}} &rarr; ' +
        '<span class="' + playerClass(w.owner) + '">' + playerName(w.owner) + '</span>, award ' +
        w.award + '/840<br>';
    }}
  }}
  resultEl.innerHTML = html;
}}

render();
</script>
</body>
</html>
"""


def ai_turn(engine, human_side):
    while not engine.state["terminal"] and engine.state["decider"] != human_side:
        pre = engine.state
        who = f"AI (P{pre['decider']})"
        if pre["mode"] == "COLLAPSE":
            pre_options = pre["collapseOptions"]
            engine.send("AIMOVE")
            idx = match_collapse_choice(pre_options, engine.state)
            chosen = describe_option(pre_options[idx - 1]) if idx else "?"
            note = f"{who} resolves the collapse -> chooses option {idx}: {chosen}"
            others = [
                f"option {j}: {describe_option(o)}"
                for j, o in enumerate(pre_options, start=1)
                if j != idx
            ]
            print(note)
            if others:
                print(f"  (not chosen: {'; '.join(others)})")
                note += f"  (not chosen: {'; '.join(others)})"
        else:
            engine.send("AIMOVE")
            la = engine.state["lastAction"]
            if la["type"] == "forced":
                note = f"{who} places the forced last mark on cell {cell_label(la['cells'][0]) if la.get('cells') else '?'}."
                print(note)
            else:
                note = describe_place(pre, engine.state, who)
                print(note)
        write_html(engine.state, human_side, note=note)
        print_board(engine.state)
        print()


def human_turn(engine, human_side):
    while not engine.state["terminal"] and engine.state["decider"] == human_side:
        s = engine.state
        who = f"You (P{human_side})"
        if s["mode"] == "MOVE":
            if s["forcedCell"] is not None:
                print(f"Only one cell left -- placing your forced mark on cell {cell_label(s['forcedCell'])}.")
                engine.send("FORCED")
                write_html(engine.state, human_side, note=f"{who} places the forced last mark.")
                print_board(engine.state)
                print()
                continue
            while True:
                raw = input("Your move, e.g. [2,7]: ").strip()
                if raw.lower() in ("quit", "exit"):
                    return "quit"
                pair = parse_pair(raw)
                if pair is None:
                    print("  Enter two distinct cell numbers 1-9, e.g. [2,7].")
                    continue
                a, b = pair
                if [min(a, b), max(a, b)] not in s["legalPairs"]:
                    print("  That pair isn't legal right now (a cell may already be classical, or repeated).")
                    continue
                pre = s
                engine.send(f"MOVE {a} {b}")
                if engine.state.get("error"):
                    print("  Rejected:", engine.state["error"])
                    continue
                note = describe_place(pre, engine.state, who)
                print(note)
                break
            write_html(engine.state, human_side, note=note)
            print_board(engine.state)
            print()
        else:  # COLLAPSE
            opts = s["collapseOptions"]
            if len(opts) == 1:
                print("Your move closed an entanglement loop, but the two orientations coincide here --")
                print("only one resolution is possible:")
                print(f"  {describe_option(opts[0])}")
                engine.send("COLLAPSE 1")
                note = f"{who} collapses the entanglement (only option) -> {describe_option(opts[0])}"
            else:
                print()
                print("Your move just closed an entanglement loop! This is a 'measurement': exactly one")
                print("of the spooky marks sharing that loop becomes real (classical) per cell, and you --")
                print("as the player who did NOT cause the loop -- choose which of the two ways it resolves:")
                for idx, opt in enumerate(opts, start=1):
                    print(f"  {idx}: {describe_option(opt)}")
                while True:
                    raw = input(f"Choice (1-{len(opts)}): ").strip()
                    if raw.isdigit() and 1 <= int(raw) <= len(opts):
                        break
                    print("  Enter a number from the list.")
                engine.send(f"COLLAPSE {raw}")
                note = f"{who} collapses the entanglement -> chooses option {raw}: {describe_option(opts[int(raw) - 1])}"
            print(note)
            write_html(engine.state, human_side, note=note)
            print_board(engine.state)
            print()
    return None


def print_final(state):
    print("=" * 40)
    print("GAME OVER")
    print_board(state)
    score = state["score"]
    margin = score / 840
    if score == 0:
        print("Result: draw (net margin 0).")
    else:
        leader = "P1" if score > 0 else "P2"
        print(f"Result: {leader} wins by {abs(margin)} points (exact margin {'+' if score > 0 else ''}{margin}).")
    if state["wonLines"]:
        print("Won lines, in scoring order:")
        for w in state["wonLines"]:
            cells = ",".join(cell_label(c) for c in w["line"])
            owner = "P1" if w["owner"] == 1 else "P2"
            print(f"  rank {w['rank']}: {{{cells}}} -> {owner}, award {w['award']}/840")
    else:
        print("No lines were completed -- draw by exhaustion.")


def main():
    human_side = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    if human_side not in (1, 2):
        sys.exit("usage: play_cli.py [1|2] [tt_file]")
    tt_path = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_TT

    print(HOW_TO_PLAY)
    print(f"You are P{human_side} ({'X, moves first' if human_side == 1 else 'O, moves second'}).")
    print("The AI plays perfectly -- the proven game value is +0.5 (P1 by half a point),")
    print("so as P2 the best possible outcome is losing by exactly that margin.")
    print(f"Loading engine ({tt_path.name}) ...")

    engine = Engine(tt_path)
    print("Ready.\n")

    try:
        while True:
            write_html(engine.state, human_side)
            print_board(engine.state)
            print()
            while not engine.state["terminal"]:
                if engine.state["decider"] == human_side:
                    if human_turn(engine, human_side) == "quit":
                        return
                else:
                    ai_turn(engine, human_side)
            print_final(engine.state)
            write_html(engine.state, human_side, note="Game over.")
            again = input("\nPlay again? (y/n): ").strip().lower()
            if again != "y":
                return
            engine.send("NEW")
            print()
    except (KeyboardInterrupt, EOFError):
        print("\nExiting.")
    finally:
        engine.quit()


if __name__ == "__main__":
    main()
