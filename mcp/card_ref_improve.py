#!/usr/bin/env python3
"""AIL card-recognition improver: read the HERO's hole cards with Claude off the BEAST all-frames
store and rebuild the tm_images image-transform references so the bot stops misreading them.

WHY: the hero's 25x25 rank/suit references on android_9max_omaha (id=267) are poor -- every card
collapses to the same wrong label (duplicate misreads like Kd 7d Kd 7d), so the bot can't read its
own Omaha hand. Inserting an EXACT crop of each real card (labelled by Claude) gives a diff~0 match.
PNG->PIL gives true R,G,B == the C++ GetDIBits format ("ff"+RRGGBB per pixel, 25 rows joined by \\n).

USAGE (two-step, Claude in the loop for the labels):
  python mcp/card_ref_improve.py crop <hand>
        -> finds the face-up hero frame, saves the 4 rank + 4 suit 25x25 crops (upscaled) to
           C:/tmp/cardref_<hand>/ for Claude to read, and prints the region map.
  python mcp/card_ref_improve.py insert <hand> <c0> <c1> <c2> <c3>
        -> each c<i> is rank+suit e.g. "2d" "8s" "5c" "6s" (or "--" to skip a card). Re-finds the
           same frame, crops, and inserts tm_images refs (rank + suit) with those labels; bumps
           tablemaps.updated_at for a live reload.

Hero card region coords on id=267 (25x25): rank y=693, suit y=715; card x = 240/276/312/348.
"""
import sys, os, psycopg2
from PIL import Image

DSN = "host=%s port=%s dbname=%s user=%s password=%s" % (
    os.environ.get("PGHOST", "127.0.0.1"), os.environ.get("PGPORT", "5432"),
    os.environ.get("PGDATABASE", "hiss"), os.environ.get("PGUSER", "postgres"),
    os.environ.get("PGPASSWORD", "dbpass"))
CARD_X = [240, 276, 312, 348]      # left edge of each hero card's 25x25 rank/suit region
RANK_Y, SUIT_Y = 693, 715
TID = 267


def conn():
    c = psycopg2.connect(DSN); c.autocommit = True; return c


def find_faceup_frame(cur, hand):
    """Pick the on-disk frame for this hand with the most card-face (white) pixels across ALL 4
    hero rank regions -> the fully-dealt, face-up moment."""
    cur.execute("SELECT png_path FROM hiss_log_frames WHERE handnumber=%s ORDER BY ts_ms", (hand,))
    paths = [r[0] for r in cur.fetchall() if r[0] and os.path.isfile(r[0])]
    best = None
    for p in paths:
        try:
            im = Image.open(p).convert("RGB"); px = im.load()
            # require white in EVERY card slot (all 4 dealt + face-up), score = min over slots
            mins = []
            for x in CARD_X:
                w = sum(1 for dx in range(0, 25, 2) for dy in range(0, 25, 2)
                        if min(px[x + dx, RANK_Y + dy]) > 175)
                mins.append(w)
            score = min(mins)
            if best is None or score > best[0]:
                best = (score, p)
        except Exception:
            pass
    return best[1] if best else None


def encode(px, l, t):
    return "\n".join("".join("ff%02x%02x%02x" % px[l + dx, t + dy] for dx in range(25))
                     for dy in range(25))


def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    mode, hand = sys.argv[1], sys.argv[2]
    c = conn(); cur = c.cursor()
    frame = find_faceup_frame(cur, hand)
    if not frame:
        print("no face-up hero frame found for hand", hand); return 1
    print("face-up frame:", frame)
    im = Image.open(frame).convert("RGB"); px = im.load()

    if mode == "crop":
        outdir = r"C:\tmp\cardref_%s" % hand
        os.makedirs(outdir, exist_ok=True)
        # one strip of the 4 cards (rank+suit) upscaled for Claude to read
        strip = im.crop((CARD_X[0] - 4, RANK_Y - 6, CARD_X[3] + 29, SUIT_Y + 31))
        strip = strip.resize((strip.size[0] * 4, strip.size[1] * 4), Image.LANCZOS)
        sp = os.path.join(outdir, "hero_strip.png"); strip.save(sp)
        print("saved %s  -> read the 4 cards, then run: insert %s <c0> <c1> <c2> <c3>" % (sp, hand))
        return 0

    if mode == "insert":
        labels = sys.argv[3:7]
        if len(labels) < 4:
            print("need 4 labels e.g. insert", hand, "2d 8s 5c 6s"); return 2
        n = 0
        for i, lab in enumerate(labels):
            if lab in ("--", "", "?"):
                continue
            rk, st = lab[0].upper().replace("10", "T"), lab[1].lower()
            x = CARD_X[i]
            cur.execute("INSERT INTO tm_images(tablemap_id,name,width,height,pixels) VALUES(%s,%s,25,25,%s)",
                        (TID, rk, encode(px, x, RANK_Y)))
            cur.execute("INSERT INTO tm_images(tablemap_id,name,width,height,pixels) VALUES(%s,%s,25,25,%s)",
                        (TID, st, encode(px, x, SUIT_Y)))
            n += 2
            print("  card%d -> rank '%s' + suit '%s' inserted" % (i, rk, st))
        cur.execute("UPDATE tablemaps SET updated_at=now() WHERE id=%s", (TID,))
        print("inserted %d refs; revision bumped (live reload)." % n)
        return 0
    print(__doc__); return 2


if __name__ == "__main__":
    sys.exit(main())
