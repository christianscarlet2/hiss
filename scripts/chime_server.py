"""chime_server.py -- tiny HTTP chime endpoint for the Windows box.
GET /chime[?sound=chime.wav] -> plays a wav through the existing play_sound.ps1 (over the speakers).
Lets a REMOTE Claude session (linux) ping this machine to chime via a hook. Bind 0.0.0.0 so the LAN
can reach it (firewall rule 'HissChime' opens 28710). Windowless via pythonw + CREATE_NO_WINDOW.
"""
import os, subprocess
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = 28710
SOUNDS = r"C:\www\openholdembot_old\scripts\sounds"
PS = r"C:\www\openholdembot_old\scripts\play_sound.ps1"
CREATE_NO_WINDOW = 0x08000000

class H(BaseHTTPRequestHandler):
    def do_GET(self):
        snd = "chime.wav"
        if "sound=" in self.path:
            snd = os.path.basename(self.path.split("sound=", 1)[1].split("&", 1)[0]) or snd
        wav = os.path.join(SOUNDS, snd)
        if not os.path.isfile(wav):
            wav = os.path.join(SOUNDS, "chime.wav")
        try:
            subprocess.Popen(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                              "-File", PS, wav], creationflags=CREATE_NO_WINDOW)
            ok = True
        except Exception:
            ok = False
        self.send_response(200 if ok else 500)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write((("chimed %s\n" % snd) if ok else "error\n").encode())

    def log_message(self, *a):
        pass

if __name__ == "__main__":
    HTTPServer(("0.0.0.0", PORT), H).serve_forever()
