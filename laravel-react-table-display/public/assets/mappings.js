(function () {
  var tbody = document.getElementById('tbody');
  var status = document.getElementById('status');
  var refresh = document.getElementById('refresh');
  var unverifiedOnly = document.getElementById('unverifiedOnly');
  var title = document.getElementById('title');
  title.textContent = 'OCR Name Mappings — port ' + (window.location.port || '80');

  function escapeHtml(value) {
    return String(value == null ? '' : value).replace(/[&<>"']/g, function (ch) {
      return { '&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;' }[ch];
    });
  }

  function setStatus(text, isError) {
    status.textContent = text || '';
    status.style.color = isError ? '#ff8080' : '#bac2ca';
  }

  function load() {
    setStatus('Loading...');
    var url = '/api/mappings' + (unverifiedOnly.checked ? '?unverified=1' : '');
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url, true);
    xhr.onreadystatechange = function () {
      if (xhr.readyState !== 4) return;
      if (xhr.status < 200 || xhr.status >= 300) {
        setStatus('HTTP ' + xhr.status, true);
        return;
      }
      var data;
      try { data = JSON.parse(xhr.responseText); }
      catch (e) { setStatus(e.message, true); return; }
      if (data.error) { setStatus(data.error, true); render([]); return; }
      var rows = data.rows || [];
      render(rows);
      setStatus(rows.length + ' mapping' + (rows.length === 1 ? '' : 's'));
    };
    xhr.send();
  }

  function render(rows) {
    var html = '';
    if (!rows.length) {
      html = '<tr><td colspan="7" class="empty">No mappings.</td></tr>';
    } else {
      for (var i = 0; i < rows.length; i++) {
        var r = rows[i];
        var verifiedBtn = r.verified
          ? '<button class="act unverify" data-id="' + r.id + '">Unverify</button>'
          : '<button class="act verify" data-id="' + r.id + '">Verify</button>';
        html +=
          '<tr class="' + (r.verified ? 'verified' : 'unverified') + '">' +
            '<td class="ocr">' + escapeHtml(r.ocr) + '</td>' +
            '<td class="actual">' + escapeHtml(r.actual) + '</td>' +
            '<td class="site">' + escapeHtml(r.site) + '</td>' +
            '<td class="vflag">' + (r.verified ? '✓' : '') + '</td>' +
            '<td class="conf">' + (r.confidence != null ? Number(r.confidence).toFixed(2) : '') + '</td>' +
            '<td class="updated">' + escapeHtml(r.updated) + '</td>' +
            '<td class="actions">' + verifiedBtn +
              ' <button class="act delete" data-id="' + r.id + '">Delete</button>' +
            '</td>' +
          '</tr>';
      }
    }
    tbody.innerHTML = html;
  }

  function actOn(action, id) {
    setStatus('Working...');
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/mappings/' + action + '?id=' + encodeURIComponent(id), true);
    xhr.onreadystatechange = function () {
      if (xhr.readyState !== 4) return;
      load();
    };
    xhr.send();
  }

  tbody.addEventListener('click', function (event) {
    var target = event.target;
    if (!target || target.tagName !== 'BUTTON') return;
    var id = target.getAttribute('data-id');
    if (!id) return;
    if (target.classList.contains('verify')) actOn('verify', id);
    else if (target.classList.contains('unverify')) actOn('unverify', id);
    else if (target.classList.contains('delete')) {
      if (confirm('Delete mapping ' + id + '?')) actOn('delete', id);
    }
  });

  refresh.addEventListener('click', load);
  unverifiedOnly.addEventListener('change', load);
  load();
})();
