// Test de bout en bout : lance le service, puis vérifie l'API REST, le front web, le protocole WebSocket
// (schéma, deltas, écritures) et les protections (chemins, hôte, origine), comme le ferait le navigateur.
// Usage : node tests/e2e.mjs <binaire>    (Node >= 22 : client WebSocket intégré)
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { startFakePlc } from './fake-plc.mjs';

const BIN = path.resolve(process.argv[2] ?? 'build/hmi-service');
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
// Windows : kill() termine le processus sans prévenir (TerminateProcess), pas d'arrêt propre observable
const WIN = process.platform === 'win32';
let failures = 0, checks = 0;

function check(cond, label) {
  checks++;
  if (cond) console.log(`  ok   ${label}`);
  else { failures++; console.log(`  ÉCHEC ${label}`); }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** démarre le service sur un port libre avec un dossier de données temporaire */
async function start(prepare, args = []) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'hmi-e2e-'));
  fs.mkdirSync(path.join(dir, 'dashboards'));
  fs.copyFileSync(path.join(ROOT, 'data/dashboards/demo.json'), path.join(dir, 'dashboards/demo.json'));
  prepare?.(dir);
  const port = 20000 + Math.floor(Math.random() * 20000);
  const proc = spawn(BIN, ['--data', dir, '--web', path.join(ROOT, 'web'), '--port', String(port), ...args], { stdio: ['ignore', 'pipe', 'pipe'] });
  let log = '';
  proc.stderr.on('data', (d) => (log += d));
  proc.stdout.on('data', (d) => (log += d));
  for (let i = 0; i < 100 && !log.includes('http://'); i++) await sleep(50);
  const base = `http://127.0.0.1:${port}`;
  const exited = new Promise((r) => proc.on('exit', (code) => r(code)));
  return { dir, port, base, proc, exited, log: () => log };
}

async function stop(s) {
  if (s.proc.exitCode === null && s.proc.signalCode === null) s.proc.kill('SIGTERM');
  const code = await Promise.race([s.exited, sleep(5000).then(() => 'timeout')]);
  fs.rmSync(s.dir, { recursive: true, force: true });
  return code;
}

/** requête HTTP brute (chemins non normalisés, en-têtes interdits par fetch) */
function raw(port, text) {
  return new Promise((resolve) => {
    const sock = net.connect(port, '127.0.0.1', () => sock.write(text));
    let data = '';
    // une connexion WebSocket acceptée reste ouverte : réponse rendue après 1 s au plus
    const timer = setTimeout(() => { sock.destroy(); resolve(data); }, 1000);
    const done = () => { clearTimeout(timer); resolve(data); };
    sock.on('data', (d) => (data += d));
    sock.on('end', done);
    sock.on('error', done);
  });
}

/** client WebSocket qui mémorise les messages reçus */
function wsClient(base) {
  const ws = new WebSocket(base.replace('http', 'ws') + '/ws');
  const msgs = [];
  const waiters = [];
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    msgs.push(m);
    for (const w of [...waiters]) if (w.pred(m)) { waiters.splice(waiters.indexOf(w), 1); w.resolve(m); }
  };
  const waitFor = (pred, ms = 3000) => {
    const found = msgs.find(pred);
    if (found) return Promise.resolve(found);
    return new Promise((resolve) => {
      const w = { pred, resolve };
      waiters.push(w);
      setTimeout(() => { const i = waiters.indexOf(w); if (i >= 0) { waiters.splice(i, 1); resolve(null); } }, ms);
    });
  };
  const opened = new Promise((r) => { ws.onopen = () => r(true); ws.onerror = () => r(false); });
  let wid = 0;
  const write = async (p, value) => {
    const id = ++wid;
    ws.send(JSON.stringify({ t: 'w', id, path: p, value }));
    return waitFor((m) => m.t === 'wr' && m.id === id);
  };
  return { ws, msgs, waitFor, opened, write, clear: () => (msgs.length = 0) };
}

/* ------------------------------------------------------------------------------------------ */

