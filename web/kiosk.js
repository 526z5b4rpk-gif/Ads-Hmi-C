// Compléments au front React, sans le modifier :
//  - bouton « Quitter » en mode exécution (#/run/…) quand le service l'autorise (--kiosk ou --quit) :
//    arrête le service, dont le code de retour est 0 (le navigateur en kiosque est fermé avec lui) ;
//  - badge « Lecture seule » quand ce poste ne peut pas modifier (poste distant sans jeton, --readonly).
(function () {
  'use strict';
  var session = null;

  function el(tag, css, text) {
    var e = document.createElement(tag);
    e.style.cssText = css;
    if (text) e.textContent = text;
    return e;
  }

  var base = 'position:fixed;z-index:2147483000;font:600 14px system-ui,sans-serif;border-radius:6px;';
  var quit = el('button', base + 'top:10px;right:10px;padding:8px 16px;border:1px solid #c0392b;' +
    'background:#e74c3c;color:#fff;cursor:pointer;box-shadow:0 2px 6px rgba(0,0,0,.3);display:none', 'Quitter');
  quit.type = 'button';
  quit.title = "Fermer l'IHM";
  var badge = el('div', base + 'bottom:10px;right:10px;padding:6px 12px;background:#555;color:#fff;opacity:.85;' +
    'pointer-events:none;display:none', 'Lecture seule');

  quit.addEventListener('click', function () {
    if (!window.confirm("Fermer l'IHM ?")) return;
    quit.disabled = true;
    fetch('/api/quit', { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{"code":0}' })
      .then(function (r) { return r.json(); })
      .then(function (j) {
        if (!j.ok) throw new Error(j.error || 'refusé');
        document.body.innerHTML = '';
        document.body.appendChild(el('div', 'font:20px system-ui,sans-serif;padding:40px;color:#888', 'IHM fermée.'));
        setTimeout(function () { window.close(); }, 300);
      })
      .catch(function (e) { quit.disabled = false; window.alert('Arrêt impossible : ' + e.message); });
  });

  function update() {
    if (!session) return;
    quit.style.display = session.quit && /^#\/run\//.test(location.hash) ? 'block' : 'none';
    badge.style.display = session.access === 'read' ? 'block' : 'none';
  }

  function init() {
    document.body.appendChild(quit);
    document.body.appendChild(badge);
    fetch('/api/session', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (s) { session = s; update(); })
      .catch(function () {});
    window.addEventListener('hashchange', update);
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
