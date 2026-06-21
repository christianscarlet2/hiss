#!/usr/bin/env python3
"""fastlink.py -- the direct TCP/UDP fast-path between BEAST and SWIFTSNAKE.

Beats the postgres bus latency on the HOT dispatch (the per-decision pathway-EV + deep-thought
requests) with a raw socket. Graceful: if the peer is down it FALLS BACK to the postgres bus, so it
is never a single point of failure.

  serve(handler)                 -- TCP responder (on swiftsnake): handler(req_dict) -> reply_dict
  request(payload, timeout)      -- BEAST -> swiftsnake request/reply (bus fallback)
  send_event(channel, payload)   -- UDP fire-and-forget event (bus.publish fallback)
  recv_events(on_msg)            -- UDP listener
"""
import os, sys, json, socket, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PEER = os.environ.get("FASTLINK_PEER", "192.168.1.39")    # the other machine (swiftsnake from BEAST)
TCP_PORT = int(os.environ.get("FASTLINK_TCP", "8077"))
UDP_PORT = int(os.environ.get("FASTLINK_UDP", "8078"))
HOST = os.environ.get("FASTLINK_HOST", "0.0.0.0")


def _recv_line(conn):
    buf = b""
    while not buf.endswith(b"\n"):
        ch = conn.recv(65536)
        if not ch:
            break
        buf += ch
    return buf


def serve(handler):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, TCP_PORT)); s.listen(64)
    print("[fastlink] TCP responder on %s:%d" % (HOST, TCP_PORT), flush=True)
    while True:
        conn, _addr = s.accept()
        threading.Thread(target=_serve_one, args=(conn, handler), daemon=True).start()


def _serve_one(conn, handler):
    try:
        req = json.loads(_recv_line(conn).decode("utf-8", "replace") or "{}")
        rep = handler(req) or {}
        conn.sendall((json.dumps(rep) + "\n").encode())
    except Exception as e:
        try:
            conn.sendall((json.dumps({"error": str(e)}) + "\n").encode())
        except Exception:
            pass
    finally:
        try:
            conn.close()
        except Exception:
            pass


def request(payload, timeout=3.0, peer=None):
    """Fast TCP request/reply; falls back to the postgres bus work-queue if the peer is unreachable."""
    try:
        s = socket.create_connection((peer or PEER, TCP_PORT), timeout=timeout)
        s.sendall((json.dumps(payload) + "\n").encode())
        s.settimeout(timeout)
        data = _recv_line(s); s.close()
        return json.loads(data.decode("utf-8", "replace") or "{}")
    except Exception:
        try:
            import bus
            wid = bus.enqueue(payload.get("kind", "pathway_eval"), payload)
            return bus.await_result(wid, timeout * 3) or {}
        except Exception:
            return {}


def send_event(channel, payload, peer=None):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sendto((channel + "\t" + json.dumps(payload)).encode(), (peer or PEER, UDP_PORT))
        s.close()
    except Exception:
        try:
            import bus; bus.publish(channel, payload)
        except Exception:
            pass


def recv_events(on_msg):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((HOST, UDP_PORT))
    while True:
        data, _ = s.recvfrom(65536)
        try:
            ch, _, body = data.decode("utf-8", "replace").partition("\t")
            on_msg(ch, json.loads(body) if body else {})
        except Exception:
            pass


if __name__ == "__main__":
    if "--serve" in sys.argv:
        serve(lambda req: {"pong": req})
    else:
        # loopback self-test: spin the responder in a thread, then round-trip a request to it.
        threading.Thread(target=lambda: serve(lambda req: {"pong": req}), daemon=True).start()
        time.sleep(0.3)
        print("fastlink loopback:", request({"kind": "ping", "x": 1}, 2.0, peer="127.0.0.1"))
