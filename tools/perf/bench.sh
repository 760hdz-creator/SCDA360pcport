#!/bin/sh
# Issue #26 perf bench. usage: tools/perf/bench.sh <tag> [ENV=..]   (env: BENCH_LAUNCH, BENCH_TIMEOUT, BENCH_LA)
# Boots _install/scda_dev.exe on a save launch, ignoring live keyboard/mouse/pad (dummy SCDA_AUTOPRESS: the
# window grabs the mouse, so a moved mouse turned the camera and made old benches noisy), logs [present]
# lines and prints heavy-frame stats (tools/perf/an.sh). Log -> _run/perf26/<tag>.log.
# Add SCDA_VBLANK_HZ=240 to lift the title's 30 fps cap (throughput, not the cap).
# Default launch: _run/maps/okh02_snow_i26.launch (user save, Okhotsk_02 snow, ~9150 draws) with
# BENCH_LA (LOCALAPPDATA holding that save; default _run/i26/la).
cd "$(dirname "$0")/../../_install" || exit 1
tag=$1; shift; mkdir -p ../_run/perf26
LA=$(cygpath -w "${BENCH_LA:-/c/SCDA-Recomp/_run/i26/la}")
env LOCALAPPDATA="$LA" SCDA_SETTINGS='C:\SCDA-Recomp\_run\i26\s1.ini' \
    SCDA_LAUNCH_DATA="${BENCH_LAUNCH:-C:/SCDA-Recomp/_run/maps/okh02_snow_i26.launch}" \
    SCDA_TRACE_PRESENT=1 SCDA_AUTOPRESS=999999:A "$@" timeout "${BENCH_TIMEOUT:-75}" ./scda_dev.exe >/dev/null 2>&1
cp scda.log ../_run/perf26/$tag.log
sh ../tools/perf/an.sh $tag
