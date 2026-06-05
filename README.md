# Wolfenstein for Analogue Pocket

[ECWolf](https://github.com/ECWolfEngine/ECWolf) running as a native openFPGA
core on the Analogue Pocket, powered by openfpgaOS (VexiiRiscv @ 100 MHz with a
hardware span-rasterizer GPU).

Supported games:

| Game | Data files |
|------|-----------|
| Wolfenstein 3D | `*.WL6` |
| Spear of Destiny | `*.SOD` |
| Spear of Destiny — Mission 2: Return to Danger | `*.SD2` |
| Spear of Destiny — Mission 3: Ultimate Challenge | `*.SD3` |
| Super 3D Noah's Ark | `*.N3D` |

---

## Setup

### 1. Install the core

Unzip the release onto the root of your Pocket SD card (it merges into
`Cores/`, `Assets/`, and `Platforms/`). If you are building from source,
`make copy CORE=wolfenstein` deploys straight to a mounted card.

### 2. Add the game data

The games are **not included**. Copy the eight data files of each game you own
into:

```
Assets/wolfenstein/common/
```

For Wolfenstein 3D that means:

```
AUDIOHED.WL6  AUDIOT.WL6   GAMEMAPS.WL6  MAPHEAD.WL6
VGADICT.WL6   VGAGRAPH.WL6  VGAHEAD.WL6   VSWAP.WL6
```

…and the same eight names with `.SOD`, `.SD2`, `.SD3`, or `.N3D` for the other
games. The files come straight out of a Steam or GOG install (or your original
media). For the Spear mission packs, rename the mission's `.SOD` files to
`.SD2` / `.SD3` — the standard ECWolf convention. All games' files coexist in
`common/`; the instance you launch binds only its own set.

Optional: `wolfmidi.zip` in the same folder replaces the AdLib soundtrack with
a MIDI music pack played through the Pocket's hardware sample-based MIDI
synth.

### 3. Play

Pocket menu → **openFPGA → Wolfenstein** → pick the game. Saves (10 slots per
game) and settings persist automatically under `Saves/wolfenstein/`.

---

## Controls

### In game

| Input | Action |
|-------|--------|
| D-pad | Move forward / back, turn left / right |
| Left stick (dock) | Move forward / back, strafe left / right |
| Right stick (dock) | Turn |
| **A** | Fire |
| **B** | Open doors / use |
| **X** / **Y** | Run + strafe modifier (hold and turn to strafe) |
| **L1** / **R1** | Strafe left / right |
| **L2** | Open doors / use |
| **R2** | Fire |
| **Select** | Next weapon |
| **Start** | Open / close the menu |

L2/R2 apply to docked controllers with triggers (8BitDo, DualShock, …); the
Pocket's own shoulder buttons are L1/R1. Analog sticks use a quadratic
response — full deflection matches d-pad speed, small deflections give fine
aim. Stick sensitivity and deadzone are tunable per axis in `ecwolf.cfg`
(`JoyAxisNSensitivity`, default 10 = d-pad-matched; `JoyAxisNDeadzone`).

### In menus

| Input | Action |
|-------|--------|
| D-pad / left stick | Navigate |
| **A** | Select |
| **B** | Back |
| **Start** | Close menu |

When saving, the name is pre-filled with the map and current date/time
(`MAP01 Jun 4 11:21:33`) — just press A to accept it.

### SNAC

Wired controllers through the cartridge-slot SNAC adapter (SNES, PSX, …) work
out of the box and map the same way.

---

## Building from source

```bash
make setup                    # install the RISC-V toolchain (one time)
make build CORE=wolfenstein   # build the core into build/wolfenstein/
make copy  CORE=wolfenstein   # deploy to a mounted Pocket SD card
make package CORE=wolfenstein # create a distributable ZIP in releases/
```

The ECWolf source lives in `src/wolfenstein/port/`, the Pocket platform layer
in `src/wolfenstein/of_ecwolf_*.{c,cpp}`, and the openfpgaOS SDK in
`src/sdk/`. See `src/wolfenstein/README.md` for the core's internals (data
slot map, renderer notes).

---

## Credits

- [ECWolf](https://github.com/ECWolfEngine/ECWolf) by Braden "Blzut3" Obrzut,
  built on Wolf4SDL, Wolfenstein 3D by id Software.
- openfpgaOS and the Pocket port by ThinkElastic.