console.log('Service normal (simulateur)');
const s = await start();
try {
  // front web
  let r = await fetch(s.base + '/');
  const html = await r.text();
  check(r.status === 200 && r.headers.get('content-type').startsWith('text/html') && html.includes('<div id="root">'), 'GET / sert index.html');
  const js = html.match(/src="(\/assets\/[^"]+\.js)"/)?.[1];
  r = await fetch(s.base + js);
  check(r.status === 200 && r.headers.get('content-type').startsWith('text/javascript') && r.headers.get('cache-control').includes('immutable'), `GET ${js} (cache immuable)`);
  r = await fetch(s.base + '/run/demo');
  check(r.status === 200 && (await r.text()).includes('<div id="root">'), 'route inconnue -> index.html (application monopage)');

  // API
  r = await fetch(s.base + '/api/config');
  const cfg = await r.json();
  check(cfg.source === 'sim' && cfg.ads.targetAdsPort === 851 && cfg.sim.cycleTime === 20, 'GET /api/config : valeurs par défaut');
  r = await fetch(s.base + '/api/dashboards');
  const list = await r.json();
  check(Array.isArray(list) && list.some((d) => d.id === 'demo' && d.name.startsWith('Démo')), 'GET /api/dashboards : écran de démo');
  r = await fetch(s.base + '/api/dashboards/demo');
  check(r.status === 200 && (await r.json()).widgets.length > 10, 'GET /api/dashboards/demo');
  r = await fetch(s.base + '/api/dashboards/absent');
  check(r.status === 404, 'écran absent -> 404');
  r = await fetch(s.base + '/api/dashboards/a.b');
  check(r.status === 400, 'identifiant invalide -> 400');
  r = await fetch(s.base + '/api/inconnue');
  check(r.status === 404 && (await r.json()).error, 'route API inconnue -> 404 JSON');

  // WebSocket : statut + schéma à l'ouverture, puis deltas
  const c = wsClient(s.base);
  check(await c.opened, 'WebSocket /ws ouvert');
  const st = await c.waitFor((m) => m.t === 'status');
  check(st?.status.state === 'connected' && st.status.source === 'sim', 'message status : simulateur connecté');
  const sch = await c.waitFor((m) => m.t === 'schema' && m.schema.leaves.length > 0);
  const leaves = sch?.schema.leaves ?? [];
  const idx = (p) => leaves.find((l) => l.path === p)?.i;
  check(leaves.length === 42 && sch.values.length === 42, 'schéma : 42 variables et leurs valeurs');
  const eState = leaves.find((l) => l.path === 'SIM.stHmi.eState');
  check(eState?.kind === 'enum' && eState.enums?.[2]?.name === 'Production', 'énumération E_State décrite');
  check(leaves.find((l) => l.path === 'SIM.stHmi.rTemperature')?.attrs?.unit === '°C', 'attribut unit (UTF-8)');
  check(sch.values[idx('SIM.stHmi.sMessage')] === "Machine à l'arrêt", 'valeur STRING décodée (Latin-1 -> UTF-8)');
  const d = await c.waitFor((m) => m.t === 'd' && m.v.length >= 2);
  check(d && typeof d.ts === 'number' && Math.abs(d.ts - Date.now()) < 5000, 'deltas reçus, horodatés');

  // écritures
  let wr = await c.write('SIM.stHmi.stCmd.bMarche', true);
  check(wr?.ok === true, 'écriture bMarche = true acceptée');
  const iRun = idx('SIM.stHmi.bRun');
  const run = await c.waitFor((m) => m.t === 'd' && m.v.some((x, k) => k % 2 === 0 && x === iRun && m.v[k + 1] === true));
  check(!!run, 'delta bRun = true après la commande (boucle complète)');
  wr = await c.write('sim.sthmi.stcmd.rconsignevitesse', '45,5');
  check(wr?.ok === true, 'écriture insensible à la casse, virgule décimale');
  const iCsg = idx('SIM.stHmi.stCmd.rConsigneVitesse');
  check(!!(await c.waitFor((m) => m.t === 'd' && m.v.some((x, k) => k % 2 === 0 && x === iCsg && m.v[k + 1] === 45.5))), 'consigne relue = 45.5');
  wr = await c.write('SIM.stHmi.stCmd.nMode', 99999);
  check(wr?.ok === false && wr.error.includes('hors limites'), 'INT hors limites refusé');
  wr = await c.write('SIM.stHmi.nexistepas', 1);
  check(wr?.ok === false && wr.error.includes('Variable inconnue'), 'variable inconnue refusée');
  wr = await c.write('SIM.stHmi.eMode', 'Maintenance');
  check(wr?.ok === true, 'énumération écrite par son nom');

  // écrans : création, notification, suppression
  c.clear();
  r = await fetch(s.base + '/api/dashboards/essai_1', { method: 'PUT', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ name: 'Essai', widgets: [] }) });
  check(r.status === 200 && (await r.json()).ok === true, 'PUT écran');
  check(!!(await c.waitFor((m) => m.t === 'dashboards')), 'notification dashboards diffusée');
  r = await fetch(s.base + '/api/dashboards/essai_1');
  const saved = await r.json();
  check(saved.id === 'essai_1' && /^\d{4}-\d\d-\d\dT.*Z$/.test(saved.updatedAt), 'écran relu avec id et updatedAt');
  r = await fetch(s.base + '/api/dashboards/essai_1', { method: 'DELETE' });
  check(r.status === 200, 'DELETE écran');
  check(!fs.existsSync(path.join(s.dir, 'dashboards/essai_1.json')), 'fichier supprimé');
  r = await fetch(s.base + '/api/dashboards/essai_1', { method: 'PUT', body: 'pas du json' });
  check(r.status === 400, 'écran invalide -> 400');

  // protections
  let resp = await raw(s.port, 'GET /../../etc/passwd HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n');
  check(resp.startsWith('HTTP/1.1 403'), 'chemin « .. » refusé');
  resp = await raw(s.port, 'GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n');
  check(resp.startsWith('HTTP/1.1 403'), 'chemin « %2e%2e » refusé');
  resp = await raw(s.port, 'GET /api/config HTTP/1.1\r\nHost: attaquant.example\r\n\r\n');
  check(resp.startsWith('HTTP/1.1 403'), 'hôte étranger refusé (DNS rebinding)');
  resp = await raw(s.port, `PUT /api/dashboards/x HTTP/1.1\r\nHost: 127.0.0.1:${s.port}\r\nOrigin: http://attaquant.example\r\nContent-Length: 2\r\n\r\n{}`);
  check(resp.startsWith('HTTP/1.1 403'), 'écriture depuis une autre origine refusée');
  resp = await raw(s.port, `GET /ws HTTP/1.1\r\nHost: 127.0.0.1:${s.port}\r\nOrigin: http://attaquant.example\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n`);
  check(resp.startsWith('HTTP/1.1 403'), 'WebSocket depuis une autre origine refusé');
  resp = await raw(s.port, `GET /ws HTTP/1.1\r\nHost: 127.0.0.1:${s.port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n`);
  check(resp.includes('Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo='), 'poignée de main WebSocket conforme RFC 6455');
  resp = await raw(s.port, `PUT /api/dashboards/x HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 999999999\r\n\r\n`);
  check(resp.startsWith('HTTP/1.1 413'), 'corps trop volumineux refusé');
  resp = await raw(s.port, 'n\'importe quoi\r\n\r\n');
  check(resp.startsWith('HTTP/1.1 400'), 'requête malformée -> 400');

  // changement de configuration : nouveau schéma diffusé
  c.clear();
  r = await fetch(s.base + '/api/config', { method: 'PUT', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ ...cfg, sim: { cycleTime: 50 } }) });
  check(r.status === 200, 'PUT /api/config');
  const sch2 = await c.waitFor((m) => m.t === 'schema' && m.schema.leaves.length === 42 && m.schema.version > sch.schema.version);
  check(!!sch2, 'source redémarrée : nouveau schéma');
  const saved_cfg = JSON.parse(fs.readFileSync(path.join(s.dir, 'config.json'), 'utf8'));
  check(saved_cfg.sim.cycleTime === 50 && saved_cfg.ads.targetAdsPort === 851, 'config.json écrit et complété');
  c.ws.close();

  // plusieurs clients simultanés
  const many = Array.from({ length: 20 }, () => wsClient(s.base));
  const okAll = await Promise.all(many.map((m) => m.waitFor((x) => x.t === 'd', 3000)));
  check(okAll.every(Boolean), '20 clients simultanés reçoivent les deltas');
  many.forEach((m) => m.ws.close());
} finally {
  const code = await stop(s);
  check(WIN ? code !== 'timeout' : code === 0, WIN ? `arrêt (Windows, code ${code})` : `arrêt propre sur SIGTERM (code ${code})`);
  if (/runtime error|AddressSanitizer|LeakSanitizer/.test(s.log())) { failures++; console.log(s.log()); }
}

