#!/usr/bin/env bash
# ----------------------------------------------------------------------------
# Lobby info-fetch choreography for the Hiss poker bot.
#
#   goto_lobby_button -> delay -> [capture full-window frame: parse pt 1]
#   -> delay -> lobby_more_info_button -> delay -> [capture: parse pt 2]
#   -> delay -> leave_lobby_button -> delay -> return_to_tables_button
#   ...now back in the game.
#
# The clicks go through /api/click-region (applied on the bot's next heartbeat).
# Parsing is done by CLAUDE reading the captured full-window frames afterward
# (Claude vision on the whole scrape, NOT OCR) and logging/ wiring the result.
#
# PREREQUISITE: the regions goto_lobby_button / lobby_more_info_button /
# leave_lobby_button / return_to_tables_button must be PLACED in the tablemap
# (drawn in Vision over the real buttons). Until then click-region is a no-op.
#
# DELAY is a magic number (2.5s) for now -- tune once we see real timing.
# ----------------------------------------------------------------------------
PORT="${1:-27654}"
DELAY="${2:-3.5}"
FRAMES="/c/www/openholdembot_old/Release/logs/frames"

capture() {  # capture the newest heartbeat frame -> C:/tmp/$1.png  (full window)
  local f; f=$(ls -t "$FRAMES"/*.bmp 2>/dev/null | head -1)
  [ -z "$f" ] && { echo "  !! no frames (Hiss not capturing)"; return 1; }
  python -c "from PIL import Image; Image.open(r'C:/www/openholdembot_old/Release/logs/frames/$(basename "$f")').save(r'C:/tmp/$1.png')" \
    && echo "  captured C:/tmp/$1.png"
}
click() {  # queue a region click; report whether the region exists
  local r; r=$(curl -s -m 4 "http://127.0.0.1:$PORT/api/click-region?name=$1")
  echo "  click $1 -> $r"
}

echo "[lobby_fetch] start (port=$PORT delay=${DELAY}s)"
click goto_lobby_button;        sleep "$DELAY"
capture lobby_main                                  # PARSE POINT 1: lobby screen
sleep "$DELAY"
click lobby_more_info_button;   sleep "$DELAY"
capture lobby_moreinfo                              # PARSE POINT 2: more-info screen
sleep "$DELAY"
click leave_lobby_button;       sleep "$DELAY"
click return_to_tables_button
echo "[lobby_fetch] done. Parse: C:/tmp/lobby_main.png + C:/tmp/lobby_moreinfo.png"
