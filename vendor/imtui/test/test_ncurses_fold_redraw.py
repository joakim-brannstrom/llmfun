#!/usr/bin/env python3
"""Terminal-level regression test for the VS16-fold row-rewrite corruption.

Drives the C++ binary test_ncurses_fold_redraw (built as the CMake target
imtui_ncurses_fold_redraw_test) on a real PTY, records the raw ncurses
output stream, and REPLAYS it against a model of a VS16-clustering
terminal (the pair U+26A0 U+FE0F renders 2 cells wide), then asserts the
final row content.

Why this layer: the bug is only observable in the terminal stream. The
TScreen grid is internally consistent pre-fix, and tmux counts the pair as
1 cell (like ncurses' own model), so neither a headless grid test nor a
tmux capture-pane can reproduce it.

The bug: ncurses' width model counts the folded VS16 pair as ONE cell
(wcwidth(U+26A0)=1, wcwidth(U+20FE0F)=0) while the grid and clustering
terminals count TWO. The first write of the row is correct (the terminal
applies its own model to the byte stream), but ncurses' virtual screen is
1 column short, so when the row is rewritten after a content change,
ncurses' diff-based patches (absolute cursor moves + single-cell writes)
land 1 column left and corrupt the row. The fix forces a full-line redraw
(wredrawln) for rows whose new or previous grid contains a fold.

Assertions (run twice: 3 frames and 4 frames):
  3 frames (pair present throughout, row rewritten twice):
    row 0 == " | ⚠ | foobarZ!" with the pair cell at col 3 staying 2-wide
    -- pre-fix the frame-2/3 patches land 1 left: 'Z' overwrites 'r' and
    '!' lands on the pipe space, so the row tail reads "foobaZ!|" and the
    assertion fails.
  4 frames (adds the fold-removal transition, pair -> plain ⚠):
    row 0 == " | ⚠  | foobarZ!" with the cell at col 3 back to width 1.
    Post-fix guard only: removing a fold shifts the whole virtual tail,
    which forces a wide rewrite via absolute addressing that self-heals
    even pre-fix; the fix's prev-grid check keeps this transition on the
    full-line redraw path as insurance.
  Both runs: control row 1 keeps "foobar" at columns 2..7.

Exit code 0 iff every assertion passes; non-zero otherwise, with a
diagnostic per failure.

Usage:
    python3 test_ncurses_fold_redraw.py <path-to-imtui_ncurses_fold_redraw_test>
"""

import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import termios

# ---------------------------------------------------------------------------
# Minimal terminal replayer (VS16-clustering model).
# ---------------------------------------------------------------------------

class Grid:
    def __init__(self, rows=24, cols=80):
        self.rows = rows
        self.cols = cols
        # cell: {row: {col: (ch, width)}}; a width-2 cell covers col..col+1
        self.cells = {}
        self.cur_row = 0
        self.cur_col = 0

    def clear_all(self):
        self.cells = {}

    def clear_to_eol(self):
        row = self.cells.setdefault(self.cur_row, {})
        for col in list(row.keys()):
            if col >= self.cur_col:
                del row[col]

    def erase_chars(self, n):
        row = self.cells.setdefault(self.cur_row, {})
        for col in range(self.cur_col, self.cur_col + n):
            if col in row:
                del row[col]
            if col - 1 in row and row[col - 1][1] == 2:
                del row[col - 1]  # erase a 2-wide cell when hitting its 2nd col

    def _clear_covering(self, col):
        row = self.cells.setdefault(self.cur_row, {})
        if col in row:
            del row[col]
        if col - 1 in row and row[col - 1][1] == 2:
            del row[col - 1]

    def put(self, cp):
        """Write one codepoint at the cursor with VS16 clustering."""
        row = self.cells.setdefault(self.cur_row, {})
        if cp == 0xFE0F:
            # VS16 after a text-default emoji base clusters into a 2-wide
            # cell (VS16-clustering terminal model). Only 0x26A0 is used by
            # this test; any other base leaves the selector as a no-op.
            if self.cur_col - 1 in row:
                base, width = row[self.cur_col - 1]
                if width == 1 and base == 0x26A0:
                    row[self.cur_col - 1] = (base, 2)
                    row.pop(self.cur_col, None)
                    self.cur_col += 1  # pair occupies one extra column
            return
        if cp < 32 or cp == 0x7F:
            return  # control chars don't print
        self._clear_covering(self.cur_col)
        row[self.cur_col] = (cp, 1)
        self.cur_col += 1

    def move(self, row, col):
        self.cur_row = max(0, min(row, self.rows - 1))
        self.cur_col = max(0, min(col, self.cols - 1))


