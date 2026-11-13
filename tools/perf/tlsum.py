# tlsum.py <scda.log with SCDA_TIMELINE>: per guest frame (E events): ms/frame, renderer busy (R..r),
# CP parse (sum of T durations, hex us), guest GPU waits by function (A events). Stages overlap.
import sys
ev = []
for line in open(sys.argv[1], errors='replace'):
    if line.startswith('[tl]'):
        t, k, a, b = line.split()[1:5]
        ev.append((float(t), k, a, b))
E = [t for t, k, a, b in ev if k == 'E']
if len(E) < 2:
    sys.exit('need 2+ frame ends in the window')
t0, t1 = E[0], E[-1]
n = len(E) - 1
render, start, cp, waits = 0.0, None, 0.0, {}
for t, k, a, b in ev:
    if not t0 <= t <= t1:
        continue
    if k == 'R':
        start = t
    elif k == 'r' and start is not None:
        render += t - start
        start = None
    elif k == 'T':
        cp += int(a, 16) / 1000
    elif k == 'A':
        waits[a] = waits.get(a, 0) + int(b) / 1000
print('frames %d  %.1f ms/frame  render %.1f  CP %.1f ms/frame' % (n, (t1 - t0) / n, render / n, cp / n))
for a, v in sorted(waits.items(), key=lambda x: -x[1])[:4]:
    print('  guest wait %s %.1f ms/frame (%.0f%%)' % (a, v / n, 100 * v / (t1 - t0)))
