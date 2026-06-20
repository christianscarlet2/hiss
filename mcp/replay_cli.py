#!/usr/bin/env python3
"""Standalone replay CLI for the AIL -- replay_hands / replay_stream / replay_frame via Bash,
INDEPENDENT of the live MCP server.

Why this exists: the hiss-bot MCP server is a long-lived child of a persistent `claude` session and
does NOT reload mcp/hiss_mcp_server.py when that file changes. A stale server therefore keeps serving
old code (e.g. the historical "local variable 'urllib' referenced before assignment" bug in the
replay handlers) for every AIL cycle until the whole session restarts -- blinding the loop's #1
diagnostic. This wrapper imports the CURRENT on-disk hiss_mcp_server.py and calls the SAME call_tool()
handlers out-of-process, so the AIL can always investigate a hand with fresh code.

Usage (from the repo root):
  python mcp/replay_cli.py hands                  # list recent hands on the replay server
  python mcp/replay_cli.py stream <hand> [ts_ms]  # hh_text + symbols + scrapes + OHF decision/trace
  python mcp/replay_cli.py frame  <hand> [ts_ms]  # save the heartbeat frame PNG to C:/tmp and print path
  python mcp/replay_cli.py latest <hand>          # latest captured ts_ms for a hand
(ts_ms omitted -> the latest frame of that hand.)
"""
import sys, os, json, base64, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))


def _load_mcp():
    # Import the CURRENT on-disk module (module-level code is just imports + constants; main()/the
    # stdio server only run under __main__, so this does not start a server).
    spec = importlib.util.spec_from_file_location("hiss_mcp_server", os.path.join(HERE, "hiss_mcp_server.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _emit(out, frame_tag=None):
    """Print call_tool's content list; decode any image to a PNG under C:/tmp."""
    for item in (out or []):
        t = item.get("type")
        if t == "text":
            print(item.get("text", ""))
        elif t == "image":
            tmp = "C:/tmp" if os.path.isdir("C:/tmp") else (os.environ.get("TEMP") or ".")
            path = os.path.join(tmp, "replay_frame_%s.png" % (frame_tag or "out"))
            with open(path, "wb") as f:
                f.write(base64.b64decode(item.get("data", "")))
            print("[saved frame PNG -> %s]" % path)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1].lower()
    hms = _load_mcp()
    if cmd == "hands":
        _emit(hms.call_tool("replay_hands", {}))
    elif cmd == "latest":
        if len(sys.argv) < 3:
            print("hand required"); return 2
        print(hms.replay_latest_ts(sys.argv[2]))
    elif cmd in ("stream", "frame"):
        if len(sys.argv) < 3:
            print("hand required"); return 2
        args = {"hand": sys.argv[2]}
        if len(sys.argv) > 3:
            args["ts"] = int(sys.argv[3])
        name = "replay_stream" if cmd == "stream" else "replay_frame"
        _emit(hms.call_tool(name, args), frame_tag="%s_%s" % (sys.argv[2], args.get("ts", "latest")))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
