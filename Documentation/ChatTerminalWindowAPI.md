# OpenHoldem Terminal Window API

The OpenHoldem terminal window is a companion tool window that opens next to the main OpenHoldem window. Drag it to the left or right side of the main window to choose which side it stays attached to.

The terminal contains named screens, similar in spirit to the Linux `screen` program. Each screen has four sections:

- `kChatTerminalContext`: broad context, startup notes, tablemap/session facts.
- `kChatTerminalState`: current scraped state, symbols, cards, balances, chair state.
- `kChatTerminalDecisions`: action evaluation, chosen action, rejected options, confidence.
- `kChatTerminalChat`: user steering notes and chat-style interaction.

Include the API with:

```cpp
#include "ChatTerminalWindow.h"
```

Append a normal line:

```cpp
ChatTerminalAppend(kChatTerminalState, "p0 balance: 125.50");
```

Append to a named screen. The screen is created automatically if it does not exist:

```cpp
ChatTerminalAppendToScreen("scraper", kChatTerminalState, "p0name: USERNAME1");
ChatTerminalAppendToScreen("autoplayer", kChatTerminalDecisions, "Action: call");
```

Stream text without automatically adding a newline:

```cpp
ChatTerminalStream(kChatTerminalDecisions, "Evaluating preflop...");
ChatTerminalStream(kChatTerminalDecisions, " done\r\n");
```

Stream to a named screen:

```cpp
ChatTerminalStreamToScreen("scraper", kChatTerminalContext, "Reading symbols...");
ChatTerminalStreamToScreen("scraper", kChatTerminalContext, " done\r\n");
```

Clear all four sections:

```cpp
ChatTerminalClear();
```

Clear a single screen:

```cpp
ChatTerminalClearScreen("scraper");
```

You can also call the window object directly when needed:

```cpp
if (p_chat_terminal != NULL) {
    p_chat_terminal->AppendMessage("main", kChatTerminalContext, "Connected to table.", false);
    p_chat_terminal->ClearTerminal();
}
```

The public helper functions post messages to the UI window, so they are the preferred API for future scraper, symbol-engine, and autoplayer instrumentation.

## Local HTTP API

OpenHoldem starts a loopback-only terminal API server on:

```text
http://127.0.0.1:27654
```

Append a line:

```text
GET /append?screen=scraper&section=state&text=p0name%3A%20USERNAME1
```

Stream text:

```text
GET /stream?screen=autoplayer&section=decisions&text=Evaluating...
```

Clear one screen:

```text
GET /clear?screen=scraper
```

Clear all screens:

```text
GET /clear
```

The `section` value can be a name (`context`, `state`, `decisions`, `chat`) or a numeric section index (`0` through `3`). The `screen` parameter is optional and defaults to `main`.
