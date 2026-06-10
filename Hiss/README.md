# Hiss — Button & Region System

How clickable regions (buttons) are defined, detected, and acted upon in Hiss, and
which symbols you need to drive them. Grounded in the actual source:
[`CScraper.cpp`](CScraper.cpp), [`CAutoplayerButton.cpp`](CAutoplayerButton.cpp),
[`CCasinoInterface.cpp`](CCasinoInterface.cpp), and
[`MagicNumbers.h`](../Shared/MagicNumbers/MagicNumbers.h).

---

## The mental model

A poker action in Hiss is **three separate things**, and they're easy to conflate:

1. **Is the button there?** → a *state* region (visibility)
2. **What is it?** → a *label* region (identity)
3. **Where do I click?** → the click target (region rect, a template hit inside an area, or a hotkey)

The scraper fills all three every heartbeat; the autoplayer reads them and clicks.

---

## The button families (region names you create)

### 1. Action buttons — `i0`…`iF` (the core ones)

Fold/call/raise/etc. Up to 16 buttons (`i0`–`iF`, hex). Each is a **trio** of regions:

| Region     | Purpose                          | Example scrape result |
|------------|----------------------------------|-----------------------|
| `iXstate`  | Is the button visible/lit?       | `true` / `on` / `yes` / `lit` / `checked` → clickable |
| `iXlabel`  | OCR the text on the button       | `"Raise"`, `"Call $4"`, `"Fold"` |
| `iXbutton` | (optional) explicit click rect   | a RECT |

**The index `i3` means nothing by itself.** Hiss does not assume "i3 = raise". It scrapes
`i3label`, reads the text, and *classifies* it.
See `ScrapeActionButtons` / `ScrapeActionButtonLabels` in
[`CScraper.cpp`](CScraper.cpp) (reads `iXstate` and `iXlabel`).

Label → action mapping is in [`CAutoplayerButton.cpp`](CAutoplayerButton.cpp). It matches the
**first few chars, lowercased, OCR-typo-tolerant**:

- `"raise"` / `ra1se` / `ralse` / `bet…` / `swag…` → **raise**
- `"call"` / `caii` / `ca11` → **call**
- `"check"` / `cheok` → **check**
- `"fold"` / `fo1d` / `foid` → **fold**
- allin (via `IsStringAllin`), `autopost`, `sitin`, `sitout`, `leave`, `rematch`, `prefold`

If OCR fails, it falls back to `iXdefaultlabel` from the tablemap.

There are also **all-in special cases**: if "allin" is showing with no check/call button it is
treated as a call; if a call button is showing but no raise button, it is treated as a raise.

### 2. Template / "area" buttons (modern casinos with moving buttons)

Instead of fixed rects, define a zone `area_buttons_zone0`…`zoneF` and Hiss runs **template
detection** inside it to find where (and whether) `fold`/`call`/`raise` are right now. The
canonical label list it searches for:

```
betsize, fold, allin, bet, raise, call, check,
sitin, sitout, leave, rematch, prefold, autopost, undefined
```

This is `ScrapeButtons(area, "action")`. The click then targets the detected template rect,
not a fixed region.

### 3. Betpot buttons (pot-fraction bet shortcuts)

Buttons like "½ pot", "¾ pot", "pot". Fixed names:

```
betpot_2_1, betpot_1_1, betpot_3_4, betpot_2_3, betpot_1_2, betpot_1_3, betpot_1_4
```

Each has a `…state` region (e.g. `betpot_1_2state`). See `ScrapeBetpotButtons`.

### 4. Bet-sizing: the slider + the input box

For raising an arbitrary amount you need one of:

- **Type-in box:** a `betsize` region (Hiss types the number into it) → `EnterBetsize`
- **Slider:** `i3slider` (track) + `i3handle` (the draggable knob). Hiss drags the handle
  proportionally → `ScrapeSlider` / `SlideBetsize`.

### 5. Interface / "spam" buttons — `i86`

