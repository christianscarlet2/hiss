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

  // ---- Tabs (Terminal | AIL) -------------------------------------------------
  var tabsEl = document.getElementById("tabs");
  var activeTab = "terminal";
  function showTab(name) {
    activeTab = name;
    var btns = tabsEl.querySelectorAll(".tab");
    for (var i = 0; i < btns.length; i++) {
      btns[i].classList.toggle("active", btns[i].getAttribute("data-tab") === name);
    }
    var views = document.querySelectorAll(".tabview");
    for (var j = 0; j < views.length; j++) {
      var on = views[j].id === "tab-" + name;
      views[j].classList.toggle("active", on);
      if (on) views[j].removeAttribute("hidden"); else views[j].setAttribute("hidden", "");
    }
    if (name === "ail") { loadAudioDevices(); ailListPoll(); ailOutputPoll(); }
    if (name === "synapse") { renderSynapse(); pollSynapse(); }
    if (name === "hiss") {
      var f = document.getElementById("hiss-frame");   // lazy-load the React table view on first view
      if (f && !f.src) f.src = "/table-display";
    }
  }
  if (tabsEl) tabsEl.addEventListener("click", function (e) {
    var b = e.target && e.target.closest ? e.target.closest(".tab") : null;
    if (b) showTab(b.getAttribute("data-tab"));
  });

  // ---- AIL control server (cross-origin to :7900 on the same host) -----------
  var AIL_BASE = location.protocol + "//" + location.hostname + ":7900";
  var ailConn = document.getElementById("ail-conn");
  var ailSwitches = document.getElementById("ail-switches");
  var ailBody = document.getElementById("ail-body");
  var ailCursor = 0;
  var ailLines = [];
  var togglingUntil = {};   // name -> ts: keep the optimistic switch state briefly after a click
  var ailAudioDevices = null;   // [{index,name}] mic list for the Voice Feedback AIL
  var ailAudioSelected = null;  // selected device index (string) or null = default mic
  var ailAudioLoaded = false;

  function loadAudioDevices(force) {
    if (ailAudioLoaded && !force) return;
    ailAudioLoaded = true;
    fetch(AIL_BASE + "/ail/audio-devices", { cache: "no-store" })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        ailAudioDevices = d.devices || [];
        ailAudioSelected = (d.selected === undefined || d.selected === null || d.selected === "") ? null : String(d.selected);
        ailListPoll();   // re-render so the Voice card shows the populated dropdown
      })
      .catch(function () { ailAudioLoaded = false; });
  }

  function setAilConn(ok) {
    if (!ailConn) return;
    ailConn.textContent = ok ? "(ail server live)" : "(ail server unreachable on :7900)";
    ailConn.className = "ail-conn" + (ok ? "" : " down");
  }

  function renderAilList(ails) {
    var now = Date.now();
    var html = "";
    for (var i = 0; i < ails.length; i++) {
      var a = ails[i];
      var checked = a.enabled;
      if (togglingUntil[a.name] && now < togglingUntil[a.name]) checked = togglingUntil[a.name + "_on"];
      var run = a.enabled
        ? (a.running ? '<span class="run">running</span>' : '<span class="stale">starting&hellip;</span>')
        : "";
      // Voice Feedback gets a mic-device dropdown.
      var extra = "";
      if (a.name === "voice") {
        var opts = '<option value="default"' + (ailAudioSelected === null ? " selected" : "") + ">Default mic</option>";
        if (ailAudioDevices === null) {
          opts += '<option disabled>loading devices&hellip;</option>';
        } else if (ailAudioDevices.length === 0) {
          opts += '<option disabled>no input devices found</option>';
        } else {
          for (var k = 0; k < ailAudioDevices.length; k++) {
            var dv = ailAudioDevices[k];
            opts += '<option value="' + dv.index + '"' + (ailAudioSelected === String(dv.index) ? " selected" : "")
              + ">" + escapeHtml("in[" + dv.index + "] " + dv.name) + "</option>";
          }
        }
        extra = '<select class="ail-audio" data-ail-audio title="Microphone device for Voice Feedback">' + opts + "</select>";
      }
      html += '<div class="ail-card' + (checked ? " on" : "") + '">'
        + '<div class="ail-icon">' + (a.icon || "&bull;") + "</div>"
        + '<div class="ail-text"><div class="ail-name">' + escapeHtml(a.label) + run + "</div>"
        + '<div class="ail-desc">' + escapeHtml(a.desc || "") + "</div>" + extra + "</div>"
        + '<label class="ail-toggle"><input type="checkbox" data-ail="' + escapeHtml(a.name) + '"'
        + (checked ? " checked" : "") + '><span class="ail-slider"></span></label>'
        + "</div>";
    }
    ailSwitches.innerHTML = html || '<div class="ail-empty">no AILs registered</div>';
  }

  function ailListPoll() {
    fetch(AIL_BASE + "/ail/list", { cache: "no-store" })
      .then(function (r) { return r.json(); })
      .then(function (d) { setAilConn(true); renderAilList(d.ails || []); })
      .catch(function () { setAilConn(false); });
  }

  if (ailSwitches) ailSwitches.addEventListener("change", function (e) {
    var cb = e.target;
    // Voice Feedback mic-device dropdown.
    if (cb && cb.hasAttribute && cb.hasAttribute("data-ail-audio")) {
      var val = cb.value;
      ailAudioSelected = (val === "default") ? null : val;
      fetch(AIL_BASE + "/ail/audio-device?index=" + encodeURIComponent(val), { cache: "no-store" })
        .then(function (r) { return r.json(); })
        .then(function () { setTimeout(ailListPoll, 600); })
        .catch(function () {});
      return;
    }
    if (!cb || !cb.hasAttribute || !cb.hasAttribute("data-ail")) return;
    var name = cb.getAttribute("data-ail");
    var on = cb.checked;
    togglingUntil[name] = Date.now() + 4000; togglingUntil[name + "_on"] = on;
    fetch(AIL_BASE + "/ail/toggle?name=" + encodeURIComponent(name) + "&on=" + (on ? 1 : 0), { cache: "no-store" })
      .then(function (r) { return r.json(); })
      .then(function () { setTimeout(ailListPoll, 600); })
      .catch(function () { setAilConn(false); });
  });

  function ailOutputPoll() {
    fetch(AIL_BASE + "/ail/output?since=" + ailCursor, { cache: "no-store" })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        setAilConn(true);
        var lines = d.lines || [];
        if (lines.length) {
          var stick = atBottom(ailBody);
          for (var i = 0; i < lines.length; i++) {
            ailLines.push('<span class="src">[' + escapeHtml(lines[i].name) + "]</span> " + ansiToHtml(lines[i].text));
          }
          if (ailLines.length > 600) ailLines = ailLines.slice(ailLines.length - 600);
          ailBody.innerHTML = ailLines.join("\n");
          if (stick) ailBody.scrollTop = ailBody.scrollHeight;
        }
        if (typeof d.cursor === "number") ailCursor = d.cursor;
      })
      .catch(function () { setAilConn(false); });
  }

  // AIL timers tick only while the AIL tab is visible.
  setInterval(function () { if (activeTab === "ail") ailListPoll(); }, 2000);
  setInterval(function () { if (activeTab === "ail") ailOutputPoll(); }, 800);

  // ---- Synapse Harmonizer tab: steering dials, what each does, pairings, + live values ----
  var SYNAPSE_DIALS = [
    { id: "style", icon: "🎚", label: "Strategy Style  (f$Style)",
      live: { kind: "symbol", name: "f$Style", map: { "0": "Small Ball", "1": "Power Poker", "2": "Hybrid" } },
      control: "Terminal:  /strategy smallball | hybrid | power",
      does: "The MASTER playing-style dial — one switch that retunes opens, 3-bets/4-bets, c-bet frequency and bet-sizing across every street at once. Small Ball (0): many cheap flops, small pots, wide & tricky postflop [Negreanu]. Power Poker (1): big bets, c-bet 0.80, bigger 3/4-bet value range, maximum pressure [Brunson]. Hybrid (2, default): balanced blend.",
      pair: "C-bet Frequency (Power drives it to 0.80) · Bet SizeMult (multiplies the style's sizes by ICM×depth) · Stack Depth (Small Ball needs depth; short stacks revert to push/fold regardless) · Opponent type (Power punishes nits / foldy tables; Small Ball exploits calling stations cheaply)." },
    { id: "cbet", icon: "🎯", label: "C-bet Frequency  (f$CbetFreq)",
      live: { kind: "symbol", name: "f$CbetFreq" },
      control: "Follows Strategy Style (0.60 smallball / hybrid default / 0.80 power)",
      does: "How often the bot fires a continuation bet as the preflop raiser in a heads-up pot. Higher = more flop fold-equity & initiative; lower = pot-control vs sticky opponents.",
      pair: "Strategy Style (Power raises it) · Opponent type (drop vs calling stations, raise vs foldy regs) · board texture (parched boards keep firing)." },
    { id: "sizemult", icon: "📏", label: "Bet SizeMult  (f$ICM_SizeMult × f$DepthSizeMult)",
      live: { kind: "note", text: "computed live (not polled — cascades)" },
      control: "Automatic: ICM chip-value × stack-depth/leverage; fed by the lobby + effective stack",
      does: "Scales EVERY OHF bet by chip-value × leverage so the size is meaningful for the stack: deep stacks bet bigger (up to ~×1.5), ≤25bb → ×1.0 / push-fold, bubble tightens (~×0.80), ITM ~×0.90. Makes 2bb mean nothing to a 3000bb stack and everything to a 10bb stack.",
      pair: "Strategy Style (it multiplies the style's base sizes) · ICM (lobby_fetch → players_remaining → f$ICM_SizeMult + f$Stage) · Stack Depth (f$DepthSizeMult)." },
    { id: "nn", icon: "🧠", label: "NN Driver  (neural net)",
      live: { kind: "endpoint", url: "/api/nn-driver", field: "engaged" },
      control: "🧠 toolbar / AIL,  or  /api/nn-driver?on=1|0",
      does: "Replaces the OHF strategy with the trained PPO neural net for NLH — bypasses the OHF read and FORCES the action (the 'student' that learned from millions of hands). On PLO/PLO8 the OHF still drives (the NN is hold'em-only).",
      pair: "Autoplayer (the NN needs it as the executor) · ULTRA (auto-flips OHF↔NN) · Strategy Style (f$Style only matters while the OHF is driving — the NN ignores it on NLH)." },
    { id: "ultra", icon: "⚡", label: "ULTRA Mode",
      live: { kind: "endpoint", url: "/api/ultra", field: "engaged" },
      control: "⚡ toolbar,  or  /api/ultra?on=1|0",
      does: "A meta-controller that randomly flips the bot between OHF and NN using the system-audio average as the entropy source — mixes the master (OHF) and the student (NN) over time.",
      pair: "NN Driver (ULTRA is what toggles it) · Strategy Style (applies during the OHF phases) · Superstition (the 666 oracle sharpens in the NN phase)." },
    { id: "superstition", icon: "🔥", label: "Superstition — 666 Card Oracle  (sb_beastfavor)",
      live: { kind: "endpoint", url: "/api/superstition", field: "engaged" },
      control: "🔥666 toolbar,  or  /api/superstition?on=1|0",
      does: "The 666 Card Oracle reads the system-audio resonance with 666 and bends the next-card catch odds (×0.666..×1.666), exposed as the sb_beastfavor symbol + an OHF superstition overlay + spoken omens. Sharpened during the NN phase under ULTRA.",
      pair: "Beast Favor (the live resonance value it feeds) · ULTRA / NN Driver (sharpens in the NN phase)." },
    { id: "autoplayer", icon: "🎰", label: "Autoplayer  (executor)",
      live: { kind: "endpoint", url: "/api/autoplayer", field: "engaged" },
      control: "toolbar / loop,  or  /api/autoplayer?on=1|0",
      does: "The always-on executor that actually clicks the table. With NN/ULTRA off it runs the OHF; with NN on it executes the NN's forced action. Must be ON for any play to happen.",
      pair: "Everything — it's the hands that carry out whatever brain (OHF or NN) is steering." }
  ];

  var synWrap = document.getElementById("syn-wrap");
  var synConn = document.getElementById("syn-conn");
  var synLive = {};

  function setSynConn(ok) {
    if (!synConn) return;
    synConn.textContent = ok ? "(live)" : "(bot unreachable)";
    synConn.className = "syn-conn" + (ok ? "" : " down");
  }

  function renderSynapse() {
    if (!synWrap) return;
    var html = "";
    for (var i = 0; i < SYNAPSE_DIALS.length; i++) {
      var d = SYNAPSE_DIALS[i];
      var lv = synLive[d.id] || "&mdash;";
      html += '<div class="syn-card">'
        + '<div class="syn-top"><span class="syn-icon">' + d.icon + '</span>'
        + '<span class="syn-label">' + escapeHtml(d.label) + '</span>'
        + '<span class="syn-val" id="synval-' + d.id + '">' + lv + '</span></div>'
        + '<div class="syn-row"><span class="syn-k">does</span><span class="syn-v">' + escapeHtml(d.does) + '</span></div>'
        + '<div class="syn-row"><span class="syn-k">pair&nbsp;with</span><span class="syn-v">' + escapeHtml(d.pair) + '</span></div>'
        + '<div class="syn-row"><span class="syn-k">control</span><span class="syn-v mono">' + escapeHtml(d.control) + '</span></div>'
        + '</div>';
    }
    synWrap.innerHTML = html;
  }

  function setSynVal(id, str) {
    synLive[id] = str;
    var el = document.getElementById("synval-" + id);
    if (el) el.innerHTML = str;
  }

  function pollSynapse() {
    var symDials = SYNAPSE_DIALS.filter(function (d) { return d.live.kind === "symbol"; });
    if (symDials.length) {
      var names = symDials.map(function (d) { return d.live.name; }).join(",");
      fetch("/api/symbols?names=" + encodeURIComponent(names), { cache: "no-store" })
        .then(function (r) { return r.json(); })
        .then(function (j) {
          setSynConn(true);
          symDials.forEach(function (d) {
            var v = j[d.live.name];
            if (v === undefined || v === null) { setSynVal(d.id, "&mdash;"); return; }
            var num = Math.round(parseFloat(v) * 100) / 100;
            var s = String(num);
            if (d.live.map && d.live.map[String(num)] !== undefined) s = d.live.map[String(num)] + " (" + num + ")";
            setSynVal(d.id, "<b>" + escapeHtml(s) + "</b>");
          });
        })
        .catch(function () { setSynConn(false); });
    }
    SYNAPSE_DIALS.filter(function (d) { return d.live.kind === "endpoint"; }).forEach(function (d) {
      fetch(d.live.url, { cache: "no-store" })
        .then(function (r) { return r.json(); })
        .then(function (j) {
          setSynConn(true);
          setSynVal(d.id, j[d.live.field] ? '<b class="on">ON</b>' : '<span class="off">OFF</span>');
        })
        .catch(function () { setSynVal(d.id, "&mdash;"); });
    });
    SYNAPSE_DIALS.filter(function (d) { return d.live.kind === "note"; }).forEach(function (d) {
      setSynVal(d.id, '<span class="note">' + escapeHtml(d.live.text) + "</span>");
    });
  }

  setInterval(function () { if (activeTab === "synapse") pollSynapse(); }, 1500);

  poll();
  setInterval(poll, 600);
})();
