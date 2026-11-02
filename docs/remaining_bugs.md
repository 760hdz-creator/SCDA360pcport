# Remaining bugs (2026-10-31)

Ranked by impact on a first release. Each entry: symptom, repro, evidence, suspected cause, status.
Evidence paths under `_run/` are local only (git-ignored).

## FIXED. Okhotsk_02: open water shows moire rings / fine stripes (2026-11-02)

- **Symptom (user screenshot):** from the ice, the open water is covered in concentric rings and fine diagonal
  stripes; the 360 (`_run/ref/okh/x360_m4.mp4` ~650-700 s) shows dark, smooth, reflective water. Not caused by
  the lens-drop workaround (same with `SCDA_NO_LENS_DROP_FIX=1 SCDA_ALIAS_RAW_Z=1`).
- **Repro:** `SCDA_LAUNCH_DATA=_run/okhblue/save2.launch` (user save on the ice), dump frame 700.
- **Evidence:** water = VS dba2eb38 / PS 56bb3779; slots 10-12 are 128x128 k_16_16 ripple-normal resolves
  (10215000/10235000/10255000) of the wave simulation on EDRAM 0 (surface 020000A0, k_16_16_16_16_FLOAT; state
  resolved to 02B36000). Those textures oscillate at pixel frequency (alternate rows / checkerboard,
  `_run/okhblue/wres1.png`) at Scale 1 too; Scale 3 adds the large rings (`_run/okhblue/water4.png`). No mips on
  the 360 either (fetch mip level 0). Clamping k_16_16 UNORM resolves (`IsFloatTextureFormat` lists 25/26, so
  resolves to them skip the saturate) did not help and was reverted.
- **Cause:** internal resolution scale. The ripple sim (PS cc5a3f0d: explicit wave equation, point-sampled
  neighbours 1/128 apart, fetch dword3 01001691) ran on a 256/384-texel host target; the upscaled grid splits
  into independent sub-lattices that drift into a checkerboard. True Scale 1 is clean (water high-frequency
  energy flat ~1.9; Scale 2 grew 2.6 -> 6.9 over 140 frames). Note `Upscaling=Native` overrides `Scale`.
- **Fix:** `TargetScale`: EDRAM surfaces with sample pitch <= 160 (ripple sim / glow 020000A0, luminance chain
  80) render at scale 1 (targets, viewport, positionScale, resolve copies, query counts, pixel watch).
  `SCDA_SCALE_SMALL_TARGETS=1` = old. Scale 2/3 water energy now flat ~1.8/2.2 (`_run/okhblue/w7crop.png`).
  Regression at Scale 3 old/new (`_run/okhblue/reg.png`): Intro 1500/2000, Jail 1620, Kinshasa01 2400,
  Okhotsk surfacing 700/900 within 1-3 levels.

## FIXED (workaround). Okhotsk_02: screen washes pale blue / cyan after surfacing (2026-11-01)

- **Symptom (user video):** surfacing at the "Use detonators to blow the ice wall" spot turns the whole frame cyan.
  Not a regression of 6949bc4 (same with `SCDA_NO_CB_SKIP=1 SCDA_NO_FETCH_CACHE=1`). The 360 reference
  (`_run/ref/okh/x360_m4.mp4`, 596-660 s) and Xenia show no wash.
- **Repro:** `SCDA_LAUNCH_DATA=_run/okhblue/save.launch` (user save, Okhotsk_02 at the surface)
  `SCDA_AUTOPRESS=600:A`, dump frame 700. Cyan from frame ~640 on.
- **Cause:** the D3D clears before the lens-drop pass (PS 7c17e516, MRT RT0 base 0 + RT1 base 0xB8, additive
  01060106) include 4x depth-only alias clears (`RB_MODECONTROL` 5, surface 05020140) at bases 0 and 0xB8.
  `AliasedClearColor` writes them as depth 1 = (0, 1, 1, 1); RT1 (0xB8) keeps it, is resolved to 012A2000 and the
  final combine (PS a5d8b762: t0 + scene + t2) adds it to the screen. Underwater (frame 580) these clears are absent.
  Skipping a5d8b762, `SCDA_NO_DEPTH_COLOR_ALIAS=1` or `SCDA_ALIAS_RAW_Z=1` each give a clean frame.
