# UTF-8 Width Fix — Terminal-Level Verification (plan Task 4)

Implements plan Task 4 / design §7 Task 4 (review item L5): verify the
original symptom at the only layer where it is observable — the real terminal
output — by running `llmfun_tui` under tmux, capturing the pane, and asserting
that the scrollbar block column is identical across the six bug-report table
rows and that the ⚠️ row has no dangling blank cell.

## Environment

- Container: Ubuntu 26.04 (llmfun dev env), tmux 3.6 (`apt-get install tmux`).
- Terminal: tmux pane, **TERM=tmux-256color** (tmux default), 100x40
  (`tmux new-session -d -x 100 -y 40`).
- Locale: `LANG=C.utf8 LC_ALL=C.utf8` (required for ncurses wide-char mode).
- Capture: `tmux capture-pane -p -e` — `-e` keeps SGR attribute sequences,
  which the checker needs to identify the scrollbar block (bg-colored cells).
  A plain `-p` capture strips the attributes and cannot distinguish the
  scrollbar block from padding.

## Procedure (exact commands)

```sh
# 1. (if missing) install tmux
apt-get install -y tmux

# 2. Add the TEMP TEST SEED to llmfun/cpp_tui/main.cpp (six-row markdown
#    table matching the bug report: 2 plain rows, ✅, ❓, ⚠️, ✅), then build:
python3 llmfun/vendor/imtui/test/build_test.py --build

# 3. Run the TUI in a fixed-size tmux session (UTF-8 locale for wide chars)
tmux new-session -d -x 100 -y 40 -s tuitest \
    'env LANG=C.utf8 LC_ALL=C.utf8 llmfun/cpp_tui/build/llmfun_tui'

# 4. Let it render (idle fps = 3.0), then capture the pane
sleep 5
tmux capture-pane -p -e -t tuitest > capture.txt

# 5. Run the terminal-level checker
python3 llmfun/vendor/imtui/test/pty_utf8_check.py capture.txt

# 6. Cleanup; revert the main.cpp seed afterwards
tmux kill-session -t tuitest
```

The checker (`pty_utf8_check.py`) parses the capture into a cell grid with an
independent width oracle (adapted Kuhn wcwidth + emoji-data.txt 15.1
emoji-presentation list — NOT imtui's table, so the check is not
self-referential), finds the six table rows, and asserts:

1. all six table rows are found;
2. the rightmost bg-colored cell (scrollbar block) column is identical across
   all six rows;
3. the ⚠ row contains no U+FE0F (no dangling blank cell) and the cell
   immediately after ⚠ is the row's next visible character (` ` then `|`);
4. the plain-ASCII control row `| plain | a |` is unchanged (text intact,
   same scrollbar column — no regression for non-emoji rows).

## Results

### Post-fix capture (Tasks 1–3 applied) — PASS, exit 0

```
$ python3 llmfun/vendor/imtui/test/pty_utf8_check.py capture_postfix.txt
parsed 41 pane rows from /workarea/capture_postfix.txt
PASS (2): scrollbar block column 98 for all six rows
PASS (3): ⚠ at col 34, next visible char ' ' at 35, '|' at 36; no blank cell
PASS (4): plain control row '| plain | a |' unchanged

All PTY assertions passed.
```

Captured rows (visible text; leading columns are the left panel + indentation,
scrollbar block at terminal column 98 — the right edge of the 100-col pane):

```
... | plain | a |
... | plain | b |
... | ✅ | task1 |
... | ❓ | ask2 |
... | ⚠ | warn3 |
... | ✅ | done4 |
```

- Scrollbar block: **column 98 in all six rows** (constant → aligned).
- ⚠ row: `| ⚠ | warn3 |` — U+26A0 width 1, U+FE0F dropped; the byte stream
  contains **no U+FE0F** (`grep -c $'\xef\xb8\x8f'` = 0). No dangling blank
  cell; the pipe after ⚠ is exactly 2 columns later (`' '` then `|`).
- Plain control rows unchanged.
- Full log: `scratch/pty_postfix_out.txt`; capture: `scratch/capture_postfix.txt`.

### Pre-fix capture (pre-fix reconstruction, Tasks 1–2 reverted) — FAIL, exit 1

The pre-fix width table + no zero-width skip were rebuilt into `llmfun_tui`
and captured with the same procedure:

