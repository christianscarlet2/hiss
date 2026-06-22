#!/usr/bin/env bash
# Copy the live BEAST OHF master to hiss-linux on swiftsnake, preserving the Omaha preflop stubs the
# headless build needs (it has no Omaha strategy files), then parse-check it there. Run after every OHF
# change (build_and_lint.py calls this; the PostToolUse coupling hook reminds you). [Emrald: copy OHF to
# hiss-server on change]
set -u
MASTER="${1:-/c/www/openholdembot_old/Release/ScarletBeast_PowerHoldem.ohf}"
REMOTE="asterisk@192.168.1.39"
[ -f "$MASTER" ] || { echo "[sync-ohf] master not found: $MASTER"; exit 1; }
scp -q -o ConnectTimeout=10 "$MASTER" "$REMOTE:/tmp/sb_master.ohf" 2>/dev/null || { echo "[sync-ohf] scp failed (swiftsnake unreachable?)"; exit 2; }
timeout 60 ssh -o ConnectTimeout=10 "$REMOTE" 'bash -s' <<'REOF'
F=/var/www/hiss-linux/strategy/ScarletBeast_linux.ohf
cp "$F" "$F.bak_$(date +%s)" 2>/dev/null
cp /tmp/sb_master.ohf "$F"
grep -q '##f\$preflop_plo8##' "$F" || printf '\n##f$preflop_plo8##\nWHEN Others Fold FORCE\n##f$preflop_omaha##\nWHEN Others Fold FORCE\n' >> "$F"
cd /var/www/hiss-linux
HISS_FORMULA=strategy/ScarletBeast_linux.ohf timeout 12 ./build/hiss 18793 >/tmp/parse_check.log 2>&1 &
P=$!; sleep 6; kill $P 2>/dev/null
ERR=$(grep -ciE 'parse error|unknown identif|error:' /tmp/parse_check.log)
echo "[sync-ohf] hiss-linux OHF = $(wc -c < "$F") bytes, parse errors = $ERR"
[ "$ERR" = "0" ] || grep -iE 'parse error|unknown identif|error:' /tmp/parse_check.log | head -6
REOF
