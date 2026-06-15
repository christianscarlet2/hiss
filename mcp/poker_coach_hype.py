#!/usr/bin/env python3
"""poker_coach_hype.py - the always-on hype pulse + pre-tournament speech for the
poker-coach skill (Lilith's voice).

  * Every 5-10 minutes (random) it speaks a short hype/wisdom line.
  * 5 minutes before a scheduled tournament start it delivers an elaborate ~5-minute
    elite-player prep speech (once per start).

Speaks via Release/lilith.exe (ElevenLabs; mutes all apps except scrcpy/ACRPoker,
plays, unmutes) and also logs each line to the postgres `coach_notes` table
(spoken=true) so it shows in learner.exe's "Poker coach (Lilith)" panel.

Tournament start times are read from the postgres settings record
`tournament_schedule` (JSON list of "HH:MM" 24h local). Default: ["18:05"].

Run:  nohup python mcp/poker_coach_hype.py > Release/logs/poker_coach.log 2>&1 &
Stop: kill the python process (the loop-stop path also kills it).
"""
import os, sys, json, time, random, subprocess

PSQL    = os.environ.get("HISS_PSQL", r"C:\Program Files\PostgreSQL\12\bin\psql.exe")
PGUSER  = os.environ.get("PGUSER", "postgres")
PGDB    = os.environ.get("PGDATABASE", "hiss")
PGPASS  = os.environ.get("PGPASSWORD", "dbpass")
RELEASE = os.environ.get("HISS_RELEASE", r"C:\www\openholdembot_old\Release")
LILITH  = os.path.join(RELEASE, "lilith.exe")

HYPE_MIN_S = int(os.environ.get("COACH_HYPE_MIN", "300"))   # 5 min
HYPE_MAX_S = int(os.environ.get("COACH_HYPE_MAX", "600"))   # 10 min
SPEECH_LEAD_S = 300                                          # speak 5 min before start
TICK_S = 30


def log(*a):
    print("[coach]", *a, file=sys.stderr, flush=True)


def psql(sql):
    env = dict(os.environ); env["PGPASSWORD"] = PGPASS
    cmd = [PSQL, "-U", PGUSER, "-d", PGDB, "-t", "-A", "-c", sql]
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=30)
    return p.stdout.strip() if p.returncode == 0 else ""


def esc(s):
    return s.replace("'", "''")


def schedule_today():
    """Return a list of (HH, MM) tournament starts from settings.tournament_schedule."""
    raw = psql("SELECT value FROM settings WHERE key='tournament_schedule';")
    times = ["18:05"]
    if raw:
        try:
            j = json.loads(raw)
            if isinstance(j, list) and j:
                times = [str(t) for t in j]
        except Exception:
            pass
    out = []
    for t in times:
        try:
            hh, mm = t.split(":"); out.append((int(hh), int(mm), t))
        except Exception:
            continue
    return out


def speak(text, kind="support"):
    """Speak via lilith.exe (blocking so lines never overlap) + log a coach_note."""
    log("speak (%s, %d chars): %s" % (kind, len(text), text[:60]))
    try:
        psql("INSERT INTO coach_notes (kind,priority,message,spoken) VALUES "
             "('%s',1,'%s',true);" % (kind, esc(text)))
    except Exception as e:
        log("coach_note insert failed:", e)
    try:
        subprocess.run([LILITH, text], cwd=RELEASE, timeout=420)
    except Exception as e:
        log("lilith failed:", e)


