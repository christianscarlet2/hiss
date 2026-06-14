#!/usr/bin/env pythonw
"""
learner.exe  --  human-decision capture + Claude Q&A for the Hiss poker bot.

What it does
------------
* Shows the LIVE table context pulled from the running hiss.exe (your hole cards,
  the board, pot, amount to call, whose turn) via its terminal HTTP server.
* You act with the Fold / Call / Bet / Raise buttons and type WHY in the reasoning
  box. Each decision (with a full game-state snapshot) is logged to postgres
  (table learner_decisions).
* Claude reads those decisions through the MCP server, compares them to what the
  OHF strategy would do, and -- when they differ or it's unsure -- posts a
  QUESTION. Questions appear in the "Questions from Claude" panel here; your answer
  is written back (table learner_questions) for Claude to read.

So the loop is:  you play + explain  ->  Claude checks vs the OHF  ->  Claude asks
-> you answer  ->  Claude proposes OHF improvements. All shared state lives in the
same postgres DB the MCP server already uses, so it "works with the MCP server".

Pure standard library + tkinter. Talks to postgres via psql.exe and to hiss.exe
via http://127.0.0.1:<port>.
"""

import os, json, subprocess, urllib.request, tkinter as tk
from tkinter import ttk, messagebox

REPO   = os.environ.get("HISS_REPO", r"C:\www\openholdembot_old")
LOGS   = os.path.join(REPO, "Release", "logs")
PORT_F = os.path.join(LOGS, "terminal_port.txt")
PSQL   = os.environ.get("HISS_PSQL", r"C:\Program Files\PostgreSQL\12\bin\psql.exe")
PGUSER = os.environ.get("PGUSER", "postgres")
PGDB   = os.environ.get("PGDATABASE", "hiss")
PGPASS = os.environ.get("PGPASSWORD", "dbpass")

# ---------------------------------------------------------------- infra
def hiss_port():
    try:
        return int(open(PORT_F).read().strip())
    except Exception:
        return 27654

def hiss_get(path):
    url = "http://127.0.0.1:%d%s" % (hiss_port(), path)
    with urllib.request.urlopen(url, timeout=3) as r:
        return r.read().decode("utf-8", "replace")

def esc(s):
    return ("" if s is None else str(s)).replace("'", "''")

PREF_FILE = os.path.join(os.path.expanduser("~"), ".hiss_learner.json")
def load_prefs():
    try:
        return json.load(open(PREF_FILE))
    except Exception:
        return {}
def save_prefs(p):
    try:
        json.dump(p, open(PREF_FILE, "w"))
    except Exception:
        pass

def run_sql(sql, read=False):
    env = dict(os.environ); env["PGPASSWORD"] = PGPASS
    flags = ["-t", "-A"] if read else []
    cmd = [PSQL, "-U", PGUSER, "-d", PGDB] + flags + ["-c", sql]
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=30,
                       creationflags=0x08000000)  # CREATE_NO_WINDOW
    if p.returncode != 0:
        raise RuntimeError(p.stderr.strip() or p.stdout.strip())
    return p.stdout

# ---------------------------------------------------------------- live context
def get_context():
    """Returns a dict of the current table situation, or {} if Hiss unreachable."""
    try:
        st = json.loads(hiss_get("/api/table-state"))
    except Exception:
        return {}
    hero = st.get("userchair", -1)
    players = st.get("players", [])
    hero_cards = ""
    if 0 <= hero < len(players):
        cs = [c for c in players[hero].get("cards", []) if c and c != "BACK"]
        hero_cards = " ".join(cs)
    board = " ".join([c for c in st.get("commonCards", []) if c])
    # a couple of symbols for the betting context (modal is suppressed server-side)
    sym = {}
    try:
        sym = json.loads(hiss_get("/api/symbols?names=betround,AmountToCall,PotSize"))
    except Exception:
        pass
    return {
        "handnumber": st.get("handnumber", ""),
        "betround": sym.get("betround"),
        "hero_cards": hero_cards,
        "board": board,
        "pot": st.get("pot"),
        "amount_to_call": sym.get("AmountToCall"),
        "raw": st,
    }

