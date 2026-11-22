# Wolfenstein 1.1.8 for Analogue Pocket

2026-11-22. Pocket only; the MiSTer package stays at 1.1.7.

## Changes

- Audio is louder. The bundled kernel and core divide the final mix by 4
  instead of 16. The game still plays sound effects and music at full
  volume, so several loud sounds at once can now clip; lower the in-game
  volumes if you hear distortion.
- Busy scenes are faster. Emitting wall, sprite and weapon columns to the
  GPU costs less CPU time. In the simulated os25 system, CPU work per frame
  falls by 8% to 17% in the six full-screen scenes measured; the two
  heaviest (a room with many sprites, and Spear of Destiny's first level)
  no longer have frames over the 60 Hz budget.
- Views with the status bar visible are faster: only the border around the
  3D view is carried over between frames instead of the whole screen
  (24% less CPU work per frame in the measured scene).
- Fixed: zone-link lookups made misaligned memory accesses on maps with an
  odd number of sound zones, each one a trap into the kernel (22 to 73 per
  frame in the measured maps).
- Fixed: on a core without GPU column lists and span groups, walls, sprites,
  the weapon and the sky were not drawn. They are now drawn on the CPU. The
  bundled os25 core has both, so its output is unchanged.
- Fixed: carrying a frame over could read the previous frame through stale
  CPU cache lines. This was found by review and was not reproduced.
- The bundled os25 core is the one shipped with Doom 1.2.3: a faster CPU
  front end and a faster GPU memory path.
- The desktop build (`make test CORE=wolfenstein`) works again.

The picture is unchanged: 64 replayed frames over seven scenes are
byte-identical to the previous renderer, and the GPU command stream is the
same.

## What was measured

Cycle counts come from a simulated os25 system with a stand-in kernel, not
from a Pocket. Mean CPU work per frame (a 60 Hz frame is 1,666,667 cycles):

| Scene | Before | Change |
| --- | ---: | ---: |
| E1M1 start | 672,290 | -8.4% |
| E1M1 turning | 589,730 | -8.3% |
| Door opening | 622,570 | -8.7% |
| Long corridor | 612,324 | -11.6% |
| Many sprites | 1,611,698 | -11.6% |
| Spear of Destiny, first level | 1,488,757 | -16.9% |
| Status bar visible | 887,938 | -23.8% |

Not covered by these checks: menus, intermissions, save and load, long
sessions, the parallax-sky path, and audio by ear. If this version is less
stable than 1.1.7 on your Pocket, reinstall 1.1.7 and report it.

## Install

Extract the ZIP at the SD-card root and merge its folders. Keep your game
data files in `Assets/wolfenstein/common/`. Install the whole ZIP:
`wolfenstein.elf`, `os25.rbf_r` and `os.bin` belong together.

The bundled os25 core is configured at 100 MHz and does not close main-clock
setup timing: seed 22 has setup slack -0.431 ns and worst hold +0.099 ns.
This remains an overclocked build.

## Executable and runtime identity

| Artifact | SHA-256 |
| --- | --- |
| Pocket Wolfenstein | `7e1be1042563f3586532a601a44bcd3bad4cc2fb48a7cbbf8d4faa5dcf512082` |
| Pocket os25 core | `54a39c1df6fec14a63a9786220d731d6d787d427caf5968f027b83467870e358` |
| Pocket OS kernel | `103c8c0ac122c8b9a21b1c4f6da3075ed3ec0f1fb20ee30948e0bce65a5d8b57` |
