# bassdr's fork of SoH

> [!IMPORTANT]
> **This is a fork, not Ship of Harkinian.** It carries features that are still in review — or too
> large to land — upstream, and runs on libultraship's `main` (the 2.0.0 line, SDL3) rather than
> the `port-maintenance` branch upstream SoH ships. Report bugs you hit here to
> [this fork](https://github.com/bassdr/Shipwright/issues), not to HarbourMasters.
>
> If you want the stable, supported game, use
> [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) instead.

## What this fork adds

| Feature | PR | Status |
| --- | --- | --- |
| FluidSynth SoundFont replacement | [#6668](https://github.com/HarbourMasters/Shipwright/pull/6668) | open, stalled on size |
| Ocarina songs from the pause menu | [#6951](https://github.com/HarbourMasters/Shipwright/pull/6951) | open |
| Song of Time plays out before the age shift | [#7058](https://github.com/HarbourMasters/Shipwright/pull/7058) | open |
| Streamed music kept alive across the enemy BGM crossfade | [#7126](https://github.com/HarbourMasters/Shipwright/pull/7126) | open |
| Streamed music fades instead of cutting | [#7225](https://github.com/HarbourMasters/Shipwright/pull/7225) | open |
| Streamed scene music resumes instead of restarting | [#7259](https://github.com/HarbourMasters/Shipwright/pull/7259) | open |
| Music packs can replace ocarina and ending tracks | [#7224](https://github.com/HarbourMasters/Shipwright/pull/7224) | open |
| Audio pump gated without engine races or added latency | [#7289](https://github.com/HarbourMasters/Shipwright/pull/7289) | open |
| Speech rate, volume and pitch | [#7208](https://github.com/HarbourMasters/Shipwright/pull/7208) | open |
| Choose where accessibility speech plays | [#7221](https://github.com/HarbourMasters/Shipwright/pull/7221) | open |
| Pause Warp no longer fires from L/Z | [#7261](https://github.com/HarbourMasters/Shipwright/pull/7261) | open |
| Active mods visible in the log and menu | [#7442](https://github.com/HarbourMasters/Shipwright/pull/7442) | open |
| Assign songs to the C-buttons and Dpad; Navi answers Saria's Song | — | fork only |

## What this fork changes in libultraship

Most of the change is not in this repository. It pins
[a libultraship fork](https://github.com/bassdr/libultraship/tree/main) that is current with
upstream `main` and adds 111 commits on top of it, about +10,950/-1,990 lines across 81 files —
mostly the renderer.

| What the fork carries | PR | Status |
| --- | --- | --- |
| Vulkan rendering backend | [#1265](https://github.com/Kenix3/libultraship/pull/1265) (KiritoDv) | open |
| GPU vertex pipeline, mipmapping, two-tile LOD, postprocessing and custom shaders | [#1262](https://github.com/Kenix3/libultraship/pull/1262), [#1263](https://github.com/Kenix3/libultraship/pull/1263) (KiritoDv) | open |
| SDL3 pull-model audio | [#1250](https://github.com/Kenix3/libultraship/pull/1250) | open |
| WASAPI start threshold and callback deadlock | [#1272](https://github.com/Kenix3/libultraship/pull/1272), [#1281](https://github.com/Kenix3/libultraship/pull/1281) | open |
| `CreateDefaultInstance` wiring, bridge component, root Context init | [#1291](https://github.com/Kenix3/libultraship/pull/1291), [#1292](https://github.com/Kenix3/libultraship/pull/1292), [#1293](https://github.com/Kenix3/libultraship/pull/1293) | open |
| Windows and macOS includes SDL2 used to provide | [#1290](https://github.com/Kenix3/libultraship/pull/1290) | open |
| fast3d segment table and `GetPixelDepth` bounds | [#1289](https://github.com/Kenix3/libultraship/pull/1289), [#1295](https://github.com/Kenix3/libultraship/pull/1295) | open |
| Window title uses the application name | [#1297](https://github.com/Kenix3/libultraship/pull/1297) | open |
| stb_image.h download verify and retry | [#1248](https://github.com/Kenix3/libultraship/pull/1248) | open |
| Unsigned GBI macros, two div-by-zero guards | [#1115](https://github.com/Kenix3/libultraship/pull/1115) | open |
| Crash handler, atomic config save, pyramidlike test (cherry-picks from `port-maintenance`) | [#1276](https://github.com/Kenix3/libultraship/pull/1276), [#1277](https://github.com/Kenix3/libultraship/pull/1277), [#1278](https://github.com/Kenix3/libultraship/pull/1278) | open |
| CPU vertex transform, Vulkan fixes, build and crash-dialog tweaks | — | fork only |

**SDL3 is the only audio backend.** WASAPI and CoreAudio were deleted rather than maintained —
the fork carried fixes for both first, then dropped them — so everything goes through one SDL3
player. This removal has no PR.

Both sides are on SDL3: the game was ported to the epoch 2 component API to match.

## Why a fork

Upstream targets `port-maintenance` and moves deliberately. This fork exists to run the above
together, and to get onto the SDL3 engine line, without waiting for each piece to land. Rows marked
"fork only" are not proposed upstream yet; everything else is.

---

![Ship of Harkinian](docs/shiptitle.darkmode.png#gh-dark-mode-only)
![Ship of Harkinian](docs/shiptitle.lightmode.png#gh-light-mode-only)

* [Website](https://www.shipofharkinian.com)
* [Discord](https://discord.com/invite/harbourmasters)

If you're having any trouble after reading through this `README`, feel free to ask for help in the Support text channels. Please keep in mind that we do not condone piracy.

## Quick Start

The Ship does not include any copyrighted assets.  You are required to provide a supported copy of the game.

### 1. Verify your ROM dump

You can verify you have dumped a supported copy of the game by using the compatibility checker at <https://ship.equipment/>. If you'd prefer to manually validate your ROM dump, you can cross-reference its `sha1` hash with the [supported hashes](docs/supportedHashes.json).

### 2. Download The Ship of Harkinian from [Releases](https://github.com/HarbourMasters/Shipwright/releases)

### 3. Launch the Game!

#### Windows

* Extract the zip
* Launch `soh.exe`

#### Linux

* Execute `soh.appimage`.  You may have to `chmod +x` the appimage via terminal.
* When prompted, select your supported copy of the game.
* Saves, settings and `oot.o2r` are stored in `~/.local/share/soh/`. If the folder you launch from already has `shipofharkinian.json`, `oot.o2r` or `oot-mq.o2r`, that folder is used instead. Set `SHIP_HOME` to choose another folder.

#### macOS

* Run `soh.app`. When prompted, select your supported copy of the game.
* You should see a notification saying `Processing OTR`, then, once the process is complete, you should get a notification saying `OTR Successfully Generated`, then the game should start.

### 4. Play!

Congratulations, you are now sailing with the Ship of Harkinian! Have fun!

## Configuration

### Default keyboard configuration

| N64 | A | B | Z | Start | Analog stick | C buttons | D-Pad |
| - | - | - | - | - | - | - | - |
| Keyboard | X | C | Z | Space | WASD | Arrow keys | TFGH |

### Other shortcuts

| Keys | Action |
| - | - |
| ESC | Toggle menu |
| F2 | Toggle capture mouse input |
| F5 | Save state |
| F6 | Change state |
| F7 | Load state |
| F9 | Toggle Text-to-Speech |
| F11 | Fullscreen |
| Tab | Toggle Alternate assets |
| Ctrl+R | Reset |

## Project Overview

Ship of Harkinian (SOH) is built atop a custom library dubbed libultraship (LUS). Back in the N64 days, there was an SDK distributed to developers named libultra; LUS is designed to mimic the functionality of libultra on modern hardware. In addition, we are dependent on the source code provided by the OOT decompilation project.

In order for the game to function, you will require a **legally acquired** ROM for Ocarina of Time. The [Compatibility Checker](https://ship.equipment/) can be used to check the compatibility of your specific rom. Any copyrighted assets are extracted from the ROM and reformatted as a .o2r archive file which the code uses.

### Graphics Backends

Currently, there are four rendering APIs supported: DirectX11 (Windows), OpenGL (all platforms), Metal (MacOS), and Vulkan (Windows and Linux). You can change which API to use in the `Settings` menu of the menubar, which requires a restart.  If you're having an issue with crashing, you can change the API in the `shipofharkinian.json` file by finding the line `gfxbackend:""` and changing the value to `sdl` for OpenGL. DirectX 11 is the default on Windows.

## Custom Assets

Custom assets are packed in `.otr` archive files. To use custom assets, place them in the `mods` folder.

If you're interested in creating and/or packing your own custom asset `.otr` files, check out the following tools:

* [**retro - OTR generator**](https://github.com/HarbourMasters64/retro)
* [**fast64 - Blender plugin**](https://github.com/HarbourMasters/fast64)

## Development

### Building

If you want to manually compile SoH, please consult the [building instructions](docs/BUILDING.md).

### Playtesting

If you want to playtest a continuous integration build, you can find them at the links below. Keep in mind that these are for playtesting only, and you will likely encounter bugs and possibly crashes.

* [Windows](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-windows.zip)
* [macOS](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-mac.zip)
* [Linux X86_64](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-linux-x86_64.zip)
* [Linux ARM](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-linux-aarch64.zip)

### Further Reading

More detailed documentation can be found in the 'docs' directory, including the aforementioned [building instructions](docs/BUILDING.md).

* [Credits](docs/CREDITS.md)
* [Custom Music](docs/CUSTOM_MUSIC.md)
* [Formatting](docs/FORMATTING.md)
* [Controller Mapping](docs/GAME_CONTROLLER_DB.md)
* [Modding](docs/MODDING.md)
* [Versioning](docs/VERSIONING.md)

<a href="https://github.com/Kenix3/libultraship/">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/poweredbylus.darkmode.png">
    <img alt="Powered by libultraship" src="./docs/poweredbylus.lightmode.png">
  </picture>
</a>