console.log('config.json illisible');
const s2 = await start((dir) => fs.writeFileSync(path.join(dir, 'config.json'), '{ "source": "ads", '));
try {
  const c = wsClient(s2.base);
  const st = await c.waitFor((m) => m.t === 'status');
  check(st?.status.state === 'error' && st.status.message.includes('config.json illisible'), 'aucune source démarrée, erreur affichée');
  const sch = await c.waitFor((m) => m.t === 'schema');
  check(sch?.schema.leaves.length === 0, 'aucune variable (pas de repli silencieux sur le simulateur)');
  check(fs.readdirSync(s2.dir).some((f) => f.startsWith('config.json.illisible-')), "copie de l'original conservée");
  c.ws.close();
} finally {
  const code = await stop(s2);
  check(WIN ? code !== 'timeout' : code === 0, 'arrêt propre');
}

console.log('Variables calculées');
const s3 = await start((dir) => fs.writeFileSync(path.join(dir, 'calculs.json'), JSON.stringify({ variables: [
  { name: 'rVolume', expr: 'SIM.stHmi.rNiveau / 100 * 2500', unit: 'L', comment: 'Volume cuve' },
  { name: 'bPlein', expr: 'rVolume > 2000' },
] })));
try {
  const c = wsClient(s3.base);
  const sch = await c.waitFor((m) => m.t === 'schema' && m.schema.leaves.length === 44);
  check(!!sch && sch.schema.roots.includes('CALC'), 'calculs.json chargé : symbole CALC dans le schéma');
  const vol = sch?.schema.leaves.find((l) => l.path === 'CALC.rVolume');
  check(vol?.type === 'LREAL' && vol.attrs?.unit === 'L' && vol.comment === 'Volume cuve', 'variable calculée décrite (type, unité, commentaire)');
  check(sch?.schema.leaves.find((l) => l.path === 'CALC.bPlein')?.kind === 'bool', 'type BOOL déduit de la comparaison');
  const iNiv = sch.schema.leaves.find((l) => l.path === 'SIM.stHmi.rNiveau').i;
  const coherent = await c.waitFor((m) => {
    if (m.t !== 'd') return false;
    const get = (i) => { for (let k = 0; k < m.v.length; k += 2) if (m.v[k] === i) return m.v[k + 1]; };
    const n = get(iNiv), v = get(vol.i);
    return n !== undefined && v !== undefined && Math.abs(v - n * 25) < 1e-3;
  }, 4000);
  check(!!coherent, 'delta : valeur calculée cohérente avec sa source, dans le même message');
  const wr = await c.write('CALC.rVolume', 1);
  check(wr?.ok === false && wr.error.includes('lecture seule'), 'écriture d\'une variable calculée refusée');

  let r = await fetch(s3.base + '/api/calc');
  const list = await r.json();
  check(list.variables.length === 2 && typeof list.variables[0].value === 'number' && list.variables[1].plcType === 'BOOL', 'GET /api/calc : définitions et valeurs');
  r = await fetch(s3.base + '/api/calc/test', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ expr: 'max(1, 2) + rVolume * 0' }) });
  check((await r.json()).value === 2, 'POST /api/calc/test : évaluation');
  r = await fetch(s3.base + '/api/calc/test', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ expr: '(1' }) });
  check((await r.json()).error?.includes('parenthèse'), 'POST /api/calc/test : erreur de syntaxe expliquée');

  c.clear();
  r = await fetch(s3.base + '/api/calc', { method: 'PUT', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ variables: [{ name: 'a', expr: 'b' }, { name: 'b', expr: '1' }] }) });
  check(r.status === 400 && (await r.json()).error.includes('définie plus bas'), 'PUT /api/calc : ordre incorrect refusé');
  check(JSON.parse(fs.readFileSync(path.join(s3.dir, 'calculs.json'), 'utf8')).variables.length === 2, 'fichier inchangé après un refus');
  r = await fetch(s3.base + '/api/calc', { method: 'PUT', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ variables: [{ name: 'rDemi', expr: 'SIM.stHmi.rNiveau / 2', junk: 1 }] }) });
  check(r.status === 200, 'PUT /api/calc');
  const sch2 = await c.waitFor((m) => m.t === 'schema' && m.schema.leaves.some((l) => l.path === 'CALC.rDemi'));
  check(!!sch2 && sch2.schema.leaves.length === 43, 'nouveau schéma diffusé aux clients');
  check(sch2 && sch2.values[sch2.schema.leaves.find((l) => l.path === 'SIM.stHmi.rNiveau').i] !== null, 'valeurs de la source conservées au changement');
  const file = JSON.parse(fs.readFileSync(path.join(s3.dir, 'calculs.json'), 'utf8'));
  check(file.variables.length === 1 && file.variables[0].junk === undefined, 'calculs.json écrit et normalisé');
  const resp = await raw(s3.port, `PUT /api/calc HTTP/1.1\r\nHost: 127.0.0.1:${s3.port}\r\nOrigin: http://evil.example\r\nContent-Length: 16\r\n\r\n{"variables":[]}`);
  check(resp.startsWith('HTTP/1.1 403'), 'PUT /api/calc depuis une autre origine refusé');
  r = await fetch(s3.base + '/calculs.html');
  check(r.status === 200 && (await r.text()).includes('Variables calculées'), 'page /calculs.html servie');
  c.ws.close();
} finally {
  const code = await stop(s3);
  check(WIN ? code !== 'timeout' : code === 0, 'arrêt propre');
  if (/runtime error|AddressSanitizer|LeakSanitizer/.test(s3.log())) { failures++; console.log(s3.log()); }
}

