(function () {
  var tbody = document.getElementById('tbody');
  var statusEl = document.getElementById('status');
  var hideSaved = document.getElementById('hideSaved');
  var saveAllBtn = document.getElementById('saveAll');
  var clearAllBtn = document.getElementById('clearAll');
  var dedupBtn = document.getElementById('dedup');
  var autoDedup = document.getElementById('autoDedup');
  var textGroup = document.getElementById('textGroup');

  // id -> { tr, input, saved, order }  (order = capture order, for tab index)
  var rendered = {};
  var currentTransform = 'AutoOcr0';

  // Flag rows whose value contains a 1 or 7 (the easily-confused digits) so they
  // stand out for a quick manual check. Updates live as the value is edited.
  function has17(v) { return /[17]/.test(v || ''); }
  function markRow(tr, value) {
    if (has17(value)) tr.classList.add('has17'); else tr.classList.remove('has17');
  }

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

  function saveRow(id, text, done) {
    api('POST', '/api/sample/save?id=' + id + '&text=' + encodeURIComponent(text), function (status, data) {
      if (done) done(data && data.ok);
    });
  }

  // Renumber the tabindex of every visible input so Tab walks them top-to-bottom.
  function retab() {
    var rows = tbody.querySelectorAll('tr');
    var t = 1;
    for (var i = 0; i < rows.length; i++) {
      var inp = rows[i].querySelector('input.label');
      if (inp && rows[i].style.display !== 'none') {
        inp.tabIndex = t++;
      } else if (inp) {
        inp.tabIndex = -1;
      }
    }
  }

  function buildRow(s) {
    var tr = document.createElement('tr');
    tr.setAttribute('data-id', s.id);

    var tdImg = document.createElement('td');
    tdImg.className = 'img';
    var img = document.createElement('img');
    img.src = '/api/sample/image?id=' + s.id;
    img.title = 'Click to load into the magnifier';
    img.addEventListener('click', function () { loadMag(s.id); });
    tdImg.appendChild(img);
    tr.appendChild(tdImg);

    var tdTxt = document.createElement('td');
    var wrap = document.createElement('div');
    wrap.className = 'labelrow';
    var input = document.createElement('input');
    input.className = 'label';
    input.type = 'text';
    input.value = s.guess || '';
    input.addEventListener('keydown', function (e) {
      if (e.key === 'Enter') { doSave(); }
    });
    input.addEventListener('input', function () { markRow(tr, input.value); });
    var unit = document.createElement('span');
    unit.className = 'unit';
    unit.textContent = '+ BB';   // " BB" is appended to the .gt.txt on save
    wrap.appendChild(input);
    wrap.appendChild(unit);
    tdTxt.appendChild(wrap);
    tr.appendChild(tdTxt);

    var tdAct = document.createElement('td');
    tdAct.className = 'actions';
    var btnSave = document.createElement('button');
    btnSave.className = 'row-save'; btnSave.textContent = 'Save'; btnSave.tabIndex = -1;
    var btnDel = document.createElement('button');
    btnDel.className = 'row-del'; btnDel.textContent = 'Delete'; btnDel.tabIndex = -1;
    var btnUnder = document.createElement('button');
    btnUnder.className = 'row-under'; btnUnder.textContent = 'Clear Underneath'; btnUnder.tabIndex = -1;
    tdAct.appendChild(btnSave);
    tdAct.appendChild(btnDel);
    tdAct.appendChild(btnUnder);
    tr.appendChild(tdAct);

    function doSave() {
      saveRow(s.id, input.value, function (ok) {
        if (ok) {
          var rec = rendered[s.id];
          if (rec) rec.saved = true;
          tr.classList.add('saved');
          applyHideSaved();
        }
      });
    }
    btnSave.addEventListener('click', doSave);

    function deleteRow(refocus) {
      // Remember the previous row's input so focus can continue after removal.
      var prevInput = null;
      if (refocus) {
        var ins = tbody.querySelectorAll('input.label');
        for (var k = 0; k < ins.length; k++) {
          if (ins[k] === input) { if (k > 0) prevInput = ins[k - 1]; break; }
        }
      }
      api('POST', '/api/sample/delete?id=' + s.id, function () {
        if (tr.parentNode) tr.parentNode.removeChild(tr);
        delete rendered[s.id];
        retab();
        if (refocus) {
          var target = (prevInput && tbody.contains(prevInput)) ? prevInput : tbody.querySelector('input.label');
          if (target) { target.focus(); target.select(); }
        }
      });
    }
    btnDel.addEventListener('click', function () { deleteRow(false); });

    // Delete key anywhere in the row removes it (then returns focus to the previous
    // row's input so you can keep clearing bad samples without the mouse).
    tr.addEventListener('keydown', function (e) {
      if (e.key === 'Delete') { e.preventDefault(); deleteRow(true); }
    });

    btnUnder.addEventListener('click', function () {
      api('POST', '/api/sample/clearunder?id=' + s.id, function () {
        refresh();   // server removed rows below; re-sync from scratch
      });
    });

    rendered[s.id] = { tr: tr, input: input, saved: !!s.saved };
    if (s.saved) tr.classList.add('saved');
    markRow(tr, input.value);
    return tr;
  }

  function applyHideSaved() {
    var hide = hideSaved.checked;
    for (var id in rendered) {
      if (!rendered.hasOwnProperty(id)) continue;
      var rec = rendered[id];
      rec.tr.style.display = (hide && rec.saved) ? 'none' : '';
    }
    retab();
  }

  function refresh() {
    api('GET', '/api/samples', function (status, rows) {
      if (status !== 200 || !rows) { statusEl.textContent = 'Disconnected'; return; }

      // Drop rows the server no longer has (deleted elsewhere).
      var live = {};
      for (var i = 0; i < rows.length; i++) { live[rows[i].id] = true; }
      for (var id in rendered) {
        if (rendered.hasOwnProperty(id) && !live[id]) {
          var rec = rendered[id];
          if (rec.tr.parentNode) rec.tr.parentNode.removeChild(rec.tr);
          delete rendered[id];
        }
      }

      var pending = 0;
      for (var j = 0; j < rows.length; j++) {
        var s = rows[j];
        if (!rendered[s.id]) {
          tbody.appendChild(buildRow(s));
        } else {
          var r = rendered[s.id];
          if (s.saved && !r.saved) { r.saved = true; r.tr.classList.add('saved'); }
        }
        if (!rendered[s.id].saved) pending++;
      }
      statusEl.textContent = rows.length + ' samples, ' + pending + ' unsaved';
      applyHideSaved();
    });
  }

  // Remove every saved row from the working list (the .png/.gt.txt files are kept;
  // this just drops them from the store so the table only shows unsaved work).
  function clearSavedRows() {
    var ids = Object.keys(rendered);
    var pending = 0;
    for (var i = 0; i < ids.length; i++) {
      if (!rendered[ids[i]].saved) continue;
      pending++;
      (function (id) {
        api('POST', '/api/sample/delete?id=' + id, function () {
          pending--;
          if (pending === 0) refresh();
        });
      })(ids[i]);
    }
    if (pending === 0) refresh();
  }

  saveAllBtn.addEventListener('click', function () {
    var ids = Object.keys(rendered);
    var pending = 0;
    function maybeClear() { if (pending === 0) clearSavedRows(); }   // clear once all saves resolve
    for (var i = 0; i < ids.length; i++) {
      var rec = rendered[ids[i]];
      if (rec.saved) continue;
      pending++;
      (function (rec, id) {
        saveRow(id, rec.input.value, function (ok) {
          if (ok) { rec.saved = true; rec.tr.classList.add('saved'); }
          pending--;
          maybeClear();
        });
      })(rec, ids[i]);
    }
    maybeClear();   // nothing was unsaved -> still clear already-saved rows
  });

  clearAllBtn.addEventListener('click', function () {
    if (!confirm('Remove ALL samples from the list? (saved files are kept)')) return;
    api('POST', '/api/samples/clear', function () { refresh(); });
  });

  var clearTrainingBtn = document.getElementById('clearTraining');
  if (clearTrainingBtn) {
    clearTrainingBtn.addEventListener('click', function () {
      if (!confirm('Delete ALL files in the training\\ folder?')) return;
      api('POST', '/api/training/clear', function (status, data) {
        if (data && typeof data.deleted === 'number') {
          statusEl.textContent = 'Deleted ' + data.deleted + ' training file(s)';
        }
      });
    });
  }

  dedupBtn.addEventListener('click', function () {
    api('POST', '/api/samples/dedup', function (status, data) {
      if (data && typeof data.removed === 'number') {
        statusEl.textContent = 'Removed ' + data.removed + ' duplicate(s)';
      }
      refresh();
    });
  });

  hideSaved.addEventListener('change', applyHideSaved);

  // ---- Transform dropdown: AutoOcr0/1 (Tesseract) + Text0..Text9 (fonts) -----
  function buildGroupOptions(counts) {
    textGroup.innerHTML = '';
    var add = function (value, label) {
      var opt = document.createElement('option');
      opt.value = value; opt.textContent = label; textGroup.appendChild(opt);
    };
    add('AutoOcr0', 'AutoOcr0');
    add('AutoOcr1', 'AutoOcr1');
    add('AutoOcr2', 'AutoOcr2');
    for (var g = 0; g < 10; g++) {
      var n = (counts && typeof counts[g] === 'number') ? counts[g] : 0;
      add('Text' + g, 'Text' + g + ' (' + n + ')');
    }
    textGroup.value = currentTransform;
  }

  function loadFontInfo() {
    api('GET', '/api/fonts/info', function (status, data) {
      if (status === 200 && data) {
        if (data.current) currentTransform = data.current;
        buildGroupOptions(data.counts || []);
      } else {
        buildGroupOptions([]);
      }
    });
  }

  // Remove all rows and re-fetch (used after a transform re-recognizes everything).
  function forceReload() {
    for (var id in rendered) {
      if (rendered.hasOwnProperty(id) && rendered[id].tr.parentNode) {
        rendered[id].tr.parentNode.removeChild(rendered[id].tr);
      }
    }
    rendered = {};
    refresh();
  }

  textGroup.addEventListener('change', function () {
    var t = textGroup.value;
    var nRows = Object.keys(rendered).length;
    if (nRows > 0) {
      if (!confirm('Apply ' + t + ' to all ' + nRows + ' row(s)?\n\n'
        + 'This re-recognizes every row from its scrape with ' + t
        + ' and overwrites the current text in the table.')) {
        textGroup.value = currentTransform;   // revert
        return;
      }
    }
    api('POST', '/api/transform?t=' + encodeURIComponent(t), function (status, data) {
      currentTransform = t;
      if (data) {
        statusEl.textContent = t + ': ' + (data.recognized || 0) + ' recognized'
          + (t.indexOf('Text') === 0 ? ' via ' + (data.fonts || 0) + ' font(s)' : '');
        loadFontInfo();
      }
      forceReload();
    });
  });

  // ---- Start/Stop sample capture (moved here from the desktop dialog) ---------
  var startStopBtn = document.getElementById('startStop');
  var capturing = false;
  function setCaptureLabel(on) {
    capturing = !!on;
    startStopBtn.textContent = on ? 'Stop' : 'Start';
    startStopBtn.classList.toggle('warn', on);   // amber while capturing
  }
  if (startStopBtn) {
    startStopBtn.addEventListener('click', function () {
      var wasCapturing = capturing;
      api('POST', '/api/capture?set=toggle', function (status, data) {
        if (data) {
          setCaptureLabel(data.capturing);
          // Asked to start but it didn't take -> not connected / no tablemap.
          if (!wasCapturing && !data.capturing) {
            statusEl.textContent = 'Connect to a table and load a tablemap in the trainer first.';
          }
        }
      });
    });
  }
  function pollCapture() {
    api('GET', '/api/capture', function (status, data) {
      if (status === 200 && data) setCaptureLabel(data.capturing);
    });
  }

  // Shared per-field-type scrape list (scrape_fields): "Scrape balances" / "Scrape
  // names" toggle which player fields the trainer scrapes (shared with Vision + the
  // Font Creation tool via the DB).
  var scrapeBalances = document.getElementById('scrapeBalances');
  var scrapeNames = document.getElementById('scrapeNames');
  var scrapeBets = document.getElementById('scrapeBets');
  function applyScrapeState(data) {
    if (!data) return;
    if (scrapeBalances) scrapeBalances.checked = !!data.balance;
    if (scrapeNames) scrapeNames.checked = !!data.name;
    if (scrapeBets) scrapeBets.checked = !!data.bet;
  }
  function loadScrapeFields() {
    api('GET', '/api/scrapefields', function (status, data) { if (status === 200) applyScrapeState(data); });
  }
  function toggleScrapeField(field, on) {
    api('POST', '/api/scrapefields?field=' + field + '&on=' + (on ? 1 : 0),
      function (status, data) { if (status === 200) applyScrapeState(data); });
  }
  if (scrapeBalances) scrapeBalances.addEventListener('change', function () { toggleScrapeField('balance', scrapeBalances.checked); });
  if (scrapeNames) scrapeNames.addEventListener('change', function () { toggleScrapeField('name', scrapeNames.checked); });
  if (scrapeBets) scrapeBets.addEventListener('change', function () { toggleScrapeField('bet', scrapeBets.checked); });
  loadScrapeFields();

  // "No OCR prefill": when on, captured rows are added with EMPTY text (no OCR
  // prepopulation) and blank/empty captures are kept as rows instead of being
  // skipped. Auto-dedup is also suppressed so those empty rows survive. The flag
  // lives in the DB (read by the capture loop in the trainer).
  var noPrefill = document.getElementById('noPrefill');
  function loadNoPrefill() {
    api('GET', '/api/noprefill', function (status, data) {
      if (status === 200 && data && noPrefill) noPrefill.checked = !!data.on;
    });
  }
  if (noPrefill) noPrefill.addEventListener('change', function () {
    api('POST', '/api/noprefill?on=' + (noPrefill.checked ? 1 : 0), function () {});
  });
  loadNoPrefill();

  // ---- Color filters + magnifier --------------------------------------------
  // Three optional ARGB+tolerance filters. When one or more is enabled, a capture
  // is only added as a row if its crop contains a pixel matching ANY enabled color
  // within tolerance (OR). The filter is stored in the DB and applied by the
  // capture loop in the trainer. The magnifier lets you pick a color off a row's
  // crop straight into the active slot.
  var cfEls = [];
  var slotDivs = document.querySelectorAll('.colorbar .cf');
  for (var ci = 0; ci < slotDivs.length; ci++) {
    var d = slotDivs[ci];
    cfEls.push({
      div: d,
      on: d.querySelector('.cf-on'),
      pick: d.querySelector('.cf-pick'),
      a: d.querySelector('.cf-a'),
      r: d.querySelector('.cf-r'),
      g: d.querySelector('.cf-g'),
      b: d.querySelector('.cf-b'),
      tol: d.querySelector('.cf-tol'),
      target: d.querySelector('.cf-target')
    });
  }
  var activeSlot = 0;

  function clamp255(n) { n = parseInt(n, 10); if (isNaN(n)) n = 0; return n < 0 ? 0 : (n > 255 ? 255 : n); }
  function hex2(n) { var s = clamp255(n).toString(16); return s.length < 2 ? '0' + s : s; }
  function rgbToHex(r, g, b) { return '#' + hex2(r) + hex2(g) + hex2(b); }
  function setActiveSlot(idx) {
    activeSlot = idx;
    for (var i = 0; i < cfEls.length; i++) {
      if (i === idx) cfEls[i].div.classList.add('active'); else cfEls[i].div.classList.remove('active');
    }
  }
  function buildColorCsv() {
    var parts = [];
    for (var i = 0; i < cfEls.length; i++) {
      var e = cfEls[i];
      parts.push([
        e.on.checked ? 1 : 0,
        clamp255(e.a.value), clamp255(e.r.value), clamp255(e.g.value), clamp255(e.b.value),
        clamp255(e.tol.value)
      ].join(','));
    }
    return parts.join(';');
  }
  function saveColors() {
    api('POST', '/api/samplecolors?v=' + encodeURIComponent(buildColorCsv()), function () {});
  }
  function applyColorToSlot(idx, r, g, b, a) {
    var e = cfEls[idx]; if (!e) return;
    e.r.value = clamp255(r); e.g.value = clamp255(g); e.b.value = clamp255(b); e.a.value = clamp255(a);
    e.pick.value = rgbToHex(r, g, b);
    saveColors();
  }
  function syncFieldsFromPicker(e) {
    var hex = e.pick.value || '#000000';
    e.r.value = parseInt(hex.substr(1, 2), 16);
    e.g.value = parseInt(hex.substr(3, 2), 16);
    e.b.value = parseInt(hex.substr(5, 2), 16);
  }
  function loadColors() {
    api('GET', '/api/samplecolors', function (status, data) {
      if (status !== 200 || !data || !data.filter) return;
      var slots = String(data.filter).split(';');
      for (var i = 0; i < cfEls.length && i < slots.length; i++) {
        var f = slots[i].split(',');
        if (f.length < 6) continue;
        var e = cfEls[i];
        e.on.checked = (f[0] === '1');
        e.a.value = clamp255(f[1]); e.r.value = clamp255(f[2]); e.g.value = clamp255(f[3]);
        e.b.value = clamp255(f[4]); e.tol.value = clamp255(f[5]);
        e.pick.value = rgbToHex(f[2], f[3], f[4]);
      }
    });
  }
  (function wireColorSlots() {
    for (var i = 0; i < cfEls.length; i++) {
      (function (e, idx) {
        e.on.addEventListener('change', saveColors);
        e.tol.addEventListener('input', saveColors);
        ['a', 'r', 'g', 'b'].forEach(function (k) {
          e[k].addEventListener('input', function () {
            e.pick.value = rgbToHex(e.r.value, e.g.value, e.b.value);
            saveColors();
          });
        });
        e.pick.addEventListener('input', function () { syncFieldsFromPicker(e); saveColors(); });
        e.target.addEventListener('click', function () { setActiveSlot(idx); });
      })(cfEls[i], i);
    }
  })();
  setActiveSlot(0);
  loadColors();

  // Magnifier: load a row crop, hover to inspect, click a pixel to fill the active
  // color slot. A 1:1 offscreen canvas is used to read pixel values.
  var magCanvas = document.getElementById('magCanvas');
  var magCtx = magCanvas ? magCanvas.getContext('2d') : null;
  var magReadout = document.getElementById('magreadout');
  var magSrc = document.createElement('canvas');
  var magSctx = magSrc.getContext('2d');
  var magImg = new Image();
  var magW = 0, magH = 0, magZoom = 1, magReady = false;

  function drawMag(hx, hy) {
    if (!magCtx) return;
    magCtx.imageSmoothingEnabled = false;
    magCtx.clearRect(0, 0, magCanvas.width, magCanvas.height);
    if (!magReady) return;
    magCtx.drawImage(magSrc, 0, 0, magW, magH, 0, 0, magW * magZoom, magH * magZoom);
    if (hx >= 0 && hy >= 0) {
      magCtx.strokeStyle = '#ff3b3b'; magCtx.lineWidth = 1;
      magCtx.strokeRect(hx * magZoom + 0.5, hy * magZoom + 0.5, magZoom - 1, magZoom - 1);
    }
  }
  function loadMag(id) {
    magImg.onload = function () {
      magW = magImg.naturalWidth || 1; magH = magImg.naturalHeight || 1;
      magSrc.width = magW; magSrc.height = magH;
      magSctx.clearRect(0, 0, magW, magH);
      magSctx.drawImage(magImg, 0, 0);
      magZoom = Math.max(2, Math.min(16, Math.floor(360 / magW)));
      magCanvas.width = magW * magZoom; magCanvas.height = magH * magZoom;
      magReady = true;
      drawMag(-1, -1);
      if (magReadout) magReadout.textContent = 'pick a pixel → Color ' + (activeSlot + 1);
    };
    magImg.src = '/api/sample/image?id=' + id + '&mag=1';
  }
  function magPixelAt(evt) {
    var rect = magCanvas.getBoundingClientRect();
    // Canvas may be CSS-scaled (max-width); map client coords back to canvas pixels.
    var sx = magCanvas.width / rect.width, sy = magCanvas.height / rect.height;
    var cx = (evt.clientX - rect.left) * sx, cy = (evt.clientY - rect.top) * sy;
    var px = Math.floor(cx / magZoom), py = Math.floor(cy / magZoom);
    if (px < 0 || py < 0 || px >= magW || py >= magH) return null;
    var d = magSctx.getImageData(px, py, 1, 1).data;
    return { x: px, y: py, r: d[0], g: d[1], b: d[2], a: d[3] };
  }
  if (magCanvas) {
    magCanvas.addEventListener('mousemove', function (evt) {
      if (!magReady) return;
      var p = magPixelAt(evt);
      if (!p) { drawMag(-1, -1); return; }
      drawMag(p.x, p.y);
      if (magReadout) magReadout.textContent =
        'rgba(' + p.r + ',' + p.g + ',' + p.b + ',' + p.a + ') → Color ' + (activeSlot + 1);
    });
    magCanvas.addEventListener('mouseleave', function () { drawMag(-1, -1); });
    magCanvas.addEventListener('click', function (evt) {
      if (!magReady) return;
      var p = magPixelAt(evt);
      if (!p) return;
      applyColorToSlot(activeSlot, p.r, p.g, p.b, p.a);
      if (!cfEls[activeSlot].on.checked) { cfEls[activeSlot].on.checked = true; saveColors(); }
      if (magReadout) magReadout.textContent =
        'Color ' + (activeSlot + 1) + ' = rgba(' + p.r + ',' + p.g + ',' + p.b + ',' + p.a + ')';
    });
  }

  loadFontInfo();
  pollCapture();
  refresh();
  // Each poll: optionally auto-remove duplicate samples first, then re-sync.
  setInterval(function () {
    if (autoDedup && autoDedup.checked && !(noPrefill && noPrefill.checked)) {
      api('POST', '/api/samples/dedup', function () { refresh(); pollCapture(); });
    } else {
      refresh();
      pollCapture();
    }
  }, 1000);
})();
