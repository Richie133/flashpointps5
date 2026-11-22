# Third-party notices

Flashpoint PS5 is licensed under GPL-3.0-or-later (`LICENSE`). It includes or
adapts the following.

## The app (`src/`, `host/`, `tooling/`, `tools/`, `runtime/`)

| Component | Where | Copyright | Licence |
|---|---|---|---|
| ps5-native-app-boilerplate: build scripts, `tooling/native` (ps5-native-tool, PIE linker script, CRT), clean-room `runtime/libc.prx` | `tooling/`, `tools/`, `runtime/`, `src/runtime/` | (C) 2026 BlackBearReloaded | GPL-3.0-or-later |
| ps5-homebrew-ui kit: renderer, draw list, fonts, input, EGL display, pad, system calls | `src/gfx/`, `src/ui/`, `src/core/`, `src/platform/ps5/` (except `services.cpp`), `host/platform_host.cpp` | (C) 2026 BlackBearReloaded | GPL-3.0-or-later |
| PayLoader: loopback HTTP client, JSON reader, job worker | `src/fp/http.*`, `src/fp/json.*`, `src/fp/worker.*` (namespace renamed) | Marice and PayLoader contributors | GPL-3.0-or-later |
| offact: the `sceImeDialog` parameter layout | `src/platform/ps5/services.cpp` | ps5-payload-dev | GPL-3.0 |
| Ruffle Flash PS5: USB keyboard and mouse record layouts, how a title loads those libraries (sysmodules 0x0106 / 0x00a9, then looked up), the touchpad data offset, and the `sceAudioOut` parameters | `src/fp/hid_records.hpp`, `src/fp/hid_common.cpp`, `src/platform/ps5/hid.cpp`, `src/platform/ps5/pad.cpp` (touch fields), `src/platform/ps5/audio_out.cpp` | ZiZc3 and Ruffle Flash PS5 contributors (layouts from the ps5-native-gamepad-input-research notes) | GPL-3.0 |
| stb_image (PNG, JPEG and GIF decoders enabled) | `src/third_party/stb/` | Sean Barrett and contributors | Public domain or MIT (`src/third_party/stb/LICENSE`) |
| libwebp 1.6.0, decoder only (Flash Museum's WebP pictures) | `src/third_party/libwebp/` | (c) 2010 Google Inc. | BSD 3-Clause (`src/third_party/libwebp/COPYING`), with Google's patent grant (`src/third_party/libwebp/PATENTS`) |
| stb_image_write (PC snapshot tool only, not in the PS5 app) | `host/third_party/stb_image_write.h` | Sean Barrett and contributors | Public domain or MIT |
| Shape shader distance functions | `src/gfx/gl_batch.cpp` | Inigo Quilez | MIT |

The PS5 app statically links [ps5-opengl](https://github.com/darkxex/ps5-opengl)
(OpenGL on the PS5, built on Mesa), under that project's licences. `make`
downloads it into `.deps/`; it is not stored in this repository.

## Music, sounds and icon

The intro music, the interface sounds (`assets/audio/`) and the icon
(`sce_sys/icon0.png`) are original to this project, synthesised and drawn by
`tools/make-audio.py` and `tools/make-icon.py`, and covered by its licence.

## Fonts (`assets/fonts/`)

| Typeface | Copyright | Licence |
|---|---|---|
| Atkinson Hyperlegible Regular and Bold | (c) 2020 Braille Institute of America, Inc. | SIL Open Font License 1.1 (`assets/fonts/OFL-AtkinsonHyperlegible.txt`) |
| Silkscreen | (c) 2001 The Silkscreen Project Authors | SIL Open Font License 1.1 (`assets/fonts/OFL-Silkscreen.txt`) |

The `.huifont` files are distance-field atlases baked from these typefaces with
`tools/font-baker`.

## The background service (`payload/`)

| Component | Where | Copyright | Licence |
|---|---|---|---|
| Flashpoint Launcher (design of the library, downloads and game server) | `payload/src/` | Flashpoint Archive contributors | MIT (`licenses/flashpoint-launcher-MIT.txt`) |
| Flashpoint Database (search API and web UI design) | `payload/src/api.c`, `payload/www/` | Flashpoint Archive contributors | MIT (`licenses/flashpoint-database-MIT.txt`) |
| FPA-Rust (flashpoint-database-builder): the metadata server's API and how its pages become a database | `payload/src/sync.c` | (c) 2024 Flashpoint Archive | MIT (`licenses/FPA-Rust-MIT.txt`) |
| flashpoint-database-api: the list of tags whose entries count as extreme | `payload/src/sync.c` | (c) 2023 IWumboYouWumbo | MIT (`licenses/flashpoint-database-api-LICENSE.txt`) |
| SQLite | `payload/third_party/sqlite/` | none | Public domain |
| miniz | `payload/third_party/miniz/` | Rich Geldreich, RAD Game Tools, Valve | MIT (`payload/third_party/miniz/LICENSE`) |
| cJSON | `payload/third_party/cjson/` | Dave Gamble and cJSON contributors | MIT (`payload/third_party/cjson/LICENSE`) |
| SHA-256 | `payload/third_party/sha256/` | Brad Conte | Public domain |

The service is built with the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk).

## Web sources

Dagobah, PiczoGame, swfchan and Flash Museum are read from their public pages
only when you open them in the app, as a browser shows them; nothing from them
is stored in this repository. Their files belong to their authors.

## Flashpoint content

This repository and the app contain no games. The app reads the Flashpoint
database you copy to the console and downloads game data, logos and
screenshots from Flashpoint Archive's servers when you use them, as Flashpoint
Infinity does. That content belongs to its respective owners.
