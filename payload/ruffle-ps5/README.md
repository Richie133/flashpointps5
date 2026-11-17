# Ruffle Flash PS5: direct launch from Flashpoint PS5

`0001-flashpoint-ps5-direct-launch.patch` applies to
[ZiZc3/Ruffle-Flash-PS5](https://github.com/ZiZc3/Ruffle-Flash-PS5) at
`1265f47` (v1.3.0), which pins Ruffle `e092c78c`.

```sh
git clone https://github.com/ZiZc3/Ruffle-Flash-PS5 && cd Ruffle-Flash-PS5
git apply /path/to/0001-flashpoint-ps5-direct-launch.patch
bash ps5/scripts/build.sh      # as in its README (PS5_Vulkan, Rust nightly)
```

If you already have a Ruffle checkout from an earlier build, also run
`git -C ~/ps5/ruffle apply "$PWD/patches/ruffle-flashpoint.patch"` once
(build.sh only applies patches to a fresh checkout).

## What it changes

- `src/flashpoint.rs` (new): on start, writes `/data/ruffle/.flashpoint-launch`
  so Flashpoint PS5 knows this build, then reads and deletes
  `/data/ruffle/launch.txt` if present.
- `src/main.rs`: a pending launch plays straight away instead of showing the
  library. The movie gets the launch command's query string as FlashVars and
  its original URL as the spoofed address. Leaving the game (quick menu →
  library) closes the app when `exit=1`, which takes you back to the browser.
- `patches/ruffle-flashpoint.patch` (applied to Ruffle by `build.sh`):
  Ruffle's `NullNavigatorBackend` answers `http(s)://host/path` from the
  folders listed in `RUFFLE_WEB_ROOTS` (`<root>/<host>/<path>`), before the
  app's usual "no network" handling. That's the layout of an unpacked
  GameZIP's `content/` and of `Legacy/htdocs`, so multi-file games that load
  from absolute URLs find their files.

`launch.txt` looks like:

```text
title=Orbit Courier
swf=/data/ruffle/flashpoint/<id>/www.example.com/play/courier.swf
url=http://www.example.com/play/courier.swf?mode=story
query=mode=story
root=/data/ruffle/flashpoint/<id>
root=/data/flashpoint/Legacy/htdocs
exit=1
```

The patch was written against the real sources and applies cleanly, but it
hasn't been compiled here (no Rust toolchain for the PS5 target was
available), so treat the first build as a check.
