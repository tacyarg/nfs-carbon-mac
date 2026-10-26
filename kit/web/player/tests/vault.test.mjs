// Cloud saves in the page (kit/web/player/vault.js, zip.js): node --test kit/web/player/tests/vault.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import { zip, unzip } from '../zip.js';
import { seal, open, keeps, profilesIn, contentHash, CloudSaves, VaultError } from '../vault.js';

const enc = new TextEncoder();

test('zip: a round trip of nested paths', async () => {
  const entries = [
    { name: 'settings.ini', data: enc.encode('x'.repeat(500)), mtime: 1000 },
    { name: 'Documents/NFS Carbon/RACER/RACER', data: crypto.randomBytes(2000), mtime: 2000 },
    { name: 'Documents/NFS Carbon/RACER/RACER.bak', data: enc.encode('backup'), mtime: 3000 },
  ];
  const z = zip(entries);
  const back = await unzip(z);
  assert.deepEqual(back.map(e => e.name).sort(), entries.map(e => e.name).sort());
  for (const e of entries) assert.deepEqual(back.find(b => b.name === e.name).data, new Uint8Array(e.data.buffer, e.data.byteOffset, e.data.length));
});

test('seal/open: a round trip; the box format; a wrong key fails to open', async () => {
  const saveKey = crypto.randomBytes(32), other = crypto.randomBytes(32);
  const files = [{ path: 'Documents/NFS Carbon/RACER/RACER', data: crypto.randomBytes(1000) }];
  const box = await seal(saveKey, files);
  assert.equal(box[0], 1);
  assert.ok(box.length > 13 + 16);
  const back = await open(saveKey, box);
  assert.deepEqual(back.map(f => f.path), ['Documents/NFS Carbon/RACER/RACER']);
  assert.deepEqual(back[0].data, new Uint8Array(files[0].data.buffer, files[0].data.byteOffset, files[0].data.length));
  await assert.rejects(open(other, box), /do not open with this key/);
  const bent = box.slice(); bent[20] ^= 1;
  await assert.rejects(open(saveKey, bent), /do not open/);
  const v2 = box.slice(); v2[0] = 2;
  await assert.rejects(open(saveKey, v2), /not a key's saves/);
  // two seals of the same saves differ (a new IV each time)
  assert.notDeepEqual(await seal(saveKey, files), box);
});

test('keeps: screenshots and logs are left out, saves and settings are not', () => {
  assert.ok(keeps('Documents/NFS Carbon/RACER/RACER'));
  assert.ok(keeps('mod-settings.json'));
  assert.ok(!keeps('nfsc0001.bmp'));
  assert.ok(!keeps('shot.png'));
  assert.ok(!keeps('game.log'));
});

test('profilesIn: Carbon profiles only ("Documents/NFS Carbon/<NAME>/<NAME>")', () => {
  const files = [
    { path: 'Documents/NFS Carbon/RACER/RACER', data: new Uint8Array(0) },
    { path: 'Documents/NFS Carbon/RACER/options.bin', data: new Uint8Array(0) },
    { path: 'Documents/NFS Carbon/OTHER/OTHER', data: new Uint8Array(0) },
    { path: 'mod-settings.json', data: new Uint8Array(0) },
  ];
  assert.deepEqual(profilesIn(files), ['RACER', 'OTHER']);
});

test('contentHash: ignores times and order, not content', async () => {
  const files = [{ path: 'a', data: enc.encode('1'), mtime: 1 }, { path: 'b', data: enc.encode('2'), mtime: 2 }];
  const h = await contentHash(files);
  assert.equal(await contentHash(files.map(f => ({ ...f, mtime: 99 }))), h);
  assert.equal(await contentHash([...files].reverse()), h);
  assert.notEqual(await contentHash(files.map((f, i) => i === 0 ? { ...f, data: enc.encode('x') } : f)), h);
});

// ---- CloudSaves: an in-memory OPFS-shaped directory and a stubbed server ----

/** A directory handle shaped like FileSystemDirectoryHandle, backed by a plain object tree.
 *  A file is a { __file: Uint8Array } wrapper kept by reference, so closing a writable updates it in place. */
function wrap(node) {
  if (node instanceof Uint8Array) return { __file: node };
  const out = {};
  for (const [k, v] of Object.entries(node)) out[k] = wrap(v);
  return out;
}
function memDir(tree = {}) {
  function handle(node) {
    if (node.__file !== undefined) {
      return {
        kind: 'file',
        getFile: async () => ({ arrayBuffer: async () => node.__file.buffer.slice(node.__file.byteOffset, node.__file.byteOffset + node.__file.byteLength), text: async () => Buffer.from(node.__file).toString() }),
        createWritable: async () => ({ _buf: null, write(d) { this._buf = d instanceof Uint8Array ? d : enc.encode(d); }, async close() { node.__file = this._buf ?? new Uint8Array(0); } }),
      };
    }
    return {
      kind: 'directory',
      async *entries() { for (const [name, child] of Object.entries(node)) yield [name, handle(child)]; },
      async getDirectoryHandle(name, { create } = {}) {
        if (!(name in node)) { if (!create) throw new Error('not found'); node[name] = {}; }
        return handle(node[name]);
      },
      async getFileHandle(name, { create } = {}) {
        if (!(name in node)) { if (!create) throw new Error('not found'); node[name] = { __file: new Uint8Array(0) }; }
        return handle(node[name]);
      },
      async removeEntry(name) { delete node[name]; },
    };
  }
  return handle(wrap(tree));
}

function fakeServer() {
  const kept = new Map();
  let rev = 1;
  const tag = n => `"${n}"`;
  const s = { now: 1000, kept, status: null };
  s.fetch = async (url, init = {}) => {
    const h = Object.fromEntries(Object.entries(init.headers || {}).map(([k, v]) => [k.toLowerCase(), v]));
    if (s.status) return new Response('no', { status: s.status });
    const token = (h.authorization || '').replace(/^Bearer /, '');
    const method = init.method || 'GET';
    const k = kept.get(token);
    if (method === 'GET') {
      if (!k) return new Response('Not found', { status: 404 });
      if (h['if-none-match'] === tag(k.rev)) return new Response(null, { status: 304, headers: { etag: tag(k.rev), 'x-vault-saved': String(k.saved) } });
      return new Response(k.body, { status: 200, headers: { etag: tag(k.rev), 'x-vault-saved': String(k.saved) } });
    }
    if (method === 'PUT') {
      if (h['if-none-match'] === '*' ? k : !k || h['if-match'] !== tag(k.rev))
        return new Response('Changed elsewhere', { status: 412, headers: k ? { etag: tag(k.rev), 'x-vault-saved': String(k.saved) } : {} });
      const body = new Uint8Array(init.body);
      const v = { rev: rev++, saved: s.now, body };
      kept.set(token, v);
      return new Response('Saved', { status: k ? 200 : 201, headers: { etag: tag(v.rev), 'x-vault-saved': String(v.saved) } });
    }
    return new Response('no', { status: 405 });
  };
  return s;
}

function memStorage() {
  const m = new Map();
  return { getItem: k => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, v), removeItem: k => m.delete(k) };
}

