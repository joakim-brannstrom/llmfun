#!/usr/bin/env python3
"""Terminal-level verification for the imtui UTF-8 width fix (plan Task 4).

Parses a `tmux capture-pane -p -e` output file (ANSI SGR sequences included),
builds a cell grid per pane row (wide chars occupy 2 columns, zero-width
chars occupy 0), finds the six markdown-table rows from the bug report and
asserts:

  1. all six table rows are found;
  2. the rightmost occupied cell (scrollbar block) column is identical across
     all six rows (the scrollbar block is the rightmost column whose cells
     carry a non-default background attribute; content cells never reach it);
  3. the cell immediately after U+26A0 (WARNING SIGN) in its row contains the
     row's next visible character -- no dangling blank cell (pre-fix the
     U+FE0F variation selector occupied its own cell and rendered as a blank);
  4. a plain-ASCII control row is unchanged (text intact, same scrollbar
     column -- no regression for non-emoji rows).

Exit code 0 iff every assertion passes; non-zero otherwise, with a diagnostic
per failure.

Terminal ground truth: tmux's grid is the source of truth here. Wide
characters occupy 2 columns in the capture; zero-width characters (U+FE0F,
U+200D, ...) occupy 0 columns but may still appear as bytes in the stream.
The parser uses its own independent width oracle (adapted Kuhn wcwidth +
emoji-data.txt 15.1 emoji-presentation list, like test_utf8_grid.cpp) to
reconstruct columns; it does NOT call into imtui's width table, so the check
is not self-referential.

IMPORTANT: capture with `tmux capture-pane -p -e` (the -e keeps the SGR
attribute sequences; without them the scrollbar block -- spaces with a
background -- is indistinguishable from padding and the check cannot run).

NOTE: assertion 3 hard-codes the Task 2 fallback behavior (narrow ⚠, VS16
dropped). If plan Task 6 (P3, emoji presentation via VS16 / combining-mark
merge) ever lands, update this checker's assertion 3 together with the Task 3
grid test, which is already required to be updated there.

Usage:
    python3 pty_utf8_check.py <capture.txt>
"""

import re
import sys

# ---------------------------------------------------------------------------
# Independent width oracle (same reference data as test_utf8_grid.cpp).
# ---------------------------------------------------------------------------

ZERO_WIDTH_RANGES = [
    (0x0000, 0x0000),  # NUL
    (0x0001, 0x001F),  # control characters
    (0x007F, 0x009F),  # DEL.. (Kuhn control rule; NBSP U+00A0 is width 1)
    (0x0300, 0x036F),  # combining diacritical marks
    (0x1AB0, 0x1AFF),  # combining diacritical marks extended
    (0x1DC0, 0x1DFF),  # combining diacritical marks supplement
    (0x200B, 0x200F),  # ZWSP, ZWNJ, ZWJ (U+200D), LRM, RLM
    (0x2028, 0x202E),  # line/paragraph separators, bidi controls
    (0x2060, 0x206F),  # word joiner, invisible operators
    (0x20D0, 0x20FF),  # combining marks for symbols
    (0xFE00, 0xFE0F),  # variation selectors (incl. VS16 U+FE0F)
    (0xFE20, 0xFE2F),  # combining half marks
    (0xFEFF, 0xFEFF),  # BOM / ZWNBSP
    (0xFFF9, 0xFFFB),  # interlinear annotation anchors
]