- **Conflict:** the clear depth 1 comes from the viewport z offset, which VTE 0x300 does not apply; the quads' z is
  ~0 (-2^-28), and Xenia (draw_util.cc, z scale/offset disabled -> depth = z) also writes 0. Depth 0 fixes
  Okhotsk but darkens Kinshasa01's sunlit ground (light mask 0x1C0, see the Jail entry below), while Xenia shows
  Kinshasa lit with Sam's shadow (user check). So 0 is the hardware value and Kinshasa's lit ground depends on
  something downstream we still get wrong. Ruled out: `RB_DEPTH_CLEAR` (always 0), clear VS 10e10ea9 (passes
  vertex z through), skipped draws (none on Kinshasa), occlusion queries (`SCDA_QUERY_ALL_VISIBLE=1` + raw z still
  dark, `_run/okhblue/kin.png`).
- **Xenia check (2026-11-01, patched Xenia Canary 96ce1ac, `tools/patches/xenia_scda_probe.patch`: `XE_LAUNCH_DATA=<file>`
  boots a map, `XE_RESOLVE_LOG=<file>` + `--readback_resolve=full` logs byte-lane means per resolve; build in
  `external/xenia-src`, SPIR-V tools in `external/vulkan-tools/sdk` via `VULKAN_SDK`):** Xenia's Kinshasa01 mask
  (0x1C0) = R ~0, G 193, B ~0, A 190, matching ours with raw z (R 0, G 199, B 0, A 200); current build has B 255.
  But Xenia's alley ground has no sun and no Sam shadow (`_run/okhblue/xe_kin2.png`), while the 360 video
  (`_run/ref360/kin.mp4`, "Retrieve plans from the camp" from ~11:30) shows strong sun and shadows. So Xenia
  shares the raw-z result and is not ground truth here.
- **Lever:** raw z + the default 8x8 s13 (036FA000, fetch `80400002 036FA086 0000E007 00A80C14`, bytes FF000000 =
  (0,0,0,1) after 8in32 + zyxw) forced white relights ground, Sam and the car (`_run/okhblue/kin4.png`): all bind
  it. The material is `min(mask.B + s13.ch + 2*oT8.x, mask.A)` (PS 63053924, mask = s15.zw).
- **Fix (per-shader workaround):** an aliased depth-only clear marks its colour target; the lens-drop pass (PS
  7c17e516) clears marked targets to black before drawing, any other draw unmarks them (`ExecuteDraw`).
  `SCDA_NO_LENS_DROP_FIX=1` = old. Verified on the save (frames 580/700/900, `_run/okhblue/fix.png`); Kinshasa01
  keeps sun and Sam's shadow.
- **Open question:** on the 360 either mask.B ~1 (clear at depth 1 as now) or the unlit-default path differs. No
  rule found that gives Kinshasa B 1 and Okhotsk glow 0 from the same clear registers/quad data.

## FIXED. Smeared, streaky floor textures (Kinshasa02 diamond-plate walkway) (2026-10-31)

- **Symptom:** walkway texture stretched into horizontal streaks/ripples (user screenshot, Kinshasa02 start).
- **Cause:** raw vertex fetch took signedness from `signed_rf_mode_all` (the signed rounding mode) instead of
  `format_comp_all`, and ignored `exp_adjust` (x 2^n). Signed packed UVs decoded as unsigned; negative coordinates
  wrapped. Textures themselves decoded fine (`SCDA_DUMP_TEXTURES`).
- **Fix:** Xenia's fields; translator v17. `SCDA_VFETCH_OLD_SIGN=1` restores the old decode (A/B). Verified
  `_run/kv3/` vs `_run/kv/` (same autopress view).

## FIXED. CleanGoggles: EMF/thermal image shifted, smeared bands at the screen edges (2026-10-31)

- **Symptom:** with `CleanGoggles=1`, EMF showed horizontal streaks over the left ~8% and bottom of the screen, a
  blotchy marble pattern over everything; thermal a band at one edge. Night vision was clean.
- **Cause:** the UV-noise passes (PS 40f6f3d3, EMF composite PS e8999e85) add `noise.xy (0..1) x PS c20` to the
  scene UV; the VS (f7f281d7) pre-subtracts the noise mean in VS c20.zw (EMF: VS c20 = (1, -1, -0.1, 1.1), PS c20 =
  (0.2, 0, 0, -0.2)). CleanGoggles zeroed PS c20, so the image moved 0.1 screen and the clamped edge smeared. The
  marble pattern is an additive (ONE/ONE) 128x128 DXT1 texture through VS ee247e0c + PS e48aa6f4 (a second use of
  the overlay PS, not matched by the scanline skip).
