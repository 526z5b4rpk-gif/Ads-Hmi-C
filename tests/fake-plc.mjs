// Faux automate TwinCAT pour les tests : serveur AMS/TCP minimal (même format binaire qu'un vrai
// routeur ADS, d'après ads-client). Programme simulé : MAIN.stPlc de type ST_Plc.
//
//   import { startFakePlc } from './fake-plc.mjs';
//   const plc = await startFakePlc();      // plc.port, plc.mem, plc.read(path), plc.bumpVersion()…
//
// Lancement autonome (essai manuel du service) : node tests/fake-plc.mjs [port]
import net from 'node:net';

const ADST = { INT16: 2, INT32: 3, REAL32: 4, REAL64: 5, UINT8: 17, STRING: 30, BIT: 33, BIGTYPE: 65 };
const F = { DataType: 1, DataItem: 2, BitValues: 0x20, Attributes: 0x1000, EnumInfos: 0x2000, PlcPointer: 0x800000 };
const SYMBOL = 'MAIN.stPlc';
const IG_MEM = 0x4040, IO_SYM = 0x1000, SIZE = 80;

/** entrée de type au format ADS (longueur incluse) */
function dt({ name, type = '', comment = '', size, offset = 0, adst = ADST.BIGTYPE, flags = F.DataType, dims = [], subs = [], attrs = [], enums = [] }) {
  if (attrs.length) flags |= F.Attributes;
  if (enums.length) flags |= F.EnumInfos;
  const s = (t) => Buffer.from(t, 'utf8');
  const parts = [];
  const head = Buffer.alloc(38);
  head.writeUInt32LE(1, 0);
  head.writeUInt32LE(size, 12);
  head.writeUInt32LE(offset, 16);
  head.writeUInt32LE(adst, 20);
  head.writeUInt32LE(flags >>> 0, 24);
  head.writeUInt16LE(s(name).length, 28);
  head.writeUInt16LE(s(type).length, 30);
  head.writeUInt16LE(s(comment).length, 32);
  head.writeUInt16LE(dims.length, 34);
  head.writeUInt16LE(subs.length, 36);
  parts.push(head, s(name), Buffer.alloc(1), s(type), Buffer.alloc(1), s(comment), Buffer.alloc(1));
  for (const d of dims) { const b = Buffer.alloc(8); b.writeInt32LE(d[0], 0); b.writeUInt32LE(d[1], 4); parts.push(b); }
  for (const sub of subs) parts.push(dt({ flags: F.DataItem, ...sub }));
  if (attrs.length) {
    const c = Buffer.alloc(2); c.writeUInt16LE(attrs.length); parts.push(c);
    for (const [k, v] of attrs) parts.push(Buffer.from([s(k).length, s(v).length]), s(k), Buffer.alloc(1), s(v), Buffer.alloc(1));
  }
  if (enums.length) {
    const c = Buffer.alloc(2); c.writeUInt16LE(enums.length); parts.push(c);
    for (const [k, v] of enums) { const b = Buffer.alloc(size); b.writeIntLE(v, 0, size); parts.push(Buffer.from([s(k).length]), s(k), Buffer.alloc(1), b); }
  }
  const body = Buffer.concat(parts);
  const len = Buffer.alloc(4);
  len.writeUInt32LE(body.length + 4);
  return Buffer.concat([len, body]);
}

/*
 * ST_Plc (80 octets, alignement TwinCAT) :
 *   0 bRun BOOL | 4 rTemp REAL {unit °C} | 8 nCount DINT | 12 eMode E_Mode (INT) | 14 sMsg STRING(20)
 *   40 aVal ARRAY [1..3] OF LREAL | 64 stSub ST_Sub (iX INT, bBit0 BIT, bBit1 BIT) | 68 rPress T_Pression
 *   72 pPtr POINTER TO INT (ignoré par le service)
 * DINT est volontairement absent de la table (repli sur les types connus du service).
 */
