First release of Flashpoint PS5 as a native PS5 app (PPSA72386).

Browse a Flashpoint Archive library on the TV with a controller, search it, and play Flash games in Ruffle Flash PS5. Game data downloads from Flashpoint's servers the first time you play each game.

**Not tested on a real PS5 yet.** The whole app runs on a PC against the PC build of its service; the console-specific parts follow ps5-native-app-boilerplate, PayLoader and the PS5 Payload SDK closely. If something goes wrong, please open an issue with `/data/fpps5/fpps5.log`.

### You need
- A PS5 that runs homebrew apps, with an ELF loader on port 9021 (etaHEN, or kstuff with elfldr). The app starts its background service through it.
- `Data/flashpoint.sqlite` from Flashpoint Infinity, copied to `/data/flashpoint/Data/` or a USB drive's `flashpoint/Data/`.
- Ruffle Flash PS5 for Flash games.

### Install
Unzip `PPSA72386.zip` and copy the `PPSA72386` folder to `/data/homebrew/`.