WIDE_RANGES = [
    (0x1100, 0x115F),  # Hangul Jamo
    (0x231A, 0x231B),  # watch, hourglass done (emoji presentation)
    (0x2329, 0x232A),  # angle brackets
    (0x23E9, 0x23F3),  # fast-forward..hourglass (emoji presentation)
    (0x23F8, 0x23FA),  # double vertical bar..record button
    (0x25FD, 0x25FE),  # medium small squares
    (0x2614, 0x2615),  # umbrella, hot beverage
    (0x2648, 0x2653),  # zodiac signs
    (0x267F, 0x267F),  # wheelchair
    (0x2693, 0x2693),  # anchor
    (0x26A1, 0x26A1),  # high voltage
    (0x26AA, 0x26AB),  # medium circles
    (0x26BD, 0x26BE),  # soccer, baseball
    (0x26C4, 0x26C5),  # snowman, sun behind cloud
    (0x26CE, 0x26CE),  # ophiuchus
    (0x26D4, 0x26D4),  # no entry
    (0x26EA, 0x26EA),  # church
    (0x26F2, 0x26F3),  # fountain, flag in hole
    (0x26F5, 0x26F5),  # sailboat
    (0x26FA, 0x26FA),  # tent
    (0x26FD, 0x26FD),  # fuel pump
    (0x2705, 0x2705),  # white heavy check mark
    (0x270A, 0x270B),  # raised fist, raised hand
    (0x2728, 0x2728),  # sparkles
    (0x274C, 0x274C),  # cross mark
    (0x274E, 0x274E),  # negative squared cross mark
    (0x2753, 0x2755),  # question/exclamation ornament marks
    (0x2757, 0x2757),  # heavy exclamation mark
    (0x2795, 0x2797),  # heavy plus/minus/division sign
    (0x27B0, 0x27B0),  # curly loop
    (0x27BF, 0x27BF),  # double curly loop
    (0x2B1B, 0x2B1C),  # black/white large square
    (0x2B50, 0x2B50),  # star
    (0x2B55, 0x2B55),  # heavy large circle
    (0x2E80, 0xA4CF),  # CJK Radicals..Yi (existing wide ranges)
    (0xA960, 0xA97C),  # Hangul Jamo Extended-A
    (0xAC00, 0xD7A3),  # Hangul syllables
    (0xF900, 0xFAFF),  # CJK compatibility ideographs
    (0xFE10, 0xFE19),  # vertical forms
    (0xFE30, 0xFE6F),  # CJK compatibility forms
    (0xFF01, 0xFF60),  # fullwidth forms
    (0xFFE0, 0xFFE6),  # fullwidth signs
    (0x1F000, 0x1F644),  # mahjong..face with rolling eyes
    (0x1F300, 0x1F6FF),  # cyclone..transport symbols
    (0x1F7E0, 0x1F7EB),  # large colored circles
    (0x1F7F0, 0x1F7F0),  # heavy equals sign
    (0x1F900, 0x1F9FF),  # supplemental symbols and pictographs
    (0x1FA70, 0x1FAFF),  # symbols and pictographs extended-A
    (0x20000, 0x2FFFD),  # CJK extension B+
    (0x30000, 0x3FFFD),  # CJK extension G
]


def oracle_width(cp):
    """Independent cell-width oracle: 0/1/2 (matches conforming terminals)."""
    for lo, hi in ZERO_WIDTH_RANGES:
        if lo <= cp <= hi:
            return 0
    # Regional indicators stay width 1 each (flag pairs are a documented
    # grapheme-cluster limitation; must win over the coarse 1F000 range).
    if 0x1F1E6 <= cp <= 0x1F1FF:
        return 1
    for lo, hi in WIDE_RANGES:
        if lo <= cp <= hi:
            return 2
    return 1


# ---------------------------------------------------------------------------
# Capture parsing: strip/parse ANSI SGR, build a cell grid per row.
# ---------------------------------------------------------------------------

SGR_RE = re.compile(r"\x1b\[([0-9;]*)m")