function cloud(srv, { saveKey, token, tree, storage } = {}) {
  return new CloudSaves({
    token: token ?? crypto.randomBytes(32), saveKey: saveKey ?? crypto.randomBytes(32),
    dir: memDir(tree ?? {}), fetch: srv.fetch, storage: storage ?? memStorage(),
  });
}

test('two tabs on one key: a 412 on save ends in conflict, and does not resend by itself', async () => {
  const srv = fakeServer();
  const a = cloud(srv, { tree: { Documents: { 'NFS Carbon': { RACER: { RACER: new Uint8Array([7, 7, 7]) } } } } });
  let putCalls = 0;
  // the other tab already took this rev, so our PUT finds a newer one there: a stubbed 412 with another ETag
  a.doFetch = async (url, init) => {
    if ((init?.method || 'GET') === 'PUT') { putCalls++; return new Response('Changed elsewhere', { status: 412, headers: { etag: '"other"', 'x-vault-saved': '2000' } }); }
    return new Response('Not found', { status: 404 });
  };
  await a.sendIfChanged();
  assert.equal(a.state.s, 'conflict');
  assert.equal(a.state.why, 'both');
  assert.equal(putCalls, 1);
  // the conflict stands until the player picks: sendIfChanged again must not PUT a second time
  await a.sendIfChanged();
  assert.equal(putCalls, 1);
  assert.equal(a.state.s, 'conflict');
});

test('never send an empty folder over a cloud copy with profiles', async () => {
  const saveKey = crypto.randomBytes(32), token = crypto.randomBytes(32);
  const srv = fakeServer();
  const a = cloud(srv, { saveKey, token, tree: { 'Documents': { 'NFS Carbon': { RACER: { RACER: new Uint8Array([1, 2, 3]) } } } } });
  await a.sendIfChanged();
  assert.equal(a.state.s, 'synced');
  assert.equal(a.link.profiles, 1);
  // storage cleared, the key kept: an empty folder must not overwrite the cloud's profile
  const b = cloud(srv, { saveKey, token, tree: {}, storage: (() => { const s = memStorage(); s.setItem('nfsc-cloud', JSON.stringify(a.link)); return s; })() });
  await b.sendIfChanged();
  assert.equal(b.state.s, 'conflict');
  assert.equal(b.state.why, 'empty');
  // the cloud's copy is unchanged
  const r = await srv.fetch('/vault', { headers: { authorization: `Bearer ${Buffer.from(token).toString('base64url')}` } });
  assert.equal(r.status, 200);
});
