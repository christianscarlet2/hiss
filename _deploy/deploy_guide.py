#!/usr/bin/env python3
# Wire the bot-profile guide into the poker site: add routes + a portal link.
import io, sys
base = "/var/www/poker.scarletbeast.com"

# 1) routes/web.php — add the two routes right after the /dev route.
rp = base + "/routes/web.php"
s = io.open(rp, encoding="utf-8").read()
if "developers/bot-profile" not in s:
    anchor = "Route::get('/dev', $developers);"
    add = (anchor
           + "\nRoute::view('/developers/bot-profile', 'bot-profile');"
           + "\nRoute::view('/bot-profile', 'bot-profile');")
    s = s.replace(anchor, add, 1)
    io.open(rp, "w", encoding="utf-8").write(s)
    print("routes/web.php: routes added")
else:
    print("routes/web.php: already present")

# 2) developers.blade.php — add a "Write a Bot Profile" button next to API Docs.
dp = base + "/resources/views/developers.blade.php"
d = io.open(dp, encoding="utf-8").read()
if "developers/bot-profile" not in d:
    old = '<a class="btn" href="/api-docs">Read the API Docs &rarr;</a>'
    old_real = '<a class="btn" href="/api-docs">Read the API Docs →</a>'
    repl = ('<a class="btn" href="/api-docs">Read the API Docs →</a> '
            '<a class="btn ghost" href="/developers/bot-profile" '
            'style="margin-top:8px">Write a Bot Profile →</a>')
    if old_real in d:
        d = d.replace(old_real, repl, 1)
    elif old in d:
        d = d.replace(old, repl, 1)
    else:
        print("developers.blade.php: anchor not found (link NOT added)")
    io.open(dp, "w", encoding="utf-8").write(d)
    print("developers.blade.php: link added")
else:
    print("developers.blade.php: already linked")
print("DONE")