def parse_capture(path):
    """Return a list of rows; each row is a dict {col: (char, bg_or_None)}.

    Wide chars occupy 2 columns (char at col, col+1 has no cell); zero-width
    chars occupy 0 columns but are still recorded so callers can detect them.
    Cells that carry a non-default background SGR attribute get bg != None.
    Also returns (rows, saw_sgr) where saw_sgr tells whether the file
    contained any SGR sequence at all (a plain `-p` capture loses the
    scrollbar block and cannot run the column checks).
    """
    with open(path, "rb") as f:
        # errors="replace": a wide char clipped at the pane's right edge (or
        # any truncated UTF-8 tail) decodes as U+FFFD, which the oracle
        # counts as width 1. Harmless for the asserted rows (none touch the
        # right edge), and keeps the parser total instead of crashing on a
        # partial multibyte sequence.
        data = f.read().decode("utf-8", errors="replace")

    saw_sgr = False
    rows = []
    for line in data.split("\n"):
        cells = {}
        col = 0
        bg = None
        i = 0
        n = len(line)
        while i < n:
            if line[i] == "\x1b":
                m = SGR_RE.match(line, i)
                if m:
                    saw_sgr = True
                    params = m.group(1)
                    if params == "" or params == "0":
                        bg = None
                    else:
                        try:
                            parts = [int(p) for p in params.split(";") if p != ""]
                        except ValueError:
                            # Unknown SGR form (e.g. colon syntax 38:5:236):
                            # keep the current background rather than crash.
                            i = m.end()
                            continue
                        # Parameters apply left to right; the last one wins.
                        # A 48;5;N triplet may appear anywhere in the sequence
                        # (combined forms like 1;48;5;236m or 0;48;5;236m),
                        # not just as the first parameter. 49 resets the
                        # background to default.
                        for k in range(len(parts)):
                            if parts[k] == 48 and k + 2 < len(parts) and parts[k + 1] == 5:
                                bg = parts[k + 2]
                            elif parts[k] == 49:
                                bg = None
                    i = m.end()
                    continue
                # Some other escape sequence: skip to its final byte.
                i += 1
                while i < n and not (0x40 <= ord(line[i]) <= 0x7E):
                    i += 1
                i += 1
                continue
            ch = line[i]
            cp = ord(ch)
            w = oracle_width(cp)
            if w == 0:
                # Zero-width char: occupies no column, record at current col.
                cells.setdefault(col, []).append((ch, bg))
            else:
                cells.setdefault(col, []).append((ch, bg))
                col += w
            i += 1
        rows.append(cells)
    return rows, saw_sgr


def row_text(cells):
    """Visible text of a row: chars in column order (zero-width chars kept)."""
    out = []
    for col in sorted(cells):
        for ch, _ in cells[col]:
            out.append(ch)
    return "".join(out)


def row_text_visible(cells):
    """Visible text with zero-width chars removed (for pattern matching)."""
    out = []
    for col in sorted(cells):
        for ch, _ in cells[col]:
            if oracle_width(ord(ch)) != 0:
                out.append(ch)
    return "".join(out)


def row_has_char(cells, target):
    for col in sorted(cells):
        for ch, _ in cells[col]:
            if ch == target:
                return col
    return None


def scrollbar_col(cells):
    """Rightmost column whose cells carry a non-default background attribute.

    Returns None if no such column exists in this row.
    """
    sb = None
    for col in sorted(cells):
        for ch, bg in cells[col]:
            if bg is not None:
                if sb is None or col > sb:
                    sb = col
    return sb


# ---------------------------------------------------------------------------
# Expected six table rows (bug report: | plain | a | .. | ✅ | done4 |).
# ---------------------------------------------------------------------------

EXPECTED = [
    ("row0 plain a", "| plain | a |", None),
    ("row1 plain b", "| plain | b |", None),
    ("row2 check",   None, "\u2705"),  # ✅
    ("row3 question", None, "\u2753"),  # ❓
    ("row4 warn",    None, "\u26A0"),   # ⚠
    ("row5 check2",  None, "\u2705"),   # ✅
]

TOKENS = {
    "row0 plain a": "a |",
    "row1 plain b": "b |",
    "row2 check":   "task1",
    "row3 question": "ask2",
    "row4 warn":    "warn3",
    "row5 check2":  "done4",
}


