# OpenHoldem Terminal Window API

The OpenHoldem terminal window is a companion tool window that opens next to the main OpenHoldem window. Drag it to the left or right side of the main window to choose which side it stays attached to.

The terminal is split into four sections:

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

Stream text without automatically adding a newline:

```cpp
ChatTerminalStream(kChatTerminalDecisions, "Evaluating preflop...");
ChatTerminalStream(kChatTerminalDecisions, " done\r\n");
```

Clear all four sections:

```cpp
ChatTerminalClear();
```

You can also call the window object directly when needed:

```cpp
if (p_chat_terminal != NULL) {
    p_chat_terminal->AppendMessage(kChatTerminalContext, "Connected to table.", false);
    p_chat_terminal->ClearTerminal();
}
```

The public helper functions post messages to the UI window, so they are the preferred API for future scraper, symbol-engine, and autoplayer instrumentation.
