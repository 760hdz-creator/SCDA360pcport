# Draft replies for GitLab issues (v0.1.6). Not posted yet.
# Format: issue number, suggested action, reply text.

## #4 Performance: High CPU usage. Close as fixed
Fixed in v0.1.6. Three of the game's own background loops (the graphics wait in the main thread, the
loading-screen/movie thread and a timer thread) kept a core at 100% even with nothing to do. On the 360 they ran on
spare hardware threads, but on a PC they took cores from everything else. They now sleep while they wait. In Ellsworth
Prison the game went from about 3.3 busy cores to about 1.1, at the same 30 fps. On a 6-core CPU like the FX-6300 that
should end the 95% lock and the stutter. If you still see high CPU on v0.1.6, please post a new scda.log. Thanks for
the detailed report!

## #7 Automatic launcher updates. Close as done
Done in v0.1.6. F1 > Update has "Install update now" (it downloads the installer, swaps scda.exe and restarts the
game). There's also a checkbox to install updates automatically at startup (`[Update] AutoInstall=1`). You need to run
the v0.1.6 installer once by hand; after that, updates can come from inside the game.

## #12 Monitor selection. Close as done
Done in v0.1.6: `[Display] Monitor=0` (main display), `1`, `2`, ... The same option is in F1 > Display when you have
more than one monitor. It takes effect at the next start.

## #21 Gamma and Brightness. Close as done
Done in v0.1.6: `[Display] Gamma=1.0` and `Brightness=1.0` (0.5-2.0), with sliders and a reset button in F1 > Display.
`Gamma` above 1 makes dark corners easier to see without washing out the rest.

## #23 Performance overlay. Close as done
Done in v0.1.6: `[Display] PerformanceOverlay=Off|FPS|Detailed`. Detailed adds frame time, the game's CPU use and the
resolution. It's also in F1 > Display.

## #1 Rebind mouse/keyboard, stick speed, FPS cap. Keep open (FPS part)
v0.1.6 adds rebinding: a `[Keys]` section in settings.ini (e.g. `A=Enter,Space`, `RT=Mouse1`, `Walk=Shift`). The README
lists every action and key name. For movement speed there's a Walk key (Shift by default): while you hold it, WASD
pushes the stick only to `[Input] WalkSpeed` (default 0.5), so Sam walks slowly and quietly. The on-screen key icons
still show the default keys. The frame-rate limit is tracked in #5.

## #13 Gamepad toggle and sensitivity. Close as done
Done in v0.1.6: `[Input] Gamepad=0` ignores controllers. `GamepadSensitivity=1.0` (0.25-3) changes how fast the
controller turns the camera. PlayStation and Switch pads already work natively (`PlayStationNative=1`).

## #14 Gamepad UI button prompts. Close as already available
This is already in: `[Input] ButtonPrompts=Auto|Xbox|PlayStation|Keyboard`. Auto follows the device you're using.
Switch controllers currently get the Xbox-style icons.

## #17 Texture filtering. Close as already available
Already there: `[Display] Anisotropy=1-16` (F1 > Display > Anisotropic filtering). Each texture otherwise keeps the
filtering the game asks for, as on the console, and anisotropic filtering is added to the smooth-filtered ones.

## #11 Upscaling method and quality. Keep open (method)
The quality presets already exist: `Upscaling=Off|Native|Quality|Balanced|Performance|UltraPerformance`. The method
is FSR 1-style (Lanczos upscale plus CAS sharpening). DLSS and XeSS need vendor SDKs and motion vectors, which this
game doesn't produce, so they aren't planned for now.

## #5 Unlock 30 FPS. Keep open
v0.1.6 fixed the CPU load (#4). The 30 fps cap comes from the game itself, and some of its timing (animations,
physics, scripts) may assume 30 fps. Unlocking it needs its own investigation so the game doesn't speed up. It's on
the list.

## #2 Idea (light meter, duplicate). Already closed

## #3 Light meter. Keep open
Possible, but the game's own visibility value has to be found in memory first, so it isn't in this release. On the
360 version, the light on Sam's back already works as the game's stealth indicator.

## #9 Add to Steam. Keep open
Planned for the installer. It has to edit Steam's shortcuts file while Steam is closed, so it will be done carefully.

## #15 Shadow resolution / #16 Dynamic shadows toggle. Keep open
The game picks its shadow-map sizes itself. Changing them means patching its rendering code, which we're looking
into. The CPU fix in v0.1.6 should help low-end PCs in the meantime. If a level still runs slowly, please tell us
which one.

## #10 Graphics API (Vulkan/DX12). Won't do for now
The renderer is Direct3D 11. The CPU load in #4 came from the game's own wait loops, not from DX11, and is fixed in
v0.1.6. A second graphics backend would be a very large job for little gain right now.

## #18 Frame generation. Won't do for now
DLSS 3 / FSR 3 frame generation needs motion vectors and depth in a form the game doesn't produce, and works best
above 30 fps. Not planned before #5.

## #19 Ambient occlusion. Won't do for now
SSAO needs a clean depth and normal buffer in the game's pipeline. Possible later; not planned yet.

## #20 HDR. Won't do for now
The game renders in SDR with its own console tonemapping. Real HDR output would need a new tonemapper. Not planned
yet.

## #6 Achievements. Keep open
Good news: the game still sends its Xbox 360 achievement unlocks (we've seen one fire during the Intro mission).
They're dropped right now. Catching them, saving them locally and showing an unlock popup is planned, along with
the achievement names and pictures from the disc.

## #8 Discord status toggle. Close (not applicable)
There's no Discord integration in this port, so nothing is sent to Discord. If you see a status, it comes from
Discord's own game detection (Discord > Settings > Activity Privacy).

## #22 Original vs fast loading. Close (not applicable)
Loading has no artificial delay. It's as fast as your disk and CPU allow, which is now faster after the #4 fix.
Emulating console load times would only add waiting.