CSI_RE = None  # parsed manually below


def replay(raw, rows=24, cols=80, grid=None):
    """Replay a raw ncurses stream; return the final Grid."""
    if grid is None:
        grid = Grid(rows, cols)
    data = bytes(raw)
    i = 0
    n = len(data)

    def parse_csi():
        nonlocal i
        # at ESC '['; find the final byte (0x40..0x7E)
        j = i + 1
        while j < n and not (0x40 <= data[j] <= 0x7E):
            j += 1
        if j >= n:
            return None, None
        body = data[i + 1:j].decode("ascii", "replace")
        final = chr(data[j])
        i = j + 1
        params = []
        if body and body[0] in "?>":
            body = body[1:]
        if body:
            for piece in body.split(";"):
                params.append(int(piece) if piece.isdigit() else 0)
        return params, final

    while i < n:
        b = data[i]
        if b == 0x1B:
            if i + 1 >= n:
                break
            if data[i + 1] == 0x5B:  # CSI
                i += 1
                params, final = parse_csi()
                if final is None:
                    break
                if final in ("H", "f"):  # CUP / HVP
                    row = (params[0] if len(params) > 0 and params[0] else 1) - 1
                    col = (params[1] if len(params) > 1 and params[1] else 1) - 1
                    grid.move(row, col)
                elif final == "d":  # VPA
                    row = (params[0] if len(params) > 0 and params[0] else 1) - 1
                    grid.move(row, grid.cur_col)
                elif final == "G":  # CHA
                    col = (params[0] if len(params) > 0 and params[0] else 1) - 1
                    grid.move(grid.cur_row, col)
                elif final == "A":  # CUU
                    n = params[0] if params and params[0] else 1
                    grid.move(grid.cur_row - n, grid.cur_col)
                elif final == "B":  # CUD
                    n = params[0] if params and params[0] else 1
                    grid.move(grid.cur_row + n, grid.cur_col)
                elif final == "C":  # CUF
                    n = params[0] if params and params[0] else 1
                    grid.move(grid.cur_row, grid.cur_col + n)
                elif final == "D":  # CUB
                    n = params[0] if params and params[0] else 1
                    grid.move(grid.cur_row, grid.cur_col - n)
                elif final == "E":  # CNL
                    n = params[0] if params and params[0] else 1
                    grid.move(grid.cur_row + n, 0)
                elif final == "F":  # CPL
                    n = params[0] if params and params[0] else 1
                    grid.move(grid.cur_row - n, 0)
                elif final == "J":  # ED
                    if not params or params[0] == 0:
                        pass  # clear to end of screen (rows below / rest of row)
                    elif params[0] == 2:
                        grid.clear_all()
                elif final == "K":  # EL
                    mode = params[0] if params else 0
                    if mode == 1:  # clear to start of line
                        row_cells = grid.cells.setdefault(grid.cur_row, {})
                        for col in list(row_cells.keys()):
                            if col <= grid.cur_col:
                                del row_cells[col]
                    elif mode == 2:  # whole line
                        grid.cells.pop(grid.cur_row, None)
                    else:
                        grid.clear_to_eol()
                elif final == "X":  # ECH
                    nchars = params[0] if params and params[0] else 1
                    grid.erase_chars(nchars)
                # everything else (SGR, mode sets, window ops) is ignored
                continue
            if data[i + 1] in (0x28, 0x29, 0x2A, 0x2B, 0x25, 0x2D, 0x2E, 0x2F):
                # 3-byte charset/SCS designations: ESC ( B, ESC ) 0, ESC * x,
                # ESC + x, ESC % G, ESC - x, ESC . x, ESC / x. Must consume
                # all three bytes — otherwise the third byte (e.g. 'B' from
                # the constant ESC ( B stream) is replayed as a printable
                # character at the current cursor and corrupts the grid.
                i += 3
                continue
            if data[i + 1] in (0x3D, 0x3E, 0x37, 0x38):
                # 2-byte sequences: ESC = / ESC > (keypad mode), ESC 7 /
                # ESC 8 (DECSC/DECRC save+restore cursor — positions are
                # preserved implicitly, nothing to replay).
                i += 2
                continue
            i += 1
            continue
        if b == 0x0A:  # LF
            grid.move(grid.cur_row + 1, grid.cur_col)
            i += 1
            continue
        if b == 0x0D:  # CR
            grid.move(grid.cur_row, 0)
            i += 1
            continue
        if b == 0x08:  # BS
            grid.move(grid.cur_row, grid.cur_col - 1)
            i += 1
            continue
        if b < 0x80:
            if b >= 0x20:
                grid.put(b)
            i += 1
            continue
        # UTF-8 multibyte: decode one codepoint (Python 3 string trick)
        j = i
        while j < n and data[j] >= 0x80:
            j += 1
        try:
            ch = data[i:j].decode("utf-8")
        except UnicodeDecodeError:
            ch = ""
        for cp in ch:
            grid.put(ord(cp))
        i = j

    return grid


