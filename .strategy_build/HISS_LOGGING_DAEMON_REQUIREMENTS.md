# Hiss advanced-logging + multi-daemon identity — requirements (Emrald, 2026-06-15)

Builds on the replay/logging system ([[replay-logging-system]]: Hiss postgres outbox →
hiss_shipper.py → hiss.scarletbeast.com LAN ingest → replay UI + MCP replay_*).

## A. Linux shim — make the full harmonized OHF load completely (IN PROGRESS)
- Pushed `Release/ScarletBeast_PowerHoldem.ohf` → `/var/www/hiss-linux/strategy/ScarletBeast.ohf`.
- Load test (HISS_FORMULA=…ScarletBeast.ohf) FAILS to parse cleanly:
  - "Can't find initialization-function … in OpenPPL-library" (history symbols) — function name
    prints as GARBAGE (`p��bKV`) ⇒ a CString lifetime/encoding bug in `compat/mfc_string.h`
    (likely GetString()/Format/Mid returning a dangling/temporary pointer).
  - "Selftest failed: Calculated 130, Expected 180" (CFormulaParser self-test).
- ROOT CAUSE (diagnosed): the OHF load fails because the OpenPPL LIBRARY (25 files,
  `files.h kOpenPPLLibraries`) never loads on Linux — two path bugs:
    1. `compat/mfc_compat.h` `CFile::Open` calls `std::fopen(path)` with NO `\`→`/`
       translation, so engine paths like `bot_logic\OpenPPL_Library\X.ohf` fail.
    2. `engine/oh/Files.cpp OpenHoldemDirectory()` uses the Win32 `GetModuleFileName`
       stub + `strrchr(path,'\\')` → returns no valid Linux base dir; and the library
       isn't bundled under /var/www/hiss-linux at all (only in /mnt/www/openholdembot/Release/bot_logic/).
  => ParseDefaultLibraries loads nothing => init-function missing => history symbols
     broken => CSelftestParserEvaluator fails (130 vs 180).
- EXACT FIX (3 steps, then `./build.sh` + retest):
    (a) `CFile::Open` (and CStdioFile): translate `\`→`/` in the path before fopen
        (one std::string replace; helps every engine file open).
    (b) `OpenHoldemDirectory()`: on Linux return the binary dir via
        `readlink("/proc/self/exe")` stripped at the last `/`, trailing `/` kept
        (or honor a `HISS_HOME` env override).
    (c) bundle `bot_logic/OpenPPL_Library/` (+ `bot_logic/DefaultBot/`) under the
        resolved base dir — copy/symlink from /mnt/www/openholdembot/Release/bot_logic/.
    Then re-run `env HISS_FORMULA=strategy/ScarletBeast.ohf ./build/hiss` — expect no
    init-function/selftest errors; verify f$Style=2 via /decide "sym".
- PROGRESS 2026-06-15 (applied on the box, rebuilt OK): FIXED the library-not-loading —
  (a) `compat/mfc_compat.h CFile::Open` now translates `\`→`/`; (b) `Files.cpp
  OpenHoldemDirectory()` returns the binary dir via `readlink("/proc/self/exe")` (HISS_HOME
  override; forward-declared readlink to avoid the unistd.h access/lseek clash); (c) Files.cpp
  dir builders use `/` not `\`; (d) `CFormulaParser` `library_path.Format("%s%s", dir, name)`
  now passes `.GetString()` (the CString-to-%s bug was corrupting the path); (e) symlinked
  `build/bot_logic -> /mnt/www/openholdembot/Release/bot_logic`. RESULT: no more
  "Can't find initialization-function" / "Selftest failed" — the 25-file OpenPPL library loads.
- REMAINING BLOCKER (now the critical one): loading ScarletBeast.ohf still emits `[MSG] Error:`
  with EMPTY/garbled bodies — the `CString`→`%s` ABI bug (below) both HIDES the real parse-error
  text AND corrupts the parser's internal function-name handling. Must fix the CString-%s bug to
  (1) see the real errors and (2) let the strategy parse. Two fix paths:
    * Targeted: add `.GetString()` to every `Format(...%s..., <CString>)` in the parse hot path
      (CParseErrors::Error, CTokenizer::CurrentFunctionName, CFormulaParser/CFunctionCollection/
      CFormulaFileSplitter name handling). Quick to start; whack-a-mole.
    * Systemic (recommended): make `compat` `CString` TRIVIALLY COPYABLE so the Itanium ABI passes
      it BY VALUE to `...` (the MFC trick needs by-value). Store a non-owning `const char*` into a
      never-freed global string arena (every mutation interns a new arena string); no std::string
      member, no dtor => trivially copyable => `%s` reads `_c` correctly. ~1 shim file, fixes ALL
      sites at once. Then re-test: expect clean load + f$Style=2 via /decide "sym".
- The harmonized strategy is ALREADY on the box at strategy/ScarletBeast.ohf; the live WINDOWS
  bot is unaffected by any of this Linux work.
- SEPARATE cosmetic bug (lower priority): `CString`→`%s` prints garbage on the Itanium
  ABI (non-trivially-copyable class passed to `...` by hidden reference). Real fix is to
  stop passing CString objects to `%s` (use GetString()) at the few diagnostic call sites,
  or make CString a trivially-copyable thin char* wrapper. Cosmetic only — doesn't affect
  decisions, just garbles diagnostic text. Bundle with the parse-error-logging port.
- Also port headless parse-error logging (CParseErrors::Error → file, suppress modal) — headless can't
  pop the MessageBox; needed for clean automated loads.

## A-RESULT (2026-06-15): Linux port now BOOTS + PARSES the harmonized strategy.
Fixed FIVE real engine bugs on the box (all rebuilt clean, `build/hiss` links):
  1. `CFile::Open` `\`→`/` translation (compat/mfc_compat.h).
  2. `OpenHoldemDirectory()` Linux base via `/proc/self/exe` (+HISS_HOME) (Files.cpp).
  3. Forward-slash dir builders + `.GetString()` on `library_path.Format` (Files.cpp / CFormulaParser.cpp).
  4. **CString→%s ABI fix**: templatized `CString::Format`/`AppendFormat` so CString args auto-
     convert to const char* (the Itanium ABI passed the non-trivially-copyable CString to a `...`
     vararg by hidden reference, corrupting paths + garbling all diagnostics). mfc_string.h.
  5. **`CFile::ReadString` clears the string at EOF** (MFC semantics) — the stale last-body-line was
     re-processed and threw a spurious "Shanky option settings" error per library file.
  + `PT_DLL_IsValidSymbol` stub → return true (pt_* validate; not-connected branch returns undefined).
  + symlinked `build/bot_logic` -> /mnt/www/openholdembot/Release/bot_logic (the 25 OpenPPL libs).
RESULT: the OpenPPL library + ScarletBeast strategy parse; engine prints "engine booted" +
"strategy formula loaded". Strategy combined as strategy/ScarletBeast_linux.ohf (headless_shims +
ScarletBeast). tgi_*/openai_* resolve via shim functions; pt_* return undefined headless.

## A-REMAINING (the precise last mile): the ~19 `explain_*` narration anchors still error
("Unknown identifier") because the parser REJECTS non-f$/list function headers ("Found unknown
function type", CFormulaParser.cpp:339) and the function-collection only exposes f$/list names as
symbols — so the bare-name shim works for tgi_/openai_ (already-registered-name path) but NOT for
explain_. FIX = a tiny **headless symbol engine** (e.g. CSymbolEngineHeadlessExplain) registered in
the engine container that claims the `explain_` prefix → returns 1.0 (always-true anchor), and
optionally `tgi_`/`openai_` → defaults (replacing the shim functions for cleanliness). Pattern: copy a
minimal CSymbolEngine* (memcmp(name,"explain_",8)==0 → *result=1; return true), register in
CEngineContainer's engine list. Then re-test: expect 0 parse errors + f$Style=2 via /decide "sym".
Headless parse-error logging (CParseErrors::Error → file + skip modal) ports cleanly alongside.

## B. Multi-daemon identity for advanced logging
- Each hiss-linux headless daemon instance (one per table/seat) must carry a DISTINCT IDENTITY when
  it does advanced logging to hiss.scarletbeast.com. Add `HISS_IDENTITY` / `daemon_id` (config +
  hiss.conf) stamped on every log/replay/report row so streams from N daemons are separable in the
  DB + replay UI. Default to handle/table/host-derived if unset.

## C. DB-backed advanced-logging settings (toggle KINDS independently)
Settings table (postgres `hiss` DB) with on/off flags per kind, per identity (or global):
  - advanced logging (verbose symbol/decision traces)
  - reporting (periodic stats/summaries)
  - replays (frame/hand capture into the replay DB)
Each independently switchable; daemons + hiss.exe read these at runtime (poll/heartbeat) and
honor them. Wire into the existing outbox/shipper so replays/logs only flow when enabled.

## D. Dual-side control plane (turn the above on/off from BOTH)
  1. **hiss.scarletbeast.com** — web UI/endpoint to flip the flags (per daemon identity or all).
  2. **hiss.exe** (Windows, non-linux) — a control to flip the same DB flags (Terminal command /
     settings UI / API), so the Windows bot and the web both drive the same settings.
Both write the same DB settings; daemons + hiss.exe converge on the DB as source of truth.

## Sequence
Finish A (shim load) → B (identity) → C (DB settings + shipper gating) → D (web + hiss.exe toggles).
Server access: ssh asterisk@192.168.1.39 (swiftsnake host, key-based, passwordless sudo);
hiss.scarletbeast.com is the ingest/replay app on the same box ([[scarletbeast-web-server]]).
SageMaker plan still follows after.