HYPE_LINES = [
    "Emrald. Eyes up. Tight is right early, but when you raise, you mean it.",
    "Patience is a weapon. Fold the trash, pounce on the spot. Your stack thanks you later.",
    "Position is power. In late, you steal; out of position, you respect. Simple, ruthless.",
    "Every fold you don't love is a chip you keep for the hand you do. Discipline pays the bills.",
    "You're not here to play hands. You're here to win chips. Pick your battles like a sniper.",
    "Negreanu small-ball: cheap pots, big position, let them hang themselves. Stay slippery.",
    "Brunson power-poker when the table's scared: bet big, take it down, no apologies.",
    "Ivey reads the soul, not the cards. Watch how they bet, not just what they bet.",
    "Hellmuth's secret? He folds. The hero call feels good and costs you the tournament.",
    "Stack in big blinds, always. Thirty deep you maneuver. Ten deep you jam. Know your gear.",
    "Aggression is a story. Tell it consistently and they'll fold the better hand.",
    "Don't tilt off a cooler. Right play, wrong card, next hand. The math doesn't care about feelings.",
    "Bubble's a bully's playground if you're big. If you're short, ladder up and live.",
    "The button is your throne. Open it wide, three-bet the openers, make them hate your seat.",
    "Suited connectors are a loan, not a gift. Cheap to see a flop, easy to let go.",
    "When in doubt, pot odds. The pot's laying you a price; do the math, then trust it.",
    "A min-raise is a question. A real raise is a statement. Don't whisper when you should roar.",
    "Protect your big blind, but don't go broke defending trash. Pick the playable, fold the rest.",
    "Short stack? Jam don't limp. Fold equity is the last weapon you've got. Use it first-in.",
    "You built a machine that jams ace-king for its last chip. Bring that same nerve.",
    "Read the table: who's scared, who's stuck, who's gambling. Sit to the left of the wild ones.",
    "Confidence, not arrogance. You've done the work. Now let the reps carry you.",
    "Don't pay off the nit. When the rock raises the river, your one pair is a museum piece.",
    "Win the small pots with position, the big pots with the nuts. Don't flip it around.",
    "Breathe. Big stack or short stack, the next decision is the only one that matters. Make it clean.",
    # --- The TAO of Poker (Larry Phillips) -- the mental game ---
    "TAO of Poker: see how many hands you can fold. Make each hand prove it is good before a chip goes in.",
    "If you're beat, fold. Forget the idea that they're bluffing. Most of the time, they have it.",
    "Discipline must be kept up until the very end. The last hour is where tournaments are won and lost.",
    "Patience is the weapon. The skilled player simply waits for a good hand, then bets it hard.",
    "When the bad beat comes, stay unflappable. Clinical detachment. The next hand does not know what just happened.",
    "Never deliberately tilt to make something happen. Steaming is how good players go broke.",
    "Calm and composure under pressure is the whole game. When you feel strong emotion, that is the moment to slow down.",
    "Do not bluff the calling station. In a loose game you still have to win pots. Value bet, don't donate.",
    "Trust your read even when it means folding again and again. An accurate read that folds is a winning read.",
    "The biggest leak in a soft game is impatience: getting over-eager to put your chips in play. Wait.",
    "Bet your premiums. When you finally get the hand, make them pay. Don't get fancy with the nuts.",
    "Stay unpredictable. Mix it up so they never know if your bet means strength or air.",
]