# ---------------------------------------------------------------- app
class Learner(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("learner.exe  --  Hiss decision trainer")
        self.geometry("560x690")
        self.configure(padx=10, pady=8)
        self.ctx = {}
        self.cur_q = None  # (id, text)

        # --- menu: Tools -> Always on Top ---
        menubar = tk.Menu(self)
        tools = tk.Menu(menubar, tearoff=0)
        self.prefs = load_prefs()
        self.on_top = tk.BooleanVar(value=bool(self.prefs.get("always_on_top", False)))
        tools.add_checkbutton(label="Always on Top", variable=self.on_top,
                              command=self.toggle_on_top)
        menubar.add_cascade(label="Tools", menu=tools)
        self.config(menu=menubar)
        # apply the persisted preference at startup
        self.wm_attributes("-topmost", bool(self.on_top.get()))

        # --- live context panel ---
        ctxf = ttk.LabelFrame(self, text="Live table (from hiss.exe)")
        ctxf.pack(fill="x", pady=4)
        self.lbl_hand  = ttk.Label(ctxf, text="hand: --"); self.lbl_hand.pack(anchor="w", padx=6)
        self.lbl_cards = ttk.Label(ctxf, text="your cards: --", font=("Segoe UI", 12, "bold"))
        self.lbl_cards.pack(anchor="w", padx=6)
        self.lbl_board = ttk.Label(ctxf, text="board: --"); self.lbl_board.pack(anchor="w", padx=6)
        self.lbl_pot   = ttk.Label(ctxf, text="pot: --   to call: --"); self.lbl_pot.pack(anchor="w", padx=6)
        self.lbl_conn  = ttk.Label(ctxf, text="", foreground="#a00"); self.lbl_conn.pack(anchor="w", padx=6)

        # --- reasoning ---
        rf = ttk.LabelFrame(self, text="Your reasoning (why are you making this play?)")
        rf.pack(fill="both", expand=False, pady=4)
        self.reason = tk.Text(rf, height=5, wrap="word"); self.reason.pack(fill="x", padx=6, pady=4)

        # --- amount + action buttons ---
        af = ttk.Frame(self); af.pack(fill="x", pady=4)
        ttk.Label(af, text="amount (bet/raise):").pack(side="left")
        self.amount = ttk.Entry(af, width=10); self.amount.pack(side="left", padx=6)

        bf = ttk.Frame(self); bf.pack(fill="x", pady=4)
        for txt, act, col in (("Fold","fold","#c0392b"), ("Call/Check","call","#2e7d32"),
                              ("Bet","bet","#1565c0"), ("Raise","raise","#6a1b9a")):
            b = tk.Button(bf, text=txt, width=11, bg=col, fg="white",
                          font=("Segoe UI", 10, "bold"),
                          command=lambda a=act: self.submit_action(a))
            b.pack(side="left", padx=4, ipady=6)

        # --- preset sizing + all-in ---
        pf = ttk.Frame(self); pf.pack(fill="x", pady=2)
        tk.Button(pf, text="All-In", width=8, bg="#922b21", fg="white",
                  font=("Segoe UI", 10, "bold"),
                  command=lambda: self.submit_action("allin")).pack(side="left", padx=3, ipady=4)
        tk.Button(pf, text="Min 2bb", width=8, bg="#1565c0", fg="white",
                  command=lambda: self.submit_bet(min_bb=2)).pack(side="left", padx=3, ipady=4)
        tk.Button(pf, text="Bet 1/4", width=7, bg="#1565c0", fg="white",
                  command=lambda: self.submit_bet(frac=0.25)).pack(side="left", padx=3, ipady=4)
        tk.Button(pf, text="Bet 1/2", width=7, bg="#1565c0", fg="white",
                  command=lambda: self.submit_bet(frac=0.50)).pack(side="left", padx=3, ipady=4)
        tk.Button(pf, text="Bet 3/4", width=7, bg="#1565c0", fg="white",
                  command=lambda: self.submit_bet(frac=0.75)).pack(side="left", padx=3, ipady=4)

        # --- follow-up: rate your own past decisions after seeing the result ---
        ff = ttk.LabelFrame(self, text="Follow-up: review a past decision (after you've seen how it played out)")
        ff.pack(fill="x", pady=6)
        row = ttk.Frame(ff); row.pack(fill="x", padx=6, pady=2)
        ttk.Label(row, text="decision:").pack(side="left")
        self.fu_pick = ttk.Combobox(row, width=52, state="readonly", values=[])
        self.fu_pick.pack(side="left", padx=6)
        ttk.Button(row, text="refresh", width=8, command=self.refresh_followups).pack(side="left")
        self.fu_note = tk.Text(ff, height=2, wrap="word"); self.fu_note.pack(fill="x", padx=6, pady=3)
        brow = ttk.Frame(ff); brow.pack(fill="x", padx=6, pady=2)
        tk.Button(brow, text="✔ Liked it", bg="#2e7d32", fg="white",
                  command=lambda: self.submit_followup(True)).pack(side="left", padx=4, ipady=2)
        tk.Button(brow, text="✖ Didn't like it", bg="#c0392b", fg="white",
                  command=lambda: self.submit_followup(False)).pack(side="left", padx=4, ipady=2)
        self._fu_ids = []

        # --- questions from Claude ---
        qf = ttk.LabelFrame(self, text="Questions from Claude")
        qf.pack(fill="both", expand=True, pady=6)
        self.q_text = tk.Text(qf, height=4, wrap="word", state="disabled", background="#fffbe6")
        self.q_text.pack(fill="x", padx=6, pady=4)
        self.answer = tk.Text(qf, height=3, wrap="word"); self.answer.pack(fill="x", padx=6)
        self.btn_ans = ttk.Button(qf, text="Send answer", command=self.send_answer)
        self.btn_ans.pack(anchor="e", padx=6, pady=4)

        self.status = ttk.Label(self, text="", foreground="#2e7d32"); self.status.pack(anchor="w")

        self.refresh_ctx()
        self.poll_questions()
        self.refresh_followups()

    # ---- live refresh
    def refresh_ctx(self):
        self.ctx = get_context()
        if not self.ctx:
            self.lbl_conn.config(text="hiss.exe not reachable")
        else:
            self.lbl_conn.config(text="")
            c = self.ctx
            self.lbl_hand.config(text="hand: %s   (betround %s)" % (c.get("handnumber") or "--", c.get("betround")))
            self.lbl_cards.config(text="your cards: %s" % (c.get("hero_cards") or "(none / not dealt in)"))
            self.lbl_board.config(text="board: %s" % (c.get("board") or "--"))
            self.lbl_pot.config(text="pot: %s   to call: %s" % (c.get("pot"), c.get("amount_to_call")))
        self.after(2000, self.refresh_ctx)

    def toggle_on_top(self):
        on = bool(self.on_top.get())
        self.wm_attributes("-topmost", on)
        self.prefs["always_on_top"] = on
        save_prefs(self.prefs)

    # ---- follow-up review of past decisions ----
    def refresh_followups(self):
        try:
            out = run_sql("SELECT id, handnumber, hero_cards, action, "
                          "COALESCE(amount::text,''), CASE WHEN self_liked IS NULL THEN '' "
                          "WHEN self_liked THEN ' [liked]' ELSE ' [disliked]' END "
                          "FROM learner_decisions ORDER BY id DESC LIMIT 20;", read=True)
        except Exception:
            out = ""
        self._fu_ids = []; labels = []
        for line in out.strip().splitlines():
            if not line.strip():
                continue
            f = line.split("|")
            while len(f) < 6: f.append("")
            self._fu_ids.append(f[0])
            labels.append("#%s  %s  %s %s%s" % (f[0], f[2] or "?", f[3], f[4], f[5]))
        self.fu_pick["values"] = labels
        if labels and not self.fu_pick.get():
            self.fu_pick.current(0)

    def submit_followup(self, liked):
        i = self.fu_pick.current()
        if i < 0 or i >= len(self._fu_ids):
            messagebox.showinfo("Pick one", "Choose a decision to review (use refresh).")
            return
        did = self._fu_ids[i]
        note = self.fu_note.get("1.0", "end").strip()
        try:
            run_sql("UPDATE learner_decisions SET self_liked=%s, self_feedback='%s', "
                    "feedback_ts=now() WHERE id=%s;" % ("true" if liked else "false", esc(note), did))
            self.fu_note.delete("1.0", "end")
            self.status.config(text="follow-up saved for decision #%s (%s)" % (did, "liked" if liked else "disliked"))
            self.refresh_followups()
        except Exception as e:
            messagebox.showerror("DB error", str(e))

    # ---- execute the action on the real table (via the bot's button-finding) ----
    def execute_on_table(self, action, amt):
        # Maps learner buttons to the bot's manual-action endpoint. Fold/Call/Allin
        # click the FCKRA button (found via label/state/button regions); Bet/Raise go
        # through the two-successive-clicks + numpad path with the amount in big blinds.
        q = "do=%s" % action
        if action in ("bet", "raise") and amt:
            q += "&amount=%s" % amt
        try:
            hiss_get("/api/action?" + q)
            return True, ""
        except Exception as e:
            return False, str(e)

    # ---- preset bet sizing (pot fractions / min), computed from the live pot ----
    def submit_bet(self, frac=None, min_bb=None):
        c = self.ctx or {}
        raw = c.get("raw", {})
        bb = (raw.get("limits", {}) or {}).get("bblind") or 1.0
        try:
            pot = float(c.get("pot") or 0)
        except Exception:
            pot = 0.0
        pot_bb = (pot / bb) if bb else pot
        if min_bb is not None:
            amt = float(min_bb)
        else:
            amt = round(frac * pot_bb, 2)
            if amt < 2:                 # never below a min open
                amt = 2.0
        self.submit_action("bet", amount=("%g" % amt))

    # ---- log a decision (and execute it on the table) ----
    def submit_action(self, action, amount=None):
        c = self.ctx or {}
        reasoning = self.reason.get("1.0", "end").strip()
        amt = (amount if amount is not None else self.amount.get()).strip()
        amt_sql = amt if amt else "NULL"
        def numornull(v):
            try: return str(float(v))
            except Exception: return "NULL"
        gs = json.dumps(c.get("raw", {}))
        sql = (
            "INSERT INTO learner_decisions "
            "(handnumber,betround,hero_cards,board,pot,amount_to_call,action,amount,reasoning,game_state) VALUES "
            "('%s',%s,'%s','%s',%s,%s,'%s',%s,'%s','%s'::jsonb);" % (
                esc(c.get("handnumber")),
                numornull(c.get("betround")),
                esc(c.get("hero_cards")), esc(c.get("board")),
                numornull(c.get("pot")), numornull(c.get("amount_to_call")),
                esc(action), amt_sql, esc(reasoning), esc(gs)))
        # 1) execute on the real table (pass through the autoplayer's button finding)
        ok, err = self.execute_on_table(action, amt)
        # 2) log the decision + reasoning for Claude to review
        try:
            run_sql(sql)
        except Exception as e:
            messagebox.showerror("DB error", str(e))
            return
        if ok:
            self.status.config(text="%s sent to table + logged  (%s)" % (action.upper(), c.get("hero_cards") or "?"))
        else:
            self.status.config(text="logged %s, but table action failed: %s" % (action.upper(), err))
        self.reason.delete("1.0", "end"); self.amount.delete(0, "end")

    # ---- questions
    def poll_questions(self):
        try:
            out = run_sql("SELECT id, replace(question, chr(10), ' ') FROM learner_questions "
                          "WHERE answered=false ORDER BY id LIMIT 1;", read=True).strip()
        except Exception:
            out = ""
        self.q_text.config(state="normal"); self.q_text.delete("1.0", "end")
        if out:
            qid, qtext = out.split("|", 1)
            self.cur_q = (qid, qtext)
            self.q_text.insert("1.0", qtext)
        else:
            self.cur_q = None
            self.q_text.insert("1.0", "(no pending questions)")
        self.q_text.config(state="disabled")
        self.after(3000, self.poll_questions)

    def send_answer(self):
        if not self.cur_q:
            messagebox.showinfo("No question", "There is no pending question to answer.")
            return
        ans = self.answer.get("1.0", "end").strip()
        if not ans:
            return
        try:
            run_sql("UPDATE learner_questions SET answer='%s', answered=true, answered_ts=now() "
                    "WHERE id=%s;" % (esc(ans), self.cur_q[0]))
            self.answer.delete("1.0", "end")
            self.status.config(text="answer sent for Q#%s" % self.cur_q[0])
            self.poll_questions()
        except Exception as e:
            messagebox.showerror("DB error", str(e))

if __name__ == "__main__":
    Learner().mainloop()