def main():
    if len(sys.argv) != 2:
        print("usage: pty_utf8_check.py <capture.txt>")
        return 2
    rows, saw_sgr = parse_capture(sys.argv[1])
    print(f"parsed {len(rows)} pane rows from {sys.argv[1]}")

    failures = 0

    # ---- Assertion 1: find the six table rows -----------------------------
    found = {}
    for row_idx, cells in enumerate(rows):
        text = row_text_visible(cells)
        for name, _, emoji in EXPECTED:
            tok = TOKENS[name]
            if name in found:
                continue
            if tok in text and (emoji is None or emoji in text):
                found[name] = row_idx
    for name, _, _ in EXPECTED:
        if name not in found:
            print(f"FAIL (1): table row {name} not found")
            failures += 1

    if failures:
        print("row texts (for debugging):")
        for row_idx, cells in enumerate(rows):
            print(f"  {row_idx:2d}: {row_text_visible(cells)!r}")
        return 1

    # ---- Assertion 2: scrollbar block column identical across all six -----
    sb_cols = {}
    for name, _, _ in EXPECTED:
        sb = scrollbar_col(rows[found[name]])
        sb_cols[name] = sb
        if sb is None:
            if not saw_sgr:
                print(f"FAIL (2): {name} has no bg-colored scrollbar cell and the "
                      f"capture has no SGR at all -- recapture with "
                      f"`tmux capture-pane -p -e` (the scrollbar block is "
                      f"spaces with a background; a plain capture loses it)")
            else:
                print(f"FAIL (2): {name} has no bg-colored scrollbar cell "
                      f"(scrollbar block missing from this row -- pre-fix the "
                      f"row is shifted so the block lands outside the pane)")
            failures += 1
    if not failures:
        unique = set(v for v in sb_cols.values())
        if len(unique) != 1:
            print(f"FAIL (2): scrollbar block column differs across rows: {sb_cols}")
            failures += 1
        else:
            print(f"PASS (2): scrollbar block column {unique.pop()} for all six rows")

    # ---- Assertion 3: no dangling blank cell after U+26A0 -----------------
    # Single verdict: the row must contain no U+FE0F AND the cells right
    # after ⚠ must be exactly one char each (' ' then '|'). The position
    # check alone is not enough: pre-fix, the zero-width U+FE0F is recorded
    # in the same capture cell as the following ' ' (stacked), so requiring
    # exactly one char per cell plus the FE0F byte check makes the verdict
    # atomic. NOTE: this asserts the Task 2 fallback (narrow ⚠, VS16
    # dropped); if Task 6 (P3, VS16 folding to a 2-cell ⚠️) lands, update
    # this assertion alongside the Task 3 grid test.
    warn_idx = found["row4 warn"]
    warn_cells = rows[warn_idx]
    text = row_text(warn_cells)
    fe0f_leak = "\uFE0F" in text
    wcol = row_has_char(warn_cells, "\u26A0")
    if wcol is None:
        print("FAIL (3): U+26A0 not found in the ⚠ row")
        failures += 1
    else:
        # Row text is `| ⚠ | warn3 |`: after ⚠ must come ' ' then '|',
        # each cell holding exactly one character.
        nxt_cells = warn_cells.get(wcol + 1, [])
        nxt2_cells = warn_cells.get(wcol + 2, [])
        pos_ok = (len(nxt_cells) == 1 and nxt_cells[0][0] == " " and
                  len(nxt2_cells) == 1 and nxt2_cells[0][0] == "|")
        if fe0f_leak:
            print("FAIL (3): U+FE0F (VS16) present in the ⚠ row "
                  "(dangling blank cell / zero-width cell leaked)")
            failures += 1
        if not pos_ok:
            print(f"FAIL (3): cells after ⚠ at col {wcol} are not exactly "
                  f"' ' at {wcol + 1} and '|' at {wcol + 2} "
                  f"(got {nxt_cells!r} and {nxt2_cells!r})")
            failures += 1
        if not fe0f_leak and pos_ok:
            print(f"PASS (3): ⚠ at col {wcol}, next visible char ' ' at "
                  f"{wcol + 1}, '|' at {wcol + 2}; no blank cell (no U+FE0F)")

    # ---- Assertion 4: plain-ASCII control row unchanged -------------------
    plain_idx = found["row0 plain a"]
    plain_cells = rows[plain_idx]
    plain_text = row_text_visible(plain_cells)
    expected_plain = "| plain | a |"
    # Strip leading indentation the renderer may add; the row itself must
    # equal the expected text and end at the same scrollbar column.
    stripped = plain_text.strip()
    if stripped != expected_plain:
        print(f"FAIL (4): plain control row text {stripped!r} != {expected_plain!r}")
        failures += 1
    elif sb_cols["row0 plain a"] != sb_cols["row4 warn"]:
        print("FAIL (4): plain control row scrollbar column differs from ⚠ row")
        failures += 1
    else:
        print(f"PASS (4): plain control row {expected_plain!r} unchanged")

    if failures:
        print(f"\n{len(rows)} rows parsed; {failures} assertion(s) FAILED")
        return 1
    print("\nAll PTY assertions passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
