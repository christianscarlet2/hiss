#!/usr/bin/env python3
# Report a seat as bot-controlled (is_bot=true) when its human owner has a machine
# (Hiss / API) actively polling — so the felt shows a bot is playing, not a human.
import io
p = "/var/www/poker.scarletbeast.com/app/Services/TableManager.php"
s = io.open(p, encoding="utf-8").read()

if "bot-controlled" in s:
    print("already applied"); raise SystemExit

# 1) After computing $hand, build a per-seat bot-controlled map and override the
#    in-hand players' is_bot (the felt's bot badge reads hand.players[].is_bot).
anchor = (
    "        $hand = ($state && $state->state)\n"
    "            ? HandEngine::fromState($state->state)->view($seatNo)\n"
    "            : null;\n")
inject = anchor + (
    "\n"
    "        // A seat is \"bot-controlled\" if it's a house bot OR its human owner has a\n"
    "        // machine (Hiss / API) actively polling in the last 10s. Reflect that so the\n"
    "        // felt shows a bot is playing rather than a human.\n"
    "        $botCutoff = now()->subSeconds(10);\n"
    "        $botSeat = [];\n"
    "        foreach ($seats as $s) {\n"
    "            $botSeat[$s->seat_no] = $s->is_bot\n"
    "                || ($s->user && $s->user->bot_seen_at && $s->user->bot_seen_at->gt($botCutoff));\n"
    "        }\n"
    "        if ($hand && !empty($hand['players'])) {\n"
    "            foreach ($hand['players'] as $sn => &$pp) {\n"
    "                if (!empty($botSeat[$sn])) {\n"
    "                    $pp['is_bot'] = true;\n"
    "                }\n"
    "            }\n"
    "            unset($pp);\n"
    "        }\n")
if anchor not in s:
    print("ANCHOR (hand=) NOT FOUND"); raise SystemExit
s = s.replace(anchor, inject, 1)

# 2) Override seats[].is_bot too (used by player lists / lobby).
seat_old = "                'is_bot' => $s->is_bot,\n"
seat_new = "                'is_bot' => (bool) ($botSeat[$s->seat_no] ?? $s->is_bot),\n"
if seat_old not in s:
    print("ANCHOR (seats is_bot) NOT FOUND"); raise SystemExit
s = s.replace(seat_old, seat_new, 1)

io.open(p, "w", encoding="utf-8").write(s)
print("patched")
