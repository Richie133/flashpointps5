#!/usr/bin/env python3
"""Builds a small fake Flashpoint folder for testing Flashpoint PS5 on a PC.

It uses the launcher's real schema (FPA-Rust migrations), invented entries,
tiny generated SWFs, a GameZIP for each GameZIP entry (kept on a fake
"remote" download server so on-demand downloads get exercised), and logos.

usage: make-testdb.py <out dir>
  <out>/flashpoint   the Flashpoint folder (pass to fpps5 --root)
  <out>/remote       serve this with:  python3 -m http.server 8765 -d <out>/remote
"""
import hashlib
import io
import os
import random
import sqlite3
import struct
import sys
import uuid
import zipfile
from datetime import datetime, timedelta, timezone

SCHEMA = """
CREATE TABLE "tag_category" ("id" integer NOT NULL, "name" varchar NOT NULL COLLATE NOCASE, "color" varchar NOT NULL, "description" varchar, PRIMARY KEY("id" AUTOINCREMENT));
CREATE TABLE "tag_alias" ("id" integer NOT NULL, "tagId" integer, "name" varchar NOT NULL COLLATE NOCASE, PRIMARY KEY("id" AUTOINCREMENT));
CREATE TABLE "tag" ("id" integer NOT NULL, "dateModified" datetime NOT NULL DEFAULT (datetime('now')), "primaryAliasId" integer, "categoryId" integer, "description" varchar NOT NULL DEFAULT '', PRIMARY KEY("id" AUTOINCREMENT));
CREATE TABLE "additional_app" ("id" varchar NOT NULL, "applicationPath" varchar NOT NULL, "autoRunBefore" boolean NOT NULL, "launchCommand" varchar NOT NULL, "name" varchar NOT NULL COLLATE NOCASE, "waitForExit" boolean NOT NULL, "parentGameId" varchar, PRIMARY KEY("id"));
CREATE TABLE "game_tags_tag" ("gameId" varchar NOT NULL, "tagId" integer NOT NULL, PRIMARY KEY("gameId","tagId"));
CREATE TABLE "game" ("id" varchar NOT NULL, "parentGameId" varchar, "title" varchar NOT NULL, "alternateTitles" varchar NOT NULL, "series" varchar NOT NULL, "developer" varchar NOT NULL, "publisher" varchar NOT NULL, "dateAdded" datetime NOT NULL, "dateModified" datetime NOT NULL DEFAULT (datetime('now')), "broken" boolean NOT NULL, "extreme" boolean NOT NULL, "playMode" varchar NOT NULL, "status" varchar NOT NULL, "notes" varchar NOT NULL, "source" varchar NOT NULL, "applicationPath" varchar NOT NULL, "launchCommand" varchar NOT NULL, "releaseDate" varchar NOT NULL, "version" varchar NOT NULL, "originalDescription" varchar NOT NULL, "language" varchar NOT NULL, "library" varchar NOT NULL, "orderTitle" varchar NOT NULL, "activeDataId" integer, "activeDataOnDisk" boolean NOT NULL DEFAULT (0), "tagsStr" varchar NOT NULL DEFAULT ('') COLLATE NOCASE, "platformsStr" varchar, "platformId" integer, "lastPlayed" datetime, "playtime" integer DEFAULT 0, "playCounter" integer DEFAULT 0, "archiveState" integer DEFAULT 2, "activeGameConfigId" integer, "activeGameConfigOwner" varchar COLLATE NOCASE, "platformName" varchar COLLATE NOCASE, "ruffleSupport" varchar NOT NULL DEFAULT '', "logoPath" varchar, "screenshotPath" varchar, "owner" varchar, PRIMARY KEY("id"));
CREATE TABLE "game_data" ("id" integer NOT NULL, "gameId" varchar, "title" varchar NOT NULL, "dateAdded" datetime NOT NULL, "sha256" varchar NOT NULL, "crc32" integer NOT NULL, "presentOnDisk" boolean NOT NULL DEFAULT (0), "path" varchar, "size" integer NOT NULL, "parameters" varchar, "applicationPath" varchar, "launchCommand" varchar, PRIMARY KEY("id" AUTOINCREMENT), CONSTRAINT "UQ_gameid_dateadded" UNIQUE("gameId", "dateAdded"));
CREATE TABLE "platform_alias" ("id" integer NOT NULL, "platformId" integer, "name" varchar NOT NULL COLLATE NOCASE, PRIMARY KEY("id" AUTOINCREMENT));
CREATE TABLE "platform" ("id" integer NOT NULL, "dateModified" datetime NOT NULL DEFAULT (datetime('now')), "primaryAliasId" integer, "description" varchar NOT NULL DEFAULT '', PRIMARY KEY("id" AUTOINCREMENT));
CREATE TABLE "game_platforms_platform" ("gameId" varchar NOT NULL, "platformId" integer NOT NULL, PRIMARY KEY("gameId","platformId"));
CREATE INDEX "IDX_gameTitle" ON "game" ("title");
"""

