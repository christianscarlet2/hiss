(function () {
  var tbody = document.getElementById('tbody');
  var statusEl = document.getElementById('status');
  var hideSaved = document.getElementById('hideSaved');
  var saveAllBtn = document.getElementById('saveAll');
  var clearAllBtn = document.getElementById('clearAll');
  var dedupBtn = document.getElementById('dedup');
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
    tdImg.appendChild(img);
    var rg = document.createElement('div');
    rg.className = 'region';
    rg.textContent = s.region + '  #' + s.id;
    tdImg.appendChild(rg);
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

    btnDel.addEventListener('click', function () {
      api('POST', '/api/sample/delete?id=' + s.id, function () {
        if (tr.parentNode) tr.parentNode.removeChild(tr);
        delete rendered[s.id];
        retab();
      });
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

  saveAllBtn.addEventListener('click', function () {
    var ids = Object.keys(rendered);
    for (var i = 0; i < ids.length; i++) {
      var rec = rendered[ids[i]];
      if (rec.saved) continue;
      (function (rec, id) {
        saveRow(id, rec.input.value, function (ok) {
          if (ok) { rec.saved = true; rec.tr.classList.add('saved'); }
          applyHideSaved();
        });
      })(rec, ids[i]);
    }
  });

  clearAllBtn.addEventListener('click', function () {
    if (!confirm('Remove ALL samples from the list? (saved files are kept)')) return;
    api('POST', '/api/samples/clear', function () { refresh(); });
  });

  dedupBtn.addEventListener('click', function () {
    api('POST', '/api/samples/dedup', function (status, data) {
      if (data && typeof data.removed === 'number') {
        statusEl.textContent = 'Removed ' + data.removed + ' duplicate(s)';
      }
      refresh();
    });
  });

  hideSaved.addEventListener('change', applyHideSaved);

  var createFontsBtn = document.getElementById('createFonts');
  if (createFontsBtn) {
    createFontsBtn.addEventListener('click', function () {
      api('POST', '/api/fonts/open', function () {});
    });
  }

  // ---- Transform dropdown: AutoOcr0/1 (Tesseract) + Text0..Text9 (fonts) -----
  function buildGroupOptions(counts) {
    textGroup.innerHTML = '';
    var add = function (value, label) {
      var opt = document.createElement('option');
      opt.value = value; opt.textContent = label; textGroup.appendChild(opt);
    };
    add('AutoOcr0', 'AutoOcr0');
    add('AutoOcr1', 'AutoOcr1');
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

  loadFontInfo();
  refresh();
  setInterval(refresh, 1000);
})();
