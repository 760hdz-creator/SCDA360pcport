#!/bin/sh
# usage: dump.sh <tag> [ENV=..]  dumps frames FR (window + rt) -> _run/gl/dump/<tag>_[rt_]<n>.bmp
# env: EXE, DLA (LOCALAPPDATA rel. to repo), DL (launch file), DS (settings ini), FR, AP, T
cd /c/SCDA-Recomp/_install
tag=$1; shift; mkdir -p ../_run/gl/dump; rm -f scda_window_*.bmp scda_frame_*.bmp
env LOCALAPPDATA="$(cygpath -w /c/SCDA-Recomp/${DLA:-_run/i26/la})" SCDA_SETTINGS="${DS:-C:\\SCDA-Recomp\\_run\\i26\\s1.ini}" \
  SCDA_LAUNCH_DATA=${DL:-C:/SCDA-Recomp/_run/maps/okh02_snow_i26.launch} SCDA_AUTOPRESS=${AP:-999999:A} \
  SCDA_DUMP_FRAME=${FR:-700,900} SCDA_DUMP_BACKBUFFER=1 SCDA_TRACE_PRESENT=1 "$@" timeout ${T:-50} ./${EXE:-scda_dev.exe} >/dev/null 2>&1
for f in scda_window_*.bmp; do [ -f "$f" ] && mv "$f" ../_run/gl/dump/${tag}_${f#scda_window_}; done
for f in scda_frame_*.bmp; do [ -f "$f" ] && mv "$f" ../_run/gl/dump/${tag}_rt_${f#scda_frame_}; done
echo "$tag fatal=$(grep -a -c '\[fatal\] exception' scda.log) $(grep -a 'internal resolution' scda.log | head -1) $(grep -a '\[present\]' scda.log | tail -1 | cut -c1-40)"