ELABORATE_SPEECH = (
    "Alright Emrald, listen close, because this is the five minutes that set the next few hours. "
    "The cards are about to fly, and right now, before a single hole card hits the felt, is when "
    "champions are made. Not on the river. Here. In your head. "
    "First, your mindset. You are not hoping tonight. You are hunting. Hope is for the people who "
    "limp in and pray. You have a plan, you have a machine behind you, and you have done the work. "
    "Sit down like you own the table, because for the next few hours, you do. "
    "Now the early game. Stacks are deep, the blinds are nothing, and the biggest mistake amateurs "
    "make is playing too many hands for too many chips. That is not us. Early, we play small ball. "
    "Cheap flops in position, fold the marginal junk, and let the maniacs spew their stacks to each "
    "other. You do not need to win the tournament in level one. You just need to still be here in "
    "level ten with chips to operate. Patience now buys violence later. "
    "Watch the table like Phil Ivey. In the first orbit, you are gathering intelligence. Who limps "
    "and folds. Who three-bets light. Who can not fold top pair. Who is on their phone, half asleep. "
    "By the time the antes kick in, you should know exactly who to steal from and exactly who to "
    "stay out of the way of. Information is the edge nobody can cooler. "
    "As the blinds climb, shift gears. This is Doyle Brunson territory now. When the table tightens "
    "up and everyone is scared to bust, you apply the pressure. You raise. You three-bet. You make "
    "them fold the slightly better hand because they are terrified of the bubble and you are not. "
    "A big stack with a brave heart is the most dangerous thing in the room. Be that. "
    "But know your stack in big blinds at all times. Above forty, you maneuver and play poker. "
    "Twenty to forty, you tighten and look for clean spots to get it in. Under fifteen, you stop "
    "limping, you stop calling, you jam. First in, with fold equity, every time. The slow death is "
    "blinding out while you wait for a hand. Do not die with chips in your stack. "
    "And the discipline, the Hellmuth in you: the hero call will whisper to you all night. It will "
    "say, he might be bluffing, pay him off, just this once. Most of the time, he is not bluffing. "
    "Fold. The chips you save by laying down one pair when the rock wakes up are the chips you shove "
    "with ace-king three hands later. Folding is not weakness. It is how you stay alive to win. "
    "When the bad beat comes, and it will come, you take it like a professional. Right play, wrong "
    "card. You got it in ahead, the math was on your side, the river was not. That is not your "
    "failure, that is variance, and variance is the price of admission to every pot you are a "
    "favorite in. Reload your focus. The very next hand does not know what just happened, and "
    "neither should your face. "
    "Near the money, the chips change value. Far from the bubble, a chip is just a chip, so you "
    "accumulate. But as the bubble nears, the chips you can lose are worth more than the chips you "
    "can win. If you are big, you bully. If you are short, you ladder, you survive, you let the "
    "other short stacks bust first. Then once you are in the money, you stop playing not to lose and "
    "you start playing to win it. Second place is the first loser, but you have to cash to get there. "
    "So here is your intention for tonight. Tight and sharp early. Aggressive and fearless in the "
    "middle. Disciplined under pressure. Ruthless on the bubble. And calm, always calm, when the "
    "cards betray you. You are not gambling. You are grinding an edge, hand after hand, decision "
    "after decision, until the chips are all in front of you. "
    "You have done the work, Emrald. The machine is sharp, the reads are coming, the nerve is there. "
    "Now go sit down, take a breath, and take their stacks. Let's get burning."
)


def main():
    log("poker coach hype daemon up. hype every %d-%ds; speech %ds before scheduled starts."
        % (HYPE_MIN_S, HYPE_MAX_S, SPEECH_LEAD_S))
    last_hype = time.time()
    next_gap = random.randint(HYPE_MIN_S, HYPE_MAX_S)
    delivered = set()   # tournament starts already given the speech (label keyed by date+HH:MM)
    while True:
        now = time.localtime()
        now_secs = now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec
        day_key = "%d-%d" % (now.tm_yday, now.tm_year)
        # Pre-tournament elite speech (5 min before each scheduled start)
        for hh, mm, label in schedule_today():
            start_secs = hh * 3600 + mm * 60
            lead = start_secs - now_secs
            key = "%s@%s" % (day_key, label)
            if SPEECH_LEAD_S - TICK_S <= lead <= SPEECH_LEAD_S + TICK_S and key not in delivered:
                delivered.add(key)
                log("pre-tournament speech for %s (%ds out)" % (label, lead))
                speak(ELABORATE_SPEECH, kind="strategy")
                last_hype = time.time()   # don't hype right after the big speech
                next_gap = random.randint(HYPE_MIN_S, HYPE_MAX_S)
        # Random hype pulse
        if time.time() - last_hype >= next_gap:
            speak(random.choice(HYPE_LINES), kind="support")
            last_hype = time.time()
            next_gap = random.randint(HYPE_MIN_S, HYPE_MAX_S)
            log("next hype in %ds" % next_gap)
        time.sleep(TICK_S)


if __name__ == "__main__":
    main()
