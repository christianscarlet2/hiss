#!/usr/bin/env python3
# Concatenate strategy segments into one master .ohf and lint it against the
# OpenPPL grammar + the symbol set provided by the library and the engine.
import os, re, glob, sys

ROOT = r"C:\www\openholdembot_old"
SEGDIR = os.path.join(ROOT, ".strategy_build", "strategy")
LIBDIR = os.path.join(ROOT, "Hiss", "bot_logic", "OpenPPL_Library")
# Output path for the concatenated master. Defaults to the LIVE Release master Hiss parses, but can be
# redirected via HISS_MASTER_OUT to lint/stage WITHOUT clobbering the running bot's OHF (the live master
# overwrite is what popped Parse Error modals mid-session when the staged OHF referenced new engine
# symbols the running binary lacked). See memory: lint-clobbers-live-master.
MASTER = os.environ.get("HISS_MASTER_OUT") or os.path.join(ROOT, "Release", "ScarletBeast_PowerHoldem.ohf")

# ---- 1. concatenate segments in filename order -----------------------------
segs = sorted(glob.glob(os.path.join(SEGDIR, "*.ohf")))
parts = []
for s in segs:
    parts.append(open(s, encoding="utf-8", errors="replace").read().rstrip() + "\n")
master_text = "\n".join(parts) + "\n"
os.makedirs(os.path.dirname(MASTER), exist_ok=True)
open(MASTER, "w", encoding="utf-8").write(master_text)
print(f"[build] wrote {MASTER} ({len(master_text)} bytes from {len(segs)} segments)")

# ---- 2. collect defined headers in our strategy ----------------------------
hdr_re = re.compile(r'^##([^#]+)##\s*$')
defined = []
for line in master_text.splitlines():
    m = hdr_re.match(line.strip())
    if m:
        defined.append(m.group(1).strip())
defined_set = set(defined)

# Headers defined in the LOADED bot_logic strategy trees (incl. Strategy\Omaha\ and \Omaha8\). Hiss loads
# these ALONGSIDE the master at runtime (one shared function collection), so f$ references from the master
# -- e.g. the game-type dispatch "WHEN isomaha RETURN f$preflop_omaha" -- resolve at runtime. Without this
# the lint linted the master ALONE and false-positived f$preflop_omaha/f$flop_omaha as undefined, which
# BLOCKED every .ohf edit. [2026-06-20]
BOTLOGIC = os.path.join(ROOT, "Release", "bot_logic", "Strategy")
for f in glob.glob(os.path.join(BOTLOGIC, "**", "*.ohf"), recursive=True):
    for line in open(f, encoding="utf-8", errors="replace"):
        m = hdr_re.match(line.strip())
        if m:
            defined_set.add(m.group(1).strip())

# ---- 3. build the valid-symbol whitelist -----------------------------------
lib_syms = set()
for f in glob.glob(os.path.join(LIBDIR, "*.ohf")):
    for line in open(f, encoding="utf-8", errors="replace"):
        m = hdr_re.match(line.strip())
        if m:
            name = m.group(1).strip()
            lib_syms.add(name)
            # library defines HoldEm_-prefixed names; the multiplexer exposes the
            # de-prefixed public name (HaveTopPair) and the suffix too.
            for pfx in ("HoldEm_", "Omaha_"):
                if name.startswith(pfx):
                    lib_syms.add(name[len(pfx):])

# built-in lowercase symbols / engine symbols we rely on
builtins = set("""
prwin nopponentsplaying nopponentsdealt nopponents issittingin
istournament isfinaltable
isomaha isplo8 isnl ispl isfl lim
random randomround randomhand randomheartbeat
bblind sblind bigblind ncallbets nbetstocall pot potsize balance
betround nplayersdealt
Raises Calls Folds Bets AmountToCall PotSize BetSize BigBlindSize StackSize
EffectiveMaxStacksizeOfActiveOpponents MaxStacksizeOfActiveOpponents
nopponentscalled nopponentsraising
lastraiseractiontime myactiontime
aggressorchair iamaggressor raischair userchair dealerchair
hero_drawdown hero_tilting raiser_drawdown raiser_recent_bigloss raiser_maybe_tilting
validator_ok validator_nerrors validator_nwarnings validator_confidence
validator_cards_ok validator_pot_ok validator_stacks_ok validator_bets_ok
table_game_info tgi_sblind tgi_bblind tgi_ante tgi_chips_per_bb tgi_level tgi_players_remaining
table_game_info_2 tgi2_handnumber tgi2_prev_handnumber
goto_lobby_button leave_lobby_button return_to_tables_button lobby_more_info_button
read_ahead_active
sb_connected sb_table_id sb_pot sb_to_call sb_to_act sb_my_seat sb_beastfavor
icm icm_fold icm_callwin icm_calllose icm_calltie
""".split())