- **Fix:** each noise fetch (slot 0 of both passes, slot 0 of the marble overlay, slot 1 of the thermal palette)
  gets a 1x1 texture of its average colour: displacement = its mean (cancelled by the VS), tints kept, no grain.
  Authentic mode unchanged. Verified Intro EMF 2700 / thermal 2900 / NV 3300 (`_run/blk/e/` clean, `_run/blk/g/`
  authentic). `SCDA_TRACE_PS` now also logs PS c20-c23.
- **Not a bug:** the flat magenta panel at the right of the thermal view is Osprey wall geometry (PS 626ba45b); it is
  in authentic mode and EMF too.

## NOT A BUG. Dark interiors darker than the 360 playthrough (Jail cell) (2026-10-31)

- **Evidence (frame means, 640x360, ours vs 360 video):** Jail cell 13,15,15 vs 19-20,26-27,24-25; Kinshasa00 room
  37,34,21 vs 44,43,23; HUD and NY night sky match (29,34,32 vs 27,33,28; 13,23,49 vs 13,24,49).
- **Gamma ramp A/B (`SCDA_NO_GAMMA_RAMP=1`):** Jail 23,26,26 (matches), Kinshasa00 52,48,34 (too bright), so the
  title's ramp toe is not simply wrong; the sky matched with it on. Not changed.
- **Pixel watch (Jail wall, guest 1000,250):** only the material pass PS 63053924 writes it (0.079), PS bools
  0x7250 (b132, b134, b137, b140-142); no additive light passes. Lights are in-shader, enabled by bools b128-b142.
- **Resolved:** two other 360 captures of the same opening shot (YouTube qOaKDvKST_Q at 2 s, 0pdqundUhtU at 38 s)
  measure 13,15,16 and 11,18,15 against ours 15,19,19 (gamma ramp on). The playthrough used first (kJJN2OoDyrE,
  Xbox One X back-compat) is brighter than a 360; do not use it for absolute brightness. That also weakens the
  statue measurement below.

## OPEN. Jail: no window-frame light patches on the gallery floor (2026-10-31)

- **Symptom:** on the 360 (user screenshot, Jail opening, camera pitched down) the floor seen through the cell
  door's lower glass has sharp window-shaped light patches; ours is evenly lit. Cell floor tiles match.
- **Repro:** `SCDA_LAUNCH_DATA=_run/maps/02_Jail_01.launch SCDA_AUTOPRESS=1500:RY=-1:10`, dump frame 1620.
- **Pipeline (frame trace, `SCDA_TRACE_RESOLVE` + `SCDA_PIXEL_WATCH`):** the floor is the material PS 63053924; its
  projector light (b132, cookie s5 = round spot 04C46000) is scaled by r1.w = min(s15.z + ..., s15.w), s15 = the
  640x360 screen-space light mask at 01EB2000 (EDRAM 0x1C0). Mask passes: colour clear + aliased depth clear
  (0,1,1,1) -> point-light shadow passes PS b5531808 (blend 01010001: colour = src, alpha = src + dst, outputs >= 0,
  c2 = (0, .8, 0, 0)) -> resolve -> copy cf73f1b6 `(s0.x, c0.w, s0.w, c0.w)` (alpha -> B) -> sun passes 49a6d6fc /
  b5531808 on G/A -> blur 055ea5f4 (blurs .xyw, passes .z through) -> material. Lightmap s12 decodes fine (RGBE).
- **Why ours is flat:** alpha starts at 1 (aliased clear), so the additive alpha saturates and B (the material's
  .z) is 1 everywhere. The window shadows are in R (overwrite of the last pass).
