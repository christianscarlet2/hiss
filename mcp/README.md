# Hiss bot MCP server

A Model Context Protocol server that gives Claude full visibility into the Hiss
poker bot. It **attaches to the running `hiss.exe`** (via its terminal HTTP server,
whose port Hiss now writes to `Release\logs\terminal_port.txt`) for live data, reads
the workspace files (read-only, confined to the repo), and queries the postgres
`hiss` DB for the tablemap and settings.

## Setup
Registered for Claude Code via [`.mcp.json`](../.mcp.json) in the repo root — Claude
auto-discovers it. No packages to install (Python stdlib only; Pillow is used only
if present, to render BMP screenshots as PNG). Just run Claude in this repo and
approve the `hiss-bot` MCP server.

## What it exposes (tools)

| Tool | Source | Purpose |
|------|--------|---------|
| `hiss_status` | hiss.exe | is the bot running / reachable, on what port |
| `terminal_panes` | hiss.exe `/api/terminal-state` | the 4 Terminal panes (Context/State/Decisions/Chat) + pinned State |
| `game_state` | hiss.exe `/api/table-state` | live internal-engine state (seats, cards, pot, blinds, button, hero, HUD) |
| `symbols` | hiss.exe `/api/symbols` | evaluate any OpenPPL/engine symbols live (`prwin,Raises,f$Style,…`) |
| `trigger_scrape_dump` | hiss.exe `/api/dump-scrapes` | make the bot dump the table screenshot + every region scrape/result |
| `table_screenshot` | `logs/scrapes/_table.bmp` | the full-table screenshot the bot saw (image) |
| `list_scrapes` / `read_scrape` | `logs/scrapes/` | per-region **raw scrape image + OCR result** (the two files per region) |
| `list_logs` / `read_log` | `Release/logs/` | any log file (crash logs, oh_*.log, scrape_perf, button_debug, …) |
| `list_ohf` / `read_ohf` | `Release/bot_logic/Strategy/` + master | the OHF strategy files |
| `search_source` / `read_source` | repo | grep + read the bot source code & debug files |
| `list_release` | `Release/` | anything in the Release directory |
| `list_tesseract_models` | repo + settings | tesseract `.traineddata`/`.checkpoint` models + AutoOcr model settings |
| `dump_tablemap` | postgres | dump the tablemap(s) from the DB to `logs/tablemap_dump.json` |
| `read_settings` | postgres | the `settings` table (key → jsonb) |

## The `/improve` flow
When you issue `/improve <text>` in the Hiss terminal, the bot captures the table
screenshot + all region scrapes/results to `logs/scrapes/` (so Claude, via this MCP
server, sees exactly what the bot saw at that moment) and logs the request. Then
ask Claude to read `table_screenshot`, `game_state`, `terminal_panes`, the relevant
`read_ohf`, and `read_scrape` for any suspicious region, and propose the edit.

## Notes / limits
- **Read-only** except: `trigger_scrape_dump` (asks the bot to write scrape files)
  and `dump_tablemap` (writes `logs/tablemap_dump.json`). File access is confined to
  the repo (no path traversal).
- The per-region "two files" are the **raw scrape image** (`<region>_raw.bmp`) and the
  **recognised text** (`<region>.txt`). If you want the binarised pre-OCR image too,
  that's a further CAutoOcr hook (not yet wired).
- Live tools require `hiss.exe` to be running; file/DB tools work regardless.
- Paths/credentials are overridable via env in `.mcp.json` (`HISS_REPO`, `HISS_PSQL`,
  `PGUSER`, `PGDATABASE`, `PGPASSWORD`).
