Flashpoint PS5 0.0.5 alpha: a native PS5 app (PPSA72386) for the Flashpoint Archive.

**Fixed in 0.0.5**
- Search crashed the app ("Game or App Error", SIGSEGV in eboot.bin). The system keyboard's library isn't loaded when a homebrew app starts, so the app's link to it pointed at nothing. The app now loads that library itself (sysmodule 0x0096) and looks its functions up.
- If the system keyboard still can't open, the app shows its own on-screen keyboard: D-pad and Cross to type, Square delete, Triangle space, Options done, Circle cancel. The touchpad pointer and a USB keyboard work in it too.

**New in 0.0.5**
- Themes, in Config: Flashpoint (the launcher's dark look), Y8 (gray page, white boxes and red underlined titles, like the classic Flash portal), Daylight, Midnight, Ember and Terminal.

**Fixed in 0.0.4**
- 0.0.3 stopped on "The app is incomplete" on every start: its background service had grown past a 4 MB limit in the app's file reader. The limit is now 64 MB, and the service is 1.9 MB instead of 4.9 MB.

**Fixed in 0.0.3**
- Search works on every web site. Dagobah uses its own search; PiczoGame and swfchan have none, so their lists are read and matched by title.
- Each tab keeps its own search. A search typed in Arcade no longer carries into Theatre or Web, where it showed in the box without being applied.
- Platform counts in the sidebar are for the open library only, and a chosen platform stays chosen when the library updates.
- After a page jump (L2/R2) the details panel shows the entry under the cursor instead of the previous one.
- When a library update finishes you stay where you were in the list instead of jumping back to the top.
- Pictures that fail to load (a slow site, a busy moment) are tried again, and switching tabs loads the new page's pictures first.
- Web lists keep loading past pages where every entry was hidden or nothing matched the search.
- Web pages and pictures are saved safely when several requests come in at once, and a saved copy is shown if a site stops answering.
- Pressing the touchpad while the pointer is hidden brings the pointer back instead of clicking a spot you can't see.
- Sorting is offered only in Arcade and Theatre (web lists come in each site's own order).
- Typing on a USB keyboard from any page searches the Arcade.

**Also in this build** (from 0.0.2, which wasn't released on its own)
- An interface laid out like the Flashpoint Launcher: Home, Arcade, Theatre, Web, Logs, Config and About, with search, sorting, a filter sidebar, a tile grid and a details panel.
- The Flashpoint library downloads itself from Flashpoint's servers on first start (no PC needed) and updates every few days. A library copied from a PC still works.
- Web: Flash from Dagobah, PiczoGame and swfchan, and "Play a .swf link" to open any Flash file in Ruffle. Entries a site marks as adult are hidden unless you turn them on in Config.
- Touchpad pointer (slide to move, press to click, two fingers to scroll), plus USB mouse and keyboard.
- A new icon, an intro with original music, and interface sounds.

**Mostly untested on a real PS5.** The interface has been seen running on a PS5; everything past the start screen was tested on a PC against the PC build of the background service, and the web sources against sample pages shaped like those sites. If something goes wrong, the Logs page shows the service's log (`/data/fpps5/fpps5.log`); please open an issue with it.

**You need**
- A PS5 that runs homebrew apps, with an ELF loader on port 9021 (etaHEN, or kstuff with elfldr). The app starts its background service through it.
- Ruffle Flash PS5 for Flash games and animations.

**Install:** unzip `PPSA72386.zip` and copy the `PPSA72386` folder to `/data/homebrew/` (or install it from a homebrew store).