/* ------------------------------------------------------------------------------------------ */

const adsConfig = (plc, extra = {}) => (dir) => fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  source: 'ads',
  ads: { targetAmsNetId: '10.0.0.2.1.1', targetAdsPort: 851, mode: 'direct', routerAddress: '127.0.0.1', routerTcpPort: plc.port,
    localAmsNetId: '10.0.0.9.1.1', localAdsPort: 32750, symbols: ['MAIN.stPlc'], cycleTime: 10, ...extra },
}));
const leafOf = (sch, p) => sch?.schema.leaves.find((l) => l.path === p);
const valueOf = (sch, p) => sch?.values[leafOf(sch, p)?.i];

console.log('Source ADS (faux automate)');
const plc = await startFakePlc({ cycleMs: 20 });
const s4 = await start(adsConfig(plc));
try {
  const c = wsClient(s4.base);
  const sch = await c.waitFor((m) => m.t === 'schema' && m.schema.leaves.length > 0, 5000);
  check(sch?.schema.leaves.length === 12, `schéma lu sur l'automate (${sch?.schema.leaves.length} variables)`);
  check(!leafOf(sch, 'MAIN.stPlc.pPtr'), 'pointeur ignoré');
  const temp = leafOf(sch, 'MAIN.stPlc.rTemp');
  check(temp?.type === 'REAL' && temp.comment === 'Température four' && temp.attrs?.unit === '°C', 'type, commentaire et attribut UTF-8');
  check(leafOf(sch, 'MAIN.stPlc.nCount')?.type === 'DINT' && valueOf(sch, 'MAIN.stPlc.nCount') === 0, 'type de base absent de la table (DINT)');
  const mode = leafOf(sch, 'MAIN.stPlc.eMode');
  check(mode?.kind === 'enum' && mode.enums?.some((e) => e.name === 'Defaut' && e.value === -1) && valueOf(sch, 'MAIN.stPlc.eMode') === 1, 'énumération (valeur négative comprise)');
  check(valueOf(sch, 'MAIN.stPlc.sMsg') === 'Bonjour' && valueOf(sch, 'MAIN.stPlc.aVal[3]') === 3.5, 'STRING et tableau');
  check(leafOf(sch, 'MAIN.stPlc.rPress')?.type === 'T_Pression' && leafOf(sch, 'MAIN.stPlc.rPress')?.attrs?.unit === 'bar', "alias : nom et attribut du type conservés");
  check(valueOf(sch, 'MAIN.stPlc.stSub.bBit0') === true && valueOf(sch, 'MAIN.stPlc.stSub.bBit1') === false && valueOf(sch, 'MAIN.stPlc.stSub.iX') === 7,
    'membres BIT (drapeau sur le membre ou sur le type)');
  const st = await c.waitFor((m) => m.t === 'status' && m.status.state === 'connected', 3000);
  check(st?.status.source === 'ads', 'statut connecté');

  let w = await c.write('MAIN.stPlc.stSub.bBit1', true);
  check(w?.ok && plc.mem[66] === 0b11, 'écriture d\'un BIT par handle (bit voisin conservé)');
  w = await c.write('MAIN.stPlc.rTemp', 42.5);
  check(w?.ok && plc.mem.readFloatLE(4) === 42.5, 'écriture REAL à l\'adresse du symbole');
  w = await c.write('MAIN.stPlc.eMode', 'Manu');
  check(w?.ok && plc.mem.readInt16LE(12) === 0, "écriture d'une énumération par son nom");
  w = await c.write('MAIN.stPlc.sMsg', 'Défaut');
  check(w?.ok && plc.mem.toString('latin1', 14, 20) === 'Défaut', 'écriture STRING (Latin-1)');
  w = await c.write('MAIN.stPlc.stSub.iX', 70000);
  check(w && !w.ok && plc.mem.readInt16LE(64) === 7, 'valeur hors bornes refusée, automate inchangé');

  c.clear();
  w = await c.write('MAIN.stPlc.bRun', true);
  const d = await c.waitFor((m) => m.t === 'd' && m.v.some((x, k) => k % 2 === 0 && x === leafOf(sch, 'MAIN.stPlc.nCount').i), 3000);
  check(w?.ok && !!d, 'notifications : valeurs du procédé reçues en delta');

  // passage en STOP puis RUN
  plc.state = 6;
  const stop = await c.waitFor((m) => m.t === 'status' && m.status.message.includes('STOP'), 3000);
  check(stop?.status.state === 'error', 'automate en STOP signalé');
  plc.state = 5;
  check(!!(await c.waitFor((m) => m.t === 'status' && m.status.state === 'connected' && m.msgs !== 0 && !m.status.message.includes('STOP') && m !== st, 3000)), 'retour en RUN');

  // nouveau programme : rechargement des symboles
  c.clear();
  plc.bumpVersion();
  const reload = await c.waitFor((m) => m.t === 'schema' && m.schema.leaves.length === 12, 5000);
  check(!!reload, 'nouveau programme détecté : symboles rechargés');

  // coupure réseau : reconnexion automatique
  await c.waitFor((m) => m.t === 'status' && m.status.state === 'connected', 3000);
  c.clear();
  plc.dropConnections();
  const lost = await c.waitFor((m) => m.t === 'status' && m.status.state === 'error', 3000);
  check(!!lost, `coupure détectée (${lost?.status.message})`);
  const back = await c.waitFor((m) => m.t === 'status' && m.status.state === 'connected', 6000);
  check(!!back, 'reconnexion automatique');
  w = await c.write('MAIN.stPlc.rTemp', 12.5);
  check(w?.ok && plc.mem.readFloatLE(4) === 12.5, 'écriture après reconnexion');

  // configuration modifiée : symbole inconnu
  c.clear();
  let r = await fetch(s4.base + '/api/config', { method: 'PUT', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ source: 'ads', ads: { symbols: ['MAIN.inconnu'], routerAddress: '127.0.0.1', routerTcpPort: plc.port, targetAmsNetId: '10.0.0.2.1.1', localAmsNetId: '10.0.0.9.1.1' } }) });
  const bad = await c.waitFor((m) => m.t === 'status' && m.status.state === 'error' && m.status.message.includes('MAIN.inconnu'), 5000);
  check(r.status === 200 && bad?.status.message.includes('symbole introuvable'), 'symbole inconnu : erreur explicite');

  // AMS Net ID invalide
  c.clear();
  r = await fetch(s4.base + '/api/config', { method: 'PUT', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ source: 'ads', ads: { targetAmsNetId: '10.0.0.2', symbols: ['MAIN.stPlc'] } }) });
  const badid = await c.waitFor((m) => m.t === 'status' && m.status.state === 'error' && m.status.message.includes('AMS Net ID'), 3000);
  check(!!badid, 'AMS Net ID invalide : erreur explicite');

  // mode routeur : port AMS attribué par le routeur
  c.clear();
  const before = plc.registered;
  r = await fetch(s4.base + '/api/config', { method: 'PUT', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ source: 'ads', ads: { mode: 'router', targetAmsNetId: '10.0.0.2.1.1', routerAddress: '127.0.0.1', routerTcpPort: plc.port, symbols: ['MAIN.stPlc'] } }) });
  const rt = await c.waitFor((m) => m.t === 'status' && m.status.state === 'connected', 5000);
  check(!!rt && plc.registered === before + 1, 'mode routeur : port AMS enregistré');
  c.ws.close();
} finally {
  const code = await stop(s4);
  check(WIN ? code !== 'timeout' : code === 0, 'arrêt propre');
  if (/runtime error|AddressSanitizer|LeakSanitizer/.test(s4.log())) { failures++; console.log(s4.log()); }
  await plc.close();
}

