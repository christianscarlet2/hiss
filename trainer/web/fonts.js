(function () {
  var tbody = document.getElementById('tbody');
  var statusEl = document.getElementById('status');
  var captureBtn = document.getElementById('capture');
  var clearAllBtn = document.getElementById('clearAll');
  var undoBtn = document.getElementById('undo');
  var emptyHint = document.getElementById('empty');

  var rendered = {};      // gid -> { tr, input }
  var defaultGroup = 0;   // sticky group new rows default to

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

  function retab() {
    var rows = tbody.querySelectorAll('tr');
    var t = 1;
    for (var i = 0; i < rows.length; i++) {
      var inp = rows[i].querySelector('input.ch');
      if (inp) inp.tabIndex = t++;
    }
  }

  function focusNext(input) {
    var inputs = tbody.querySelectorAll('input.ch');
    for (var i = 0; i < inputs.length; i++) {
      if (inputs[i] === input && i + 1 < inputs.length) { inputs[i + 1].focus(); inputs[i + 1].select(); return; }
    }
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

    var tdC = document.createElement('td');
    tdC.className = 'c';
    var input = document.createElement('input');
    input.className = 'ch';
    input.type = 'text';
    input.maxLength = 1;
    input.value = gl.assigned || '';
    if (input.value) input.classList.add('done');
    function commit() {
      // Assigning a char creates the font (and purges duplicates) server-side;
      // the periodic refresh removes the now-covered rows shortly after.
      api('POST', '/api/fonts/setchar?gid=' + gl.gid + '&ch=' + encodeURIComponent(input.value), function () {});
      if (input.value) input.classList.add('done'); else input.classList.remove('done');
    }
    input.addEventListener('input', function () {
      commit();
      if (input.value) focusNext(input);   // auto-advance for fast labeling
    });
    input.addEventListener('keydown', function (e) {
      if (e.key === 'Enter') { e.preventDefault(); commit(); focusNext(input); }
    });
    tdC.appendChild(input);
    tr.appendChild(tdC);

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
      api('POST', '/api/fonts/setgroup?gid=' + gl.gid + '&group=' + defaultGroup, function () {});
    });
    tdGrp.appendChild(grpSel);
    tr.appendChild(tdGrp);

    // Apply this row's group to every row below it.
    var tdAg = document.createElement('td');
    tdAg.className = 'ag';
    var applyBtn = document.createElement('button');
    applyBtn.textContent = 'Apply Group Below';
    applyBtn.tabIndex = -1;
    applyBtn.addEventListener('click', function () {
      var val = grpSel.value;
      defaultGroup = parseInt(val, 10);
      var rows = tbody.querySelectorAll('tr');
      var below = false;
      for (var i = 0; i < rows.length; i++) {
        if (rows[i] === tr) { below = true; continue; }
        if (!below) continue;
        var sel = rows[i].querySelector('td.grp select');
        var rgid = rows[i].getAttribute('data-gid');
        if (sel && rgid) {
          sel.value = val;
          api('POST', '/api/fonts/setgroup?gid=' + rgid + '&group=' + val, function () {});
        }
      }
    });
    tdAg.appendChild(applyBtn);
    tr.appendChild(tdAg);

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

    rendered[gl.gid] = { tr: tr, input: input };
    return tr;
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
          delete rendered[id];
        }
      }
      var labeled = 0;
      for (var j = 0; j < rows.length; j++) {
        var gl = rows[j];
        if (!rendered[gl.gid]) tbody.appendChild(buildRow(gl));
        if (rows[j].assigned) labeled++;
      }
      retab();
      emptyHint.style.display = rows.length ? 'none' : '';
      statusEl.textContent = rows.length + ' glyph(s), ' + labeled + ' labeled';
      if (onDone) onDone();
    });
  }

  captureBtn.addEventListener('click', function () {
    statusEl.textContent = 'Capturing…';
    api('POST', '/api/fonts/capture', function () { refresh(); });
  });

  clearAllBtn.addEventListener('click', function () {
    if (!confirm('Discard ALL pending glyphs? (Saved fonts are kept.)')) return;
    api('POST', '/api/fonts/clear', function () { refresh(); });
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
        if (rendered.hasOwnProperty(id) && rendered[id].tr.parentNode) {
          rendered[id].tr.parentNode.removeChild(rendered[id].tr);
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

  refresh();
  setInterval(refresh, 1500);
})();