- **Contradiction:** Kinshasa01 has the same two clears but no additive passes; its lit ground needs B = clear alpha
  = 1. Clear alpha 0 (stencil in A) gives the Jail patches but puts Kinshasa's ground in shadow. Reading R instead
  of B (swizzle without the resolve's red/blue swap) also gives the patches but turns bloom yellow -> cyan.
  Unresolved: maybe Kinshasa should issue additive light passes too (occlusion / visibility), or the EDRAM
  alias semantics differ.
- **Clear quads' own data (2026-10-31, `SCDA_TRACE_DEPTH_ALIAS=1` "clear quad" lines):** the mask clear is
  two quads of VS 10e10ea9: a 4x depth/stencil alias (mode 5) over rows 0-352 and a 1x colour quad over rows
  352-360 (the only rows the alias misses: that strip was the NY/Hotel "bottom band"). Both quads have
  vertex z 0 (VTE 0x300 = raw z, the zs -1 / zo 1 viewport is not applied) and vertex colour (0, 0, 0, 0)
  (dwords 3-6). PS c0 (0, 0, 0, 0.533/0.502) is a stale constant: the clear PS outputs the interpolator.
  So by the data the whole mask starts at 0 (depth 0, stencil 0 = dword 0). That gives Jail's patches.
  (Superseded below: Kinshasa needs 1.)
  `AliasedClearColor`'s (0, 1, 1, 1) uses zo, which VTE 0x300 does not apply. It probably hides another bug
  that darkens Kinshasa when B = 0. The material computes `r1.w = mask.B + s13.<ch> + mask.R * c253.w`,
  then `min(r1.w, mask.A)`, where s13 is a DXT1 (fmt 18) baked-shadow texture and the channel is chosen by
  the sign of c15.z. Next: check that term on Kinshasa's lit ground (s13 decode/channel, c15/c253).
- **Kinshasa term check (2026-10-31, extended `SCDA_TRACE_PS`: PS c15/c16/c220/c253/c254, VS c240-243/c255,
  bools, first 16 bytes per texture slot; `_run/jm/kin_ps3.log`):** correction: the third term is
  `iT[8].x * c253.w` (VS oT8), not mask.R. It is 0 on Kinshasa: VS c47defcd writes oT8.xy =
  c242.zw * (1 - ...) when b7, else `-abs(x) > c255.w`; c242 = 0, c255.w = 0, b7 clear. 363 of 990 draws bind
  the default 8x8 s13 at 036FA000 (fmt 6, endian 8in32, swizzle zyxw, bytes FF000000 = (0, 0, 0, 1)): R/G/B
  are 0, so for them the light is `min(mask.B, mask.A)`. Kinshasa has no light passes before the copy, so
  mask.B = the clear's alpha. A/B `SCDA_ALIAS_RAW_Z=1` (clear depth = raw z 0, `_run/jm/kin_on_rawz.png`):
  the whole alley ground goes dark and Sam's sun shadow disappears; the walls (real s13) stay lit. So on
  the console mask.B must be ~1 there: the clear is 1, not 0.
- **Clears identical:** Jail 1620 and Kinshasa 2400 trace the same mask clear: same registers (zs -1, zo 1,
  ref 00FF0000, zctl 00008777, VTE 0x300) and the same quads (z -3.7e-9, colour 0). One clear value cannot
  give B = 0 on Jail and B = 1 on Kinshasa, so the clear stays (0, 1, 1, 1). Jail's patches must come from
  downstream: with B saturated the floor is `min(1, mask.A)` = the sun term A (49a6d6fc REVSUB on G/A;
  on Jail b5531808 MIN only writes G), or from the post-material light passes. Next: dump the Jail mask's A
  (`SCDA_DUMP_RESOLVES`, 01EB2000 at 1620) and compare the window shapes with the 360 shot.
- **Mask dumps (2026-10-31, `SCDA_DUMP_RESOLVES` + `SCDA_TRACE_FRAME=1619`, `_run/jres/`, `_run/jres2/`):** before
  the copy the Jail mask's A is 255 on every pixel (clear 1 + additive b5531808 passes), so the copy's B is 1
  everywhere. Sequence: mask clear -> point-light shadow maps (00DB2000, EDRAM 0x000-0x1BF, no overlap with
  0x1C0) -> b5531808 passes (all blend 01010001, mask F) -> resolve -> copy cf73f1b6 -> character-shadow map +
  6x 49a6d6fc (REVSUB G/A) -> blur. On Jail, 49a6d6fc's shadow map holds one guard silhouette (a character
  shadow, not window light) and subtracts 0 here. Kinshasa 2400: clear -> resolve -> copy (no pass writes B)
  -> 49a6d6fc/b5531808 MIN on G/A. Copy PS and its fetch (zyxw) are identical on both maps.
- **Window patches are baked:** the Jail floor binds a real s13 (02882000, DXT1 512x512); its B channel holds
  the window-grid light pattern (`_run/jtex/s13.png`). The cookie s5 (04C46000) really is a round spot. The
  material scales the shadowed light by `min(mask.B + s13.ch, mask.A)`, so the baked pattern shows only where
  mask.B is ~0. With B = 1 (our clear alpha) every baked shadow is swamped, on every map.
  So the console baseline B is probably 0, yet Kinshasa needs B ~1: with B = 0 its car and Sam (dynamic
  actors, never lightmapped) lose the sun too, so missing lightmaps cannot explain it.
