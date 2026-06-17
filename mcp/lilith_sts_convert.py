#!/usr/bin/env python3
"""lilith_sts_convert.py -- batch ElevenLabs speech-to-speech (voice changer) to the Lilith voice.

Converts every .mp3 in a folder to the Lilith voice. Long files are split into chunks (the STS
endpoint won't take a 47-min file in one call), each chunk is converted, then re-joined with ffmpeg.
Resumable: finished outputs and already-converted chunks are skipped, so a re-run continues.

CREDIT RESERVE: polls the account balance before every chunk and STOPS before the balance would
drop below LILITH_RESERVE_CREDITS (default 75000). Each STS call's real cost is read from the
`character-cost` response header and tallied.

  python lilith_sts_convert.py W:\castaneda [W:\castaneda\lilith]

Reads the key from ~/.hiss_learner.json (needs speech_to_speech + user_read permissions).
"""
import os, sys, json, glob, subprocess

VOICE   = os.environ.get("LILITH_VOICE_ID", "mLw8kuDeVGqVstOYjRII")
MODEL   = os.environ.get("LILITH_STS_MODEL", "eleven_multilingual_sts_v2")
CHUNK_S = int(os.environ.get("LILITH_CHUNK_S", "300"))             # seconds per chunk
OUTFMT  = os.environ.get("LILITH_OUTPUT_FORMAT", "mp3_44100_128")
STABIL  = os.environ.get("LILITH_STABILITY", "0.5")
SIMIL   = os.environ.get("LILITH_SIMILARITY", "0.85")
RESERVE = int(os.environ.get("LILITH_RESERVE_CREDITS", "75000"))   # never spend below this
RATE_PM = float(os.environ.get("LILITH_RATE_PER_MIN", "1700"))     # pre-spend estimate (obs ~1610), high=safe
KEY     = json.load(open(os.path.expanduser("~/.hiss_learner.json"))).get("elevenlabs_api_key", "")
SUB_URL = "https://api.elevenlabs.io/v1/user/subscription"
SPENT   = 0


class StopBudget(Exception):
    pass


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def duration(path):
    r = run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path])
    try:
        return float(r.stdout.strip())
    except ValueError:
        return 0.0


def remaining_credits():
    """limit - used, or None if the balance can't be read."""
    r = run(["curl", "-sS", SUB_URL, "-H", "xi-api-key: " + KEY])
    try:
        d = json.loads(r.stdout)
        return int(d["character_limit"]) - int(d["character_count"])
    except Exception:
        return None


def split_chunks(src, workdir):
    """Split src into CHUNK_S-second mp3 chunks (stream-copy, fast). Returns ordered chunk paths."""
    os.makedirs(workdir, exist_ok=True)
    pat = os.path.join(workdir, "chunk_%04d.mp3")
    if not glob.glob(os.path.join(workdir, "chunk_*.mp3")):
        run(["ffmpeg", "-y", "-i", src, "-f", "segment", "-segment_time", str(CHUNK_S),
             "-c", "copy", pat])
    return sorted(glob.glob(os.path.join(workdir, "chunk_*.mp3")))


def sts(in_path, out_path):
    """Convert one chunk via ElevenLabs speech-to-speech. Returns (ok, msg, character_cost)."""
    url = "https://api.elevenlabs.io/v1/speech-to-speech/%s?output_format=%s" % (VOICE, OUTFMT)
    vs = '{"stability":%s,"similarity_boost":%s}' % (STABIL, SIMIL)
    hdr = out_path + ".hdr"
    r = run(["curl", "-sS", "-D", hdr, "-o", out_path, "-w", "%{http_code}", "-X", "POST", url,
             "-H", "xi-api-key: " + KEY, "-F", "audio=@" + in_path,
             "-F", "model_id=" + MODEL, "-F", "voice_settings=" + vs])
    code = r.stdout.strip()[-3:]
    cost = 0
    try:
        for line in open(hdr, encoding="utf-8", errors="ignore"):
            if line.lower().startswith("character-cost:"):
                cost = int(line.split(":", 1)[1].strip())
    except Exception:
        pass
    if os.path.exists(hdr):
        os.remove(hdr)
    if code != "200":
        body = ""
        try:
            body = open(out_path, "rb").read()[:300].decode("utf-8", "ignore")
        except Exception:
            pass
        if os.path.exists(out_path):
            os.remove(out_path)               # don't leave an error-JSON as a "chunk"
        return False, "HTTP %s %s" % (code, body), 0
    return True, "ok", cost


