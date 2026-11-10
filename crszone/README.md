# Crisis Zone for PC

Namco's 1999 arcade light-gun shooter **Crisis Zone** (System 23 Evolution 2), rebuilt to run natively on a PC on the same System 23 host
as Time Crisis 2 (`../tc2`): the game's own programs -- the MIPS main CPU, the H8 sub CPU and the I/O board's H8 -- are translated to C,
and the hardware around them is our own code. Nothing interprets ROM code; MAME was used only as a reference.

**No game data is included.** You need your own copy of MAME's ROM set `crszone.zip` (Crisis Zone, World CSZO4 Ver. B).

## Download

Crisis Zone is in the combined release (0.9.0 and later) on the [Releases page](https://github.com/spacestate1/namco22-decompile/releases):

| Package | For |
|---|---|
| `CrisisZone-x86_64.AppImage` | Linux (64-bit) and the **Steam Deck** -- `chmod +x` it and run it |
| `namco-2x-systems_VERSION_amd64.deb` | Ubuntu / Debian / Mint: the `crisiszone` command and a menu entry |
| `windows-release-VERSION.zip` | Windows 10 / 11 (64-bit) -- unzip, put `crszone.zip` in `roms`, double-click `CrisisZone.exe` |

The first start unpacks `crszone.zip` and checks every chip against MAME's checksums ([ROM_CHECKSUMS.md](../docs/ROM_CHECKSUMS.md)).

## Building from source (Linux)

```bash
./build.sh /path/to/crszone.zip      # unpacks and checks the ROM, translates the two H8 programs, builds
./launch.sh                          # play (./launch.sh 2 = a 2x window, --fullscreen)
```
The keys and the light gun are Time Crisis 2's: [HOW TO PLAY](../tc2/packaging/linux/HOW-TO-PLAY.txt). The game program is ~170 MB of
generated C (`gen/crszone_lifted_*.c`): allow about 1 GB of memory per parallel compile (`JOBS=2 ./build.sh ...`).
Windows: `./build-windows.sh` (MinGW-w64 cross compiler) after `./build.sh`.

## What works

Attract (matches MAME), coins, play with the light gun / mouse / pad, sound, the operator's test menu, the battery RAM saved between
sessions, widescreen, any internal resolution, frame-rate options and motion smoothing, gun recoil output and pad rumble on each shot
(Display > Pad rumble on recoil). Not tested on real Windows hardware or with a real light gun.

Custom crosshair: put PNG files (8-bit, up to 256 x 256) in a `crosshairs/` folder beside the program and pick one in
Escape -> Display -> Crosshair image.
