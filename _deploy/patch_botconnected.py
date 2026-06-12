#!/usr/bin/env python3
# Wire up "a bot is connected to my seat" across the poker site:
#  * BotToken stamps users.bot_seen_at on every machine request
#  * viewFor exposes you.bot_connected (bot seen in the last 10s)
#  * the React ActionBar hides the human controls and shows a notice
import io, sys
base = "/var/www/poker.scarletbeast.com"

def patch(path, olds, new, label, required=True):
    p = base + "/" + path
    s = io.open(p, encoding="utf-8").read()
    if new in s:
        print(f"{label}: already applied"); return
    for old in olds:
        if old in s:
            s = s.replace(old, new, 1)
            io.open(p, "w", encoding="utf-8").write(s)
            print(f"{label}: patched"); return
    print(f"{label}: ANCHOR NOT FOUND" + ("  (REQUIRED)" if required else ""))

# 1) User model — cast the new column.
patch("app/Models/User.php",
      ["'last_seen_at' => 'datetime',"],
      "'last_seen_at' => 'datetime',\n            'bot_seen_at' => 'datetime',",
      "User casts")

# 2) BotToken middleware — stamp bot_seen_at alongside last_seen_at.
patch("app/Http/Middleware/BotToken.php",
      ["$user->forceFill(['last_seen_at' => now()])->saveQuietly();"],
      "$user->forceFill(['last_seen_at' => now(), 'bot_seen_at' => now()])->saveQuietly();",
      "BotToken stamp")

# 3) viewFor — expose you.bot_connected.
patch("app/Services/TableManager.php",
      ["'seat_no' => $seatNo,\n                'chips' => $user->chips,"],
      "'seat_no' => $seatNo,\n                'chips' => $user->chips,\n"
      "                'bot_connected' => (bool) ($user->bot_seen_at "
      "&& $user->bot_seen_at->gt(now()->subSeconds(10))),",
      "viewFor bot_connected")

# 4) Felt.jsx ActionBar — gate the controls.
fjsx = "resources/js/poker/Felt.jsx"
s = io.open(base + "/" + fjsx, encoding="utf-8").read()
if "you.bot_connected" not in s:
    # 4a) compute botOn
    s = s.replace(
        "  const bbc = state.table.bb;\n",
        "  const bbc = state.table.bb;\n  const botOn = !!(you && you.bot_connected);\n", 1)
    # 4b) don't let hotkeys fire while the bot is driving
    s = s.replace(
        "if (skin !== 'desktop' || !myTurn || busy) return;",
        "if (skin !== 'desktop' || !myTurn || busy || botOn) return;", 1)
    # 4c) bot-connected takes over the action bar
    s = s.replace(
        "  if (!myTurn) {\n",
        "  if (botOn) {\n"
        "    return (\n"
        "      <div className=\"actbar\">\n"
        "        <div className=\"center-msg bot-on\" style={{ padding: '10px', display: 'flex', alignItems: 'center', gap: 8, justifyContent: 'center' }}>\n"
        "          <span style={{ fontSize: 18 }}>\U0001F916</span>\n"
        "          <span>{'Bot connected — it’s playing your seat'}{myTurn ? ' (acting…)' : ''}.</span>\n"
        "        </div>\n"
        "      </div>\n"
        "    );\n"
        "  }\n\n"
        "  if (!myTurn) {\n", 1)
    io.open(base + "/" + fjsx, "w", encoding="utf-8").write(s)
    print("Felt.jsx ActionBar: patched")
else:
    print("Felt.jsx ActionBar: already applied")

print("DONE")
