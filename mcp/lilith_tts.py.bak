"""
lilith_tts.py -- shared Lilith (ElevenLabs) read-aloud used by BOTH learner.exe and
the MCP server / tilt detector, so the bot side can speak even when learner isn't open.

speak(text): generate the audio FIRST (sound stays on during the slow synth), then
mute every app except scrcpy + ACR Poker ~3s before reading, play it (blocking), and
unmute afterward. Config (API key + voice id) is read from ~/.hiss_learner.json.

Pure standard library + pycaw (for per-app mute). Safe to call with no key/voice
(it just no-ops with a message on stderr).
"""

import os, json, sys, time, tempfile, ctypes, urllib.request

# The windowless (console=False) PyInstaller build has sys.stdout/stderr/stdin = None, so any write or
# read would crash (that's why lilith popped an "unhandled exception" dialog). Back them with devnull so
# the read-aloud runs silently with no console and no crash. [Emrald: lilith must never pop a window]
for _nm in ("stdout", "stderr"):
    if getattr(sys, _nm, None) is None:
        try: setattr(sys, _nm, open(os.devnull, "w"))
        except Exception: pass
if getattr(sys, "stdin", None) is None:
    try: sys.stdin = open(os.devnull, "r")
    except Exception: pass

PREF_FILE = os.path.join(os.path.expanduser("~"), ".hiss_learner.json")
KEEP_UNMUTED = ["scrcpy", "acrpoker"]   # process-name substrings that stay audible


def _prefs():
    try:
        return json.load(open(PREF_FILE))
    except Exception:
        return {}


def set_app_mutes(mute_others, keep_names=KEEP_UNMUTED):
    try:
        from pycaw.pycaw import AudioUtilities, ISimpleAudioVolume
    except Exception:
        return
    for s in AudioUtilities.GetAllSessions():
        try:
            if not s.Process:
                continue
            name = (s.Process.name() or "").lower()
            vol = s._ctl.QueryInterface(ISimpleAudioVolume)
            if not mute_others:
                vol.SetMute(0, None)
            else:
                keep = any(k in name for k in keep_names)
                vol.SetMute(0 if keep else 1, None)
        except Exception:
            pass


def eleven_tts(api_key, voice_id, text, out_path):
    body = json.dumps({
        "text": text,
        "model_id": "eleven_multilingual_v2",
        "voice_settings": {"stability": 0.5, "similarity_boost": 0.75},
    }).encode("utf-8")
    req = urllib.request.Request(
        "https://api.elevenlabs.io/v1/text-to-speech/%s" % voice_id,
        data=body, method="POST",
        headers={"xi-api-key": api_key, "Content-Type": "application/json",
                 "Accept": "audio/mpeg"})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = r.read()
    with open(out_path, "wb") as f:
        f.write(data)
    return out_path


def play_audio(path):
    # Blocks until playback finishes so the caller can unmute afterward.
    winmm = ctypes.windll.winmm
    winmm.mciSendStringW("close lilithq", None, 0, None)
    winmm.mciSendStringW('open "%s" type mpegvideo alias lilithq' % path, None, 0, None)
    winmm.mciSendStringW("play lilithq wait", None, 0, None)
    winmm.mciSendStringW("close lilithq", None, 0, None)


def sapi_tts(text):
    """FREE, offline fallback: the built-in Windows System.Speech voice (no API key, no credits). Synthesizes
    AND plays in one call (blocks until done). Prefers a female voice to match Lilith. [Emrald: free TTS fallback]"""
    import subprocess
    safe = (text or "").replace("'", "''")
    ps = ("Add-Type -AssemblyName System.Speech; "
          "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
          "$s.Rate = 0; $s.Volume = 100; "
          "try { $s.SelectVoiceByHints([System.Speech.Synthesis.VoiceGender]::Female) } catch {}; "
          "$s.Speak('%s'); $s.Dispose()" % safe)
    # CREATE_NO_WINDOW (0x08000000): never flash a console. Generous timeout for long lines.
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", ps],
                   timeout=180, creationflags=0x08000000)


def speak(text, lead_secs=3.0):
    """Read `text` aloud. ElevenLabs (premium) if a working key is configured; otherwise -- or if it fails
    (e.g. 401 / OUT OF CREDITS / no network) -- fall back to the FREE built-in Windows voice so Lilith always
    speaks. Returns (ok, message). [Emrald: free computer-voice fallback when ElevenLabs has no credits]"""
    text = (text or "").strip()
    if not text:
        return False, "empty text"
    p = _prefs()
    key = p.get("elevenlabs_api_key", "")
    voice = p.get("elevenlabs_voice_id", "")
    out = None
    engine = "SAPI"
    # 1) Try to SYNTH with ElevenLabs first (sound still on during the slow network synth). On ANY failure,
    #    out stays None and we fall through to the free SAPI voice below.
    if key and voice:
        try:
            out = os.path.join(tempfile.gettempdir(), "lilith_say.mp3")
            eleven_tts(key, voice, text, out)
            engine = "elevenlabs"
        except Exception as e:
            out = None
            sys.stderr.write("[lilith] elevenlabs synth failed (%s) -> free SAPI voice\n" % e)
    muted = False
    try:
        set_app_mutes(True); muted = True          # mute others (keep scrcpy + ACR)
        time.sleep(lead_secs)                       # ~3s before reading
        if out:
            play_audio(out)                         # ElevenLabs MP3
        else:
            sapi_tts(text)                          # FREE Windows voice (synth + play)
        return True, "spoke %d chars (%s)" % (len(text), engine)
    except Exception as e:
        return False, "tts error: %s" % e
    finally:
        if muted:
            set_app_mutes(False)                    # turn sound back on


if __name__ == "__main__":
    # CLI: lilith.exe "text to read"   (or pipe text on stdin)
    msg = " ".join(sys.argv[1:]).strip() or sys.stdin.read().strip()
    ok, info = speak(msg)
    sys.stderr.write("[lilith] %s: %s\n" % ("ok" if ok else "fail", info))
    sys.exit(0 if ok else 1)
