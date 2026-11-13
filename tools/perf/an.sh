#!/bin/sh
# an.sh <tag>: draws/render ms per 6 s, then stats over heavy presents (>= ${HEAVY:-8500} draws, t > 8 s):
# mean interval between consecutive heavy presents, slow share (> 40 ms), renderer busy ms, skipped guest frames.
L=$(dirname "$0")/../../_run/perf26/$1.log
grep -a "^\[present\]" $L | awk '{t=int($NF/6)*6; d[t]+=$4; r[t]+=$15; n[t]++} END{for(k in d) printf "%d:%d/%.1f\n", k, d[k]/n[k], r[k]/n[k]}' | sort -n | tr '\n' ' '; echo
grep -a "^\[present\]" $L | awk -v t=$1 -v H=${HEAVY:-8500} '{x=$NF; h=($4>=H)} h&&ph&&x>8{n++; s+=x-p; r+=$15; if (x-p>0.040) slow++} x>8{if(pf && $2!=pf+1) g+=$2-pf-1; pf=$2} {p=x; ph=h} END{if(n) printf "%s heavy %d interval %.1f ms (%.1f fps) slow %.0f%% render %.1f ms skipped %d\n", t, n, 1000*s/n, n/s, 100*slow/n, r/n, g; else print t, "no heavy presents"}'
