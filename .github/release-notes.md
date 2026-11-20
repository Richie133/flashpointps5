Flashpoint PS5 0.0.2 alpha: a native PS5 app (PPSA72386) for the Flashpoint Archive.

**New in 0.0.2**
- An interface laid out like the Flashpoint Launcher: Home, Arcade, Theatre, Web, Logs, Config and About, with search, sorting, a filter sidebar, a tile grid and a details panel.
- The Flashpoint library downloads itself from Flashpoint's servers on first start (no PC needed) and updates every few days. A library copied from a PC still works.
- Arcade (games) and Theatre (animations), filtered by platform, "Plays on PS5", recently played and downloaded.
- Web: Flash from Dagobah, PiczoGame and swfchan, and "Play a .swf link" to open any Flash file in Ruffle. Entries a site marks as adult are hidden unless you turn them on in Config.
- Touchpad pointer (slide to move, press to click, two fingers to scroll), plus USB mouse and keyboard (type to search).
- A new icon, an intro with original music, and interface sounds.

**Not tested on a real PS5 yet.** Everything runs on a PC against the PC build of the background service. The web sources were tested against sample pages shaped like those sites, not the live sites. If something goes wrong, the Logs page shows the service's log (`/data/fpps5/fpps5.log`); please open an issue with it.

**You need**
- A PS5 that runs homebrew apps, with an ELF loader on port 9021 (etaHEN, or kstuff with elfldr). The app starts its background service through it.
- Ruffle Flash PS5 for Flash games and animations.

**Install:** unzip `PPSA72386.zip` and copy the `PPSA72386` folder to `/data/homebrew/` (or install it from a homebrew store).
