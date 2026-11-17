# Flashpoint PS5

[Flashpoint Launcher](https://github.com/FlashpointProject/launcher) and the
[Flashpoint Database](https://github.com/FlashpointProject/flashpoint-database)
merged into one PS5 homebrew payload. Browse your Flashpoint library on the TV
with a controller, search it the way the Flashpoint Database site does, and
play Flash games natively through [Ruffle Flash PS5](https://github.com/ZiZc3/Ruffle-Flash-PS5).

> **Status:** builds with the ps5-payload-sdk and is tested end to end on a PC
> (same code, console calls stubbed). It has **not been run on a real PS5 yet**.
> The console-specific parts follow ps5-payload-dev/websrv and the SDK samples
> closely, but expect first-run fixes.

## How it works

```
PS5 browser (or a phone/PC)          flashpoint-ps5.elf (payload, runs in the background)
┌──────────────────────────┐  HTTP   ┌──────────────────────────────────────────────┐
│ Flashpoint Database UI,  │ ──────► │ :22600  UI + JSON API (db-api compatible)     │
│ re-cut for a TV + pad    │         │ :22500  Flashpoint-style game server          │
└──────────────────────────┘         │ SQLite  Data/flashpoint.sqlite (launcher DB)  │
                                     │ sceHttp GameZIPs, logos, Legacy files on demand│
                                     └───────────────┬──────────────────────────────┘
                                                     │ unpack + /data/ruffle/launch.txt
                                                     ▼
                                     Ruffle Flash PS5 app (PPSA68091) plays the SWF
```

What came from where:

| From the **launcher** | From the **database** |
|---|---|
| Reads Flashpoint's own `flashpoint.sqlite` (FPA-Rust schema) | Search by any field, comma-separated values, "match any" |
| Downloads GameZIPs on demand from Flashpoint's servers, checks sha256 | Same endpoints and fields: `/api/search`, `/addapps`, `/platforms`, `/stats`, `/files` |
| Game server: `content/<host>/<path>` + `Legacy/htdocs`, proxy requests too | Entry viewer: logo, screenshot, metadata, additional apps, GameZIP file list |
| Additional apps, `:message:` entries, last played / play count | Statistics page |
| Logos and screenshots fetched and cached like Infinity | Online mode: searches the live database (`db-api.unstable.life`) |

## What plays on the PS5

| Platform | On the PS5 | How |
|---|---|---|
| Flash | Yes | Unpacked, then handed to **Ruffle Flash PS5** (native Ruffle, Vulkan) |
| HTML5 | Partly | Opens in the PS5 browser via the game server. That browser has no WebGL or Web Audio, so some games are blank or silent |
| Shockwave, Unity, Java, Silverlight… | No | No PS5 player exists yet; the UI says so |

Ruffle can't run inside the PS5 browser: Sony's WebKit build has JIT and
WebAssembly switched off. That's why Flash goes to the native app.

**Bonus:** open `http://<ps5-ip>:22600` on a phone or PC on the same network to
browse the PS5's library there. "Play in this browser" runs Flash games with
Ruffle on that device, streaming the files from the PS5's game server.

## Install

You need a jailbroken PS5 running the [websrv](https://github.com/ps5-payload-dev/websrv)
payload and its Homebrew Launcher.

1. Copy `FlashpointPS5/` (from the release zip, or `make homebrew` → `dist/`) to
   `/data/homebrew/` (or a USB drive's `homebrew/` folder).
2. On a PC, get **Flashpoint Infinity** from flashpointarchive.org and run it once.
   Copy its `Data/flashpoint.sqlite` to the PS5 keeping the layout:
   `/data/flashpoint/Data/flashpoint.sqlite`, or `flashpoint/Data/flashpoint.sqlite`
   on a USB drive. Already-downloaded `Data/Games`, `Data/Images` and
   `Legacy/htdocs` can come too; anything missing downloads when needed.
3. Install **Ruffle Flash PS5** (`PPSA68091`) with kstuff + ShadowMountPlus, as its README says.
4. Start **Flashpoint PS5** from the Homebrew Launcher. It opens the PS5 browser on its UI.

Folders searched for a Flashpoint folder, in order: `/data/flashpoint`,
`/mnt/usb0-3/flashpoint`, `/mnt/ext0-1/flashpoint`.

### Direct launch (recommended)

The stock Ruffle Flash app only plays files from its own library, so by default
Play **copies** the game into `/data/ruffle/games/<Title>/` (with the Flashpoint
logo as its cover) and opens the app, where you pick it.

`ruffle-ps5/0001-flashpoint-ps5-direct-launch.patch` adds proper support to
Ruffle Flash PS5: the game starts immediately, gets its launch command's query
string as FlashVars, believes it was loaded from its original URL (sitelocks),
and requests for `http://host/path` are answered from the unpacked GameZIP and
`Legacy/htdocs`. Build that app with the patch applied; Flashpoint PS5 detects
it (`/data/ruffle/.flashpoint-launch`) and switches over by itself.
See `ruffle-ps5/README.md`.

### Controls (PS5 browser)

| Button | Does |
|---|---|
| D-pad | Move between tiles, fields and buttons |
| ✕ | Open / press |
| ○ | Back |
| L1 / R1 | Previous / next section |
| L2 / R2 | Previous / next page (the timeline above the results) |

## Settings

Optional `fpps5.json`, looked for in the homebrew folder, then `/data/fpps5/`.
Every key is optional; see `homebrew/FlashpointPS5/fpps5.json.example`.

| Key | Default | |
|---|---|---|
| `flashpointPath` | auto-detected | Flashpoint folder |
| `runPath` | `/data/ruffle/flashpoint` | Where games are unpacked (Ruffle must be able to read it) |
| `keepRunDirs` | `4` | Unpacked games kept; older ones are removed |
| `uiPort` / `gameServerPort` | `22600` / `22500` | |
| `openBrowser` | `true` | Open the PS5 browser on start |
| `gameDataSources` | Flashpoint's `gib-roms/Games/` | GameZIP mirrors (launcher format accepted too) |
| `imagesUrl`, `legacyFallbackUrl` | Infinity's servers | On-demand logos/screenshots and Legacy files |
| `onlineApi` | `https://db-api.unstable.life` | For "Search in: Flashpoint database online" |
| `ruffleTitleId`, `ruffleDir`, `ruffleMode` | `PPSA68091`, `/data/ruffle`, `auto` | `auto`, `direct` or `export` |

Log: `/data/fpps5/fpps5.log`.

## Building

PS5 payload (needs [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk); nothing else, all libraries are vendored):

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make            # build/ps5/flashpoint-ps5.elf
make homebrew   # dist/FlashpointPS5/ ready to copy
make test PS5_HOST=192.168.1.50   # send to elfldr on port 9021
```

PC test build (Linux/macOS/FreeBSD, uses the `curl` tool for downloads):

```sh
make -f Makefile.pc
python3 tools/make-testdb.py /tmp/fptest          # fake Flashpoint folder + fake download server
python3 -m http.server 8765 -d /tmp/fptest/remote &
./build/pc/fpps5 --root /tmp/fptest/flashpoint --run /tmp/fptest/run --ruffle-dir /tmp/fptest/ruffle
# open http://localhost:22600  (add --config with gameDataSources pointing at :8765 to test downloads)
```

`--www <dir>` serves the UI from disk instead of the copy built into the binary.

## API

| Endpoint | |
|---|---|
| `GET /api/search` | db-api parameters (`smartSearch`, any field, `any`, `filter`, `fields`) plus `limit`, `offset`, `sort`, `dir`, `installed`, `played`, `ps5`. Total in `X-Total-Count` |
| `GET /api/game?id=` | Entry, game data, additional apps, and how it plays on the PS5 |
| `GET /api/addapps`, `/api/platforms`, `/api/stats`, `/api/files?id=` | As the db-api |
| `POST /api/play?id=[&addApp=]` | Download → unpack → hand off. Poll while it answers `pending` |
| `GET /api/prepare?id=` | Download and unpack only |
| `GET /api/jobs`, `/api/cancel?id=`, `/api/config`, `/api/quit` | |
| `GET /images/<Logos|Screenshots>/…` | Cached, fetched on demand |
| `:22500 /<host>/<path>` or proxy requests | Game files |

## Credits and licenses

GPL-3.0-or-later (see `LICENSE`), because the console launch code follows
websrv and the Ruffle patch extends Ruffle Flash PS5, both GPL-3.0.

- Flashpoint Launcher and Flashpoint Database by the Flashpoint Archive team, MIT (`licenses/`).
  `www/fields.json`, `www/sort.json`, the logo and the entry/metadata layout come from the database frontend.
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) and [websrv](https://github.com/ps5-payload-dev/websrv) by John Törnblom.
- [Ruffle Flash PS5](https://github.com/ZiZc3/Ruffle-Flash-PS5) by ZiZc3; [Ruffle](https://ruffle.rs).
- SQLite (public domain), miniz (MIT), cJSON (MIT), Brad Conte's sha256 (public domain).
- Atkinson Hyperlegible and Silkscreen fonts, SIL OFL 1.1 (`www/fonts/`).

Not affiliated with Flashpoint Archive, Sony or the Ruffle project. No games are included.
