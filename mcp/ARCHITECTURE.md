# Hiss MCP — Architecture

## TL;DR
There is **no MCP server compiled into Vision or Hiss**. The MCP is a **standalone
Python process** (`mcp/hiss_mcp_server.py`) that Claude Code launches. It **attaches
to the running `hiss.exe`** over HTTP, and reads **Vision's data indirectly** (via the
shared postgres DB and the per-region scrape dumps Hiss writes). Nothing MCP-related
is linked into either app binary.

```
            ┌─────────────────────────┐
 Claude ───►│  hiss_mcp_server.py      │  (stdio JSON-RPC 2.0, stdlib only;
 (Code)     │  launched via .mcp.json  │   started by Claude Code per project)
            └──────────┬───────┬───────┘
                       │       │  └──────────────► filesystem (repo, Release\, logs, OHF, tesseract)
       HTTP 127.0.0.1  │       │  psql.exe
                       ▼       ▼
            ┌──────────────┐  ┌──────────────────────────────┐
            │  hiss.exe    │  │  postgres                     │
            │  ChatTerminal│  │   - hiss DB (tablemaps, fonts,│
            │  Server :auto│  │     regions, settings, learner│
            └──────────────┘  │     loop tables, game_state)  │
                              │   - "PT4 DB" (PokerTracker 4)  │
                              └──────────────────────────────┘
```

## 1. The MCP server (`mcp/hiss_mcp_server.py`)
- Self-contained Python, **standard library only** (Pillow optional, only to convert
  BMP screenshots to PNG). Launched by Claude Code from `.mcp.json`.
- Transport: **stdio JSON-RPC 2.0** (`initialize`, `tools/list`, `tools/call`).
- Discovers the live bot's HTTP port from `Release\logs\terminal_port.txt`
  (falls back to 27654).

## 2. How it attaches to **Hiss** (live, over HTTP)
Hiss runs an HTTP terminal server (`Hiss/ChatTerminalServer.cpp`). The MCP server is a
client of these endpoints — this is the entire Hiss-side surface that exists for it:

| Endpoint | Purpose |
|----------|---------|
| `/api/terminal-state` | the 4 Terminal panes (Context/State/Decisions/Chat) + pinned State |
| `/api/table-state` | live internal-engine game state (seats, cards, pot, blinds, button, hero, HUD) |
| `/api/symbols?names=…` | evaluate OpenPPL/engine symbols live; **unknown-symbol modal is suppressed** so a bad name can't freeze the heartbeat |
| `/api/dump-scrapes` | one-shot: dump full-table screenshot + every region's raw image + OCR result to `logs\scrapes\` |
| `/api/action?do=fold\|check\|call\|bet\|raise\|allin[&amount=bb]` | manual FCKRA / sized bet — **consumed on the heartbeat thread**, waits for the bot's turn |
| `/api/autoplayer?on=0\|1` | engage/disengage the autoplayer |
| `/api/terminal-input` | inject text into the terminal command box |
| `/api/mappings` | name-mapping review |

Supporting Hiss-side bits added for the MCP:
- **Port file**: `ChatTerminalServer` writes the bound port to `logs\terminal_port.txt`.
- **Heartbeat-consumed control flags** (`g_mcp_action_request`, `g_mcp_action_amount`,
  `g_mcp_autoplayer_request`, `g_mcp_action_set_tick`) so actions run on the thread that
  normally acts (never blocking the GUI), wait for the bot's turn, and expire if stale.
- **Unknown-symbol modal suppression** (`g_suppress_unknown_symbol_warning`) around
  `/api/symbols` so a typo can't pop a blocking dialog and stall the bot.

> Design rule honored throughout: **never do network/HTTP work on the heartbeat
> thread** (the Scarlet-Beast stall lesson). The HTTP server runs on its own thread;
> control requests are picked up by the heartbeat as cheap flag checks.

## 3. How it relates to **Vision** (indirect — no code in Vision)
Vision (OpenScrape, the region/font editor) has **no MCP or HTTP server**. The MCP
reaches Vision's world two ways:
1. **Postgres** — Vision's tablemaps, regions, fonts, and images live in the `hiss` DB
   (`tablemaps`, `tm_regions`, `tm_fonts`, `tm_images`, `settings`). MCP tools
   `pg_query` / `dump_tablemap` / `read_settings` / `card_scrapes` / `card_image` /
   `log_card_correction` read and diagnose that data.
2. **Region list additions** — the only Vision *code* change for this work was adding
   scrape regions to `Vision/ListOfSymbols.cpp` (`c0tourney_title`, `c0tourney_id`,
   `c0tourney_level`, `c0table_name`) so they can be drawn; Hiss then scrapes them and
   the hand-history writer / MCP consume the values.

The per-region card images the MCP returns come from Hiss's `logs\scrapes\` dump
(`<region>_raw.bmp` + `<region>.txt`), not from Vision.

## 4. Databases reachable
Through the local `postgres` server (via `psql.exe`, creds from `.mcp.json`):
- **`hiss`** — tablemaps/regions/fonts/settings; the analysis-loop tables
  (`bot_session_state`, `bot_hand_review`, `bot_feedback`, `game_state_log`); the
  learner tables (`learner_decisions`, `learner_questions`); `card_corrections`.
- **`PT4 DB`** — PokerTracker 4's database (e.g. `tourney_hand_histories.history` holds
  the raw ACR hand text used as the hand-history-format template).
`pg_query` defaults to read-only (SELECT/WITH/EXPLAIN/SHOW); `allow_write=true` opts in.

## 5. Tool families
- **Live (Hiss):** `hiss_status`, `terminal_panes`, `game_state`, `symbols`,
  `table_screenshot`, `replay_screenshot`, `trigger_scrape_dump`,
  `start_hiss`, `stop_hiss`, `autoplayer_toggle`, `fckra_action`
- **Vision / scrape data:** `card_scrapes`, `card_image`, `log_card_correction`,
  `list_scrapes`, `read_scrape`
- **Database:** `pg_query`, `pg_databases`, `pg_tables`, `pg_describe`,
  `dump_tablemap`, `read_settings`
- **Files:** `read_source`, `search_source`, `read_ohf`, `list_ohf`, `read_log`,
  `list_logs`, `list_release`, `list_tesseract_models`
- **Learner loop:** `learner_decisions`, `learner_ask`, `learner_answers`

## 6. Lifecycle / gotchas
- Claude must be (re)started to pick up new tools after the server's tool list changes.
- The server is the lifecycle of Claude's MCP client, **not** of `hiss.exe`. If Hiss
  isn't running, the live tools report unreachable; file/postgres tools still work.
- `hiss.exe` picks a free port starting at 27654; always resolve via the port file.
