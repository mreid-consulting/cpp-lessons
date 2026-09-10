// Copy buttons, nav filter, active-section tracking.
(function () {
  document.querySelectorAll('.copy').forEach(function (btn) {
    btn.addEventListener('click', function () {
      var code = btn.closest('figure').querySelector('code');
      var text = code.innerText;
      var done = function () {
        btn.textContent = 'copied';
        btn.classList.add('done');
        setTimeout(function () {
          btn.textContent = 'copy';
          btn.classList.remove('done');
        }, 1400);
      };
      if (navigator.clipboard && window.isSecureContext) {
        navigator.clipboard.writeText(text).then(done, fallback);
      } else {
        fallback();
      }
      function fallback() {
        var ta = document.createElement('textarea');
        ta.value = text;
        ta.style.position = 'fixed';
        ta.style.opacity = '0';
        document.body.appendChild(ta);
        ta.select();
        try { document.execCommand('copy'); done(); } catch (e) { /* clipboard unavailable */ }
        document.body.removeChild(ta);
      }
    });
  });

  var q = document.getElementById('q');
  if (q) {
    q.addEventListener('input', function () {
      var term = q.value.trim().toLowerCase();
      document.querySelectorAll('.navtree details').forEach(function (d) {
        var shown = 0;
        d.querySelectorAll('li').forEach(function (li) {
          var hit = !term || li.textContent.toLowerCase().indexOf(term) !== -1;
          li.classList.toggle('hidden', !hit);
          if (hit) shown++;
        });
        d.style.display = shown ? '' : 'none';
        if (term) d.open = true;
      });
    });
    document.addEventListener('keydown', function (e) {
      if (e.key === '/' && document.activeElement !== q) {
        e.preventDefault();
        q.focus();
      }
    });
  }

  var here = document.querySelector('.navtree li.here');
  if (here) here.scrollIntoView({ block: 'center' });

  document.addEventListener('keydown', function (e) {
    if (e.target.tagName === 'INPUT' || e.metaKey || e.ctrlKey || e.altKey) return;
    var sel = e.key === 'ArrowRight' ? '.pg.next' : e.key === 'ArrowLeft' ? '.pg.prev' : null;
    if (!sel) return;
    var link = document.querySelector(sel);
    if (link) window.location.href = link.href;
  });
})();
