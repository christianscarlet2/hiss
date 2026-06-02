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
  var covRow1 = document.getElementById('covRow1');
  var covRow2 = document.getElementById('covRow2');

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

    var tdC = document.createElement('td');
    tdC.className = 'c';
    var input = document.createElement('input');
    input.className = 'ch';
    input.type = 'text';
    input.maxLength = 1;
    input.value = gl.assigned || '';
    if (input.value) input.classList.add('done');
    else ocrFill(gl.gid, input);   // suggest the character via the chosen transform
    function commit() {
      var val = input.value;
      if (val) {
        // If this glyph already has a font, confirm before (re)creating it. On "No",
        // clear the typed character WITHOUT saving or removing the row.
        var rec = rendered[gl.gid];
        if (rec && rec.accounted &&
            !confirm('This glyph has already been accounted for.\nContinue and (re)create the font?')) {
          input.value = '';
          input.classList.remove('done');
          return;
        }
        // Assigning a char creates the font AND rescans every glyph server-side, so
        // any other now-recognized glyphs drop out too. Show the per-row spinners
        // while that runs, then advance to the next unlabeled glyph when it finishes.
        input.classList.add('done');
        setScanning(true);
        api('POST', '/api/fonts/setchar?gid=' + gl.gid + '&ch=' + encodeURIComponent(val), function () {
          refresh(function () { setScanning(false); focusFirstEmpty(); });
        });
      } else {
        // Clearing a char creates no font and triggers no rescan.
        input.classList.remove('done');
        api('POST', '/api/fonts/setchar?gid=' + gl.gid + '&ch=', function () {});
      }
    }
    input.addEventListener('input', commit);
    input.addEventListener('keydown', function (e) {
      if (e.key === 'Enter') { e.preventDefault(); commit(); }
    });
    tdC.appendChild(input);
    var spin = document.createElement('span');
    spin.className = 'spin';   // visible only while tbody has the .scanning class
    tdC.appendChild(spin);
    tr.appendChild(tdC);

    // Per-row colour state, seeded from the list payload (region r$ defaults).
    var st = { a: clamp255(gl.a), r: clamp255(gl.r), g: clamp255(gl.g), b: clamp255(gl.b),
               radius: parseInt(gl.radius, 10) || 0 };

    var tdGrp = document.createElement('td');
    tdGrp.className = 'grp';
    var grpSel = document.createElement('select');
    grpSel.tabIndex = -1;
    for (var gi = 0; gi < 10; gi++) {
      var o = document.createElement('option');
      o.value = String(gi); o.textContent = 'Text' + gi;
      grpSel.appendChild(o);
    }
    grpSel.value = String(typeof gl.group === 'number' ? gl.group : defaultGroup);
    grpSel.addEventListener('change', function () {
      defaultGroup = parseInt(grpSel.value, 10);
      // Setting the transform re-pulls the colour/radius default from a region using
      // it, regenerates the glyph, and returns the applied colour for the fields.
      api('POST', '/api/fonts/glyph/group?gid=' + gl.gid + '&group=' + defaultGroup, function (status, data) {
        if (data && data.ok) {
          st.a = clamp255(data.a); st.r = clamp255(data.r); st.g = clamp255(data.g);
          st.b = clamp255(data.b); st.radius = parseInt(data.radius, 10) || 0;
          syncFields(); reloadImages();
        }
      });
    });
    tdGrp.appendChild(grpSel);
    tr.appendChild(tdGrp);

    // Apply this row's group + colour + radius to every row below it (regenerates each).
    var tdAg = document.createElement('td');
    tdAg.className = 'ag';
    var applyBtn = document.createElement('button');
    applyBtn.textContent = 'Apply Group Below';
    applyBtn.tabIndex = -1;
    applyBtn.addEventListener('click', function () {
      defaultGroup = parseInt(grpSel.value, 10);
      var q = '/api/fonts/applybelow?gid=' + gl.gid + '&group=' + defaultGroup
        + '&a=' + st.a + '&r=' + st.r + '&g=' + st.g + '&b=' + st.b + '&radius=' + st.radius;
      api('POST', q, function () { rebuildAll(); });
    });
    tdAg.appendChild(applyBtn);
    tr.appendChild(tdAg);

    var tdR = document.createElement('td');
    tdR.className = 'r';
    tdR.textContent = gl.region + ' · Text' + gl.group;
    tdR.title = gl.hexmash || '';
    tr.appendChild(tdR);

    // ---- Colour editor (a details row toggled open under this row) ----
    var editTr = document.createElement('tr');
    editTr.className = 'coloredit';
    editTr.style.display = 'none';
    var editTd = document.createElement('td');
    editTd.colSpan = 8;
    editTr.appendChild(editTd);
    var panel = document.createElement('div');
    panel.className = 'cedit';
    editTd.appendChild(panel);

    // Two enlarged, pixelated pick targets: the glyph reference and the whole scrape.
    var pickWrap = document.createElement('div');
    pickWrap.className = 'pickimgs';
    function makePick(label, which) {
      var box = document.createElement('div');
      box.className = 'pickbox';
      var lb = document.createElement('div'); lb.className = 'lbl'; lb.textContent = label;
      var im = document.createElement('img');
      im.className = 'pick';
      im.src = '/api/fonts/glyph/' + which + '?gid=' + gl.gid;
      im.addEventListener('mousemove', function (e) { showMag(im, e); });
      im.addEventListener('mouseleave', hideMag);
      im.addEventListener('click', function (e) {
        var p = naturalXY(im, e);
        api('GET', '/api/fonts/glyph/pixel?gid=' + gl.gid + '&img=' + which + '&px=' + p.x + '&py=' + p.y,
          function (status, data) {
            if (data && data.ok) {
              st.a = clamp255(data.a); st.r = clamp255(data.r); st.g = clamp255(data.g); st.b = clamp255(data.b);
              syncFields(); doRegen();
            }
          });
      });
      box.appendChild(lb); box.appendChild(im);
      return { box: box, img: im };
    }
    var pkReg = makePick('Reference — click to pick', 'regular');
    var pkFull = makePick('Scrape — click to pick', 'full');
    pickWrap.appendChild(pkReg.box);
    pickWrap.appendChild(pkFull.box);
    panel.appendChild(pickWrap);

    // A/R/G/B + radius fields and a live swatch.
    var fields = document.createElement('div');
    fields.className = 'fields';
    var swatch = document.createElement('span');
    swatch.className = 'swatch';
    fields.appendChild(swatch);
    var inputs = {};
    function makeField(key, label, min, max) {
      var l = document.createElement('label');
      l.textContent = label;
      var inp = document.createElement('input');
      inp.type = 'number'; inp.className = 'cfield';
      if (min !== null) inp.min = min;
      if (max !== null) inp.max = max;
      inp.tabIndex = -1;
      function commitField() {
        st.a = clamp255(inputs.a.value); st.r = clamp255(inputs.r.value);
        st.g = clamp255(inputs.g.value); st.b = clamp255(inputs.b.value);
        st.radius = parseInt(inputs.radius.value, 10) || 0;
        doRegen();
      }
      inp.addEventListener('change', commitField);
      inp.addEventListener('keydown', function (e) { if (e.key === 'Enter') { e.preventDefault(); commitField(); } });
      l.appendChild(inp);
      inputs[key] = inp;
      fields.appendChild(l);
    }
    makeField('a', 'A', 0, 255);
    makeField('r', 'R', 0, 255);
    makeField('g', 'G', 0, 255);
    makeField('b', 'B', 0, 255);
    makeField('radius', 'Radius', null, null);
    panel.appendChild(fields);

    function syncFields() {
      inputs.a.value = st.a; inputs.r.value = st.r; inputs.g.value = st.g; inputs.b.value = st.b;
      inputs.radius.value = st.radius;
      swatch.style.backgroundColor = 'rgb(' + st.r + ',' + st.g + ',' + st.b + ')';
    }
    function reloadImages() {
      imgVer++;
      var bust = '&v=' + imgVer;
      img.src = '/api/fonts/glyph/image?gid=' + gl.gid + bust;       // mask thumbnail
      imgR.src = '/api/fonts/glyph/regular?gid=' + gl.gid + bust;    // reference thumbnail
      pkReg.img.src = '/api/fonts/glyph/regular?gid=' + gl.gid + bust;
      pkFull.img.src = '/api/fonts/glyph/full?gid=' + gl.gid + bust;
      syncFields();
    }
    function doRegen() {
      var q = '/api/fonts/glyph/regen?gid=' + gl.gid
        + '&a=' + st.a + '&r=' + st.r + '&g=' + st.g + '&b=' + st.b + '&radius=' + st.radius;
      api('POST', q, function () { reloadImages(); });
    }
    syncFields();

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
        if (editTr.parentNode) editTr.parentNode.removeChild(editTr);
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
    // regenerate. "Apply Group Below" then propagates whatever colour the row holds.
    var col1Btn = document.createElement('button');
    col1Btn.textContent = 'C1'; col1Btn.tabIndex = -1; col1Btn.className = 'colsel'; col1Btn.title = 'Use Colour 1';
    var col2Btn = document.createElement('button');
    col2Btn.textContent = 'C2'; col2Btn.tabIndex = -1; col2Btn.className = 'colsel'; col2Btn.title = 'Use Colour 2';
    function chooseColour(which) {
      var c = colourVals(which);
      st.a = c.a; st.r = c.r; st.g = c.g; st.b = c.b; st.radius = c.radius;
      col1Btn.classList.toggle('on', which === 1);
      col2Btn.classList.toggle('on', which === 2);
      syncFields();
      doRegen();
    }
    col1Btn.addEventListener('click', function () { chooseColour(1); });
    col2Btn.addEventListener('click', function () { chooseColour(2); });
    tdA.appendChild(col1Btn);
    tdA.appendChild(col2Btn);
    // Toggle the colour editor open/closed for this row.
    var colorBtn = document.createElement('button');
    colorBtn.textContent = '🎨'; colorBtn.tabIndex = -1; colorBtn.title = 'Pick colour / radius';
    colorBtn.className = 'colorbtn';
    colorBtn.addEventListener('click', function () {
      var open = editTr.style.display === 'none';
      editTr.style.display = open ? '' : 'none';
      colorBtn.classList.toggle('on', open);
      if (open) syncFields();
    });
    tdA.appendChild(colorBtn);
    var del = document.createElement('button');
    del.textContent = '✕'; del.tabIndex = -1;
    del.addEventListener('click', function () { removeRow(false); });
    tdA.appendChild(del);
    tr.appendChild(tdA);

    // Delete key anywhere in the row removes the whole row, then returns focus
    // to the previous input field so labeling can continue without the mouse.
    tr.addEventListener('keydown', function (e) {
      if (e.key === 'Delete') { e.preventDefault(); removeRow(true); }
    });

    if (gl.accounted) tr.classList.add('accounted');
    rendered[gl.gid] = { tr: tr, editTr: editTr, input: input,
                         accounted: !!gl.accounted, region: gl.region, assigned: gl.assigned || '' };
    return tr;
  }

  // Fixed bottom bar: the full target charset over two rows (digits/symbols, then
  // letters), highlighting the glyphs not yet accounted for (no font in the current
  // group). Coverage is fetched per the sticky working group (defaultGroup).
  var CHARSET_ROW1 = '0123456789._-';
  var CHARSET_ROW2 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
  function renderCoverage(coveredStr) {
    if (!covRow1 || !covRow2) return;
    var covered = {}, i;
    for (i = 0; i < coveredStr.length; i++) covered[coveredStr.charAt(i)] = true;
    var missing = 0;
    function fill(rowEl, chars) {
      rowEl.innerHTML = '';
      for (var k = 0; k < chars.length; k++) {
        var ch = chars.charAt(k);
        var acc = !!covered[ch];
        var s = document.createElement('span');
        s.className = 'covchip' + (acc ? '' : ' unaccounted');
        s.textContent = ch;
        rowEl.appendChild(s);
        if (!acc) missing++;
      }
    }
    fill(covRow1, CHARSET_ROW1);
    fill(covRow2, CHARSET_ROW2);
    if (glyphbarSummary) {
      glyphbarSummary.textContent = 'Unaccounted for glyphs (Text' + defaultGroup + '): ' + missing + ' remaining';
    }
  }
  function updateCoverageFooter() {
    api('GET', '/api/fonts/coverage?group=' + defaultGroup, function (status, data) {
      renderCoverage((data && data.ok && data.chars) ? data.chars : '');
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
          tbody.appendChild(rendered[gl.gid].editTr);   // details row follows its glyph row
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
        statusEl.textContent = 'Deleted ' + (data.removed || 0) + ' font record(s) from the database';
        refresh();   // fonts changed -> refresh coverage + accounted markers
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
