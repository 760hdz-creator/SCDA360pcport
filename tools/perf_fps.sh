# usage: perf_ab.sh <map> [env...] -> fps over t=40..70 s from SCDA_TRACE_PRESENT
cd /c/SCDA-Recomp/_install
m=$1; shift
env SCDA_LAUNCH_DATA="C:/SCDA-Recomp/_run/maps/$m.launch" SCDA_TRACE_PRESENT=1 "$@" timeout 75 ./scda_dev.exe >/dev/null 2>&1
grep -a "^\[present\]" scda.log | awk -v m=$m '{t=$NF} t>=40&&t<70{n++} END{printf "%s fps %.1f\n", m, n/30}'
