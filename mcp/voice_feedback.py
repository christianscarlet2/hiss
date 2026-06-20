#!/usr/bin/env python3
"""voice_feedback.py -- spoken feedback loop for Hiss.

Records the mic, transcribes each spoken utterance (faster-whisper, offline), timestamps it, pins it
to the LIVE hand the bot is in, and stores it as a replay-aligned INDICATOR in postgres
`voice_feedback`. The improvement loop (AIL) then reads these and acts on your feedback.

Flow per utterance:
  mic --(energy VAD)--> wav buffer --(whisper)--> text
  + ts_ms (epoch) + bot context (/api/table-state, /api/symbols, mode)
  -> INSERT voice_feedback(ts_ms, handnumber, betround, board, hero_cards, pot, mode, transcript, category)

ts_ms + handnumber align each note to the replay frames (logs/frames/<ms>.bmp) and hands, so the
advanced replay system can mark exactly where you spoke. A light keyword category tags the marker
colour (praise / criticism / instruction / question / note); the AIL does the real interpretation.

  python voice_feedback.py [--bot-url http://127.0.0.1:27654] [--device <idx|name>] [--list] [--model base.en]
"""
import sys, os, time, json, urllib.request, threading, queue
import numpy as np

# Whisper transcripts can contain characters outside Windows' cp1252 console codepage; without this,
# print()-ing such a transcript raises UnicodeEncodeError and kills the daemon. Force UTF-8 output.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

SR = 16000                      # whisper-native sample rate
BLOCK = 480                     # 30 ms blocks
HANG_S = 0.8                    # trailing silence that ends an utterance
MIN_S = 0.4                     # ignore blips shorter than this
MAX_S = 25.0                    # force-flush a very long utterance
DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")


def _argval(flag, default=None):
    if flag in sys.argv:
        i = sys.argv.index(flag)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


BOT   = (_argval("--bot-url") or "http://127.0.0.1:27654").rstrip("/")
MODEL = _argval("--model") or "base.en"
DEVICE_ARG = _argval("--device")


def _get(url):
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception:
        return {}


def now_ms():
    return int(time.time() * 1000)


def categorize(t):
    s = t.lower()
    if "?" in t or s.split()[:1] and s.split()[0] in ("why", "what", "how", "when", "should", "shouldn't"):
        cat = "question"
    elif any(w in s for w in ("bad", "wrong", "mistake", "terrible", "hate", "awful", "shouldn't", "should not", "don't", "do not")):
        cat = "criticism"
    elif any(w in s for w in ("good", "nice", "great", "perfect", "love", "well played", "beautiful", "yes")):
        cat = "praise"
    elif any(w in s for w in ("raise", "fold", "call", "bet", "always", "never", "make it", "chase", "stop", "start", "size", "bigger", "smaller", "tighter", "looser")):
        cat = "instruction"
    else:
        cat = "note"
    sent = 1.0 if cat == "praise" else (-1.0 if cat == "criticism" else 0.0)
    return cat, sent


def hand_context():
    ts = _get(BOT + "/api/table-state")
    sym = _get(BOT + "/api/symbols?names=betround,nouts")
    mode = "OHF"
    if _get(BOT + "/api/nn-driver").get("engaged"):
        mode = "NN"
    elif _get(BOT + "/api/ultra").get("engaged"):
        mode = "ULTRA"
    board = " ".join(c for c in (ts.get("commonCards") or []) if c)
    hero = ""
    uc = ts.get("userchair", -1)
    for p in (ts.get("players") or []):
        if p.get("chair") == uc:
            hero = " ".join(c for c in (p.get("cards") or []) if c and c != "BACK")
            break
    try:
        betround = int(float(sym.get("betround") or 0))
    except Exception:
        betround = 0
    return {
        "handnumber": str(ts.get("handnumber") or ""),
        "betround": betround,
        "board": board,
        "hero_cards": hero,
        "pot": float(ts.get("pot") or 0),
        "mode": mode,
    }


def store(conn, ts_ms, text, ctx, cat, sent):
    cur = conn.cursor()
    cur.execute(
        "INSERT INTO voice_feedback (ts_ms, handnumber, betround, board, hero_cards, pot, mode, transcript, category, sentiment) "
        "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)",
        (ts_ms, ctx["handnumber"], ctx["betround"], ctx["board"], ctx["hero_cards"],
         ctx["pot"], ctx["mode"], text, cat, sent))
    # Also drop a replay INDICATOR into the existing debug stream: hiss_shipper.py already drains
    # hiss_log_debug to the replay system (replay.html / replayer), so each spoken note shows up
    # aligned to this exact hand (by handnumber + ts_ms) with NO server-side changes.
    cur.execute(
        "INSERT INTO hiss_log_debug (ts_ms, handnumber, category, line, shipped) "
        "VALUES (%s,%s,'voice',%s,false)",
        (ts_ms, ctx["handnumber"], "[%s] %s" % (cat.upper(), text)))
    conn.commit()


