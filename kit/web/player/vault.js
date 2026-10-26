// Cloud saves on nfscarbon.xyz: this browser's save folder kept on the server too, sealed under the player's key
// (planning/specs/2026-10-26-quick-online-and-cloud-saves-design.md, section 4). The server stores what it is sent
// under SHA-256(token) and gives it back to whoever shows the token; it never sees the key or the saves.
//
// Box format: a version byte (1), a 12-byte IV, the AES-256-GCM ciphertext and its tag, AAD "nfscarbon.xyz key v1".
// The folder packed is a stored zip.ts, deflated. Carbon profiles are the paths matching
// "Documents/NFS Carbon/<NAME>/<NAME>"; screenshots and logs (*.bmp, *.png, *.log) are left out.
//
// Ported from nfsu2-web's web/src/app/vault.ts (MIT; Copyright (c) 2026 th0mk; portions (c) 2026 Talha Müderrisoğlu).

import { zip, unzip } from './zip.js';

/** what the server takes (crates/vault VaultOptions::sized) */
export const MAX_BOX = 256 << 10;
/** the most a box's zip inflates to */
const MAX_PLAIN = 256 << 20;
const AAD = 'nfscarbon.xyz key v1';
export const LINK_KEY = 'nfsc-cloud';
const enc = new TextEncoder();

export class VaultError extends Error {
  constructor(message, code = 'VAULT') { super(message); this.code = code; }
}

/** A file of the save folder: { path, data: Uint8Array, mtime? }. */

/** Carbon profiles only, not screenshots or logs: a browser with no profile of its own never sends an
 *  empty folder over a cloud copy that has some. */
export const keeps = (path) => !/\.(bmp|png|log)$/i.test(path);

const PROFILE_RE = /^Documents\/NFS Carbon\/([^/]+)\/\1$/;
/** The profile names among the files ("Documents/NFS Carbon/<NAME>/<NAME>"). */
export function profilesIn(files) {
  return files.map(f => PROFILE_RE.exec(f.path)).filter(Boolean).map(m => m[1]);
}

/** a stream's bytes, at most max of them */
async function bytesOf(s, max = Infinity) {
  const rd = s.getReader(), parts = [];
  let n = 0;
  try {
    for (;;) {
      const { done, value } = await rd.read();
      if (done) break;
      if ((n += value.length) > max) throw new VaultError('these saves are larger than a key keeps', 'VAULT-TOO-LARGE');
      parts.push(value);
    }
  } catch (e) { void rd.cancel().catch(() => {}); throw e; }
  const out = new Uint8Array(n);
  let p = 0;
  for (const x of parts) { out.set(x, p); p += x.length; }
  return out;
}

function importKey(saveKey) {
  return crypto.subtle.importKey('raw', saveKey, 'AES-GCM', false, ['encrypt', 'decrypt']);
}

/** The files as a zip, deflated, then sealed under saveKey (Uint8Array(32), from key.js derive()). */
export async function seal(saveKey, files) {
  const z = zip(files.map(f => ({ name: f.path, data: f.data, mtime: f.mtime })));
  const packed = await bytesOf(new Blob([z]).stream().pipeThrough(new CompressionStream('deflate-raw')));
  const key = await importKey(saveKey);
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const ct = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv, additionalData: enc.encode(AAD) }, key, packed));
  const out = new Uint8Array(13 + ct.length);
  out[0] = 1; out.set(iv, 1); out.set(ct, 13);
  return out;
}

/** seal()'s box, opened back into files. Throws when the box is not one of ours or the key is wrong. */
export async function open(saveKey, box) {
  if (box.length < 13 + 16 || box[0] !== 1) throw new VaultError('what the server holds is not a key\'s saves', 'VAULT-NOT-SAVES');
  const key = await importKey(saveKey);
  let packed;
  try {
    packed = new Uint8Array(await crypto.subtle.decrypt({ name: 'AES-GCM', iv: box.slice(1, 13), additionalData: enc.encode(AAD) }, key, box.slice(13)));
  } catch { throw new VaultError('these saves do not open with this key', 'VAULT-WRONG-KEY'); }
  let plain;
  try { plain = await bytesOf(new Blob([packed]).stream().pipeThrough(new DecompressionStream('deflate-raw')), MAX_PLAIN); }
  catch (e) { throw e instanceof VaultError ? e : new VaultError(`these saves are damaged (${e instanceof Error ? e.message : e})`, 'VAULT-DAMAGED'); }
  const entries = await unzip(plain);
  return entries.map(e => ({ path: e.name, data: e.data, mtime: e.mtime }));
}

