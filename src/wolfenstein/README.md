# Wolfenstein / ECWolf openfpgaOS Core

This core keeps ECWolf's SDL-facing platform code intact. Pocket-specific
behavior is implemented in the SDK SDL compatibility layer:

- `src/sdk/include/SDL2/SDL.h`
- `src/sdk/of_sdl2.c`

The ECWolf source lives in `src/wolfenstein/port/`. There is no downloader or
missing-source fallback in this core.

## Runtime Data

Runtime data lives in `Assets/wolfenstein/common/` after release assembly. The
provided instances reference the game's data files **directly** as APF data
slots — there is no CUE/BIN disc image and no `/cd` ISO bridge. The eight ECWolf
data files are extracted from the original media and dropped into `common/`:

- `Wolf3D.json`: `AUDIOHED.WL6` `AUDIOT.WL6` `GAMEMAPS.WL6` `MAPHEAD.WL6`
  `VGADICT.WL6` `VGAGRAPH.WL6` `VGAHEAD.WL6` `VSWAP.WL6`
- `Spear.json`: the same eight files with the `.SOD` extension

Both games' files coexist in `common/`; selecting an instance binds only that
game's files (so ECWolf sees exactly one IWAD). ECWolf's data search path is the
virtual root (`$PROGDIR` = `/`), where the bound slots are visible via
`opendir`/`readdir`, so the engine finds the files directly with no disc mount.

Data-slot map (the firmware supports IDs up to 31; saves keep their fixed
nonvolatile range 10-19, and slots 7/8 are the system Sound Bank / Shared
Config, so the eight data files take 5, 6, 9 and 20-24):

| Slot | Contents |
|------|----------|
| 1 | `os.bin` (OS binary) |
| 2 | `wolf3d.ini` / `spear.ini` (OS config) |
| 3 | `wolfenstein.elf` (application) |
| 4 | `wolfmidi.zip` (MIDI music replacement pack) |
| 5, 6 | `AUDIOHED`, `AUDIOT` |
| 7 | `bank.ofsf` (MIDI SoundFont, preloaded by the SDK MIDI path) |
| 8 | `ecwolf.cfg` (shared config, nonvolatile) |
| 9 | `GAMEMAPS` |
| 10-19 | `savegam0.ecs` … `savegam9.ecs` (nonvolatile saves) |
| 20-24 | `MAPHEAD`, `VGADICT`, `VGAGRAPH`, `VGAHEAD`, `VSWAP` |

Wolfenstein 3-D and Spear of Destiny use IMF/AdLib music in the original media,
not Standard MIDI. The OpenFPGA build does not run the OPL emulator or convert
IMF at runtime: place `wolfmidi.zip` in `common/` (slot 4) as the MIDI music
replacement pack. If the pack is missing, or a song is not in it, music is
disabled for that song. The wrapper derives ECWolf's data extension from
whichever `VSWAP` variant the instance bound (`VSWAP.WL6` → `.wl6`,
`VSWAP.SOD` → `.sod`).

To regenerate the data files from a CUE/BIN dump: `bchunk` the BIN to an ISO,
then extract `Install/data/WOLF3D/*.WL6` (Wolf3D CD) or the root `*.SOD` files
(Spear CD) into `common/`.

ECWolf also has its own engine support data (`IWADINFO`, actor definitions,
map remap tables, strings, fonts, and related lumps). That data is not part of
the original CUE/BIN game media. For this core it should be embedded into the
application build, not exposed as another APF data slot. The Makefile packages
`wadsrc/static/` into an internal `ecwolf.pk3` blob and the resource loader
serves that in memory when ECWolf asks for `ecwolf.pk3`.

## Rendering

The default Pocket build uses a Wolfenstein-specific fast wall renderer, the
OpenFPGA GPU for affine wall columns, and the SDK triple-buffer queue for
presentation. SDLFB locks a hardware draw slot, renders into that slot, and
presents it with `of_gpu_flip_to()` instead of copying a software buffer through
`SDL_RenderPresent()`. Full-screen gameplay frames are treated as full redraws
so the draw page does not have to be seeded from the previous frame; retained
framebuffer behavior is still preserved for partial redraw paths such as menus.
The OpenFPGA build defaults saved/loaded config to `ViewSize=21`, so gameplay
uses this full-screen path instead of ECWolf's smaller viewport plus CPU status
bar redraw.

Gameplay uses ECWolf's canonical wall traversal and submits eligible wall,
sky, floor/ceiling, sprite, and weapon spans to the OpenFPGA GPU. This keeps
the complicated map/door/pushwall behavior in one renderer while still using
the hardware span path for the expensive pixel work. Normal full-screen
gameplay keeps GPU work queued until the direct video-frame present, avoiding a
CPU-visible framebuffer sync in the middle of the frame. The app uses a 320x240
APF framebuffer for direct triple buffering, with the 320x200 Wolfenstein
gameplay viewport centered vertically inside it. The 320x240 scaler mode is
marked 10:9 so the centered 320x200 viewport displays at the original 4:3 game
aspect.

The OpenFPGA build always enables direct GPU video presentation and the GPU
span renderer.

### GPU/CPU coherence (region sync)

The OpenFPGA GPU is an asynchronous span rasterizer: draw commands are staged
and only run when kicked at present, GPU writes go straight to SDRAM bypassing
the CPU cache, and `of_gpu_finish()` is a blocking fence wait. When a primitive
cannot go on the GPU (an odd-sized sprite column, a non-power-of-two parallax
sky column, ...) the renderer draws that column on the CPU into the same buffer.

The fallback path uses region-scoped coherence modelled on the Doom OpenFPGA
renderer (`../Doom/src/doom/cdoom/doom/r_gpu.c`): per-cache-line dirty/valid
tracking in `of_ecwolf_gpu.cpp`, `OF_WolfGPU_PrepareForCPUAccessRect/Column`
drains the GPU only when work is outstanding and invalidates only the touched
lines, `gpu_prepare_for_gpu_write()` publishes pending CPU pixels before each GPU
write so a 64-byte cache line is never both CPU-dirty and GPU-written, and the
CPU writes are flushed in one coalesced pass at present. The GPU frame stays
active across fallbacks. The old path turned every CPU fallback into a full
`of_gpu_finish()` + whole-frame cache invalidate + frame teardown that also
forced the rest of the frame onto the CPU and a whole-frame flush at present.
Base Wolf3D/Spear content keeps all walls/floor/sprites/weapon on the GPU and
never falls back, so it stays on the fully asynchronous present path.

This path is compile-verified but has not yet been validated on hardware.

## Saves

ECWolf uses `ecwolf.cfg` for settings and `savegamN.ecs` for saves. The core
maps these into nonvolatile APF slots:

- slot 8: `ecwolf.cfg`
- slots 10-19: `savegam0.ecs` through `savegam9.ecs`

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
