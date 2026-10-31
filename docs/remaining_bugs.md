# Remaining bugs (2026-10-31)

Ranked by impact on a first release. Each entry: symptom, repro, evidence, suspected cause, status.
Evidence paths under `_run/` are local only (git-ignored).

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
- **Regressed:** the sniper-scope bottom band (below) shows cyan again. The scope frame's blur pass (PS b1649a4c)
  reads this mask; putting stencil in A instead removed the band but shadowed the ground again.

## FIXED. Scale 2+ showed stale tiles (2026-10-30)

- **Symptom:** at `Scale=2`, screen rows 256 and below (EDRAM tiles 1-2) froze on an old image (01_Intro cutscene).
- **Cause:** predicated tiling resolves tiles 1-2 to bases inside tile 0's resolve texture. The renderer picked the
  containing texture by the first match in an `unordered_map`; an older texture that also spanned the base could win,
  and the map's order changes as it rehashes (hence timing- and run-dependent, worse at Scale 2).
- **Fix:** the most recently copied containing texture wins (`Resolved::lastCopy`). Verified on Intro frames
  380-440 at Scale 2 (`_run/s2/seq2/`); same frames froze before (`_run/s2/seq/`).

## PARTLY OPEN. Sniper scope: cyan band and pink rows at the bottom of the screen (2026-10-31)

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

## P2. Fog and light shafts differ slightly from the console

- **Symptom:** small brightness differences in fog and light shafts (Iceland Osprey bay, Okhotsk ice-hole edge).
- **Evidence:** `_run/census/` against `_run/ref/ref.mp4`.
- **Suspected cause:** Xenos clamps each blend write; Direct3D 11 does not (FP16 targets reach alpha 2-8).
- **Status:** open, low priority.

## P3. Scale 2+ edge row/column and faint line at a tile split

- **Symptom:** above `Scale=1`, some post passes miss one pixel row/column at the screen edge; a faint horizontal
  line can show about a third of the way down (EDRAM tile split).
- **Status:** open, cosmetic.

## P3. Brightness slider may not change the gamma ramp

- **Symptom:** unverified. The title uploads its gamma ramp twice at boot; the in-game brightness slider was never
  reached in a test, so whether it re-uploads the ramp is unknown.
- **Repro:** Options -> brightness, with `SCDA_TRACE_GAMMA=1`.
- **Status:** needs a check.
