(function () {
  var tbody = document.getElementById('tbody');
  var statusEl = document.getElementById('status');
  var groupSel = document.getElementById('group');
  var captureBtn = document.getElementById('capture');
  var saveAllBtn = document.getElementById('saveAll');
  var clearAllBtn = document.getElementById('clearAll');
  var emptyHint = document.getElementById('empty');

  var rendered = {};      // gid -> { tr, input }
  var currentGroup = 0;

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

  for (var g = 0; g < 10; g++) {
    var opt = document.createElement('option');
    opt.value = String(g);
    opt.textContent = 'Text' + g;
    groupSel.appendChild(opt);
  }
  groupSel.value = '0';

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

    var tdC = document.createElement('td');
    tdC.className = 'c';
    var input = document.createElement('input');
    input.className = 'ch';
    input.type = 'text';
    input.maxLength = 1;
    input.value = gl.assigned || '';
    if (input.value) input.classList.add('done');
    function commit() {
      api('POST', '/api/fonts/setchar?gid=' + gl.gid + '&ch=' + encodeURIComponent(input.value), function () {});
      if (input.value) input.classList.add('done'); else input.classList.remove('done');
    }
    input.addEventListener('input', commit);
    input.addEventListener('keydown', function (e) {
      if (e.key === 'Enter') { e.preventDefault(); commit(); focusNext(input); }
    });
    tdC.appendChild(input);
    tr.appendChild(tdC);

    var tdR = document.createElement('td');
    tdR.className = 'r';
    tdR.textContent = gl.region + ' · Text' + gl.group;
    tdR.title = gl.hexmash || '';
    tr.appendChild(tdR);

    var tdA = document.createElement('td');
    tdA.className = 'a';
    var del = document.createElement('button');
    del.textContent = '✕'; del.tabIndex = -1;
    del.addEventListener('click', function () {
      api('POST', '/api/fonts/delete?gid=' + gl.gid, function () {
        if (tr.parentNode) tr.parentNode.removeChild(tr);
        delete rendered[gl.gid];
        retab();
      });
    });
    tdA.appendChild(del);
    tr.appendChild(tdA);

    rendered[gl.gid] = { tr: tr, input: input };
    return tr;
  }

  function refresh() {
    api('GET', '/api/fonts/list', function (status, rows) {
      if (status !== 200 || !rows) { statusEl.textContent = 'Disconnected'; return; }
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
    });
  }

  groupSel.addEventListener('change', function () {
    currentGroup = parseInt(groupSel.value, 10);
    api('POST', '/api/fonts/group?group=' + currentGroup, function () {});
  });

  captureBtn.addEventListener('click', function () {
    statusEl.textContent = 'Capturing…';
    api('POST', '/api/fonts/capture?group=' + currentGroup, function () { refresh(); });
  });

  saveAllBtn.addEventListener('click', function () {
    api('POST', '/api/fonts/save', function (status, data) {
      if (data && typeof data.saved === 'number') statusEl.textContent = 'Saved ' + data.saved + ' font(s) to the tablemap';
      refresh();
    });
  });

  clearAllBtn.addEventListener('click', function () {
    if (!confirm('Discard ALL pending glyphs? (Saved fonts are kept.)')) return;
    api('POST', '/api/fonts/clear', function () { refresh(); });
  });

  // Tell the server our initial working group.
  api('POST', '/api/fonts/group?group=0', function () {});
  refresh();
  setInterval(refresh, 1500);
})();