- **oT8 term ruled out:** `r1.w` also adds `2 * oT8.x` (c253.w = 2). VS c47defcd sets oT8 = `-abs(x) > c255.w`
  (always 0) when b7 is off. The corpus has 81 `-abs(x) > 0.0` and 9 `abs(x) >= 0.0` (constant results), which
  looked like a decode bug, but an independent decoder agrees (temp reg bit 7 = abs, negate after abs = -|x|,
  scalar SGT = src > 0). Decode matches, so oT8.x = 0 on Kinshasa.
  `SCDA_QUERY_ALL_VISIBLE=1` (all occlusion queries visible) changed nothing on Jail.
- **Ruled out:** negative light alpha (PS b5531808 outputs `0.111 * sum(shadow tests * c2.y * cookie)` >= 0,
  c2 = (0, .8, 0, 0), so the unclamped-source idea does not apply). Alias off (`SCDA_NO_DEPTH_COLOR_ALIAS=1`)
  shows Jail patches, but only because rows 0-352 keep the previous frame's mask. Kinshasa then gets a black
  bottom strip (the 0-alpha colour quad).
- **Done meanwhile (translator v18):** resolve copies are bound raw, so the shader now applies the fetch swizzle
  composed with the resolve's dest swap (RB_COPY_DEST_INFO bit 24). For ARGB resolves this cancels (no change);
  fmt 36 / 31 resolves now read (r, 1, 1, 1) / (r, g, 1, 1) and A0A forces alpha 1. A/B (`SCDA_NO_RESOLVE_SWIZZLE=1`)
  at frame 2000: Kinshasa01, Cozumel_01 equal; Intro within run-to-run drift. rtrace draw lines now log blend;
  `SCDA_TRACE_RESOLVE` logs each resolve fetch's swizzle.

## OPEN. NY Statue of Liberty pedestal dimmer than the 360 (2026-10-31)

- **Symptom:** floodlit pedestal ~30% darker and less golden than the 360 playthrough (ours top-20% pixels
  108,101,89 vs 148-173,133-155,106-121); sky and HUD match within a few levels, so exposure is not the cause.
- **Repro:** `SCDA_LAUNCH_DATA=_run/maps/10_newyork_02.launch SCDA_AUTOPRESS=1130:RX=-1:10`, dump frame 1150.
- **Evidence:** `SCDA_PIXEL_WATCH=1150,1240,70` (logs every draw changing that guest pixel): the scene value
  (0.48, 0.49, 0.34) comes from one material pass, PS 63053924 (point lights with cube attenuation maps, now bound);
  the additive light pass after it (PS b5409fb6, ONE/ONE) adds ~0.017. Then tonemap (e54f8aac) and bloom (a5d8b762).
- **Next:** trace PS 63053924's light constants on the pedestal draw (`SCDA_TRACE_PS=630539246fe9a5e3`) and its
  texture inputs; compare bloom strength. The camera drifts between runs, so pick the pixel per run.

## FIXED. Black reflections on water, glass, metal and shiny floors (2026-10-31)

- **Symptom:** dull/grey water (NY boat), window glass without its reflection layer (Cozumel cabin), flat metal.
- **Cause:** cube textures (fetch dimension 3) were never bound; every `tfetchCube` read 0. 71 of 464 game shaders
  sample a cube map.
- **Fix:** six faces one `TextureCubeFaceBytes` apart (Xenia layout) into a `TextureCube` at t64+, mips generated.
  `SCDA_NO_CUBE=1` (A/B), `SCDA_TRACE_CUBE=1`. Real 3D textures: none seen on Intro/Kinshasa01/Okhotsk_01/Jail.

## FIXED. Draws lost to shader compile failures (2026-10-31)

- **Cozumel ocean black:** water PS d5defa8e runs CUBEv on a float constant; the translator printed `r255`.
- **Okhotsk_02 underwater pass, HQ_05_B:** loop shaders referenced undeclared `iN`; loop constants now read
  `g_Loops` (count, aL start, step).
- **Hotel particles:** point lists were skipped; now expanded to sprites (geometry shader) with param gen.
- **Common package (3 PS):** 1D texture fetches (`tfetch1D`) had no translation; 1D textures now bind as one-row
  2D textures (size_1d width).
- **Whole game:** 4532 unique shaders from all 20 `.lin` packages translate and compile (sweep 2026-10-31).
- **Offline check:** `SCDA_SHADER_SWEEP=<dir> scda_tests` compiles every `*.ps.bin` / `*.vs.bin` under dir.
  Microcode from the `.lin` packages: inflate, then each container at magic `102A1100` (pixel) / `102A1101`
  (vertex), microcode at container + virtualSize + shader.physicalOffset. `SCDA_TRACE_SKIP=1` logs skipped draws.

