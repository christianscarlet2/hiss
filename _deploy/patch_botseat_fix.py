#!/usr/bin/env python3
# Fix the earlier mis-placed edit: the botSeat-aware is_bot landed in
# startHandLocked() (no $botSeat there) instead of viewFor()'s seats map.
import io
p = "/var/www/poker.scarletbeast.com/app/Services/TableManager.php"
s = io.open(p, encoding="utf-8").read()

# 1) Revert the stray usage (undefined $botSeat in startHandLocked) back to plain.
stray = "'is_bot' => (bool) ($botSeat[$s->seat_no] ?? $s->is_bot),"
n_stray = s.count(stray)
if n_stray >= 1:
    s = s.replace(stray, "'is_bot' => $s->is_bot,", 1)  # only the first (startHandLocked)
    print(f"reverted stray botSeat usage (had {n_stray})")
else:
    print("no stray botSeat usage found")

# 2) Apply botSeat to viewFor()'s seats map (uniquely identified by $s->user?->...).
old = ("                'name' => $s->user?->username ?? $s->user?->name,\n"
       "                'avatar' => $s->user?->avatar,\n"
       "                'is_bot' => $s->is_bot,\n")
new = ("                'name' => $s->user?->username ?? $s->user?->name,\n"
       "                'avatar' => $s->user?->avatar,\n"
       "                'is_bot' => (bool) ($botSeat[$s->seat_no] ?? $s->is_bot),\n")
if new in s:
    print("viewFor seats map: already botSeat-aware")
elif old in s:
    s = s.replace(old, new, 1)
    print("viewFor seats map: patched")
else:
    print("viewFor seats map: ANCHOR NOT FOUND")

io.open(p, "w", encoding="utf-8").write(s)
print("DONE")
