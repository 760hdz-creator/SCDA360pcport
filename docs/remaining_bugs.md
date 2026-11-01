# Remaining bugs (2026-10-31)

Ranked by impact on a first release. Each entry: symptom, repro, evidence, suspected cause, status.
Evidence paths under `_run/` are local only (git-ignored).

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

## P3. Fog and light shafts: no confirmed difference left

- **Checked 2026-11-01:** Iceland light shaft frame 2300 vs ref. The earlier "too bright" match was a timing
  mismatch: ref 171 s is as blown out as ours (luma p90 230 vs 238, 4% clipped in both). Shaft pixel traced per draw
  (`SCDA_PIXEL_WATCH`): scene 0.52, then the game's own post adds 0.5 x scene and 0.5 x a 20-tap radial blur (PS
  7b395466, c255 = 0.0625/0/0.987, i31 = 20) -> 1.0. Faithful to the ucode. Ref is slightly more cyan (mean G-R 18 vs
  10): not separated from capture tint (no HUD calibration done).
- **Blend clamp theory struck for this scene:** no blend in it reads destination alpha, so FP16 alpha > 1 never
  reaches colour. Left: 7e3 precision (7-bit mantissa) vs FP16 for faint layers; would need programmable blending.
- **Not re-checked:** Osprey bay fog (census frame 2000 vs ref ~161-163 s).
- **Status:** open only if a side-by-side at an aligned moment shows a difference.

## P3. Scale 2+ edge row/column and faint line at a tile split

- **Symptom:** above `Scale=1`, some post passes miss one pixel row/column at the screen edge; a faint horizontal
  line can show about a third of the way down (EDRAM tile split).
- **Status:** open, cosmetic.

## P3. Brightness slider may not change the gamma ramp

- **Symptom:** unverified. The title uploads its gamma ramp twice at boot; the in-game brightness slider was never
  reached in a test, so whether it re-uploads the ramp is unknown.
- **Repro:** Options -> brightness, with `SCDA_TRACE_GAMMA=1`.
- **Status:** needs a check.