## FIXED. Dark band over the last 16 screen rows (NY boat, Hotel) (2026-10-31)

- **Symptom:** the bottom 16 rows of the screen were about half as bright as the rows above (10_newyork_02,
  04_Hotel; any map using the 640x360 light mask).
- **Cause:** the title's aliased depth-1 clear of the light mask (EDRAM 0x1C0) stops a few mask rows short of row 360.
  On the console those tiles keep other surfaces' bits; here they stayed 0 (= shadow), and the full-screen lighting
  pass sampled them for the last 16 screen rows. Found with `SCDA_DUMP_RESOLVES` (mask rows ~356+ black).
- **Fix:** a non-zero aliased clear also clears the whole host colour target (`SCDA_ALIAS_QUAD_ONLY=1` for A/B).
  Verified `_run/st/` (NY 1500/2400/2600, Hotel, Kinshasa01: no band).

## OPEN. HQ_01_A: no character shadows (issue #6) (2026-11-02)

- **Symptom (user shot, 360 ref `_run/ref360/hq1.mp4` ~148 s):** characters cast no shadow in the HQ rooms.
- **Repro:** `00_HQ_01_A.launch` + `SCDA_AUTOPRESS=1250:A,1350:A` (skips "Undercover guidelines"), frame 1600
  (~3360 draws). `SCDA_TRACE_RESOLVE=1600` -> `_run/shadow/rtrace1600.log`.
- **Pipeline:** light 1: casters (VS a9c616aa/a6763477, PS 47d032be writes linear light depth to R) into 32F
  surf 10000410 -> resolve @128 to 00DB2000 (1024). Mask draws 1005-1225 (PS b5531808, 9-tap compare `tap >= ref`,
  lit = c2.y 0.8, shadow = c2.x 0, average c255.x = 1/9; colour replace, alpha ONE/ONE) into 0A000280 base 1C0.
  Light 2: casters into 08000230 -> resolve @1226 to 00DB2000 (512), mask draws 1226-1238 (VS 537ca434). Then PS
  cf73f1b6 writes (mask.R, c0.w, mask.A, c0.w): the mask alpha becomes B.
- **Ruled out:** storage/resolve (caster PS forced to 0.5 reads back 0.5); constants (caster c20 == receiver c24, the
  same light depth plane; receiver UV rows c25/c26 = 0.5 x caster clip x/y + 0.5 w, exact).
- **Evidence:** debug overrides `_run/shadow/mkdbg.py` (`SCDA_HLSL_OVERRIDE=_run/shadow/dbg_<v>`): HQ light 1 has 0 of
  9 taps passing on almost every pixel, tap ~0.25 vs ref ~0.52 (ratio ~0.486, constant). Kinshasa01, same PS, partly
  passes (tap ~ ref on ~19k px). HQ data also shows the mask draws changing only R although RB_COLOR_MASK is 0xF:
  unexplained.
- **Clear colour:** the mask starts (0, 1, 1, 1) (AliasedClearColor, depth 1), so additive alpha saturates and B is
  flat. `SCDA_ALIAS_RAW_Z=1` (depth 0) darkens the whole stairwell without a visible character shadow and is known to
  darken Kinshasa's ground: not a fix on its own. Fix the compare first, then revisit.
- **2026-11-02 result: the stairwell repro (frame 1600) is NOT a bug.** Probes (`_run/shadow/mk{dbg2,probe,uv,exp,plane,pos}.py`,
  `run.sh`, `cmp.py`) cleared the emulation: caster/receiver c2.x equal, light plane c20 == c24 (w and |n|), sampled UV ==
  caster texel, caster expected UV == receiver UV. Tap ~0.5 x ref is a real occluder: light 1 comes in through a window and
  a 10-index mesh (ib 057171F6, caster draw ~112, VS a9c616aa) fills the opening in the map. `SCDA_SKIP_IB=057171F6/a9c616aa1e646735`
  lights the mask (window-grid light on the right wall, Sam's shadow), but the 360 ref (`_run/ref360/hq1.mp4` ~40 s, same
  view, `_run/shadow/ref/cmp.png`) shows no window light there either: the occlusion is correct.
- **Still open:** with the mask lit (skip-ib run) the final frame did not change at all (`_run/shadow/ab_skip.png`), so the
  mask may not reach the screen (clear alpha 1 + ONE/ONE saturating, then PS cf73f1b6 moves mask A into B). Next: a repro
  where the 360 clearly shows a character shadow (wooden room ~148 s), then A/B the mask path there.
