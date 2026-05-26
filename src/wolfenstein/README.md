# Wolfenstein / ECWolf openfpgaOS Core

This core keeps ECWolf's SDL-facing platform code intact. Pocket-specific
behavior is implemented in the SDK SDL compatibility layer:

- `src/sdk/include/SDL2/SDL.h`
- `src/sdk/of_sdl2.c`

The ECWolf source lives in `src/wolfenstein/port/`. There is no downloader or
missing-source fallback in this core.

## Runtime Data

Runtime data lives in `Assets/wolfenstein/common/` after release assembly. The
provided instances map the original CD images into APF data slots:

- `Wolf3D.json`: `Wolfenstein3D.cue` and `Wolfenstein3D.bin`
- `Spear.json`: `Spear of Destiny (USA).cue` and `Spear of Destiny (USA).bin`

The core no longer advertises or packages loose `AUDIOHED`, `GAMEMAPS`,
`MAPHEAD`, `VSWAP`, or shareware instance folders. Slot 14 is the CUE sheet and
slot 15 is the BIN image. The runtime tries to mount slot 14 first and falls
back to slot 15 if the OS image only supports mounting the BIN directly.
ECWolf also has a small `/cd` bridge for OpenFPGA builds: it indexes slot 15 as
ISO9660 directly, including MODE1/2352 BIN sectors from CUE/BIN dumps, and
serves the discovered files through ECWolf's normal file API.
Wolfenstein 3-D and Spear of Destiny use their own AdLib/OPL music data from
the game media. The default instances also map the SDK `bank.ofsf` SoundFont in
slot 4, matching the SDK MIDI demo/test preload path, so the OS MIDI/SoundFont
preload path is available when ECWolf or later
ports need it.
The OpenFPGA wrapper also derives ECWolf's data extension from the selected
instance (`.wl6` for `Wolf3D.json`, `.sod` for `Spear.json`) so mixed CD images
do not drop into ECWolf's text IWAD picker.

ECWolf also has its own engine support data (`IWADINFO`, actor definitions,
map remap tables, strings, fonts, and related lumps). That data is not part of
the original CUE/BIN game media. For this core it should be embedded into the
application build, not exposed as another APF data slot. The Makefile packages
`wadsrc/static/` into an internal `ecwolf.pk3` blob and the resource loader
serves that in memory when ECWolf asks for `ecwolf.pk3`.

## Saves

ECWolf uses `ecwolf.cfg` for settings and `savegamN.ecs` for saves. The core
maps these into nonvolatile APF slots:

- slot 20: `ecwolf.cfg`
- slots 21-30: `savegam0.ecs` through `savegam9.ecs`

`of_ecwolf_openfpga.c` registers those basenames with the openFPGA file service
before `main()` and mounts the selected CUE/BIN disc image at
`/cd`. Target builds also define `OF_ECWOLF_OPENFPGA`; the guarded path in
`port/filesys.cpp` makes ECWolf enumerate config and saves from the virtual root
where APF files are visible.

## Controls

The SDK SDL shim exposes the Pocket controls through both SDL_GameController
and SDL_Joystick, matching ECWolf's default control scheme:

- D-pad / left stick: move
- right stick: turn
- A: fire
- B: use / open
- X: run
- Y: strafe
- L: previous weapon
- R: next weapon
- Start: menu / escape
- Select: status / map key