FLASH_AP = "FPSoftware\\Flash\\flashplayer_32_sa.exe"
BROWSER_AP = "FPSoftware\\FPNavigator\\fpnavigator.exe"


def swf(color, width=550, height=400):
    """A valid one-frame SWF with a background colour."""
    nbits = 16
    bits = f"{nbits:05b}" + "".join(format(v & ((1 << nbits) - 1), f"0{nbits}b") for v in (0, width * 20, 0, height * 20))
    bits += "0" * (-len(bits) % 8)
    rect = int(bits, 2).to_bytes(len(bits) // 8, "big")
    body = rect + struct.pack("<HH", 24 << 8, 1)
    body += struct.pack("<H", (9 << 6) | 3) + bytes(color)  # SetBackgroundColor
    body += struct.pack("<H", 1 << 6)  # ShowFrame
    body += struct.pack("<H", 0)  # End
    return b"FWS" + bytes([10]) + struct.pack("<I", 8 + len(body)) + body


HTML5_GAME = """<!doctype html><html><head><meta charset="utf-8"><title>Lantern Drift</title>
<style>html,body{margin:0;background:#101a33;height:100%}canvas{display:block;margin:auto}</style></head>
<body><canvas id="c" width="640" height="400"></canvas><script src="game.js"></script></body></html>"""
HTML5_JS = """const c=document.getElementById('c').getContext('2d');let t=0;
function f(){t+=0.02;c.fillStyle='#101a33';c.fillRect(0,0,640,400);
for(let i=0;i<12;i++){const x=320+Math.cos(t+i)*200,y=200+Math.sin(t*1.3+i)*120;
c.fillStyle='hsl('+(i*30)+',80%,65%)';c.beginPath();c.arc(x,y,14,0,7);c.fill();}
c.fillStyle='#fff';c.font='24px sans-serif';c.fillText('Lantern Drift',240,40);requestAnimationFrame(f);}f();"""

WORDS_A = ["Orbit", "Mossy", "Paper", "Neon", "Clockwork", "Pocket", "Velvet", "Rusty", "Cosmic", "Tiny",
           "Hollow", "Bubble", "Thunder", "Pixel", "Lunar", "Copper", "Frosty", "Wobbly", "Secret", "Turbo"]
WORDS_B = ["Courier", "Tunnels", "Kingdom", "Defender", "Garden", "Heist", "Racer", "Lighthouse", "Factory",
           "Sprint", "Puzzle", "Island", "Dungeon", "Bakery", "Circus", "Rescue", "Tower", "Voyage", "Arcade", "Monsters"]
DEVS = ["Kettle Bros", "Saffron Labs", "Mintbox", "Yellowcake", "Pip & Co", "Orrery Games", "Small Hours", "Glasshouse"]
TAGS = ["Puzzle", "Platformer", "Action", "Arcade", "Point and Click", "Racing", "Strategy", "Comedy", "Cartoon", "Music"]
PLATFORMS = ["Flash"] * 12 + ["HTML5"] * 3 + ["Shockwave"] * 2 + ["Unity", "Java"]
PALETTE = [(0xdd, 0x04, 0x28), (0x2f, 0x6f, 0xd8), (0x23, 0x9b, 0x6b), (0xe8, 0x9a, 0x1c), (0x8e, 0x44, 0xc9),
           (0x16, 0x8f, 0xa3), (0xcf, 0x5a, 0x9b), (0x55, 0x6b, 0x2f)]


def logo(path, title, color, seed):
    from PIL import Image, ImageDraw, ImageFont
    rnd = random.Random(seed)
    img = Image.new("RGB", (256, 256), color)
    d = ImageDraw.Draw(img)
    for _ in range(6):
        x, y, r = rnd.randint(0, 256), rnd.randint(0, 256), rnd.randint(20, 90)
        shade = tuple(min(255, c + rnd.randint(20, 70)) for c in color)
        d.ellipse((x - r, y - r, x + r, y + r), fill=shade)
    try:
        font = ImageFont.truetype("DejaVuSans-Bold.ttf", 26)
    except OSError:
        font = ImageFont.load_default()
    words = title.split()
    lines = [" ".join(words[: len(words) // 2 or 1]), " ".join(words[len(words) // 2 or 1:])]
    y = 150
    for line in lines:
        if line:
            d.text((14, y), line, fill="white", font=font, stroke_width=2, stroke_fill=(20, 20, 30))
            y += 32
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path)


def img_rel(kind, gid):
    return f"{kind}/{gid[0:2]}/{gid[2:4]}/{gid}.png"


def main():
    out = sys.argv[1]
    fp = os.path.join(out, "flashpoint")
    remote = os.path.join(out, "remote")
    for p in ["Data/Games", "Data/Images", "Legacy/htdocs"]:
        os.makedirs(os.path.join(fp, p), exist_ok=True)
    for p in ["Games", "Images", "htdocs"]:
        os.makedirs(os.path.join(remote, p), exist_ok=True)
    db_path = os.path.join(fp, "Data", "flashpoint.sqlite")
    if os.path.exists(db_path):
        os.remove(db_path)
    db = sqlite3.connect(db_path)
    db.executescript(SCHEMA)

    plat_ids = {}
    for name in sorted(set(PLATFORMS) | {"Silverlight"}):
        cur = db.execute("INSERT INTO platform (description) VALUES ('')")
        pid = cur.lastrowid
        aid = db.execute("INSERT INTO platform_alias (platformId, name) VALUES (?, ?)", (pid, name)).lastrowid
        db.execute("UPDATE platform SET primaryAliasId=? WHERE id=?", (aid, pid))
        plat_ids[name] = pid
    tag_ids = {}
    for name in TAGS + ["Extreme"]:
        tid = db.execute("INSERT INTO tag (description) VALUES ('')").lastrowid
        aid = db.execute("INSERT INTO tag_alias (tagId, name) VALUES (?, ?)", (tid, name)).lastrowid
        db.execute("UPDATE tag SET primaryAliasId=? WHERE id=?", (aid, tid))
        tag_ids[name] = tid

    rnd = random.Random(7)
    base_date = datetime(2019, 1, 1, tzinfo=timezone.utc)
    made = []

    def add_game(title, platform, ap, lc, library="arcade", tags=None, zip_files=None, extreme=False,
                 desc="", dev=None, notes="", logo_local=True, legacy_files=None, add_apps=None, gid=None):
        gid = gid or str(uuid.UUID(int=rnd.getrandbits(128), version=4))
        tags = tags or rnd.sample(TAGS, 2)
        if extreme:
            tags = tags + ["Extreme"]
        added = base_date + timedelta(days=rnd.randint(0, 2000), seconds=rnd.randint(0, 86400),
                                      microseconds=rnd.randint(0, 999) * 1000)
        added_s = added.strftime("%Y-%m-%d %H:%M:%S.") + f"{added.microsecond // 1000:03d}"
        dev = dev or rnd.choice(DEVS)
        active = None
        if zip_files is not None:
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
                for name, data in zip_files.items():
                    z.writestr("content/" + name, data)
                z.writestr("meta.yaml", f"Title: {title}\n")
            data = buf.getvalue()
            ms = int(added.replace(microsecond=0).timestamp()) * 1000 + added.microsecond // 1000
            fname = f"{gid}-{ms}.zip"
            with open(os.path.join(remote, "Games", fname), "wb") as f:
                f.write(data)
            cur = db.execute(
                "INSERT INTO game_data (gameId, title, dateAdded, sha256, crc32, presentOnDisk, path, size,"
                " parameters, applicationPath, launchCommand) VALUES (?,?,?,?,?,0,NULL,?,NULL,?,?)",
                (gid, "Data", added_s, hashlib.sha256(data).hexdigest().upper(), 0, len(data), ap, lc))
            active = cur.lastrowid
            g_ap, g_lc = "", ""
        else:
            g_ap, g_lc = ap, lc
        for rel, content in (legacy_files or {}).items():
            path = os.path.join(fp, "Legacy", "htdocs", rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(content)
        db.execute(
            "INSERT INTO game (id, title, alternateTitles, series, developer, publisher, dateAdded, dateModified,"
            " broken, extreme, playMode, status, notes, source, applicationPath, launchCommand, releaseDate,"
            " version, originalDescription, language, library, orderTitle, activeDataId, activeDataOnDisk,"
            " tagsStr, platformsStr, platformName, logoPath, screenshotPath, owner)"
            " VALUES (?,?,?,?,?,?,?,?,0,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,?,?,?,?,?,'flashpoint-archive')",
            (gid, title, "", "", dev, dev if rnd.random() < 0.5 else "", added_s, added_s, int(extreme),
             "Single Player", "Playable", notes, f"https://{dev.split()[0].lower()}.example.com/", g_ap, g_lc,
             str(2003 + rnd.randint(0, 12)), "", desc, "en", library, title.lower(), active,
             "; ".join(tags), platform, platform, img_rel("Logos", gid), img_rel("Screenshots", gid)))
        db.execute("INSERT INTO game_platforms_platform VALUES (?, ?)", (gid, plat_ids[platform]))
        for t in tags:
            db.execute("INSERT OR IGNORE INTO game_tags_tag VALUES (?, ?)", (gid, tag_ids[t]))
        for i, (name, a_ap, a_lc) in enumerate(add_apps or []):
            db.execute("INSERT INTO additional_app VALUES (?,?,0,?,?,0,?)", (str(uuid.uuid4()), a_ap, a_lc, name, gid))
        color = PALETTE[len(made) % len(PALETTE)]
        # some logos are local, the rest only on the fake image server
        target = os.path.join(fp if logo_local else remote, "Data/Images" if logo_local else "Images",
                              img_rel("Logos", gid))
        logo(target, title, color, gid)
        if len(made) < 12:
            logo(os.path.join(remote, "Images", img_rel("Screenshots", gid)), title + " screenshot",
                 tuple(c // 2 for c in color), gid + "s")
        made.append((gid, title, platform))
        return gid

    # The entries the tests use
    add_game("Orbit Courier", "Flash", FLASH_AP, "http://www.orbitcourier.example.com/play/courier.swf?mode=story",
             tags=["Arcade", "Action"], gid="0a1b2c3d-1111-4aaa-8bbb-000000000001",
             desc="Deliver parcels between moons before your fuel runs out.",
             zip_files={"www.orbitcourier.example.com/play/courier.swf": swf((0x22, 0x44, 0x99)),
                        "www.orbitcourier.example.com/play/levels.xml": b"<levels><level n='1'/></levels>",
                        "www.orbitcourier.example.com/play/music/theme.mp3": b"ID3fake"},
             add_apps=[("Level editor", FLASH_AP, "http://www.orbitcourier.example.com/play/editor.swf"),
                       ("Read the manual", ":message:", "Use the arrow keys to steer and space to drop a parcel.")])
    add_game("Lantern Drift", "HTML5", BROWSER_AP, "http://games.lanterndrift.example.com/drift/index.html",
             tags=["Puzzle", "Music"], gid="0a1b2c3d-2222-4aaa-8bbb-000000000002",
             desc="Guide paper lanterns across a night river.",
             zip_files={"games.lanterndrift.example.com/drift/index.html": HTML5_GAME,
                        "games.lanterndrift.example.com/drift/game.js": HTML5_JS})
    add_game("Mossy Tunnels Classic", "Flash", FLASH_AP, "http://legacy.mossy.example.com/flash/mossy.swf",
             tags=["Platformer"], gid="0a1b2c3d-3333-4aaa-8bbb-000000000003",
             desc="An old Legacy entry: its files live in htdocs, not a GameZIP.",
             legacy_files={"legacy.mossy.example.com/flash/mossy.swf": swf((0x33, 0x88, 0x44)),
                           "legacy.mossy.example.com/flash/level1.txt": b"level=1"})
    add_game("Velvet Circus", "Shockwave", "FPSoftware\\Shockwave\\PJ101\\SPR.exe",
             "http://www.velvetcircus.example.com/circus.dcr", tags=["Comedy"],
             gid="0a1b2c3d-4444-4aaa-8bbb-000000000004", desc="Needs the Shockwave plugin.",
             zip_files={"www.velvetcircus.example.com/circus.dcr": b"XFIR"})
    add_game("Copper Lighthouse", "Flash", FLASH_AP, "http://www.copper.example.com/lighthouse.swf",
             library="theatre", tags=["Cartoon"], gid="0a1b2c3d-5555-4aaa-8bbb-000000000005",
             desc="A short animation, fetched from the Legacy server on demand.", logo_local=False)
    os.makedirs(os.path.join(remote, "htdocs", "www.copper.example.com"), exist_ok=True)
    with open(os.path.join(remote, "htdocs", "www.copper.example.com", "lighthouse.swf"), "wb") as f:
        f.write(swf((0xaa, 0x66, 0x22)))

    # Filler for paging and stats
    for i in range(260):
        title = f"{rnd.choice(WORDS_A)} {rnd.choice(WORDS_B)}" + (f" {rnd.randint(2, 4)}" if rnd.random() < 0.2 else "")
        platform = rnd.choice(PLATFORMS)
        host = f"www.{title.split()[0].lower()}{i}.example.com"
        if platform == "Flash":
            lc, ap = f"http://{host}/game.swf", FLASH_AP
            files = {f"{host}/game.swf": swf(PALETTE[i % len(PALETTE)])}
        elif platform == "HTML5":
            lc, ap = f"http://{host}/index.html", BROWSER_AP
            files = {f"{host}/index.html": HTML5_GAME, f"{host}/game.js": HTML5_JS}
        else:
            lc, ap = f"http://{host}/game.bin", "FPSoftware\\Other\\player.exe"
            files = {f"{host}/game.bin": b"\0"}
        add_game(title, platform, ap, lc, library="theatre" if rnd.random() < 0.15 else "arcade",
                 extreme=rnd.random() < 0.05, zip_files=files, logo_local=rnd.random() < 0.6,
                 desc="Invented test entry.")
    db.commit()
    db.close()
    print(f"{len(made)} entries in {db_path}")
    print(f"serve the fake download server with: python3 -m http.server 8765 -d {remote}")


if __name__ == "__main__":
    main()