# keywords / actions / operators that are not symbols
keywords = set("""
WHEN RETURN FORCE AND OR NOT XOR Others
Fold Check Call CheckCall Beep
Raise RaiseTo RaiseBy RaiseMax RaiseMin RaisePot
RaiseHalfPot RaiseThirdPot RaiseFourthPot RaiseTwoThirdPot RaiseThreeFourthPot
Allin SitOut Leave Close
true false
""".split())

valid = set()
valid |= lib_syms
valid |= builtins
valid |= keywords
valid |= defined_set

# card / hand-range tokens like AA, AKs, AKo, 76s, T9o, 22
def is_card_token(tok):
    return bool(re.fullmatch(r'[2-9TJQKA]{2}[so]?', tok))

# ---- 4. walk the file, validate per function -------------------------------
errors = []
warnings = []
unknown = {}  # token -> set(functions)

cur = None
# tokens we will skip when checking identifiers
num_re = re.compile(r'^-?\d+(\.\d+)?$')
ident_re = re.compile(r'[A-Za-z_][A-Za-z0-9_$]*')

func_bodies = {}  # name -> list of (lineno, text)
order = []
for i, raw in enumerate(master_text.splitlines(), 1):
    line = raw.rstrip()
    h = hdr_re.match(line.strip())
    if h:
        cur = h.group(1).strip()
        func_bodies.setdefault(cur, [])
        order.append(cur)
        continue
    if cur is None:
        continue
    func_bodies[cur].append((i, line))