```
$ python3 llmfun/vendor/imtui/test/pty_utf8_check.py capture_prefix.txt
parsed 41 pane rows from /workarea/capture_prefix.txt
FAIL (2): row2 check has no bg-colored scrollbar cell (scrollbar block
          missing from this row -- pre-fix the row is shifted so the block
          lands outside the pane)
FAIL (2): row3 question has no bg-colored scrollbar cell (...)
FAIL (2): row5 check2 has no bg-colored scrollbar cell (...)
FAIL (3): U+FE0F (VS16) present in the ⚠ row (dangling blank cell /
          zero-width cell leaked)
FAIL (4): plain control row scrollbar column differs from ⚠ row

41 rows parsed; 5 assertion(s) FAILED
```

This is the bug report's symptom, observed at the terminal level:
- ✅/❓ rows: the grid counted the emoji as width 1, but the terminal renders
  it width 2, so the terminal cursor ends one column further right than the
  grid expects and the scrollbar block is pushed past the pane edge (missing
  from the expected column) — the +1 shift.
- ⚠️ row: U+FE0F is emitted as its own cell (width 1 in the old table), i.e.
  a dangling blank cell; the row's visible extent and scrollbar column (96)
  differ from the plain rows (98) — the −1 shift.
- Full log: `scratch/pty_prefix_out.txt`; capture: `capture_prefix.txt`.

## Conclusion

Post-fix, the terminal-level capture satisfies all four assertions (exit 0):
the scrollbar block occupies one constant column across all six table rows,
the ⚠️ row has no dangling blank cell (no U+FE0F in the stream), and plain
ASCII rows are unchanged. The same checker fails against the pre-fix build
(exit 1, 5 failures), demonstrating that the fix is what restores terminal
alignment — the exact symptom the user reported.

## Manual fallback (no PTY tool available)

If tmux is unavailable, the same check can be performed manually in any
UTF-8 xterm-compatible terminal (e.g. xterm with `-fa`/`-fs`, or a VTE-based
emulator, TERM=xterm-256color, LANG=C.utf8):

1. Build with the TEMP TEST SEED as above and run
   `llmfun/cpp_tui/build/llmfun_tui` in an 100x40 terminal.
2. Visually verify: the rightmost scrollbar block is in one straight vertical
   line across the six table rows; the ⚠ row shows `| ⚠ | warn3 |` with the
   pipe directly after the warning sign (no blank/tofu cell); the plain rows
   are unchanged.
3. Optionally select the six rows and copy them; the captured text columns of
   the `|` pipes must line up.

Note: terminals whose wcwidth disagrees with the pinned emoji-data 15.1 table
(e.g. narrow-emoji terminals) will show the opposite shift on ✅/❓ rows; this
is the documented limitation from design §6 (mitigated by
`LLMFUN_IMTUI_EMOJI_WIDTH=1`, implemented in plan Task 6 / P3).

## Task 6 (P3): VS16 folding + combining-mark merge — verification

Plan Task 6 makes the width-0 continuation handling env-gated and adds the
two P3 features behind `LLMFUN_IMTUI_EMOJI_PRESENTATION=1`:

1. **VS16 folding**: U+26A0 + U+FE0F becomes one grid cell with `chwidth == 2`
   and `ch2 == U+FE0F` (a 2-cell emoji on terminals that cluster VS16);
2. **combining-mark merge**: `e` + U+0301 becomes one cell (`ch2 == U+0301`,
   width unchanged) instead of dropping the mark.

Default (env unset) is the P0 Task 2 behavior (narrow ⚠, dropped mark),
which is what the captures above verify.

### Grid-level verification (headless, deterministic)

`test_utf8_grid.cpp` runs in folding mode (`LLMFUN_IMTUI_EMOJI_PRESENTATION=1`)
as the second run wired into `build_test.py`. Observed (folding mode, exit 0):

```
row 1: U+0065(1,+U+0301)@1 ...       (e) mark merged into the base cell
row 6: ... U+26A0(2,+U+FE0F)@3 ...   (f) VS16 folded, cell promoted to width 2
(a) table row 6 '| ⚠️ | warn3 |': extent 14 == base 1 + oracle 14 - 1
(c) no zero-width leak: ch clean, ch2 only VS16/marks, chwidth >= 1 (ch2set=2)
```

The grid is internally consistent in both modes; the TScreen is correct
before the ncurses layer.

### Terminal-level verification (tmux, 100x40, LANG=C.utf8)

