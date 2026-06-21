# HISS strategy directives — the brain-era rewrite spine (Emrald, 2026-06-21)

The OHF + NN-driver + brain rewrite must honor ALL of these. They are the philosophy, not options.

1. **EXPLOIT- & PLAYER-FOCUSED, not card/gamestate-focused.** Decisions are driven first by who the
   villain is and how to exploit them (introspection, HUD profiling, observer branch, timing tells,
   range reads); the hole cards + board are a secondary input. The brain commands both the OHF and the
   NN-driver; the card model is the fallback, not the master.

2. **Player profiling is a first-class input.** HUD stats (VPIP/PFR/AF/fold-to-3bet/cbet/etc., gametype-
   separated) + the per-opponent introspection model (rhythm/exploits/timing/range/tilt) weight the
   decision heavily.

3. **PRETEND / REPRESENTATION (tied to MISCHIEF).** In the right spots, represent a hand you don't have:
   bet heavy when you catch a scare card, or barrel a line that credibly reps the nuts, to FOLD the
   villain out — especially when you can PREDICT their cards from their betting pattern + introspection +
   intuition. Pick the pathway that maximizes CURRENT AND FUTURE profit.

4. **SMALL BALL — run the entire table.** Default to getting involved in many pots, betting/acting often,
   applying constant small pressure, and brute-forcing positive-EV exits from the situations you create.
   Volume of +EV spots over waiting for premiums. (Reverts to small/tight when the table punishes it or
   the session-trend says "don't get fancy".)

5. **Predict villain cards from betting patterns + introspection**, and reason over the exploit-pathway
   FORKS (what each line makes them do) before committing — confirm the most profitable pathway.

6. **Add synapses, dials/knobs, symbols, and technology as necessary.** New engine symbols, knob
   channels, synapses, and infra are in-scope for the rebuild whenever they sharpen the above.

7. **Pot-committed never folds dust** (already in f$preflop both trees); **all-in commits the full stack**
   (round up — no dust left; autoplayer RaiseMax/betsize path, in the rebuild).

8. **The bot must ACT.** The isfinalanswer turn-latch + the SB-engine heartbeat-stall gate are
   non-negotiable: a brilliant strategy that never clicks is worthless.

GOTO branches (f$ObsStrategy, observer-driven): ISOLATE_SHORTSTACK, ATTACK_FOLDERS, PUNISH_TILT,
VALUE_STATION, DONKFEST_CHEAP, STEADY, PRESS_DOWNTREND — each sets aggro/bluff/openrange + mischief
affinity, and the OHF/NN obey with EXPLOIT PRECEDENCE.
