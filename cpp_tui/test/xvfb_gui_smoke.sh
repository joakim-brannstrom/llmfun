#!/bin/sh
# Opt-in GUI smoke on a headless runner. Requires xvfb-run + a GL driver (llvmpipe).
# Exit codes: 0 = pass, 77 = skipped (no Xvfb), other = failure.
command -v xvfb-run >/dev/null 2>&1 || { echo "SKIP: xvfb-run not available"; exit 77; }
BIN="${1:-./llmfun_tui}"
[ -x "$BIN" ] || { echo "SKIP/FAIL: $BIN not built"; exit 77; }
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a -s "-screen 0 1024x768x24" "$BIN" --gui --frames 30
