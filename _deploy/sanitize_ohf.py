#!/usr/bin/env python3
import io, os
paths = [
    r"C:\www\openholdembot_old\ScarletBeast_Test.ohf",
    r"C:\www\openholdembot_old\Release\ScarletBeast_Test.ohf",
    r"C:\www\openholdembot_old\Release - Optimized\ScarletBeast_Test.ohf",
]
# Map common smart/unicode punctuation to ASCII; anything else non-ASCII is dropped.
repl = {
    0x2014: "-", 0x2013: "-", 0x2012: "-", 0x2212: "-",   # dashes
    0x2018: "'", 0x2019: "'", 0x201A: "'",                 # single quotes
    0x201C: '"', 0x201D: '"', 0x201E: '"',                 # double quotes
    0x2026: "...",                                          # ellipsis
    0x00A0: " ",                                            # nbsp
    0x00BD: "1/2", 0x00BE: "3/4", 0x00BC: "1/4",            # fractions
}
for p in paths:
    if not os.path.exists(p):
        print(f"SKIP (missing): {p}"); continue
    s = io.open(p, encoding="utf-8", errors="replace").read()
    out = []
    removed = 0
    for ch in s:
        o = ord(ch)
        if o < 128:
            out.append(ch)
        elif o in repl:
            out.append(repl[o]); removed += 1
        else:
            removed += 1  # drop any other non-ASCII
    txt = "".join(out)
    # Write back as plain ASCII with CRLF (Windows-friendly for the parser).
    io.open(p, "w", encoding="ascii", newline="\r\n").write(txt)
    # Verify clean.
    bad = sum(1 for ch in txt if ord(ch) > 127)
    print(f"{p}: replaced/removed {removed} non-ASCII char(s), remaining non-ASCII = {bad}")
print("DONE")
