(function () {
  var tbody = document.getElementById('tbody');
  var statusEl = document.getElementById('status');
  var hideSaved = document.getElementById('hideSaved');
  var saveAllBtn = document.getElementById('saveAll');

  // Remember which ids we've already rendered so live edits aren't clobbered
  // by the poll, and so newly captured samples slot in without a full redraw.
  var rendered = {};   // id -> { tr, input, saved }

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

  function buildRow(s) {
    var tr = document.createElement('tr');

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
    var input = document.createElement('input');
    input.className = 'label';
    input.type = 'text';
    input.value = s.guess || '';
    input.addEventListener('keydown', function (e) {
      if (e.key === 'Enter') { doSave(); }
    });
    tdTxt.appendChild(input);
    tr.appendChild(tdTxt);

    var tdAct = document.createElement('td');
    var btn = document.createElement('button');
    btn.className = 'row-save';
    btn.textContent = 'Save';
    tdAct.appendChild(btn);
    tr.appendChild(tdAct);

    function doSave() {
      saveRow(s.id, input.value, function (ok) {
        if (ok) {
          var rec = rendered[s.id];
          if (rec) { rec.saved = true; }
          tr.classList.add('saved');
          applyHideSaved();
        }
      });
    }
    btn.addEventListener('click', doSave);

    rendered[s.id] = { tr: tr, input: input, saved: !!s.saved };
    if (s.saved) { tr.classList.add('saved'); }
    return tr;
  }

  function applyHideSaved() {
    var hide = hideSaved.checked;
    for (var id in rendered) {
      if (!rendered.hasOwnProperty(id)) continue;
      var rec = rendered[id];
      rec.tr.style.display = (hide && rec.saved) ? 'none' : '';
    }
  }

  function refresh() {
    api('GET', '/api/samples', function (status, rows) {
      if (status !== 200 || !rows) {
        statusEl.textContent = 'Disconnected';
        return;
      }
      var pending = 0;
      for (var i = 0; i < rows.length; i++) {
        var s = rows[i];
        if (!rendered[s.id]) {
          tbody.appendChild(buildRow(s));   // newest at the bottom
        } else {
          // Reflect saved-state changes coming from the server.
          var rec = rendered[s.id];
          if (s.saved && !rec.saved) {
            rec.saved = true;
            rec.tr.classList.add('saved');
          }
        }
        if (!rendered[s.id].saved) pending++;
      }
      statusEl.textContent = rows.length + ' samples, ' + pending + ' unsaved';
      applyHideSaved();
    });
  }

  saveAllBtn.addEventListener('click', function () {
    // Push every current input value, then ask the server to flush.
    var ids = Object.keys(rendered);
    var remaining = 0;
    for (var i = 0; i < ids.length; i++) {
      var rec = rendered[ids[i]];
      if (rec.saved) continue;
      remaining++;
      (function (rec, id) {
        saveRow(id, rec.input.value, function (ok) {
          if (ok) { rec.saved = true; rec.tr.classList.add('saved'); }
          applyHideSaved();
        });
      })(rec, ids[i]);
    }
    if (remaining === 0) { statusEl.textContent = 'Nothing to save'; }
  });

  hideSaved.addEventListener('change', applyHideSaved);

  refresh();
  setInterval(refresh, 1000);
})();
