#!/usr/bin/env python3
"""lilith_sts_convert.py -- batch ElevenLabs speech-to-speech (voice changer) to the Lilith voice.

Converts every .mp3 in a folder to the Lilith voice. Long files are split into chunks (the STS
endpoint won't take a 47-min file in one call), each chunk is converted, then re-joined with ffmpeg.
Resumable: finished outputs and already-converted chunks are skipped, so a re-run continues.

  python lilith_sts_convert.py W:\castaneda [W:\castaneda\lilith]

Reads the key from ~/.hiss_learner.json (needs the speech_to_speech permission). Voice + model
are overridable via env. NOTE: STS bills per minute of source audio -- this whole folder is
~12 hours, so it is a large spend. Test on one file first (pass a single .mp3 as arg 1).
"""
import os, sys, json, glob, subprocess, urllib.request, urllib.error

VOICE   = os.environ.get("LILITH_VOICE_ID", "mLw8kuDeVGqVstOYjRII")
MODEL   = os.environ.get("LILITH_STS_MODEL", "eleven_multilingual_sts_v2")
CHUNK_S = int(os.environ.get("LILITH_CHUNK_S", "300"))          # seconds per chunk
OUTFMT  = os.environ.get("LILITH_OUTPUT_FORMAT", "mp3_44100_128")
STABIL  = os.environ.get("LILITH_STABILITY", "0.5")
SIMIL   = os.environ.get("LILITH_SIMILARITY", "0.85")
KEY     = json.load(open(os.path.expanduser("~/.hiss_learner.json"))).get("elevenlabs_api_key", "")


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def duration(path):
    r = run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path])
    try:
        return float(r.stdout.strip())
    except ValueError:
        return 0.0


def split_chunks(src, workdir):
    """Split src into CHUNK_S-second mp3 chunks (stream-copy, fast). Returns ordered chunk paths."""
    os.makedirs(workdir, exist_ok=True)
    pat = os.path.join(workdir, "chunk_%04d.mp3")
    if not glob.glob(os.path.join(workdir, "chunk_*.mp3")):
        run(["ffmpeg", "-y", "-i", src, "-f", "segment", "-segment_time", str(CHUNK_S),
             "-c", "copy", pat])
    return sorted(glob.glob(os.path.join(workdir, "chunk_*.mp3")))


def sts(in_path, out_path):
    """Convert one chunk via ElevenLabs speech-to-speech. Returns (ok, msg)."""
    url = "https://api.elevenlabs.io/v1/speech-to-speech/%s?output_format=%s" % (VOICE, OUTFMT)
    vs = '{"stability":%s,"similarity_boost":%s}' % (STABIL, SIMIL)
    # curl handles multipart cleanly and is reliable on this box (urllib LAN/TLS quirks aside).
    r = run(["curl", "-sS", "-o", out_path, "-w", "%{http_code}", "-X", "POST", url,
             "-H", "xi-api-key: " + KEY, "-F", "audio=@" + in_path,
             "-F", "model_id=" + MODEL, "-F", "voice_settings=" + vs])
    code = r.stdout.strip()[-3:]
    if code != "200":
        body = ""
        try:
            body = open(out_path, "rb").read()[:300].decode("utf-8", "ignore")
        except Exception:
            pass
        if os.path.exists(out_path):
            os.remove(out_path)                  # don't leave an error-JSON as a "chunk"
        return False, "HTTP %s %s" % (code, body)
    return True, "ok"


def join_chunks(chunk_outs, final_path):
    listfile = final_path + ".concat.txt"
    with open(listfile, "w") as f:
        for c in chunk_outs:
            f.write("file '%s'\n" % c.replace("\\", "/"))
    run(["ffmpeg", "-y", "-f", "concat", "-safe", "0", "-i", listfile, "-c", "copy", final_path])
    os.remove(listfile)


def convert_file(src, outdir):
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
        ok, msg = sts(ch, co)
        if not ok:
            print("[%s] chunk %d/%d FAILED: %s" % (base, i + 1, len(chunks), msg), flush=True)
            return False
        outs.append(co)
        print("  chunk %d/%d ok" % (i + 1, len(chunks)), flush=True)
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
    print("voice=%s model=%s chunk=%ss out=%s  files=%d" % (VOICE, MODEL, CHUNK_S, outdir, len(files)), flush=True)
    ok = sum(1 for f in files if convert_file(f, outdir))
    print("=== %d/%d files converted ===" % (ok, len(files)), flush=True)


if __name__ == "__main__":
    main()
