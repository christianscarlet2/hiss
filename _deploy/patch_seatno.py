#!/usr/bin/env python3
# Fix: viewFor() derived the caller's seat from the CURRENT HAND's players, which
# excludes busted / sitting-out players (they aren't dealt in). That made
# you.seat_no null right when a bot needs it to rebuy. Derive it from the seat
# table instead, so a seated-but-sitting-out player still reports their seat.
import io
p = "/var/www/poker.scarletbeast.com/app/Services/TableManager.php"
s = io.open(p, encoding="utf-8").read()

old = (
    "        $seatNo = null;\n"
    "        if ($user && $state && $state->state) {\n"
    "            foreach ($state->state['players'] as $sn => $p) {\n"
    "                if (($p['user_id'] ?? null) === $user->id) {\n"
    "                    $seatNo = (int) $sn;\n"
    "                }\n"
    "            }\n"
    "        }\n")
new = (
    "        // Derive the caller's seat from the SEAT table (works even when they are\n"
    "        // sitting out / busted and thus absent from the current hand's players).\n"
    "        $seatNo = null;\n"
    "        if ($user) {\n"
    "            foreach ($seats as $s) {\n"
    "                if ($s->user_id === $user->id && $s->status !== 'empty') {\n"
    "                    $seatNo = (int) $s->seat_no;\n"
    "                    break;\n"
    "                }\n"
    "            }\n"
    "        }\n")

if new in s:
    print("already applied")
elif old in s:
    s = s.replace(old, new, 1)
    io.open(p, "w", encoding="utf-8").write(s)
    print("patched")
else:
    print("ANCHOR NOT FOUND")