`i86`/`spam0…` are generic "click this thing" buttons (close popups, OK dialogs, "I'm back")
with no semantic label — identified purely by name (`IsNameI86` → type `k_button_i86`).
Scraped via `ScrapeInterfaceButtons` (`i86Xstate`).

---

## How a click happens

`CAutoplayerButton::Click()` in order:

1. If `_clickable` is false → bail (button not visible).
2. **Hotkey first** — if you defined `iXbuttonhotkey`, it presses the key and stops (no mouse).
3. Else resolve the rect (template hit in an area, or the fixed region).
4. Click per `iXbuttonclickmethod`: `single` (default), `double`, or `nothing`.

Tablemap symbols that tune clicking (`GetTMSymbol`):

- `buttonclickmethod`, `iXbuttonclickmethod`
- `betsizeselectionmethod` (type vs slider vs betpot), `betsizeinterpretationmethod`,
  `betsizeconfirmationmethod` (e.g. Enter), `betsizedeletionmethod`, `betpotmethod`

---

## The symbols you'll actually use

### State / "should I even act?" — from [`CSymbolEngineAutoplayer.h`](CSymbolEngineAutoplayer.h)

- **`ismyturn`** — true if fold/call/raise are visible (`myturnbits & 0x07`)
- **`myturnbits`** — bitmask of which of F/C/K/R/A are showing
- **`isfinalanswer`** — buttons have been stable long enough to trust the read (act now)
- **`issittingin` / `issittingout` / `isautopost`**
- **`fckra`** string (`GetFCKRAString`) — which action buttons are currently visible

### The autoplayer functions YOU write (the bot's brain) — `f$…`

Formulas the engine evaluates to decide actions. Names from
[`MagicNumbers.h`](../Shared/MagicNumbers/MagicNumbers.h):

**Primary** (return true to take that action; `f$betsize` returns the dollar amount):

```
f$fold  f$check  f$call  f$raise  f$betsize  f$allin
f$betpot_2_1  f$betpot_1_1  f$betpot_3_4  f$betpot_2_3
f$betpot_1_2  f$betpot_1_3  f$betpot_1_4   f$beep
```

**Hopper (table management):**

```
f$sitin  f$sitout  f$leave  f$rematch  f$autopost  f$close  f$rebuy
f$select_formula_file
```

**Other:** `f$prefold`, `f$chat`, `f$delay`, `f$allin_on_betsize_balance_ratio`,
`f$shoot_replay_frame`

**Init hooks:** `f$init_on_startup / _on_connection / _on_handreset / _on_new_round /
_on_my_turn / _on_heartbeat`

**PrWin tuning:** `f$prwin_number_of_opponents`, `…_iterations`, `…_topclip`,
`f$prwin_mustplay/willplay/wontplay`

**ICM:** `f$icm_prize1…9`

### "What did I already do?" — history symbols ([`CSymbolEngineHistory.h`](CSymbolEngineHistory.h))

Per betround: `didfold`, `didcall`, `didrais`, `didchec`, `didalli`, `didswag` (betsize).

---

## Minimum viable tablemap (practical checklist)

To get a bot that can actually act, you need at least:

- **Action visibility + identity:** `i0state`…`i4state` + matching `i0label`…`i4label`
  (covering fold/call/check/raise/allin), **or** an `area_buttons_zone0` with templates.
- **Bet sizing (if you raise arbitrary amounts):** a `betsize` input region **or**
  `i3slider` + `i3handle`.
- **Popups:** at least one `i86`/`spam` button for "close/OK".
- **Game-state regions** (so the symbols are meaningful): dealer button (`pNdealer`),
  per-seat `pNcardfaceX`, `pNbet`, `pNbalance`, `pNseated`/`pNactive`, common cards
  `c0cardfaceX`, pot.
- **Formulas:** `f$fold/f$call/f$check/f$raise/f$betsize/f$allin` (start trivial, e.g.
  `f$fold = 1`, and build up).

---

## One-line summary

> **State regions decide *if*, label regions decide *what*, click targets decide *where*,
> and your `f$…` formulas decide *whether*.** Hiss scrapes the first three from the screen
> each heartbeat; you own the fourth.