/* ------------------------------------------------------------------------------------------ */

const sanitizerClean = (srv) => { if (/runtime error|AddressSanitizer|LeakSanitizer/.test(srv.log())) { failures++; console.log(srv.log()); } };
const putDash = (base, headers = {}) => fetch(base + '/api/dashboards/essai', { method: 'PUT',
  headers: { 'content-type': 'application/json', ...headers }, body: JSON.stringify({ name: 'Essai', widgets: [] }) });

console.log('Bouton « Quitter » et code de retour');
const s5 = await start(null, ['--quit']);
try {
  let r = await fetch(s5.base + '/api/session');
  const ses = await r.json();
  check(ses.access === 'full' && ses.local === true && ses.quit === true && ses.kiosk === false, 'GET /api/session : poste local, quitter autorisé');
  r = await fetch(s5.base + '/');
  check((await r.text()).includes('/kiosk.js'), 'index.html charge kiosk.js');
  r = await fetch(s5.base + '/kiosk.js');
  check(r.status === 200 && (await r.text()).includes('/api/quit'), 'GET /kiosk.js');
  const resp = await raw(s5.port, `POST /api/quit HTTP/1.1\r\nHost: 127.0.0.1:${s5.port}\r\nOrigin: http://evil.example\r\nContent-Length: 10\r\n\r\n{"code":1}`);
  check(resp.startsWith('HTTP/1.1 403'), 'POST /api/quit depuis une autre origine refusé');
  r = await fetch(s5.base + '/api/quit', { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{"code":300}' });
  check(r.status === 400, 'code de retour hors 0-255 refusé');
  r = await fetch(s5.base + '/api/quit', { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{"code":3}' });
  check(r.status === 200, 'POST /api/quit');
  const code = await Promise.race([s5.exited, sleep(3000).then(() => 'timeout')]);
  check(code === 3, `service arrêté avec le code de retour demandé (${code})`);
} finally {
  await stop(s5);
  sanitizerClean(s5);
}

console.log('Lecture seule (--readonly)');
const s6 = await start(null, ['--readonly']);
try {
  let r = await fetch(s6.base + '/api/session');
  const ses = await r.json();
  check(ses.access === 'read' && ses.readonly === true && ses.quit === false, 'session en lecture seule');
  r = await putDash(s6.base);
  check(r.status === 403 && (await r.json()).error.includes('lecture seule'), 'modification d\'écran refusée');
  r = await fetch(s6.base + '/api/config', { method: 'PUT', headers: { 'content-type': 'application/json' }, body: '{"source":"sim"}' });
  check(r.status === 403, 'modification de la configuration refusée');
  r = await fetch(s6.base + '/api/quit', { method: 'POST', body: '{}' });
  check(r.status === 403, 'arrêt refusé');
  r = await fetch(s6.base + '/api/calc/test', { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{"expr":"1+1"}' });
  check(r.status === 200 && (await r.json()).value === 2, 'évaluation d\'expression autorisée (sans effet)');
  const c = wsClient(s6.base);
  await c.waitFor((m) => m.t === 'schema' && m.schema.leaves.length > 0);
  const w = await c.write('SIM.stHmi.stCmd.bMarche', true);
  check(w && !w.ok && w.error.includes('lecture seule'), 'écriture WebSocket refusée');
  check(!!(await c.waitFor((m) => m.t === 'd', 2000)), 'valeurs toujours reçues');
  c.ws.close();
} finally {
  const code = await stop(s6);
  check(WIN ? code !== 'timeout' : code === 0, 'arrêt propre');
  sanitizerClean(s6);
}

console.log('Jeton d\'accès (poste distant)');
const lanIp = Object.values(os.networkInterfaces()).flat().find((a) => a && a.family === 'IPv4' && !a.internal)?.address;
if (!lanIp) {
  console.log('  (ignoré : aucune adresse réseau non locale sur cette machine)');
} else {
  const TOKEN = 'jeton-de-test-1234';
  const s7 = await start(null, ['--host', '0.0.0.0', '--token', TOKEN]);
  const remote = `http://${lanIp}:${s7.port}`;
  try {
    let r = await fetch(remote + '/api/session');
    let ses = await r.json();
    check(ses.access === 'read' && ses.local === false, `poste distant (${lanIp}) sans jeton : lecture seule`);
    r = await fetch(s7.base + '/api/session');
    check((await r.json()).access === 'full', 'poste local : accès complet sans jeton');
    r = await putDash(remote);
    check(r.status === 403 && (await r.json()).error.includes('jeton'), 'poste distant sans jeton : modification refusée');
    r = await putDash(remote, { authorization: 'Bearer mauvais-jeton-0000' });
    check(r.status === 403, 'mauvais jeton refusé');
    r = await putDash(remote, { authorization: `Bearer ${TOKEN}` });
    check(r.status === 200, 'jeton dans l\'en-tête Authorization accepté');
    r = await fetch(remote + `/?token=${TOKEN}`);
    const ck = r.headers.get('set-cookie') ?? '';
    check(ck.includes(`hmi_token=${TOKEN}`) && ck.includes('HttpOnly') && ck.includes('SameSite=Strict'), 'jeton dans l\'adresse : mémorisé en cookie');
    r = await fetch(remote + '/api/session', { headers: { cookie: `hmi_token=${TOKEN}` } });
    check((await r.json()).access === 'full', 'cookie accepté');
    const ro = wsClient(remote);
    await ro.waitFor((m) => m.t === 'schema' && m.schema.leaves.length > 0);
    let w = await ro.write('SIM.stHmi.stCmd.bMarche', true);
    check(w && !w.ok, 'WebSocket distant sans jeton : écriture refusée');
    ro.ws.close();
    const full = (() => { const ws = new WebSocket(`ws://${lanIp}:${s7.port}/ws?token=${TOKEN}`); const m = []; ws.onmessage = (e) => m.push(JSON.parse(e.data)); return { ws, m }; })();
    await new Promise((res) => { full.ws.onopen = res; setTimeout(res, 2000); });
    full.ws.send(JSON.stringify({ t: 'w', id: 9, path: 'SIM.stHmi.stCmd.bMarche', value: true }));
    for (let i = 0; i < 40 && !full.m.some((x) => x.t === 'wr'); i++) await sleep(50);
    w = full.m.find((x) => x.t === 'wr');
    check(w?.ok === true, 'WebSocket distant avec jeton : écriture acceptée');
    full.ws.close();
  } finally {
    const code = await stop(s7);
    check(WIN ? code !== 'timeout' : code === 0, 'arrêt propre');
    sanitizerClean(s7);
  }
}

console.log('Kiosque');
{
  // navigateur absent ou fermé : le service s'arrête de lui-même (Node rejette les options du navigateur)
  const s8 = await start(null, ['--kiosk', '--browser', process.execPath]);
  const code = await Promise.race([s8.exited, sleep(5000).then(() => 'timeout')]);
  check(code === 0 && s8.log().includes('navigateur fermé'), `navigateur fermé : service arrêté (code ${code})`);
  await stop(s8);
}
if (!WIN) {
  // faux navigateur : note ses arguments et son PID, puis attend (comme un navigateur ouvert)
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'hmi-kiosk-'));
  const fake = path.join(tmp, 'navigateur.sh');
  fs.writeFileSync(fake, `#!/bin/sh\necho $$ > "${tmp}/pid"\nprintf '%s\\n' "$@" > "${tmp}/args"\nexec sleep 60\n`, { mode: 0o755 });
  const s9 = await start(null, ['--kiosk', '--screen', 'demo', '--browser', fake]);
  try {
    for (let i = 0; i < 60 && !fs.existsSync(path.join(tmp, 'args')); i++) await sleep(50);
    const args = fs.existsSync(path.join(tmp, 'args')) ? fs.readFileSync(path.join(tmp, 'args'), 'utf8').split('\n') : [];
    check(args.includes('--kiosk') && args.includes(`http://127.0.0.1:${s9.port}/#/run/demo`), 'navigateur lancé en kiosque sur l\'écran choisi');
    const prof = args.find((a) => a.startsWith('--user-data-dir='))?.slice(16) ?? '';
    check(path.isAbsolute(prof) && prof.endsWith('kiosque-navigateur') && fs.existsSync(prof), 'profil de navigateur dédié (chemin absolu)');
    const ses = await (await fetch(s9.base + '/api/session')).json();
    check(ses.kiosk === true && ses.quit === true, 'session : kiosque, bouton Quitter actif');
    const pid = Number(fs.readFileSync(path.join(tmp, 'pid'), 'utf8'));
    await fetch(s9.base + '/api/quit', { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{"code":0}' });
    const code = await Promise.race([s9.exited, sleep(5000).then(() => 'timeout')]);
    let alive = true;
    for (let i = 0; i < 40 && alive; i++) { try { process.kill(pid, 0); await sleep(50); } catch { alive = false; } }
    check(code === 0 && !alive, `Quitter : service arrêté (code ${code}) et navigateur fermé`);
  } finally {
    await stop(s9);
    sanitizerClean(s9);
    fs.rmSync(tmp, { recursive: true, force: true });
  }
}

console.log(`\n${checks} vérifications, ${failures} échec(s)`);
process.exit(failures ? 1 : 0);