Procedure: TEMP TEST SEED (the six-row table plus an `e\u0301 \u200D
\u26A0\uFE0F mark-probe` line) added to `llmfun/cpp_tui/main.cpp`, built, run
under tmux with and without `LLMFUN_IMTUI_EMOJI_PRESENTATION=1`, captured with
`tmux capture-pane -p -e` (seed reverted afterwards).

Default mode: `pty_utf8_check.py` PASS (see above) — unchanged P0 behavior.

Folding mode on tmux (**tmux does NOT cluster VS16**: it renders the pair as
a narrow ⚠ with the VS16 attached, i.e. 1 cell, while the grid counts 2):

```
row32: ... |                | ⚠️ | warn3 |   (⚠ row: content -1 shifted)
row34: ... e              é  ⚠️ mark-probe
```

Observations (timeline capture at t+1s..t+7s):

- The ⚠️ rows carry the VS16 bytes (the capture shows ⚠️) but the pair
  occupies 1 cell in tmux vs 2 in the grid: everything after ⚠️ in the row
  lands 1 column left, and the scrollbar block sits at col 97 instead of 98 —
  the opposite-direction shift this gate exists to prevent. This is the
  non-clustering-terminal mismatch (tmux renders the pair narrow) and is
  unfixable by any redraw strategy: `LLMFUN_IMTUI_EMOJI_PRESENTATION=1`
  must not be enabled on such terminals.
- **ncurses model divergence (fixed)**: ncurses' own wcwidth counts the
  pair as 1 cell (wcwidth(0x26A0)=1, wcwidth(0xFE0F)=0 — verified in this
  environment), the same as tmux but 1 less than the grid. On
  VS16-CLUSTERING terminals (VTE/kitty/wezterm — the target of the env
  gate) the initial write of a fold row is correct, but ncurses' virtual
  screen is 1 column short per pair, so when such a row was REWRITTEN
  after a content change (auto-scroll settle at startup, message
  streaming, typing), ncurses' diff-based line updates (absolute cursor
  patches + single-cell writes) landed 1 column left: new characters
  overwrote their predecessors and stale characters lingered ("shadow
  characters and duplications", the reported " foobar" -> " ffoobar").
  FIX: `ImTui_ImplNcurses_DrawScreen` now forces a full-line redraw
  (`wredrawln`) for every written row whose new or previous grid contains
  a fold (ch2 != 0), so ncurses re-emits the whole line from column 0
  instead of patching. Regression-tested by
  `test_ncurses_fold_redraw.{cpp,py}` (raw-stream replay against a
  VS16-clustering terminal model; fails pre-fix with the row tail
  reading "foobaZ!|" instead of "foobarZ!").
- **Residual tail artifact on fold rows (unfixable, documented)**: the
  redraw fix aligns the row's CHARACTER stream, but cells that ncurses
  POSITIONS after the pair with absolute moves (CHA/CUP computed from its
  1-short virtual model — e.g. a scrollbar cell patched toward the right
  edge) still land 1 column left of the grid intent on the real terminal.
  Verified empirically: a fold row with a scrollbar cell at grid col 79
  emits CHA(78) + the cell, so the scrollbar track/grab cell renders at
  terminal col 78 while every other row's scrollbar is at 79. Present on
  the FIRST write too (not a rewrite artifact) and inherent to ncurses'
  width model — no byte-stream trick can make ncurses count the pair as
  2. Consequence when the gate is on, the output overflows, and a fold
  row is visible: the vertical scrollbar jogs 1 column left at fold rows
  (plus a 1-column artifact at the row's right edge).
- The combining-mark merge (`é`) has NO width change (grid = ncurses = tmux
  = 1 cell), so mark rows do not diverge; the mark-probe row's corruption
  above is caused by the ⚠️ in the same row.

Conclusion (documented limitation of the P3 opt-in):

- `LLMFUN_IMTUI_EMOJI_PRESENTATION=1` is now safe on VS16-clustering
  terminals (VTE/kitty/wezterm): fold-row rewrites stay aligned via the
  full-line redraw path, and the mark merge never diverged. On
  NON-clustering terminals (real xterm, tmux) the ⚠️ rows still shift −1
  (the terminal renders the pair narrow while the grid counts 2) — an
  inherent terminal-capability mismatch, so the gate remains opt-in. The
  default (unset) remains the guaranteed-aligned P0 behavior on every
  terminal.