const TYPE_LIST = [
  dt({ name: 'ST_Plc', size: SIZE, comment: 'Données IHM', subs: [
    { name: 'bRun', type: 'BOOL', size: 1, offset: 0, adst: ADST.UINT8, comment: 'Machine en marche' },
    { name: 'rTemp', type: 'REAL', size: 4, offset: 4, adst: ADST.REAL32, comment: 'Température four', attrs: [['unit', '°C']] },
    { name: 'nCount', type: 'DINT', size: 4, offset: 8, adst: ADST.INT32, comment: 'Pièces' },
    { name: 'eMode', type: 'E_Mode', size: 2, offset: 12, adst: ADST.INT16, comment: 'Mode' },
    { name: 'sMsg', type: 'STRING(20)', size: 21, offset: 14, adst: ADST.STRING, comment: 'Message' },
    { name: 'aVal', type: 'ARRAY [1..3] OF LREAL', size: 24, offset: 40, adst: ADST.REAL64 },
    { name: 'stSub', type: 'ST_Sub', size: 4, offset: 64 },
    { name: 'rPress', type: 'T_Pression', size: 4, offset: 68, adst: ADST.REAL32, comment: 'Pression' },
    { name: 'pPtr', type: 'POINTER TO INT', size: 8, offset: 72, adst: 21 },
  ] }),
  dt({ name: 'ST_Sub', size: 4, subs: [
    { name: 'iX', type: 'INT', size: 2, offset: 0, adst: ADST.INT16 },
    { name: 'bBit0', type: 'BIT', size: 1, offset: 16, adst: ADST.BIT, flags: F.DataItem | F.BitValues },
    // drapeau BitValues seulement sur le type BIT (cas TwinCAT) : le service doit le déduire du type
    { name: 'bBit1', type: 'BIT', size: 1, offset: 17, adst: ADST.BIT, comment: 'Bit 1' },
  ] }),
  dt({ name: 'E_Mode', type: 'INT', size: 2, adst: ADST.INT16, enums: [['Manu', 0], ['Auto', 1], ['Defaut', -1]] }),
  dt({ name: 'ARRAY [1..3] OF LREAL', type: 'LREAL', size: 24, adst: ADST.REAL64, dims: [[1, 3]] }),
  dt({ name: 'T_Pression', type: 'REAL', size: 4, adst: ADST.REAL32, attrs: [['unit', 'bar']] }),
  dt({ name: 'POINTER TO INT', type: 'INT', size: 8, adst: 21, flags: F.DataType | F.PlcPointer }),
  dt({ name: 'STRING(20)', size: 21, adst: ADST.STRING }),
  dt({ name: 'BOOL', size: 1, adst: ADST.UINT8 }),
  dt({ name: 'BIT', size: 1, adst: ADST.BIT, flags: F.DataType | F.BitValues }),
  dt({ name: 'INT', size: 2, adst: ADST.INT16 }),
  dt({ name: 'REAL', size: 4, adst: ADST.REAL32 }),
  dt({ name: 'LREAL', size: 8, adst: ADST.REAL64 }),
];
const TYPES = Buffer.concat(TYPE_LIST);
/** nom (minuscules) -> entrée, pour la lecture d'un type par son nom (0xF011, utilisée par ads-client) */
const TYPE_BY_NAME = new Map(TYPE_LIST.map((b) => [b.subarray(42, 42 + b.readUInt16LE(32)).toString('utf8').toLowerCase(), b]));

/** variables adressables par handle (BIT notamment) : chemin -> [offset, taille, bit] */
const HANDLES = {
  'main.stplc.stsub.bbit0': [64 + 2, 1, 0],
  'main.stplc.stsub.bbit1': [64 + 2, 1, 1],
  'main.stplc.rtemp': [4, 4, -1],
};

function symbolEntry() {
  const name = Buffer.from(SYMBOL), type = Buffer.from('ST_Plc'), comment = Buffer.from('Symbole IHM');
  const h = Buffer.alloc(30);
  h.writeUInt32LE(IG_MEM, 4);
  h.writeUInt32LE(IO_SYM, 8);
  h.writeUInt32LE(SIZE, 12);
  h.writeUInt32LE(ADST.BIGTYPE, 16);
  h.writeUInt16LE(0, 20);
  h.writeUInt16LE(0, 22);
  h.writeUInt16LE(name.length, 24);
  h.writeUInt16LE(type.length, 26);
  h.writeUInt16LE(comment.length, 28);
  const b = Buffer.concat([h, name, Buffer.alloc(1), type, Buffer.alloc(1), comment, Buffer.alloc(1)]);
  b.writeUInt32LE(b.length, 0);
  return b;
}

