(function () {
  var tbody = document.getElementById('tbody');
  var statusEl = document.getElementById('status');
  var captureBtn = document.getElementById('capture');
  var clearAllBtn = document.getElementById('clearAll');
  var undoBtn = document.getElementById('undo');
  var emptyHint = document.getElementById('empty');

  var rendered = {};      // gid -> { tr, input }
  var defaultGroup = 0;   // sticky group new rows default to
  var gcArmed = false;    // global eyedropper armed? (pick from any thumbnail)
  var fontTransform = document.getElementById('fontTransform');
  var currentTransform = 'AutoOcr0';   // transform used to auto-suggest the CHAR
  var autoCapture = document.getElementById('autoCapture');
  var autoRemoveAccounted = document.getElementById('autoRemoveAccounted');
  var glyphbarSummary = document.getElementById('glyphbarSummary');
  var glyphbarSummary2 = document.getElementById('glyphbarSummary2');
  var covRow1 = document.getElementById('covRow1');
  var covRow2 = document.getElementById('covRow2');
  var cov2Row1 = document.getElementById('cov2Row1');
  var cov2Row2 = document.getElementById('cov2Row2');
  var covBlock2 = document.getElementById('covBlock2');
  var numColoursSel = document.getElementById('numColours');
  var numColours = 2;     // "Do you have multiple colors?" default = 2
  var colourCov = {};     // group -> {1:{char:true}, 2:{char:true}} (persisted in localStorage)

  function api(method, url, cb) {
    var xhr = new XMLHttpRequest();
    xhr.open(method, url, true);
    xhr.onreadystatechange = function () {
      if (xhr.readyState === 4) {
        var data = null;
        try { data = JSON.parse(xhr.responseText); } catch (e) {}
        if (cb) cb(xhr.status, data);
      }
    };
    xhr.send();
  }

  // Reload a row's reference (regular) images after the server regenerates them
  // (the OCR step rebuilds the reference with a darkest-colour border).
  function reloadRefImage(gid) {
    var rec = rendered[gid];
    if (!rec) return;
    imgVer++;
    var bust = '&v=' + imgVer;
    var grImg = rec.tr.querySelector('td.gr img');
    if (grImg) grImg.src = '/api/fonts/glyph/regular?gid=' + gid + bust;
    if (rec.editTr) {
      var picks = rec.editTr.querySelectorAll('img.pick');
      if (picks.length) picks[0].src = '/api/fonts/glyph/regular?gid=' + gid + bust;   // reference pick
    }
  }

  // Ask the server to OCR this glyph's REFERENCE image (the glyph sub-crop, not the
  // whole scrape) with the current transform and pre-fill the CHAR input. Only fills
  // an empty field, so it never clobbers a value the user already typed. The server
  // also regenerates the reference with a 3px darkest-colour border, so reload it.
  function ocrFill(gid, input) {
    api('GET', '/api/fonts/glyph/ocr?gid=' + gid + '&t=' + encodeURIComponent(currentTransform),
      function (status, data) {
        if (data && data.ok) {
          if (data.ch && !input.value) input.value = data.ch;
          reloadRefImage(gid);
        }
      });
  }

  function retab() {
    var rows = tbody.querySelectorAll('tr');
    var t = 1;
    for (var i = 0; i < rows.length; i++) {
      var inp = rows[i].querySelector('input.ch');
      if (inp) inp.tabIndex = t++;
    }
  }

  // After a rescan rebuilds the table, jump to the first still-unlabeled glyph so
  // the user can keep labeling top-to-bottom without reaching for the mouse.
  function focusFirstEmpty() {
    var inputs = tbody.querySelectorAll('input.ch');
    for (var i = 0; i < inputs.length; i++) {
      if (!inputs[i].value) { inputs[i].focus(); inputs[i].select(); return; }
    }
  }

  // Toggle the table-wide "scanning" state that reveals the per-row spinners.
  function setScanning(on) {
    if (on) tbody.classList.add('scanning'); else tbody.classList.remove('scanning');
  }

  function clamp255(n) { n = parseInt(n, 10); if (isNaN(n)) n = 0; return n < 0 ? 0 : (n > 255 ? 255 : n); }

  // Cache-buster so a regenerated glyph's <img> elements reload instead of showing
  // the stale cached PNG.
  var imgVer = 0;

  // ---- Eyedropper magnifier (vision.exe style: zoomed pixel grid + crosshair) ----
  var mag = document.createElement('canvas');
  mag.id = 'magnifier';
  mag.width = 144; mag.height = 144;
  mag.style.display = 'none';
  document.body.appendChild(mag);
  var magCtx = mag.getContext('2d');

  function showMag(imgEl, ev) {
    if (!imgEl.naturalWidth) return;
    var rect = imgEl.getBoundingClientRect();
    var nx = Math.floor((ev.clientX - rect.left) / rect.width * imgEl.naturalWidth);
    var ny = Math.floor((ev.clientY - rect.top) / rect.height * imgEl.naturalHeight);
    var srcWin = 48;                 // natural PNG pixels shown across the magnifier
    magCtx.imageSmoothingEnabled = false;
    magCtx.fillStyle = '#202020';
    magCtx.fillRect(0, 0, mag.width, mag.height);
    magCtx.drawImage(imgEl, nx - srcWin / 2, ny - srcWin / 2, srcWin, srcWin,
      0, 0, mag.width, mag.height);
    // Centered crosshair: black shadow then white, with a small gap in the middle.
    var cx = mag.width / 2, cy = mag.height / 2;
    function cross(color, w) {
      magCtx.strokeStyle = color; magCtx.lineWidth = w; magCtx.beginPath();
      magCtx.moveTo(cx - 9, cy); magCtx.lineTo(cx - 3, cy);
      magCtx.moveTo(cx + 3, cy); magCtx.lineTo(cx + 9, cy);
      magCtx.moveTo(cx, cy - 9); magCtx.lineTo(cx, cy - 3);
      magCtx.moveTo(cx, cy + 3); magCtx.lineTo(cx, cy + 9);
      magCtx.stroke();
    }
    cross('#000', 3); cross('#fff', 1);
    magCtx.strokeStyle = '#fff'; magCtx.lineWidth = 1;
    magCtx.strokeRect(0.5, 0.5, mag.width - 1, mag.height - 1);
    // Keep it on-screen, offset from the cursor.
    var mx = ev.clientX + 18, my = ev.clientY + 18;
    if (mx + mag.width > window.innerWidth) mx = ev.clientX - mag.width - 18;
    if (my + mag.height > window.innerHeight) my = ev.clientY - mag.height - 18;
    mag.style.left = mx + 'px';
    mag.style.top = my + 'px';
    mag.style.display = 'block';
  }
  function hideMag() { mag.style.display = 'none'; }

  // Map a click/hover on a pixelated image to its natural pixel coordinate.
  function naturalXY(imgEl, ev) {
    var rect = imgEl.getBoundingClientRect();
    return {
      x: Math.floor((ev.clientX - rect.left) / rect.width * imgEl.naturalWidth),
      y: Math.floor((ev.clientY - rect.top) / rect.height * imgEl.naturalHeight)
    };
  }

  function buildRow(gl) {
    var tr = document.createElement('tr');
    tr.setAttribute('data-gid', gl.gid);

    var tdG = document.createElement('td');
    tdG.className = 'g';
    var img = document.createElement('img');
    img.src = '/api/fonts/glyph/image?gid=' + gl.gid;
    tdG.appendChild(img);
    tr.appendChild(tdG);

    var tdGR = document.createElement('td');
    tdGR.className = 'gr';
    var imgR = document.createElement('img');
    imgR.src = '/api/fonts/glyph/regular?gid=' + gl.gid;
    tdGR.appendChild(imgR);
    tr.appendChild(tdGR);

    var tdGF = document.createElement('td');
    tdGF.className = 'gf';
    var imgF = document.createElement('img');
    imgF.src = '/api/fonts/glyph/full?gid=' + gl.gid;
    tdGF.appendChild(imgF);
    tr.appendChild(tdGF);

    // When the global eyedropper is armed, the Reference/Scrape thumbnails become
    // pick targets that feed the toolbar colour (with the live magnifier).
    function armPick(imgEl, which) {
      imgEl.addEventListener('mousemove', function (e) { if (gcArmed) showMag(imgEl, e); });
      imgEl.addEventListener('mouseleave', hideMag);
      imgEl.addEventListener('click', function (e) {
        if (!gcArmed) return;
        e.preventDefault();
        var p = naturalXY(imgEl, e);
        api('GET', '/api/fonts/glyph/pixel?gid=' + gl.gid + '&img=' + which + '&px=' + p.x + '&py=' + p.y,
          function (status, data) { if (data && data.ok) gcSetPicked(data.a, data.r, data.g, data.b); });
      });
    }
    armPick(imgR, 'regular');
    armPick(imgF, 'full');

    // Per-row colour state (seeded from region r$), the chosen colour (1/2), and the
    // font group this glyph belongs to (used for the colour-coverage footer).
    var st = { a: clamp255(gl.a), r: clamp255(gl.r), g: clamp255(gl.g), b: clamp255(gl.b),
               radius: parseInt(gl.radius, 10) || 0 };
    var rowColour = 1;
    var rowGroup = (typeof gl.group === 'number') ? gl.group : defaultGroup;

    function reloadImages() {
      imgVer++;
      var bust = '&v=' + imgVer;
      img.src = '/api/fonts/glyph/image?gid=' + gl.gid + bust;     // mask thumbnail
      imgR.src = '/api/fonts/glyph/regular?gid=' + gl.gid + bust;  // reference thumbnail
    }

    var tdC = document.createElement('td');
    tdC.className = 'c';
    var input = document.createElement('input');
    input.className = 'ch';
    input.type = 'text';
    input.maxLength = 1;
    input.value = gl.assigned || '';
    if (input.value) input.classList.add('done');
    else ocrFill(gl.gid, input);   // suggest the character via the chosen transform
    // Saving is now an explicit action: C1/C2 (or F1/F2) create the font under that
    // colour. Typing only edits the character -- it never saves or removes the row.
    function saveChar(which) {
      var val = input.value;
      if (!val) return;   // colour chosen as a preview only; nothing to save yet
      var rec = rendered[gl.gid];
      // Warn before (re)creating a font that would duplicate something already there:
      // the same character already saved under this colour in this group takes
      // precedence (the case the coverage footer tracks); otherwise the glyph shape
      // itself may already have a font (accounted). Show at most one confirm.
      var cov = ensureCov(rowGroup);
      var warn = null;
      if (cov[which] && cov[which][val]) {
        warn = 'Colour ' + which + ' already has a font for "' + val + '" in Text' + rowGroup
             + '.\nContinue and (re)create it?';
      } else if (rec && rec.accounted) {
        warn = 'This glyph has already been accounted for.\nContinue and (re)create the font?';
      }
      if (warn && !confirm(warn)) return;
      input.classList.add('done');
      setScanning(true);
      api('POST', '/api/fonts/setchar?gid=' + gl.gid + '&ch=' + encodeURIComponent(val), function () {
        markColour(rowGroup, which, val);   // C1 -> colour-1 row, C2 -> colour-2 row
        refresh(function () { setScanning(false); focusFirstEmpty(); });
      });
    }
    tdC.appendChild(input);
    var spin = document.createElement('span');
    spin.className = 'spin';   // visible only while tbody has the .scanning class
    tdC.appendChild(spin);
    tr.appendChild(tdC);

    var tdR = document.createElement('td');
    tdR.className = 'r';
    tdR.textContent = gl.region + ' · Text' + gl.group;
    tdR.title = gl.hexmash || '';
    tr.appendChild(tdR);

    function removeRow(refocus) {
      // Capture the previous input before removal so we can return focus to it.
      var prevInput = null;
      if (refocus) {
        var ins = tbody.querySelectorAll('input.ch');
        for (var k = 0; k < ins.length; k++) {
          if (ins[k] === input) { if (k > 0) prevInput = ins[k - 1]; break; }
        }
      }
      api('POST', '/api/fonts/delete?gid=' + gl.gid, function () {
        if (tr.parentNode) tr.parentNode.removeChild(tr);
        delete rendered[gl.gid];
        retab();
        if (refocus) {
          var target = (prevInput && tbody.contains(prevInput)) ? prevInput : tbody.querySelector('input.ch');
          if (target) { target.focus(); target.select(); }
        }
      });
    }

    var tdA = document.createElement('td');
    tdA.className = 'a';
    // Colour 1 / Colour 2 chooser: copy the chosen global colour into this row and
    // regenerate. The chosen colour decides which footer coverage row this glyph
    // counts toward when saved. F1/F2 trigger these (see the row keydown below).
    var col1Btn = document.createElement('button');
    col1Btn.textContent = 'C1'; col1Btn.tabIndex = -1; col1Btn.className = 'colsel'; col1Btn.title = 'Use Colour 1 (F1)';
    var col2Btn = document.createElement('button');
    col2Btn.textContent = 'C2'; col2Btn.tabIndex = -1; col2Btn.className = 'colsel'; col2Btn.title = 'Use Colour 2 (F2)';
    function chooseColour(which) {
      rowColour = which;
      var c = colourVals(which);
      st.a = c.a; st.r = c.r; st.g = c.g; st.b = c.b; st.radius = c.radius;
      col1Btn.classList.toggle('on', which === 1);
      col2Btn.classList.toggle('on', which === 2);
      // Re-segment the glyph with the chosen colour, then save the font under it.
      var q = '/api/fonts/glyph/regen?gid=' + gl.gid
        + '&a=' + st.a + '&r=' + st.r + '&g=' + st.g + '&b=' + st.b + '&radius=' + st.radius;
      api('POST', q, function () { reloadImages(); saveChar(which); });
    }
    col1Btn.addEventListener('click', function () { chooseColour(1); });
    col2Btn.addEventListener('click', function () { chooseColour(2); });
    tdA.appendChild(col1Btn);
    tdA.appendChild(col2Btn);
    var del = document.createElement('button');
    del.textContent = '✕'; del.tabIndex = -1;
    del.addEventListener('click', function () { removeRow(false); });
    tdA.appendChild(del);
    tr.appendChild(tdA);

    // Row keys: Delete removes the row; F1/F2 act as the C1/C2 buttons.
    tr.addEventListener('keydown', function (e) {
      if (e.key === 'Delete') { e.preventDefault(); removeRow(true); }
      else if (e.key === 'F1') { e.preventDefault(); chooseColour(1); }
      else if (e.key === 'F2') { e.preventDefault(); chooseColour(2); }
    });

    // Re-segment this glyph with the toolbar colour it is currently using (rowColour),
    // updating only the thumbnails — no font is saved. Used when the toolbar Colour 1/2
    // or its radius is adjusted, so the segmentation preview tracks the colour live.
    function regenPreview() {
      var c = colourVals(rowColour);
      st.a = c.a; st.r = c.r; st.g = c.g; st.b = c.b; st.radius = c.radius;
      var q = '/api/fonts/glyph/regen?gid=' + gl.gid
        + '&a=' + st.a + '&r=' + st.r + '&g=' + st.g + '&b=' + st.b + '&radius=' + st.radius;
      api('POST', q, function () { reloadImages(); });
    }

    if (gl.accounted) tr.classList.add('accounted');
    rendered[gl.gid] = { tr: tr, input: input,
                         accounted: !!gl.accounted, region: gl.region, assigned: gl.assigned || '',
                         getColour: function () { return rowColour; }, regenPreview: regenPreview };
    return tr;
  }

  // ---- Footer coverage: which target characters still need a glyph ----
  // With "2 colours", coverage is tracked PER COLOUR from the C1/C2 button used when
  // each glyph is saved (persisted in localStorage, keyed by font group). With "1
  // colour" it shows the database coverage (any font for the char) in a single block.
  var CHARSET_ROW1 = '0123456789._-';
  var CHARSET_ROW2 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';

  // Darkest colour of the first scrape, used as the unaccounted-chip background so
  // the chips preview the digit's table appearance (dark bg + picker-coloured text).
  var darkestScrape = null;   // 'rgb(r,g,b)'
  var darkestGid = null;
  function computeDarkest(gid) {
    if (gid == null || gid === darkestGid) return;
    darkestGid = gid;
    var im = new Image();
    im.onload = function () {
      try {
        var cv = document.createElement('canvas');
        cv.width = im.naturalWidth; cv.height = im.naturalHeight;
        if (!cv.width || !cv.height) return;
        var ctx = cv.getContext('2d');
        ctx.drawImage(im, 0, 0);
        var d = ctx.getImageData(0, 0, cv.width, cv.height).data;
        var minLum = 1e9, dr = 0, dg = 0, db = 0;
        for (var i = 0; i < d.length; i += 4) {
          var r = d[i], g = d[i + 1], b = d[i + 2];
          var lum = 0.299 * r + 0.587 * g + 0.114 * b;
          if (lum < minLum) { minLum = lum; dr = r; dg = g; db = b; }
        }
        darkestScrape = 'rgb(' + dr + ',' + dg + ',' + db + ')';
        updateCoverageFooter();
      } catch (e) {}
    };
    im.src = '/api/fonts/glyph/full?gid=' + gid;
  }
  // Current picker colour (1 or 2) as a CSS rgb() string, for the chip text colour.
  function colourRgb(which) {
    var c = colourVals(which);
    return 'rgb(' + c.r + ',' + c.g + ',' + c.b + ')';
  }

  function ensureCov(g) { g = String(g); if (!colourCov[g]) colourCov[g] = { 1: {}, 2: {} }; return colourCov[g]; }
  function loadCovState() {
    try {
      var n = parseInt(localStorage.getItem('fcc_num'), 10);
      if (n === 1 || n === 2) numColours = n;
      var raw = localStorage.getItem('fcc');
      if (raw) {
        var obj = JSON.parse(raw);
        for (var g in obj) {
          if (!obj.hasOwnProperty(g)) continue;
          var c = ensureCov(g);
          var s1 = (obj[g] && obj[g]['1']) || '', s2 = (obj[g] && obj[g]['2']) || '';
          for (var i = 0; i < s1.length; i++) c[1][s1.charAt(i)] = true;
          for (var j = 0; j < s2.length; j++) c[2][s2.charAt(j)] = true;
        }
      }
    } catch (e) {}
  }
  function saveCovState() {
    try {
      var obj = {};
      for (var g in colourCov) {
        if (!colourCov.hasOwnProperty(g)) continue;
        obj[g] = { '1': Object.keys(colourCov[g][1]).join(''), '2': Object.keys(colourCov[g][2]).join('') };
      }
      localStorage.setItem('fcc', JSON.stringify(obj));
      localStorage.setItem('fcc_num', String(numColours));
    } catch (e) {}
  }
  // Record that character `ch` was saved under colour `colour` (1/2) in `group`.
  function markColour(group, colour, ch) {
    var c = ensureCov(group);
    c[colour][ch] = true;
    saveCovState();
    updateCoverageFooter();
  }

  function fillCharsetRows(rowEl1, rowEl2, coveredObj, summaryEl, labelPrefix, textColor) {
    var missing = 0;
    function fill(rowEl, chars) {
      if (!rowEl) return;
      rowEl.innerHTML = '';
      for (var k = 0; k < chars.length; k++) {
        var ch = chars.charAt(k);
        var acc = !!coveredObj[ch];
        var s = document.createElement('span');
        s.className = 'covchip' + (acc ? '' : ' unaccounted');
        s.textContent = ch;
        if (!acc) {
          // Preview the glyph: darkest-scrape background + the picker's colour as text.
          if (darkestScrape) s.style.backgroundColor = darkestScrape;
          if (textColor) s.style.color = textColor;
          missing++;
        }
        rowEl.appendChild(s);
      }
    }
    fill(rowEl1, CHARSET_ROW1);
    fill(rowEl2, CHARSET_ROW2);
    if (summaryEl) summaryEl.textContent = labelPrefix + ': ' + missing + ' remaining';
  }

  function renderFooter(dbChars) {
    var g = String(defaultGroup);
    var c = ensureCov(g);
    if (numColours === 2) {
      if (covBlock2) covBlock2.style.display = '';
      // First time (nothing tracked yet): seed Colour 1 from existing DB fonts.
      if (dbChars && Object.keys(c[1]).length === 0 && Object.keys(c[2]).length === 0) {
        for (var i = 0; i < dbChars.length; i++) c[1][dbChars.charAt(i)] = true;
        saveCovState();
      }
      fillCharsetRows(covRow1, covRow2, c[1], glyphbarSummary, 'Colour 1 — unaccounted (Text' + defaultGroup + ')', colourRgb(1));
      fillCharsetRows(cov2Row1, cov2Row2, c[2], glyphbarSummary2, 'Colour 2 — unaccounted (Text' + defaultGroup + ')', colourRgb(2));
    } else {
      if (covBlock2) covBlock2.style.display = 'none';
      var covered = {};
      if (dbChars) for (var j = 0; j < dbChars.length; j++) covered[dbChars.charAt(j)] = true;
      fillCharsetRows(covRow1, covRow2, covered, glyphbarSummary, 'Unaccounted for glyphs (Text' + defaultGroup + ')', colourRgb(1));
    }
  }
  function updateCoverageFooter() {
    api('GET', '/api/fonts/coverage?group=' + defaultGroup, function (status, data) {
      renderFooter((data && data.ok && data.chars) ? data.chars : '');
    });
  }

  loadCovState();
  if (numColoursSel) {
    numColoursSel.value = String(numColours);
    numColoursSel.addEventListener('change', function () {
      numColours = (parseInt(numColoursSel.value, 10) === 1) ? 1 : 2;
      saveCovState();
      updateCoverageFooter();
    });
  }

  // Delete every pending glyph that already has a font (used after Capture when
  // "Auto-remove accounted" is on).
  function removeAccountedRows() {
    var ids = Object.keys(rendered);
    var pending = 0;
    for (var i = 0; i < ids.length; i++) {
      if (!rendered[ids[i]].accounted) continue;
      pending++;
      (function (id) {
        api('POST', '/api/fonts/delete?gid=' + id, function () { pending--; if (pending === 0) refresh(); });
      })(ids[i]);
    }
  }

  function doCapture() {
    statusEl.textContent = 'Capturing…';
    api('POST', '/api/fonts/capture', function () {
      refresh(function () {
        if (autoRemoveAccounted && autoRemoveAccounted.checked) removeAccountedRows();
      });
    });
  }

  function refresh(onDone) {
    api('GET', '/api/fonts/list', function (status, rows) {
      if (status !== 200 || !rows) { statusEl.textContent = 'Disconnected'; if (onDone) onDone(); return; }
      var live = {};
      for (var i = 0; i < rows.length; i++) live[rows[i].gid] = true;
      for (var id in rendered) {
        if (rendered.hasOwnProperty(id) && !live[id]) {
          var rec = rendered[id];
          if (rec.tr.parentNode) rec.tr.parentNode.removeChild(rec.tr);
          if (rec.editTr && rec.editTr.parentNode) rec.editTr.parentNode.removeChild(rec.editTr);
          delete rendered[id];
        }
      }
      var labeled = 0;
      for (var j = 0; j < rows.length; j++) {
        var gl = rows[j];
        if (!rendered[gl.gid]) {
          var newTr = buildRow(gl);
          tbody.appendChild(newTr);
        } else {
          // Keep an existing row's accounted state + marker in sync with the server.
          var rec = rendered[gl.gid];
          rec.accounted = !!gl.accounted;
          rec.region = gl.region;
          rec.assigned = gl.assigned || '';
          rec.tr.classList.toggle('accounted', rec.accounted);
        }
        if (rows[j].assigned) labeled++;
      }
      // The footer tracks coverage for the group the pending glyphs belong to.
      if (rows.length && typeof rows[0].group === 'number') defaultGroup = rows[0].group;
      if (rows.length) computeDarkest(rows[0].gid);   // darkest colour of the first scrape
      retab();
      emptyHint.style.display = rows.length ? 'none' : '';
      statusEl.textContent = rows.length + ' glyph(s), ' + labeled + ' labeled';
      updateCoverageFooter();
      if (onDone) onDone();
    });
  }

  // Tear down every rendered row (glyph + its details row) and rebuild from the
  // server, so regenerated images reload fresh. Used after Apply-Below.
  function rebuildAll() {
    for (var id in rendered) {
      if (rendered.hasOwnProperty(id)) {
        var rec = rendered[id];
        if (rec.tr.parentNode) rec.tr.parentNode.removeChild(rec.tr);
        if (rec.editTr && rec.editTr.parentNode) rec.editTr.parentNode.removeChild(rec.editTr);
      }
    }
    rendered = {};
    refresh();
  }

  captureBtn.addEventListener('click', doCapture);

  clearAllBtn.addEventListener('click', function () {
    if (!confirm('Discard ALL pending glyphs? (Saved fonts are kept.)')) return;
    api('POST', '/api/fonts/clear', function () { refresh(); });
  });

  // Delete every saved t$ font record for this tablemap from the hiss database
  // (does not touch the pending glyph list). Destructive, so confirm first.
  var deleteTmBtn = document.getElementById('deleteTm');
  deleteTmBtn.addEventListener('click', function () {
    if (!confirm('Delete ALL font records (t$) for this tablemap from the hiss database?')) return;
    api('POST', '/api/fonts/deletealltm', function (status, data) {
      if (data && data.ok) {
        colourCov = {};      // every glyph is unaccounted again
        saveCovState();
        statusEl.textContent = 'Deleted ' + (data.removed || 0) + ' font record(s) from the database';
        refresh();           // re-renders the footer (all chars unaccounted)
      } else {
        statusEl.textContent = (data && data.error) ? data.error : 'Delete failed';
      }
    });
  });

  // Undo the last delete or label/create (Ctrl+Z). A labeled row's font is removed
  // from the tablemap and the row is restored for re-labeling.
  function doUndo() {
    // Remember which rows exist now; the restored row(s) come back with fresh gids,
    // so anything new after the refresh is what undo brought back.
    var before = {};
    for (var pid in rendered) if (rendered.hasOwnProperty(pid)) before[pid] = true;
    api('POST', '/api/fonts/undo', function (status, data) {
      if (data && typeof data.restored === 'number') {
        statusEl.textContent = data.restored ? ('Restored ' + data.restored + ' row(s)') : 'Nothing to undo';
      }
      // Full rebuild so the restored row(s) definitely re-render.
      for (var id in rendered) {
        if (rendered.hasOwnProperty(id)) {
          var rec = rendered[id];
          if (rec.tr.parentNode) rec.tr.parentNode.removeChild(rec.tr);
          if (rec.editTr && rec.editTr.parentNode) rec.editTr.parentNode.removeChild(rec.editTr);
        }
      }
      rendered = {};
      refresh(function () {
        // Focus the first restored row's char input so labeling can continue.
        var rows = tbody.querySelectorAll('tr');
        for (var i = 0; i < rows.length; i++) {
          var gid = rows[i].getAttribute('data-gid');
          if (gid && !before[gid]) {
            var inp = rows[i].querySelector('input.ch');
            if (inp) { inp.focus(); inp.select(); }
            return;
          }
        }
      });
    });
  }
  undoBtn.addEventListener('click', doUndo);
  document.addEventListener('keydown', function (e) {
    if ((e.ctrlKey || e.metaKey) && (e.key === 'z' || e.key === 'Z')) {
      e.preventDefault();
      doUndo();
    }
  });

  // ---- Global per-group colour picker (toolbar) ----
  var gcA = document.getElementById('gcA');
  var gcR = document.getElementById('gcR');
  var gcG = document.getElementById('gcG');
  var gcB = document.getElementById('gcB');
  var gcRad = document.getElementById('gcRad');
  var gcGroup = document.getElementById('gcGroup');
  var gcApply = document.getElementById('gcApply');
  var gcApplyAll = document.getElementById('gcApplyAll');
  var gcEyedrop = document.getElementById('gcEyedrop');
  var gcSwatch = document.getElementById('gcSwatch');
  var gcHelp = document.getElementById('gcHelp');
  var helpOverlay = document.getElementById('helpOverlay');
  var helpClose = document.getElementById('helpClose');
  var gc2A = document.getElementById('gc2A');
  var gc2R = document.getElementById('gc2R');
  var gc2G = document.getElementById('gc2G');
  var gc2B = document.getElementById('gc2B');
  var gc2Rad = document.getElementById('gc2Rad');
  var gc2Swatch = document.getElementById('gc2Swatch');
  var gc2Eyedrop = document.getElementById('gc2Eyedrop');

  for (var ggi = 0; ggi < 10; ggi++) {
    var go = document.createElement('option');
    go.value = String(ggi); go.textContent = 'Text' + ggi;
    gcGroup.appendChild(go);
  }

  function gcSync() {
    gcSwatch.style.backgroundColor =
      'rgb(' + clamp255(gcR.value) + ',' + clamp255(gcG.value) + ',' + clamp255(gcB.value) + ')';
  }
  function gc2Sync() {
    gc2Swatch.style.backgroundColor =
      'rgb(' + clamp255(gc2R.value) + ',' + clamp255(gc2G.value) + ',' + clamp255(gc2B.value) + ')';
  }
  [gcA, gcR, gcG, gcB].forEach(function (inp) { inp.addEventListener('input', gcSync); });
  [gc2A, gc2R, gc2G, gc2B].forEach(function (inp) { inp.addEventListener('input', gc2Sync); });
  gcSync(); gc2Sync();

  // Persist the two toolbar colours in the DB settings table and reload them on open.
  function colourCsv(which) {
    var c = colourVals(which);
    return c.a + ',' + c.r + ',' + c.g + ',' + c.b + ',' + c.radius;
  }
  function saveColours() {
    api('POST', '/api/fonts/colours?c1=' + encodeURIComponent(colourCsv(1))
      + '&c2=' + encodeURIComponent(colourCsv(2)), function () {});
  }
  function applyColourCsv(which, csv) {
    if (!csv) return;
    var p = ('' + csv).split(',');
    if (p.length < 4) return;
    if (which === 2) {
      gc2A.value = clamp255(p[0]); gc2R.value = clamp255(p[1]); gc2G.value = clamp255(p[2]); gc2B.value = clamp255(p[3]);
      if (p.length >= 5) gc2Rad.value = parseInt(p[4], 10) || 0;
      gc2Sync();
    } else {
      gcA.value = clamp255(p[0]); gcR.value = clamp255(p[1]); gcG.value = clamp255(p[2]); gcB.value = clamp255(p[3]);
      if (p.length >= 5) gcRad.value = parseInt(p[4], 10) || 0;
      gcSync();
    }
  }
  function loadColours() {
    api('GET', '/api/fonts/colours', function (status, data) {
      if (data && data.ok) {
        applyColourCsv(1, data.c1);
        applyColourCsv(2, data.c2);
        updateCoverageFooter();   // footer chip text colours follow the pickers
      }
    });
  }
  // Live-tint the footer chips as a colour is edited; persist when the edit commits.
  [gcA, gcR, gcG, gcB, gc2A, gc2R, gc2G, gc2B].forEach(function (inp) {
    inp.addEventListener('input', function () { updateCoverageFooter(); });
  });
  [gcA, gcR, gcG, gcB, gcRad, gc2A, gc2R, gc2G, gc2B, gc2Rad].forEach(function (inp) {
    inp.addEventListener('change', saveColours);
  });

  // Adjusting a toolbar colour or its radius re-segments every pending glyph currently
  // using that colour (rowColour, default 1), so the thumbnails track the colour live.
  // Preview only — nothing is saved until C1/C2 (or F1/F2) on a row. Debounced so a run
  // of keystrokes / spinner clicks coalesces into one regen pass.
  function debounce(fn, ms) {
    var t = null;
    return function () { if (t) clearTimeout(t); t = setTimeout(fn, ms); };
  }
  function regenGlyphsForColour(which) {
    for (var id in rendered) {
      if (!rendered.hasOwnProperty(id)) continue;
      var rec = rendered[id];
      if (rec.getColour && rec.getColour() === which && rec.regenPreview) rec.regenPreview();
    }
  }
  var regenC1 = debounce(function () { regenGlyphsForColour(1); }, 300);
  var regenC2 = debounce(function () { regenGlyphsForColour(2); }, 300);
  [gcA, gcR, gcG, gcB, gcRad].forEach(function (inp) { inp.addEventListener('input', regenC1); });
  [gc2A, gc2R, gc2G, gc2B, gc2Rad].forEach(function (inp) { inp.addEventListener('input', regenC2); });

  loadColours();

  // Read one of the two global colour blocks (which = 1 or 2).
  function colourVals(which) {
    if (which === 2) {
      return { a: clamp255(gc2A.value), r: clamp255(gc2R.value), g: clamp255(gc2G.value),
               b: clamp255(gc2B.value), radius: parseInt(gc2Rad.value, 10) || 0 };
    }
    return { a: clamp255(gcA.value), r: clamp255(gcR.value), g: clamp255(gcG.value),
             b: clamp255(gcB.value), radius: parseInt(gcRad.value, 10) || 0 };
  }

  // Eyedropper: gcArmTarget is 0 (off), 1 (Colour 1) or 2 (Colour 2). While armed,
  // clicking any Reference/Scrape thumbnail fills that block (see armPick in buildRow).
  var gcArmTarget = 0;
  function setArmed(target) {
    gcArmTarget = target;
    gcArmed = (target !== 0);
    gcEyedrop.classList.toggle('on', target === 1);
    gc2Eyedrop.classList.toggle('on', target === 2);
    document.body.classList.toggle('eyedropping', gcArmed);
    if (!gcArmed) hideMag();
  }
  gcEyedrop.addEventListener('click', function () { setArmed(gcArmTarget === 1 ? 0 : 1); });
  gc2Eyedrop.addEventListener('click', function () { setArmed(gcArmTarget === 2 ? 0 : 2); });
  // Called by a thumbnail pick — fills the armed colour block and disarms.
  function gcSetPicked(a, r, g, b) {
    if (gcArmTarget === 2) {
      gc2A.value = clamp255(a); gc2R.value = clamp255(r); gc2G.value = clamp255(g); gc2B.value = clamp255(b);
      gc2Sync();
    } else {
      gcA.value = clamp255(a); gcR.value = clamp255(r); gcG.value = clamp255(g); gcB.value = clamp255(b);
      gcSync();
    }
    var n = gcArmTarget || 1;
    setArmed(0);
    saveColours();            // persist the picked colour
    updateCoverageFooter();   // re-tint the footer chips
    regenGlyphsForColour(n);  // re-segment glyphs using this colour with the picked value
    statusEl.textContent = 'Picked Colour ' + n + ' rgb(' + clamp255(r) + ',' + clamp255(g) + ',' + clamp255(b) + ') — set radius';
  }

  // Selecting a group pre-fills the fields from a region using that transform.
  gcGroup.addEventListener('change', function () {
    api('GET', '/api/fonts/groupdefaults?group=' + gcGroup.value, function (status, data) {
      if (data && data.ok) {
        gcA.value = clamp255(data.a); gcR.value = clamp255(data.r);
        gcG.value = clamp255(data.g); gcB.value = clamp255(data.b);
        gcRad.value = parseInt(data.radius, 10) || 0;
        gcSync();
      }
    });
  });

  gcApply.addEventListener('click', function () {
    var group = parseInt(gcGroup.value, 10);
    var q = '/api/fonts/applygroup?group=' + group
      + '&a=' + clamp255(gcA.value) + '&r=' + clamp255(gcR.value)
      + '&g=' + clamp255(gcG.value) + '&b=' + clamp255(gcB.value)
      + '&radius=' + (parseInt(gcRad.value, 10) || 0);
    api('POST', q, function (status, data) {
      var n = (data && typeof data.count === 'number') ? data.count : 0;
      statusEl.textContent = 'Applied colour to ' + n + ' Text' + group + ' row(s)';
      rebuildAll();
    });
  });

  // Write the colour/radius to EVERY balance region (repairs regions that scrape
  // nothing), then regenerate all pending glyphs. Overwrites region colours, so confirm.
  gcApplyAll.addEventListener('click', function () {
    if (!confirm('Set this colour + radius on ALL balance regions in the tablemap?\nThis overwrites each region\'s colour. Re-capture afterwards to pull glyphs.')) return;
    var q = '/api/fonts/applyall?a=' + clamp255(gcA.value) + '&r=' + clamp255(gcR.value)
      + '&g=' + clamp255(gcG.value) + '&b=' + clamp255(gcB.value)
      + '&radius=' + (parseInt(gcRad.value, 10) || 0);
    api('POST', q, function (status, data) {
      var rg = (data && typeof data.regions === 'number') ? data.regions : 0;
      var gl = (data && typeof data.glyphs === 'number') ? data.glyphs : 0;
      statusEl.textContent = 'Set colour on ' + rg + ' region(s), regenerated ' + gl + ' glyph(s). Re-capture to pull from fixed regions.';
      rebuildAll();
    });
  });

  function showHelp(on) { helpOverlay.style.display = on ? '' : 'none'; }
  gcHelp.addEventListener('click', function () { showHelp(true); });
  helpClose.addEventListener('click', function () { showHelp(false); });
  helpOverlay.addEventListener('click', function (e) { if (e.target === helpOverlay) showHelp(false); });
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape' && helpOverlay.style.display !== 'none') showHelp(false);
  });

  // ---- Transform dropdown: drives the CHAR auto-suggest (AutoOcr0/1 + Text0..9) --
  if (fontTransform) {
    var addOpt = function (v) {
      var o = document.createElement('option'); o.value = v; o.textContent = v; fontTransform.appendChild(o);
    };
    addOpt('AutoOcr0'); addOpt('AutoOcr1');
    for (var ti = 0; ti < 10; ti++) addOpt('Text' + ti);
    fontTransform.value = currentTransform;
    fontTransform.addEventListener('change', function () {
      currentTransform = fontTransform.value;
      // Re-suggest the character for every still-unlabeled row using the new transform.
      for (var id in rendered) {
        if (!rendered.hasOwnProperty(id)) continue;
        var rec = rendered[id];
        if (rec.input && !rec.input.classList.contains('done')) {
          rec.input.value = '';
          ocrFill(parseInt(id, 10), rec.input);
        }
      }
    });
  }

  refresh();
  // Poll: when "Auto capture" is on, re-capture scrapes on each tick (which also
  // refreshes + auto-removes accounted if enabled); otherwise just refresh.
  setInterval(function () {
    if (autoCapture && autoCapture.checked) doCapture();
    else refresh();
  }, 1500);
})();
