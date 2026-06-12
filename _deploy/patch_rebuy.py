#!/usr/bin/env python3
# Add a "rebuy" (top-up) endpoint so a busted-but-seated player can buy back in
# from bankroll without leaving their seat. Hiss calls it automatically.
import io
base = "/var/www/poker.scarletbeast.com"

def patch(path, anchor, insert, label, before=True):
    p = base + "/" + path
    s = io.open(p, encoding="utf-8").read()
    if "'rebuy'" in s and "rebuy" in insert and label.startswith("route") and "tables/{table}/rebuy" in s:
        print(f"{label}: already present"); return
    if insert.strip() and insert.strip() in s:
        print(f"{label}: already present"); return
    if anchor not in s:
        print(f"{label}: ANCHOR NOT FOUND"); return
    s = s.replace(anchor, (insert + anchor) if before else (anchor + insert), 1)
    io.open(p, "w", encoding="utf-8").write(s)
    print(f"{label}: patched")

# 1) Routes (bot API + human web), right after the sit route.
patch("routes/api.php",
      "        Route::post('/tables/{table}/sit', [PlayController::class, 'sit']);\n",
      "        Route::post('/tables/{table}/rebuy', [PlayController::class, 'rebuy']);\n",
      "route api.rebuy", before=False)
patch("routes/web.php",
      "    Route::post('/tables/{table}/sit', [PlayController::class, 'sit']);\n",
      "    Route::post('/tables/{table}/rebuy', [PlayController::class, 'rebuy']);\n",
      "route web.rebuy", before=False)

# 2) Controller method — insert before leave().
ctrl_method = (
    "    public function rebuy(Request $request, PokerTable $table)\n"
    "    {\n"
    "        $data = $request->validate([\n"
    "            'amount' => ['required', 'integer', 'min:1'],\n"
    "        ]);\n"
    "        try {\n"
    "            $seat = $this->tm->rebuy($table, $request->user(), $data['amount']);\n"
    "        } catch (\\Throwable $e) {\n"
    "            return response()->json(['error' => $e->getMessage()], 422);\n"
    "        }\n"
    "        \\App\\Jobs\\TableTickJob::dispatch($table->id)->onQueue('poker_default');\n"
    "        return response()->json(['ok' => true, 'seat' => $seat->seat_no, 'state' => $this->tm->viewFor($table, $request->user())]);\n"
    "    }\n\n")
patch("app/Http/Controllers/PlayController.php",
      "    public function leave(Request $request, PokerTable $table)\n",
      ctrl_method, "PlayController::rebuy", before=True)

# 3) Service method — insert before standUp's docblock.
svc_method = (
    "    /**\n"
    "     * Top a seated player's stack back up from their bankroll -- the \"buy back\n"
    "     * in\" after busting. Only when not live in the current hand; capped at\n"
    "     * max_buy_in. $target is the desired total stack.\n"
    "     */\n"
    "    public function rebuy(PokerTable $table, User $user, int $target): Seat\n"
    "    {\n"
    "        if ($table->tournament_id) {\n"
    "            throw new \\RuntimeException('Tournament seats re-enter via the bracket.');\n"
    "        }\n"
    "        if ($target < $table->min_buy_in || $target > $table->max_buy_in) {\n"
    "            throw new \\RuntimeException(\"Rebuy target must be between {$table->min_buy_in} and {$table->max_buy_in}.\");\n"
    "        }\n"
    "        return $this->withLock($table, function () use ($table, $user, $target) {\n"
    "            $seat = Seat::where('table_id', $table->id)->where('user_id', $user->id)\n"
    "                ->where('status', '!=', 'empty')->first();\n"
    "            if (!$seat) {\n"
    "                throw new \\RuntimeException('Not seated at this table.');\n"
    "            }\n"
    "            // Never alter a stack while the player is live in the current hand.\n"
    "            $state = TableState::find($table->id);\n"
    "            if ($state && $state->state && $this->handInProgress($state)) {\n"
    "                $p = $state->state['players'][$seat->seat_no] ?? null;\n"
    "                if ($p && ($p['in_hand'] ?? false)) {\n"
    "                    throw new \\RuntimeException('Cannot rebuy while in a hand.');\n"
    "                }\n"
    "            }\n"
    "            if ($seat->stack >= $target) {\n"
    "                return $seat;\n"
    "            }\n"
    "            $add = $target - $seat->stack;\n"
    "            if (!$user->is_bot) {\n"
    "                if ($user->chips < $add) {\n"
    "                    throw new \\RuntimeException('Insufficient bankroll to rebuy.');\n"
    "                }\n"
    "                Bankroll::adjust($user->id, -$add, 'rebuy', \"Rebuy at {$table->name}\", $table);\n"
    "            }\n"
    "            $seat->stack += $add;\n"
    "            $seat->status = 'sitting';\n"
    "            $seat->save();\n"
    "            return $seat;\n"
    "        });\n"
    "    }\n\n")
patch("app/Services/TableManager.php",
      "    /** Stand up, returning the felt stack to bankroll. Disallowed mid-hand. */\n",
      svc_method, "TableManager::rebuy", before=True)

print("DONE")