- **Tool:** `SCDA_SKIP_IB=<ib>[/<vs>][,..]` skips one mesh (optionally in one pass only).

## FIXED. Kinshasa ground and walls stuck in shadow, no character shadows (2026-10-31)

- **Symptom:** after the scope-band fix (34638bd) all sunlit ground in Kinshasa was dark, Sam cast no shadow and floor
  textures looked flat. `SCDA_NO_DEPTH_COLOR_ALIAS=1` restored the console look.
- **Cause:** the title clears its 640x360 sun-shadow mask (EDRAM 0x1C0) with a depth-only D3D clear through a 4x
  alias at depth 1.0 (viewport z scale -1, offset 1), stencil 0. The aliased tiles then hold the depth/stencil dword,
  which reads as colour (0, 1, 1, 1) = lit. 34638bd wrote 0 (= fully shadowed).
- **Fix:** `AliasedClearColor` writes the real dword (stencil in R, 24 depth bits in G/B/A, Xenia's D24S8 and 8_8_8_8
  layouts) via the blend factor and a no-kill PS. Clears at depth 0 still give 0. Verified on Kinshasa01 frame 2400
  (`_run/k1c/` against `_run/k1b/` before). Trace: `SCDA_TRACE_DEPTH_ALIAS=1` (or `=<frame>` for every one in a frame).
- **Scope band:** came back with the real clear colour; the 360 shows no band under the scope (Kinshasa scope shots
  in the 360 playthrough), so the band pass (PS 3a7a1a91) is skipped (`SCDA_SCOPE_BAND=1` draws it). Why its input is
  transparent on the console is not known.

## FIXED. Scale 2+ showed stale tiles (2026-10-30)

- **Symptom:** at `Scale=2`, screen rows 256 and below (EDRAM tiles 1-2) froze on an old image (01_Intro cutscene).
- **Cause:** predicated tiling resolves tiles 1-2 to bases inside tile 0's resolve texture. The renderer picked the
  containing texture by the first match in an `unordered_map`; an older texture that also spanned the base could win,
  and the map's order changes as it rehashes (hence timing- and run-dependent, worse at Scale 2).
- **Fix:** the most recently copied containing texture wins (`Resolved::lastCopy`). Verified on Intro frames
  380-440 at Scale 2 (`_run/s2/seq2/`); same frames froze before (`_run/s2/seq/`).

## FIXED (shortcut). Sniper scope: cyan band and pink rows at the bottom of the screen (2026-10-31)

- **Symptom:** in Kinshasa scope view, the bottom ~20% of the screen turned flat cyan `(0,198,198)`, green over the
  scope body, and the last 5 rows pink. (The purple lens was fixed earlier by the resolve-container fix.)
- **Repro:** `SCDA_LAUNCH_DATA=C:/SCDA-Recomp/_run/maps/07_Kinshasa01.launch SCDA_AUTOPRESS=2150:X,2250:RS`
  (X draws the rifle, R3 raises the scope), dump frame 2400.
- **Cause:** the last post pass (PS `3a7a1a91e12b339a`) blends a 640x360 texture over the bottom of the screen with
  alpha `a * 2 * (5y - 4)`. That texture is resolved from EDRAM base 0x1C0, which the title "clears" with two
  D3D clear draws at the start of the frame. Both were lost:
  1. The clears put z at -2^-28 with `PA_CL_CLIP_CNTL.CLIP_DISABLE` set; D3D11 depth-clipped them. The rasterizer now
     honours bit 16 (`DepthClipEnable = !clip_disable`). This fixed the pink rows.
  2. The second clear is depth-only (`RB_MODECONTROL` 5) through a 4x alias with depth at the colour base. On
     Xenos the depth write replaces the colour tiles; our host depth target is separate, so the colour kept last
     frame's mask (cyan background, green scope body, alpha 0.8). Such draws now also write colour 0
     (`DepthOnlyOverwritesColor`).
- **Evidence:** `_run/scope3/` (traces `t4.txt`..`t7.txt`, per-draw dumps `dd*/`, fixed frames `fix/`).

## Open feature. Extra Settings inside the game's own main menu

- **Wanted:** an "EXTRA SETTINGS" entry in the game's main menu that opens the same settings.
- **Now:** the settings are the F1 / L3+R3 overlay; after sign-in the update notification names the shortcut.
- **Needs:** reverse-engineering the title's UnrealScript menu pages (UE2.5 GUI in `game/System` packages) to add
  an item and route its selection to the overlay. Not started.