export async function startFakePlc({ port = 0, cycleMs = 50 } = {}) {
  const mem = Buffer.alloc(SIZE);
  mem.writeFloatLE(20, 4);
  mem.writeInt16LE(1, 12);
  mem.write('Bonjour', 14, 'latin1');
  [1.5, 2.5, 3.5].forEach((v, i) => mem.writeDoubleLE(v, 40 + i * 8));
  mem.writeInt16LE(7, 64);
  mem[66] = 0b01; // bBit0 = TRUE, bBit1 = FALSE
  mem.writeFloatLE(1.25, 68);

  const plc = {
    port: 0, mem, state: 5, symVersion: 1, clients: new Set(), writes: [], registered: 0, notifCount: 0,
    read: (off, n) => mem.subarray(off, off + n),
    bumpVersion() { plc.symVersion = (plc.symVersion + 1) & 0xff; },
    dropConnections() { for (const c of plc.clients) c.sock.destroy(); },
    close: () => new Promise((r) => { clearInterval(timer); plc.dropConnections(); server.close(() => r()); }),
  };

  let nextHandle = 1;
  const server = net.createServer((sock) => {
    const c = { sock, notifs: new Map(), handles: new Map(), buf: Buffer.alloc(0) };
    plc.clients.add(c);
    sock.on('close', () => plc.clients.delete(c));
    sock.on('error', () => {});
    sock.on('data', (d) => {
      c.buf = Buffer.concat([c.buf, d]);
      while (c.buf.length >= 6) {
        const len = c.buf.readUInt32LE(2);
        if (c.buf.length < 6 + len) break;
        const frame = c.buf.subarray(0, 6 + len);
        c.buf = c.buf.subarray(6 + len);
        onFrame(c, frame);
      }
    });
  });

  function send(c, hdr, cmd, flags, invoke, data) {
    const f = Buffer.alloc(38 + data.length);
    f.writeUInt32LE(32 + data.length, 2);
    hdr.src.copy(f, 6); f.writeUInt16LE(hdr.srcPort, 12);   // destinataire = émetteur de la requête
    hdr.dst.copy(f, 14); f.writeUInt16LE(hdr.dstPort, 20);
    f.writeUInt16LE(cmd, 22); f.writeUInt16LE(flags, 24); f.writeUInt32LE(data.length, 26);
    f.writeUInt32LE(invoke, 34);
    data.copy(f, 38);
    c.sock.write(f);
  }
  const u32 = (...v) => { const b = Buffer.alloc(4 * v.length); v.forEach((x, i) => b.writeUInt32LE(x >>> 0, 4 * i)); return b; };

  function sample(n) {
    if (n.ig === 0xf008) return Buffer.from([plc.symVersion]);
    return Buffer.from(mem.subarray(n.io - IO_SYM, n.io - IO_SYM + n.len));
  }
  function notify(c, handle, data) {
    const body = Buffer.concat([u32(0, 1), Buffer.alloc(8), u32(1, handle, data.length), data]);
    body.writeUInt32LE(body.length - 4, 0);
    plc.notifCount++;
    send(c, c.hdr, 8, 4, 0, body);
  }

  function onFrame(c, f) {
    const tcp = f.readUInt16LE(0);
    if (tcp === 0x1000) { // enregistrement d'un port (mode routeur)
      plc.registered++;
      const r = Buffer.alloc(14);
      r.writeUInt16LE(0x1000, 0); r.writeUInt32LE(8, 2);
      Buffer.from([10, 0, 0, 2, 1, 1]).copy(r, 6); r.writeUInt16LE(32905, 12);
      c.sock.write(r);
      return;
    }
    if (tcp !== 0) return;
    const hdr = { dst: f.subarray(6, 12), dstPort: f.readUInt16LE(12), src: f.subarray(14, 20), srcPort: f.readUInt16LE(20) };
    c.hdr = hdr;
    const cmd = f.readUInt16LE(22), invoke = f.readUInt32LE(34), d = f.subarray(38);
    const reply = (data) => send(c, hdr, cmd, 5, invoke, data);
    // erreur : code résultat, suivi d'une longueur nulle pour les lectures (comme TwinCAT)
    const err = (code) => reply(cmd === 2 || cmd === 9 ? u32(code, 0) : u32(code));
    const ok = (data = Buffer.alloc(0), withLen = true) => reply(Buffer.concat([u32(0), withLen ? u32(data.length) : Buffer.alloc(0), data]));
    if (hdr.dstPort === 10000 && (cmd === 1 || cmd === 4)) { const b = Buffer.alloc(4); b.writeUInt16LE(5, 0); return ok(cmd === 4 ? b : Buffer.concat([Buffer.from([3, 1, 0xa8, 0x0f]), Buffer.from('TwinCAT System'.padEnd(16, '\0'))]), false); }
    if (hdr.dstPort !== 851) return err(0x6);
    switch (cmd) {
      case 1: return ok(Buffer.concat([Buffer.from([3, 1]), Buffer.from([0xa8, 0x0f]), Buffer.from('Fake PLC'.padEnd(16, '\0'))]), false);
      case 4: { const b = Buffer.alloc(4); b.writeUInt16LE(plc.state, 0); return ok(b, false); }
      case 2: { // lecture
        const ig = d.readUInt32LE(0), io = d.readUInt32LE(4), n = d.readUInt32LE(8);
        if (ig === 0xf00c) {
          const b = Buffer.alloc(64);
          b.writeUInt32LE(1, 0); b.writeUInt32LE(symbolEntry().length, 4); b.writeUInt32LE(11, 8);
          b.writeUInt32LE(TYPES.length, 12); b.writeUInt32LE(65001, 28);
          return ok(b.subarray(0, Math.min(64, n)));
        }
        if (ig === 0xf00e) return ok(TYPES.subarray(0, n));
        if (ig === IG_MEM && io >= IO_SYM && io - IO_SYM + n <= SIZE) return ok(Buffer.from(mem.subarray(io - IO_SYM, io - IO_SYM + n)));
        return err(0x702);
      }
      case 9: { // lecture/écriture
        const ig = d.readUInt32LE(0), wlen = d.readUInt32LE(12), w = d.subarray(16, 16 + wlen);
        const name = w.toString('latin1').replace(/\0.*$/, '').toLowerCase();
        if (ig === 0xf009) return name === SYMBOL.toLowerCase() ? ok(symbolEntry()) : err(0x710);
        if (ig === 0xf011) return TYPE_BY_NAME.has(name) ? ok(TYPE_BY_NAME.get(name)) : err(0x710);
        if (ig === 0xf003) {
          if (!HANDLES[name]) return err(0x710);
          const h = nextHandle++;
          c.handles.set(h, HANDLES[name]);
          return ok(u32(h));
        }
        return err(0x701);
      }
      case 3: { // écriture
        const ig = d.readUInt32LE(0), io = d.readUInt32LE(4), n = d.readUInt32LE(8), data = d.subarray(12, 12 + n);
        if (ig === IG_MEM && io >= IO_SYM && io - IO_SYM + n <= SIZE) {
          data.copy(mem, io - IO_SYM);
          plc.writes.push({ offset: io - IO_SYM, data: Buffer.from(data) });
          return reply(u32(0));
        }
        if (ig === 0xf005) {
          const h = c.handles.get(io);
          if (!h) return err(0x710);
          const [off, size, bit] = h;
          if (bit >= 0) mem[off] = data[0] ? mem[off] | (1 << bit) : mem[off] & ~(1 << bit);
          else data.copy(mem, off, 0, size);
          plc.writes.push({ offset: off, bit, data: Buffer.from(data) });
          return reply(u32(0));
        }
        if (ig === 0xf006) { c.handles.delete(d.readUInt32LE(12)); return reply(u32(0)); }
        return err(0x702);
      }
      case 6: { // abonnement
        const n = { ig: d.readUInt32LE(0), io: d.readUInt32LE(4), len: d.readUInt32LE(8), mode: d.readUInt32LE(12) };
        if (n.ig !== 0xf008 && !(n.ig === IG_MEM && n.io >= IO_SYM)) return err(0x702);
        const h = nextHandle++;
        n.last = sample(n);
        c.notifs.set(h, n);
        reply(Buffer.concat([u32(0), u32(h)]));
        notify(c, h, n.last); // valeur initiale, comme TwinCAT
        return;
      }
      case 7: c.notifs.delete(d.readUInt32LE(0)); return reply(u32(0));
      default: return err(0x701);
    }
  }

  // cycle automate : procédé + notifications sur changement
  const timer = setInterval(() => {
    if (plc.state === 5 && mem[0]) {
      mem.writeFloatLE(mem.readFloatLE(4) + 0.5, 4);
      mem.writeInt32LE(mem.readInt32LE(8) + 1, 8);
    }
    for (const c of plc.clients)
      for (const [h, n] of c.notifs) {
        const cur = sample(n);
        if (!cur.equals(n.last)) { n.last = cur; notify(c, h, cur); }
      }
  }, cycleMs);

  await new Promise((r) => server.listen(port, '127.0.0.1', r));
  plc.port = server.address().port;
  return plc;
}

if (import.meta.url === `file://${process.argv[1]}` || process.argv[1]?.endsWith('fake-plc.mjs')) {
  const plc = await startFakePlc({ port: Number(process.argv[2] ?? 48898) });
  console.log(`Faux automate ADS sur 127.0.0.1:${plc.port} (AMS Net ID quelconque, port ADS 851, symbole ${SYMBOL})`);
}