/** What the files hold, whatever their times: SHA-256 over their paths and bytes, in path order. */
export async function contentHash(files) {
  const sorted = [...files].sort((a, b) => a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
  const parts = [];
  for (const f of sorted) {
    const name = enc.encode(f.path), head = new Uint8Array(8), v = new DataView(head.buffer);
    v.setUint32(0, name.length, true); v.setUint32(4, f.data.length, true);
    parts.push(head, name, f.data);
  }
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', await new Blob(parts).arrayBuffer()));
  return Array.from(digest, b => b.toString(16).padStart(2, '0')).join('');
}

const base64url = (b) => btoa(String.fromCharCode(...b)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');

function remoteOf(r) {
  const rev = r.headers.get('etag'), saved = Number(r.headers.get('x-vault-saved')), e = r.headers.get('x-vault-earlier');
  return rev && saved ? { rev, saved, earlier: e ? Number(e) : null } : null;
}

// ------------------------------------------------------------- the save folder --

async function listDir(dir) {
  const out = [];
  async function walk(d, rel) {
    for await (const [name, h] of d.entries()) {
      const r = rel ? `${rel}/${name}` : name;
      if (h.kind === 'directory') await walk(h, r); else out.push(r);
    }
  }
  await walk(dir, '');
  return out;
}

async function dirOf(base, path, create) {
  let d = base;
  for (const p of path) d = await d.getDirectoryHandle(p, { create });
  return d;
}

async function readFile(dir, path) {
  const p = path.split('/');
  const d = await dirOf(dir, p.slice(0, -1), false);
  return new Uint8Array(await (await (await d.getFileHandle(p[p.length - 1])).getFile()).arrayBuffer());
}

async function writeFile(dir, path, data) {
  const p = path.split('/');
  const d = await dirOf(dir, p.slice(0, -1), true);
  const w = await (await d.getFileHandle(p[p.length - 1], { create: true })).createWritable();
  await w.write(data); await w.close();
}

async function clearDir(dir) {
  const names = [];
  for await (const [name] of dir.entries()) names.push(name);
  for (const n of names) await dir.removeEntry(n, { recursive: true });
}

// ------------------------------------------------------------- syncing --

/**
 * One browser's link to the cloud copy under its key: the version it has (rev), the content hash of the
 * files here as they were then, when that version was saved, and the copy of an earlier day.
 */
function loadLink(storage) { try { return JSON.parse(storage?.getItem(LINK_KEY) || 'null'); } catch { return null; } }

/**
 * Syncs this browser's save folder (an OPFS directory handle) with the server's /vault under one key.
 * onState(state) is called on every change; state.s is one of:
 *   'synced'   { saved }           the cloud has this browser's version
 *   'conflict' { why, theirs }     both sides changed ('both'), this browser has no profile while the cloud
 *                                  keeps some ('empty'), the cloud's has none while this browser does ('bare'),
 *                                  or the cloud's is older than this browser's ('older'); the player picks
 *                                  keepMine() or useTheirs()
 *   'full'                         the saves are larger than the server takes
 *   'off'                          the server has no vault for this key right now (404/503): quietly off
 *   'error'  { msg }               the last try failed; the next sendIfChanged/takeIfNewer tries again
 */
export class CloudSaves {
  constructor({ token, saveKey, dir, onState, fetch: f, storage }) {
    this.token = token;
    this.saveKey = saveKey;
    this.dir = dir;
    this.onState = onState || (() => {});
    this.doFetch = f || ((...a) => fetch(...a));
    this.storage = storage !== undefined ? storage : (typeof localStorage !== 'undefined' ? localStorage : null);
    this.link = loadLink(this.storage);
    this.state = { s: 'idle' };
    this.queue = Promise.resolve();
  }

  set(state) { this.state = state; this.onState(state); }
  setLink(l) {
    this.link = l;
    if (this.storage) { if (l) this.storage.setItem(LINK_KEY, JSON.stringify(l)); else this.storage.removeItem(LINK_KEY); }
  }

  req(method, headers = {}, body) {
    return this.doFetch('/vault', { method, headers: { authorization: `Bearer ${base64url(this.token)}`, ...headers }, body, cache: 'no-store' });
  }

  /** one job at a time, in order; a failure's message shows in state unless it is a vault-full (handled by the caller) */
  run(job) {
    const p = this.queue.then(async () => {
      try { return await job(); }
      catch (e) {
        if (e instanceof VaultError && e.code === 'VAULT-TOO-LARGE') this.set({ s: 'full' });
        else this.set({ s: 'error', msg: e instanceof Error ? e.message : String(e) });
        throw e;
      }
    });
    this.queue = p.catch(() => {});
    return p;
  }

  /** this browser's save folder, minus screenshots and logs (the name lives on the server: POST /name {code}) */
  async files() {
    const paths = await listDir(this.dir);
    const files = [];
    for (const p of paths) if (keeps(p)) files.push({ path: p, data: await readFile(this.dir, p) });
    return files;
  }

  /** a cloud version in place of the save folder here */
  async restore(files) {
    await clearDir(this.dir);
    for (const f of files) await writeFile(this.dir, f.path, f.data);
  }

  async boxOf(files) {
    const box = await seal(this.saveKey, files);
    if (box.length > MAX_BOX) throw new VaultError('these saves are larger than the server takes', 'VAULT-TOO-LARGE');
    return box;
  }

  linked(remote, hash, files) {
    this.setLink({ rev: remote.rev, hash, saved: remote.saved, earlier: remote.earlier, profiles: profilesIn(files).length });
    this.set({ s: 'synced', saved: remote.saved });
  }

  /** Another browser's newer version, taken when this browser's files did not change since the last sync. */
  takeIfNewer() {
    return this.run(async () => {
      const r = await this.req('GET', this.link ? { 'if-none-match': this.link.rev } : {});
      // 404: nothing kept under this key (none yet, or gone since the last sync): the next save puts it there with
      // If-None-Match: *.  A server without a vault answers 404 here too; its PUT then fails, which turns saves off.
      if (r.status === 404) { this.setLink(null); return this.set({ s: 'idle' }); }
      if (r.status === 503) return this.set({ s: 'off' });
      if (r.status === 304) { if (this.link) this.set({ s: 'synced', saved: this.link.saved }); return; }
      const remote = remoteOf(r);
      if (!r.ok || !remote) return this.set({ s: 'off' });
      const box = new Uint8Array(await r.arrayBuffer());
      const [files, theirs] = await Promise.all([this.files(), open(this.saveKey, box)]);
      const hash = await contentHash(files), theirHash = await contentHash(theirs);
      if (theirHash === hash) return this.linked(remote, hash, files);
      const unchanged = !this.link || this.link.hash === hash;
      const older = !!(this.link && remote.saved < this.link.saved);
      const bare = !profilesIn(theirs).length && profilesIn(files).length > 0;
      if (unchanged && !older && !bare) { await this.restore(theirs); return this.linked(remote, theirHash, theirs); }
      this.set({ s: 'conflict', why: older ? 'older' : bare ? 'bare' : 'both', theirs: remote });
    });
  }

  /** This browser's save folder sent, when it changed since the last sync. */
  sendIfChanged() {
    return this.run(async () => {
      if (this.state.s === 'conflict') return;
      const files = await this.files(), hash = await contentHash(files);
      if (this.link && hash === this.link.hash) return;
      if (!profilesIn(files).length && this.link && this.link.profiles > 0) return this.set({ s: 'conflict', why: 'empty', theirs: null });
      const box = await this.boxOf(files);
      const r = await this.req('PUT', this.link ? { 'if-match': this.link.rev } : { 'if-none-match': '*' }, box);
      if (r.status === 412) return this.set({ s: 'conflict', why: 'both', theirs: remoteOf(r) });
      if (r.status === 404 || r.status === 503) return this.set({ s: 'off' });
      const remote = remoteOf(r);
      if (!r.ok || !remote) return this.set({ s: 'off' });
      this.linked(remote, hash, files);
    });
  }

  /** A conflict settled for this browser's folder: sent over whatever the cloud has. */
  keepMine() {
    return this.run(async () => {
      if (this.state.s !== 'conflict') return;
      const theirs = this.state.theirs;
      const files = await this.files(), hash = await contentHash(files), box = await this.boxOf(files);
      const rev = (theirs && theirs.rev) ?? this.link?.rev ?? null;
      const r = await this.req('PUT', rev ? { 'if-match': rev } : { 'if-none-match': '*' }, box);
      if (r.status === 412) return this.set({ s: 'conflict', why: 'both', theirs: remoteOf(r) });
      if (r.status === 404 || r.status === 503) return this.set({ s: 'off' });
      const remote = remoteOf(r);
      if (!r.ok || !remote) return this.set({ s: 'off' });
      this.linked(remote, hash, files);
    });
  }

  /** A conflict settled for the cloud's version: it replaces this browser's folder. */
  useTheirs() {
    return this.run(async () => {
      if (this.state.s !== 'conflict') return;
      const r = await this.req('GET');
      if (r.status === 404 || r.status === 503) return this.set({ s: 'off' });
      const remote = remoteOf(r);
      if (!r.ok || !remote) return this.set({ s: 'off' });
      const box = new Uint8Array(await r.arrayBuffer());
      const theirs = await open(this.saveKey, box);
      await this.restore(theirs);
      this.linked(remote, await contentHash(theirs), theirs);
    });
  }
}
