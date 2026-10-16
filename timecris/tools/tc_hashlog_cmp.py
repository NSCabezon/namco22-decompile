#!/usr/bin/env python3
"""tc_hashlog_cmp.py -- compare the sessions of tc_core's hash logs (TC_CORE_HASHLOG; vision/tc_core_test.m TC_TEST_QUIT).

    tc_hashlog_cmp.py LOG [LOG ...]

Session 0 of the first log is the reference (a cold boot). Every other session, of every log, must have the same line for every
frame: the board-memory hash ("ram", with the frame number rr) and the picture hash ("px"). Prints one line per session and the
first frames that differ; exits 1 if any session differs or is shorter than the reference."""
import re, sys
from collections import defaultdict

def load(path):
    sess = defaultdict(dict)                      # session -> {(frame, kind): value}
    for line in open(path):
        m = re.match(r"S(\d+) F(\d+) (ram rr=\d+|px) ([0-9a-f]+)", line)
        if m:
            sess[int(m.group(1))][(int(m.group(2)), m.group(3).split()[0])] = (m.group(3), m.group(4))
    return sess

logs = [(p, load(p)) for p in sys.argv[1:]]
if not logs or 0 not in logs[0][1]:
    sys.exit("no session 0 in %s" % (sys.argv[1:] or ["(no log)"]))
ref = logs[0][1][0]
frames = max(f for f, _ in ref)
print("reference: %s session 0, %d frames (%d ram + %d px hashes)" % (logs[0][0], frames, sum(k == "ram" for _, k in ref), sum(k == "px" for _, k in ref)))
bad = 0
for path, sess in logs:
    for s in sorted(sess):
        if path == logs[0][0] and s == 0:
            continue
        got = sess[s]
        diff = sorted(k for k in ref if got.get(k) != ref[k])
        missing = sum(1 for k in ref if k not in got)
        if diff:
            bad += 1
            print("%s session %d: %d of %d hashes differ (%d missing); first: %s" % (path, s, len(diff), len(ref), missing,
                  ", ".join("F%d %s %s vs %s" % (f, k, got.get((f, k), ("-", "-"))[1], ref[(f, k)][1]) for f, k in diff[:3])))
        else:
            print("%s session %d: identical, %d frames" % (path, s, frames))
sys.exit(1 if bad else 0)