# ---------------------------------------------------------------------------
# PTY driver
# ---------------------------------------------------------------------------

def run_binary(binary, frames):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    env = dict(os.environ)
    env["TERM"] = "xterm-256color"
    env["LANG"] = "C.utf8"
    env["LC_ALL"] = "C.utf8"
    proc = subprocess.Popen(
        [binary, str(frames)],
        stdin=slave, stdout=slave, stderr=slave,
        env=env, close_fds=True,
    )
    os.close(slave)
    data = bytearray()
    silence = 0
    deadline = 20
    try:
        while True:
            ready, _, _ = select.select([master], [], [], 1.0)
            if ready:
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                data.extend(chunk)
                silence = 0
            else:
                silence += 1
                deadline -= 1
                if proc.poll() is not None:
                    break
                if silence >= 3 or deadline <= 0:
                    break
    finally:
        try:
            proc.kill()
        except OSError:
            pass
        try:
            os.close(master)
        except OSError:
            pass
    return bytes(data)


def visible(cells_row, start, end):
    """Reconstruct visible text of a row segment from the Grid cell dict."""
    out = []
    col = start
    while col < end:
        cell = cells_row.get(col)
        if cell is None:
            if col - 1 in cells_row and cells_row[col - 1][1] == 2:
                col += 1
                continue
            out.append(" ")
            col += 1
        else:
            ch, width = cell
            out.append(chr(ch))
            col += width
    return "".join(out)


def check(binary, frames, expected_row0, expected_pair_width, expected_row1):
    raw = run_binary(binary, frames)
    if not raw:
        print(f"FAIL ({frames} frames): no output captured from {binary}")
        return False
    grid = replay(raw)
    row0 = grid.cells.get(0, {})
    ok = True
    got = visible(row0, 0, 16)
    if got != expected_row0:
        print(f"FAIL ({frames} frames): row 0 mismatch")
        print(f"  expected: {expected_row0!r}")
        print(f"  got:      {got!r}")
        ok = False
    pair_cell = row0.get(3)
    if pair_cell != (0x26A0, expected_pair_width):
        print(f"FAIL ({frames} frames): pair cell at col 3 is {pair_cell!r}, "
              f"expected (0x26A0, {expected_pair_width})")
        ok = False
    row1 = grid.cells.get(1, {})
    got1 = visible(row1, 2, 8)
    if got1 != expected_row1:
        print(f"FAIL ({frames} frames): row 1 mismatch")
        print(f"  expected: {expected_row1!r}")
        print(f"  got:      {got1!r}")
        ok = False
    if ok:
        print(f"PASS ({frames} frames): row 0 == {got!r}, pair cell width "
              f"{expected_pair_width}, row 1 control intact")
    return ok


def main():
    if len(sys.argv) < 2:
        print("usage: test_ncurses_fold_redraw.py <path-to-imtui_ncurses_fold_redraw_test>")
        return 2
    binary = sys.argv[1]
    if not os.path.exists(binary):
        print(f"Binary not found: {binary} (build first; see build_test.py)")
        return 1

    ok = True
    # 3 frames: pair present the whole time; row rewritten twice. The pair
    # cell must stay 2-wide and the row tail must read "foobarZ!" — pre-fix
    # the frame-2/3 patches land 1 left ('Z' overwrites 'r', '!' hits the
    # pipe space) and the tail reads "foobaZ!|".
    ok &= check(binary, 3, " | \u26a0 | foobarZ!", 2, "foobar")
    # 4 frames: fold removed in the last frame -> plain 1-wide ⚠ and the
    # placeholder cell becomes a real space.
    ok &= check(binary, 4, " | \u26a0  | foobarZ!", 1, "foobar")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