## FIXED. Black walls and floors near the camera (2026-11-01, regression from 445b9de)

- **Symptom (user):** Kinshasa01 start (alley wall), Iceland Osprey ramp, Cozumel_01 room: walls/floors black.
- **Cause:** the far-only sky depth nudge in `XenosViewport` tested `s.z < 1e-4` per vertex. Vertices behind the
  camera (w < 0) also have z/w < 0, got the nudge, and after the multiply by negative w pulled near-plane-crossing
  triangles' depth below their Z prepass (VS 74e0c24e, z write) -> the colour pass (VS c47defcd, GEQUAL, no
  write) failed. Fix: nudge only when w > 0. Translator v24.
- **Found with:** `SCDA_PIXEL_WATCH_DEPTH=1` (depth per draw at the watched pixel; one depth write = prepass
  3508), `SCDA_SKY_DEPTH_EPS=0` (black gone), `SCDA_DEPTH_EPS_ALL=<v>` (debug: nudge at every depth).
- **Checked:** Kinshasa01 1200/1500, Iceland 1000-1900, Cozumel_01 1000-1900 fixed (`_run/black/v_*.png`,
  `fix/`). NY02 sky A-B-A 200 frames: 0 glitches (control with the nudge off also 0: test not sensitive).

## FIXED. Sky under the ice smeared with underwater shapes (2026-11-01)

- **Symptom:** Iceland dive, looking up at the ice hole: dark smears over the sky and clouds.
- **Cause:** the water's above-surface view (640x360 target 10010410, resolved to 12116000) is drawn with user clip
  plane 0 (`PA_CL_CLIP_CNTL` bit 0, plane at `PA_CL_UCP_0` 0x2388). We ignored user clip planes, so Sam and the
  underwater geometry were drawn into it. The VS wrapper now writes `SV_ClipDistance0/1` = dot(exported position,
  plane) for enabled planes (none with clip_disable), as Xenia does. Translator v22. `SCDA_NO_UCP=1` = old.
- **Evidence:** `_run/fog/` (`res_ab.png` the texture, `up_zoom.png` the view, `ref_zoom.png` ref 172.5/175.5 s;
  `okh_ab.png` Okhotsk_02 texture 0A41D000: diving Sam gone, guard and barrels on the ice kept).
- **Checked:** Hotel bathroom mirror and Cozumel_01 use no clip planes (unchanged). newyork_02 frames 1200-2000:
  27.8-28.6 fps (older exe 25.7-26.2), no cost. The old census 2550 view (grey surface, straight cut) was not
  reproduced in either build, so that exact symptom is unconfirmed.

## DONE. Fog and light shafts: no confirmed difference left (closed 2026-11-01)

- **Checked 2026-11-01:** Iceland light shaft frame 2300 vs ref. The earlier "too bright" match was a timing
  mismatch: ref 171 s is as blown out as ours (luma p90 230 vs 238, 4% clipped in both). Shaft pixel traced per draw
  (`SCDA_PIXEL_WATCH`): scene 0.52, then the game's own post adds 0.5 x scene and 0.5 x a 20-tap radial blur (PS
  7b395466, c255 = 0.0625/0/0.987, i31 = 20) -> 1.0. Faithful to the ucode. Ref is slightly more cyan (mean G-R 18 vs
  10): not separated from capture tint (no HUD calibration done).
- **Blend clamp theory struck for this scene:** no blend in it reads destination alpha, so FP16 alpha > 1 never
  reaches colour. Left: 7e3 precision (7-bit mantissa) vs FP16 for faint layers; would need programmable blending.
- **Not re-checked:** Osprey bay fog (census frame 2000 vs ref ~161-163 s).
- **Status:** closed 2026-11-01 by the user; the Osprey bay side-by-side was skipped. Reopen only if a player reports a difference.

## P3. Scale 2+ edge row/column and faint line at a tile split

- **Symptom:** above `Scale=1`, some post passes miss one pixel row/column at the screen edge; a faint horizontal
  line can show about a third of the way down (EDRAM tile split).
- **Status:** open, cosmetic.

## P3. Brightness slider may not change the gamma ramp

- **Symptom:** unverified. The title uploads its gamma ramp twice at boot; the in-game brightness slider was never
  reached in a test, so whether it re-uploads the ramp is unknown.
- **Repro:** Options -> brightness, with `SCDA_TRACE_GAMMA=1`.
- **Status:** needs a check.