# numeric/expression-only functions (no WHEN needed)
for name, body in func_bodies.items():
    code_lines = [(n, t) for (n, t) in body
                  if t.strip() and not t.strip().startswith("//")]
    # f$prwin_number_of_iterations is a bare number; allow
    for (n, t) in code_lines:
        s = t.strip()
        # strip trailing inline comment
        s_nocom = re.split(r'//', s, 1)[0].rstrip()
        if not s_nocom:
            continue
        # strip double-quoted string literals (e.g. explanation templates) so their
        # free text and {placeholders} are not linted as code.
        s_nocom = re.sub(r'"[^"]*"', '""', s_nocom)
        # structural: WHEN-lines must contain an action or RETURN, and end in FORCE
        if s_nocom.startswith("WHEN"):
            if "FORCE" not in s_nocom:
                errors.append(f"{name}:{n}: WHEN line missing FORCE -> {s_nocom}")
            has_action = bool(re.search(r'\b(RETURN|Fold|Check|Call|Beep|Raise\w*|Allin|SitOut|Leave)\b', s_nocom))
            if not has_action:
                errors.append(f"{name}:{n}: WHEN line has no action/RETURN -> {s_nocom}")
        # balanced parens
        if s_nocom.count("(") != s_nocom.count(")"):
            errors.append(f"{name}:{n}: unbalanced parentheses -> {s_nocom}")
        # illegal operators the REAL OpenHoldem parser rejects but a text-concat lint
        # would otherwise miss. OpenPPL has NO not-equal operator: '<' only forms '<=' or
        # '<<', so '<>' tokenizes as two binary operators ('<' then '>') and the parser
        # dies with "Unexpected token ... Found: binary operator >". Use NOT (a = b).
        for badop in ("<>", "!=", "=<", "=>"):
            if badop in s_nocom:
                errors.append(f"{name}:{n}: illegal operator '{badop}' "
                              f"(OpenPPL has no not-equal; rewrite as NOT (a = b)) -> {s_nocom}")
        # identifier validation. First strip whole card-range tokens (AA, AKs, 97o, T9s, ...) so
        # digit-leading ones like "97o"/"98s" aren't mis-tokenized: the leading digit isn't a valid
        # identifier start, so ident_re would otherwise capture only the trailing suit letter ('o'/'s')
        # and flag it as an unknown symbol. The real OpenHoldem parser accepts these card tokens.
        s_idents = re.sub(r'\b[2-9TJQKA]{2}[so]?\b', ' ', s_nocom)
        for tok in ident_re.findall(s_idents):
            if tok in valid: continue
            if is_card_token(tok): continue
            if num_re.match(tok): continue
            if tok.startswith("pt_"):                    # PokerTracker stat
                # Most pt_ stats accept positional suffixes (_smallblind/_bigblind/_cutoff/...), but a
                # few are raischair-only and the REAL parser REJECTS the positional form with a modal
                # that BLOCKS Hiss startup (e.g. pt_hands_bigblind, 2026-06-20). "hands" is the sample
                # count and is special-cased to _raischair / chair-number only. Catch that class here.
                m_pt = re.fullmatch(r'pt_(hands)_(smallblind|bigblind|cutoff|firstcaller|headsup|dealer|button|lastcaller)', tok)
                if m_pt:
                    errors.append(f"{name}:{n}: invalid PokerTracker symbol '{tok}' "
                                  f"('{m_pt.group(1)}' is raischair-only; the positional suffix is rejected "
                                  f"by the engine parser and blocks Hiss startup)")
                continue
            if tok.startswith("explain"): continue        # dynamic explanation triggers
            if tok.startswith("openai"): continue         # dynamic OpenAI advisor + advice-knob symbols
            if tok.startswith("intro_") or tok.startswith("exploit_"): continue   # introspection engine symbols
            if tok.startswith("obsbranch") or tok.startswith("brain_action") or tok.startswith("mischief"): continue  # Phase-2 brain knob channels
                                                          #   (CSymbolEngineOpponentIntrospection; chair-suffixed)
            if tok.startswith("f$"):
                if tok not in defined_set:
                    errors.append(f"{name}:{n}: undefined function reference '{tok}'")
                continue
            if tok.startswith("list"):
                if tok not in defined_set:
                    errors.append(f"{name}:{n}: undefined list reference '{tok}'")
                continue
            # 0b... binary handled as ident? skip pure binary
            if re.fullmatch(r'0b[01]+', tok): continue
            unknown.setdefault(tok, set()).add(f"{name}:{n}")

# ---- 5. report -------------------------------------------------------------
print(f"[lint] defined functions/lists: {len(defined_set)}")
print(f"[lint] whitelist symbols: {len(valid)}")
if errors:
    print(f"\n[ERRORS] {len(errors)}")
    for e in errors: print("  " + e)
else:
    print("\n[lint] no structural / reference errors")

if unknown:
    print(f"\n[UNKNOWN IDENTIFIERS] {len(unknown)} (verify these are real OpenPPL symbols):")
    for tok in sorted(unknown):
        locs = sorted(unknown[tok])[:4]
        print(f"  {tok:42s} @ {', '.join(locs)}")
else:
    print("[lint] all identifiers resolved against the whitelist")

# Auto-copy the regenerated master to hiss-linux on swiftsnake whenever the OHF changes (best-effort; a
# missing connection never fails the lint). This keeps the headless self-play strategy in lockstep with the
# live bot for PPO/CFR training. [Emrald: copy the OHF to hiss-server on change]
if not errors:
    try:
        import subprocess
        _sync = os.path.join(ROOT, "scripts", "sync_ohf_to_hisslinux.sh")
        if os.path.isfile(_sync):
            _r = subprocess.run(["bash", _sync, MASTER], capture_output=True, text=True, timeout=90)
            for _ln in (_r.stdout + _r.stderr).splitlines():
                if "[sync-ohf]" in _ln:
                    print(_ln)
    except Exception as _e:
        print(f"[sync-ohf] skipped: {_e}")

sys.exit(1 if errors else 0)
