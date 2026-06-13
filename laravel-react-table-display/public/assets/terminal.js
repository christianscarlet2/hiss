(function () {
  "use strict";

  // ---- ANSI SGR -> HTML ------------------------------------------------------
  // Standard 16-colour palette (0-7 normal, 8-15 bright). Matches a dark
  // terminal theme close to the in-app libvterm panes.
  var PALETTE = [
    "#0c1117", "#ff5f5f", "#3df57a", "#f5d93d",
    "#5f9bff", "#c77dff", "#3df5e0", "#cdd6df",
    "#5a6b78", "#ff8787", "#7dffa8", "#ffe97d",
    "#9bc1ff", "#e0a8ff", "#7dfff0", "#ffffff"
  ];

  function escapeHtml(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }

  function color256(n) {
    if (n < 16) return PALETTE[n];
    if (n >= 232) {
      var v = 8 + (n - 232) * 10;
      return "rgb(" + v + "," + v + "," + v + ")";
    }
    n -= 16;
    var r = Math.floor(n / 36), g = Math.floor((n % 36) / 6), b = n % 6;
    var conv = function (c) { return c === 0 ? 0 : 55 + c * 40; };
    return "rgb(" + conv(r) + "," + conv(g) + "," + conv(b) + ")";
  }

  function defaultState() {
    return { fg: null, bg: null, bold: false, italic: false, underline: false, inverse: false };
  }

  function styleString(st) {
    var fg = st.fg, bg = st.bg;
    if (st.inverse) { var t = fg; fg = bg || "#0a0e12"; bg = t || "#3df57a"; }
    var css = [];
    if (fg) css.push("color:" + fg);
    if (bg) css.push("background:" + bg);
    if (st.bold) css.push("font-weight:bold");
    if (st.italic) css.push("font-style:italic");
    if (st.underline) css.push("text-decoration:underline");
    return css.join(";");
  }

  function applySgr(st, params) {
    for (var i = 0; i < params.length; i++) {
      var p = params[i];
      if (p === 0) { var d = defaultState(); st.fg = d.fg; st.bg = d.bg; st.bold = d.bold; st.italic = d.italic; st.underline = d.underline; st.inverse = d.inverse; }
      else if (p === 1) st.bold = true;
      else if (p === 22) st.bold = false;
      else if (p === 3) st.italic = true;
      else if (p === 23) st.italic = false;
      else if (p === 4) st.underline = true;
      else if (p === 24) st.underline = false;
      else if (p === 7) st.inverse = true;
      else if (p === 27) st.inverse = false;
      else if (p >= 30 && p <= 37) st.fg = PALETTE[p - 30];
      else if (p >= 90 && p <= 97) st.fg = PALETTE[p - 90 + 8];
      else if (p >= 40 && p <= 47) st.bg = PALETTE[p - 40];
      else if (p >= 100 && p <= 107) st.bg = PALETTE[p - 100 + 8];
      else if (p === 39) st.fg = null;
      else if (p === 49) st.bg = null;
      else if (p === 38 || p === 48) {
        var isFg = (p === 38);
        var mode = params[i + 1];
        if (mode === 5) { var c = color256(params[i + 2] | 0); if (isFg) st.fg = c; else st.bg = c; i += 2; }
        else if (mode === 2) { var col = "rgb(" + (params[i + 2] | 0) + "," + (params[i + 3] | 0) + "," + (params[i + 4] | 0) + ")"; if (isFg) st.fg = col; else st.bg = col; i += 4; }
      }
    }
  }

  // Convert a string containing ANSI escape sequences to HTML. Recognises SGR
  // (\x1b[...m). Other CSI / cursor sequences (e.g. clear-screen, home) are
  // stripped so they do not appear as literal garbage.
  function ansiToHtml(text) {
    var st = defaultState();
    var out = "";
    var open = false;
    var i = 0;
    var n = text.length;
    function flushOpen() { if (open) { out += "</span>"; open = false; } }
    function openSpan() { flushOpen(); var s = styleString(st); if (s) { out += '<span style="' + s + '">'; open = true; } }
    var pendingText = "";
    function emit() { if (pendingText) { out += escapeHtml(pendingText); pendingText = ""; } }

    while (i < n) {
      var ch = text.charCodeAt(i);
      if (ch === 0x1b && i + 1 < n) {
        var next = text[i + 1];
        if (next === "[") {
          // CSI
          var j = i + 2;
          var seq = "";
          while (j < n) {
            var c = text[j];
            if (c >= "@" && c <= "~") break; // final byte
            seq += c; j++;
          }
          var final = text[j] || "";
          if (final === "m") {
            emit();
            var params = seq.split(";").map(function (x) { return x === "" ? 0 : parseInt(x, 10); });
            applySgr(st, params);
            openSpan();
          }
          // else: cursor/clear/etc — drop it
          i = j + 1;
          continue;
        } else {
          // other escape (e.g. \x1b H) — skip the escape + 1 char
          i += 2;
          continue;
        }
      }
      pendingText += text[i];
      i++;
    }
    emit();
    flushOpen();
    return out;
  }

  // ---- DOM + polling ---------------------------------------------------------
  var panes = [
    document.getElementById("pane-0"),
    document.getElementById("pane-1"),
    document.getElementById("pane-2"),
    document.getElementById("pane-3")
  ];
  var statusEl = document.getElementById("status");
  var lastHtml = ["", "", "", ""];

  function atBottom(el) {
    return el.scrollHeight - el.scrollTop - el.clientHeight < 24;
  }

  function render(idx, text) {
    var html = ansiToHtml(text || "");
    if (html === lastHtml[idx]) return;
    var stick = atBottom(panes[idx]);
    panes[idx].innerHTML = html;
    lastHtml[idx] = html;
    if (stick) panes[idx].scrollTop = panes[idx].scrollHeight;
  }

  function setStatus(txt, cls) {
    statusEl.textContent = txt;
    statusEl.className = "status" + (cls ? " " + cls : "");
  }

  function poll() {
    fetch("/api/terminal-state", { cache: "no-store" })
      .then(function (r) { return r.json(); })
      .then(function (data) {
        setStatus("live", "live");
        var secs = data.sections || [];
        // Pane 1 = State: prefer the pinned (fixed) block when present.
        var stateText = (data.pinned && data.pinned.length) ? data.pinned : (secs[1] || "");
        render(0, secs[0] || "");
        render(1, stateText);
        render(2, secs[2] || "");
        render(3, secs[3] || "");
      })
      .catch(function () { setStatus("disconnected", "down"); });
  }

  // ---- prompt input ----------------------------------------------------------
  var form = document.getElementById("prompt-form");
  var input = document.getElementById("prompt-input");
  var history = [];
  var histIdx = -1;

  function sendCmd(cmd) {
    if (!cmd) return;
    fetch("/api/terminal-input?text=" + encodeURIComponent(cmd), { method: "POST" })
      .then(function () { poll(); })
      .catch(function () {});
  }

  form.addEventListener("submit", function (e) {
    e.preventDefault();
    var cmd = input.value.trim();
    if (!cmd) return;
    history.push(cmd);
    histIdx = history.length;
    input.value = "";
    sendCmd(cmd);
  });

  // Steering buttons (hijack / stop / play) -> same console-command channel.
  var hijackBtn = document.getElementById("hijack-btn");
  if (hijackBtn) hijackBtn.addEventListener("click", function () { sendCmd("/hijack"); });
  var stopBtn = document.getElementById("stop-btn");
  if (stopBtn) stopBtn.addEventListener("click", function () { sendCmd("/stop"); });
  var playBtn = document.getElementById("play-btn");
  if (playBtn) playBtn.addEventListener("click", function () { sendCmd("/play"); });

  input.addEventListener("keydown", function (e) {
    if (e.key === "ArrowUp") {
      if (histIdx > 0) { histIdx--; input.value = history[histIdx]; e.preventDefault(); }
    } else if (e.key === "ArrowDown") {
      if (histIdx < history.length - 1) { histIdx++; input.value = history[histIdx]; }
      else { histIdx = history.length; input.value = ""; }
      e.preventDefault();
    }
  });

  poll();
  setInterval(poll, 600);
})();