def join_chunks(chunk_outs, final_path):
    listfile = final_path + ".concat.txt"
    with open(listfile, "w") as f:
        for c in chunk_outs:
            f.write("file '%s'\n" % c.replace("\\", "/"))
    run(["ffmpeg", "-y", "-f", "concat", "-safe", "0", "-i", listfile, "-c", "copy", final_path])
    os.remove(listfile)


def convert_file(src, outdir):
    global SPENT
    base = os.path.splitext(os.path.basename(src))[0]
    final = os.path.join(outdir, base + ".mp3")
    if os.path.exists(final) and os.path.getsize(final) > 1000:
        print("[skip] %s already done" % base, flush=True)
        return True
    work = os.path.join(outdir, "_work", base)
    chunks = split_chunks(src, work)
    print("[%s] %.1f min -> %d chunk(s)" % (base, duration(src) / 60.0, len(chunks)), flush=True)
    outs = []
    for i, ch in enumerate(chunks):
        co = os.path.join(work, "out_%04d.mp3" % i)
        if os.path.exists(co) and os.path.getsize(co) > 1000:
            outs.append(co); continue
        rem = remaining_credits()
        est = duration(ch) / 60.0 * RATE_PM
        if rem is not None and (rem - est) < RESERVE:
            print("[budget] STOP: remaining %d - est chunk %d would breach %d reserve"
                  % (rem, int(est), RESERVE), flush=True)
            raise StopBudget()
        ok, msg, cost = sts(ch, co)
        if not ok:
            print("[%s] chunk %d/%d FAILED: %s" % (base, i + 1, len(chunks), msg), flush=True)
            # Out of credits / quota also looks like a non-200 -> stop the whole run.
            if "quota" in msg.lower() or "401" in msg or "402" in msg or "429" in msg:
                raise StopBudget()
            return False
        outs.append(co); SPENT += cost
        print("  chunk %d/%d ok (cost %d, run-spent %d, ~rem %s)"
              % (i + 1, len(chunks), cost, SPENT, (rem - cost) if rem is not None else "?"), flush=True)
    join_chunks(outs, final)
    print("[%s] DONE -> %s" % (base, final), flush=True)
    return True


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    src = sys.argv[1]
    files = [src] if src.lower().endswith(".mp3") else sorted(glob.glob(os.path.join(src, "*.mp3")))
    outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(files[0]) if files else ".", "lilith")
    os.makedirs(outdir, exist_ok=True)
    if not KEY:
        print("no elevenlabs key in ~/.hiss_learner.json"); sys.exit(1)
    rem0 = remaining_credits()
    print("voice=%s model=%s chunk=%ss reserve=%d start_balance=%s out=%s files=%d"
          % (VOICE, MODEL, CHUNK_S, RESERVE, rem0, outdir, len(files)), flush=True)
    ok = 0
    try:
        for f in files:
            if convert_file(f, outdir):
                ok += 1
    except StopBudget:
        print("=== hit the %d-credit reserve; stopping clean ===" % RESERVE, flush=True)
    remN = remaining_credits()
    print("=== %d/%d files fully converted | this run spent ~%d | balance now %s ==="
          % (ok, len(files), SPENT, remN), flush=True)


if __name__ == "__main__":
    main()