def _setting_mic_device():
    """The mic chosen in Hiss Preferences > Audio (postgres settings:
    key='audio', value->>'mic_device'). Empty / missing => use the system default.
    Set from CDlgSAPrefs26; lets the AIL voice loop honor the GUI picker with no
    --device flag."""
    try:
        import psycopg2
        c = psycopg2.connect(DSN)
        cur = c.cursor()
        cur.execute("SELECT value->>'mic_device' FROM settings WHERE key='audio'")
        row = cur.fetchone()
        c.close()
        if row and row[0]:
            return row[0].strip()
    except Exception:
        pass
    return None


def pick_device():
    import sounddevice as sd
    # Command-line --device wins; otherwise fall back to the Preferences > Audio mic.
    want = DEVICE_ARG if DEVICE_ARG is not None else _setting_mic_device()
    if not want:
        return None  # default input
    try:
        return int(want)
    except ValueError:
        for i, d in enumerate(sd.query_devices()):
            if d["max_input_channels"] > 0 and want.lower() in d["name"].lower():
                return i
    return None


def main():
    import sounddevice as sd
    if "--list" in sys.argv:
        for i, d in enumerate(sd.query_devices()):
            if d["max_input_channels"] > 0:
                print("in[%d] %s" % (i, d["name"]))
        return

    import psycopg2
    from faster_whisper import WhisperModel
    conn = psycopg2.connect(DSN)
    print("[voice] loading whisper '%s' (first run downloads the model)..." % MODEL, flush=True)
    model = WhisperModel(MODEL, device="cpu", compute_type="int8")
    dev = pick_device()
    print("[voice] feedback loop online -> %s | mic device=%s | speak and it's pinned to the live hand"
          % (BOT, dev if dev is not None else "default"), flush=True)

    q = queue.Queue()

    def cb(indata, frames, time_info, status):
        q.put(indata[:, 0].copy())

    # ---- energy VAD: noise floor calibration ----
    buf = []
    voiced = []
    in_speech = False
    silence_blocks = 0
    hang_blocks = int(HANG_S * SR / BLOCK)
    min_blocks = int(MIN_S * SR / BLOCK)
    max_blocks = int(MAX_S * SR / BLOCK)
    noise = 0.005
    calib = []

    with sd.InputStream(samplerate=SR, blocksize=BLOCK, channels=1, dtype="float32",
                        device=dev, callback=cb):
        # ~1s ambient calibration
        t0 = time.time()
        while time.time() - t0 < 1.0:
            try:
                calib.append(np.sqrt(np.mean(q.get(timeout=1.0) ** 2)))
            except queue.Empty:
                break
        if calib:
            noise = max(0.003, float(np.median(calib)))
        start_th = max(0.012, noise * 3.0)
        end_th = max(0.008, noise * 2.0)
        print("[voice] calibrated: noise=%.4f start=%.4f end=%.4f -- listening." % (noise, start_th, end_th), flush=True)

        while True:
            try:
                block = q.get(timeout=1.0)
            except queue.Empty:
                continue
            rms = float(np.sqrt(np.mean(block ** 2)))
            if not in_speech:
                if rms >= start_th:
                    in_speech = True
                    voiced = [block]
                    silence_blocks = 0
            else:
                voiced.append(block)
                if rms < end_th:
                    silence_blocks += 1
                else:
                    silence_blocks = 0
                if silence_blocks >= hang_blocks or len(voiced) >= max_blocks:
                    end_ms = now_ms()
                    in_speech = False
                    if len(voiced) - silence_blocks < min_blocks:
                        continue  # too short -> ignore
                    audio = np.concatenate(voiced).astype(np.float32)
                    voiced = []
                    try:
                        # vad_filter (Silero) drops non-speech so whisper stops hallucinating on
                        # music/silence; condition_on_previous_text=False kills the "Okay. Okay.
                        # Okay." repetition loops that flooded the feedback queue. (AIL 2026-06-20)
                        segs, _ = model.transcribe(audio, language="en", beam_size=1,
                                                   vad_filter=True,
                                                   condition_on_previous_text=False,
                                                   no_speech_threshold=0.6)
                        text = " ".join(s.text for s in segs).strip()
                    except Exception as e:
                        print("[voice] transcribe error:", e, flush=True)
                        continue
                    if not text or len(text) < 2:
                        continue
                    ctx = hand_context()
                    cat, sent = categorize(text)
                    try:
                        store(conn, end_ms, text, ctx, cat, sent)
                    except Exception as e:
                        conn.rollback()
                        print("[voice] db error:", e, flush=True)
                    print("[voice] [%s|%s hand %s board:%s] %s"
                          % (cat.upper(), ctx["mode"], ctx["handnumber"] or "-", ctx["board"] or "-", text), flush=True)


if __name__ == "__main__":
    main()
