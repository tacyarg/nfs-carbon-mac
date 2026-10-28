// node --test kit/web/tests/wasmfs_fetch.test.mjs
//
// kit/web/wasmfs_fetch.js is an Emscripten JS library, not a module: this harness reads it,
// keeps the modern (.bytes()) branch of its one #if/#else/#endif, substitutes its two
// {{{ cDefs.* }}} macros, and evaluates it in a vm context with stubs for everything Emscripten
// would otherwise supply (addToLibrary, the wasmFS$ globals, HEAPU8, self, fetch, ...), then
// drives the captured `_wasmfs_create_fetch_backend_js` library entry directly.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import fs from 'node:fs';
import url from 'node:url';

const SOURCE_PATH = url.fileURLToPath(new URL('../wasmfs_fetch.js', import.meta.url));

function loadLibrarySource() {
  let src = fs.readFileSync(SOURCE_PATH, 'utf8');
  // Keep only the #else (.bytes()) branch of the single #if/#else/#endif block.
  src = src.replace(/#if[^\n]*\n[\s\S]*?#else\n([\s\S]*?)#endif\n/, '$1');
  src = src.replaceAll('{{{ cDefs.ENOENT }}}', '44').replaceAll('{{{ cDefs.EBADF }}}', '8');
  return src;
}
const LIBRARY_SOURCE = loadLibrarySource();

const ORIGIN = 'https://game.example';
const BLOCK_SIZE = 1048576;

// Deterministic bytes for a fake file: cheap, reproducible, not real game data.
function fakeFileBytes(path, size) {
  let seed = 0;
  for (let i = 0; i < path.length; i++) seed = (seed * 31 + path.charCodeAt(i)) >>> 0;
  const buf = new Uint8Array(size);
  for (let i = 0; i < size; i++) buf[i] = (seed + i) & 0xff;
  return buf;
}

function headers(obj) {
  const map = new Map(Object.entries(obj || {}).map(([k, v]) => [k.toLowerCase(), v]));
  return { has: (k) => map.has(k.toLowerCase()), get: (k) => map.get(k.toLowerCase()) ?? null };
}

function fakeResponse(status, body, hdrs) {
  const bytes = body instanceof Uint8Array ? body : body == null ? new Uint8Array(0) : new TextEncoder().encode(body);
  return {
    ok: status >= 200 && status < 300,
    status,
    headers: headers(hdrs),
    async bytes() { return bytes; },
    async arrayBuffer() { return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength); },
    async json() { return JSON.parse(new TextDecoder().decode(bytes)); },
  };
}

/**
 * A fake server: a manifest (or none), listed files with real content, and unlisted
 * "stock" files served HEAD + Range/whole, in the exact shapes the library expects.
 * `opts.shortBlock` / `opts.failBlock` corrupt one block's answer for test 6.
 */
function makeServer(opts = {}) {
  const requests = [];
  const listed = opts.listed || {};
  const stock = opts.stock || {};
  const manifestStatus = opts.manifestStatus ?? 200;
  const files = Object.entries(listed).map(([p, f]) => ({ p, s: f.size, m: f.mtime }));
  if (opts.withDirEntry) files.unshift({ p: 'CARS', d: 1 });
  const manifest = { v: 1, files, blocks: { url: '/data-blocks/v2/', size: BLOCK_SIZE, max: 8 } };
  const contents = {};
  for (const [p, f] of Object.entries(listed)) contents[p] = fakeFileBytes(p, f.size);
  for (const [p, f] of Object.entries(stock)) contents[p] = fakeFileBytes(p, f.size);

  let activeBlocks = 0, peakBlocks = 0;
  const entryByLowerPath = new Map(Object.entries(listed).map(([p, f]) => [p.toLowerCase(), { p, ...f }]));

  async function fetch(input, init) {
    const u = new URL(String(input), ORIGIN);
    requests.push({ path: u.pathname, search: u.search, method: (init && init.method) || 'GET', headers: init && init.headers });

    if (u.pathname === '/data-manifest') {
      if (manifestStatus !== 200) return fakeResponse(manifestStatus, null);
      return fakeResponse(200, JSON.stringify(manifest), { 'content-type': 'application/json' });
    }

    if (u.pathname.startsWith('/data-blocks/v2/')) {
      const path = u.pathname.slice('/data-blocks/v2/'.length).split('/').map(decodeURIComponent).join('/');
      const entry = entryByLowerPath.get(path.toLowerCase());
      const qs = new URLSearchParams(u.search);
      const b = Number(qs.get('b'));
      if (!entry) return fakeResponse(404, null);
      activeBlocks++; peakBlocks = Math.max(peakBlocks, activeBlocks);
      await new Promise((r) => setTimeout(r, 5)); // let concurrent calls overlap, like a real network
      activeBlocks--;
      if (opts.failBlock && opts.failBlock.path === path && opts.failBlock.block === b) {
        return fakeResponse(500, null);
      }
      const start = b * BLOCK_SIZE;
      const end = Math.min(start + BLOCK_SIZE, entry.size);
      let slice = contents[path].subarray(start, end);
      if (opts.shortBlock && opts.shortBlock.path === path && opts.shortBlock.block === b) {
        slice = slice.subarray(0, slice.length - 1);
      }
      return fakeResponse(200, slice);
    }

    if (u.pathname.startsWith('/data/')) {
      const path = decodeURIComponent(u.pathname.slice('/data/'.length));
      const content = contents[path];
      if (!content) return fakeResponse(404, null);
      if ((init && init.method) === 'HEAD') {
        return fakeResponse(200, null, { 'content-length': String(content.length), 'accept-ranges': 'bytes' });
      }
      const range = init && init.headers && init.headers['Range'];
      if (range) {
        const m = /bytes=(\d+)-(\d+)/.exec(range);
        const s = Number(m[1]), e = Math.min(Number(m[2]) + 1, content.length);
        return fakeResponse(206, content.subarray(s, e));
      }
      return fakeResponse(200, content);
    }

    return fakeResponse(404, null);
  }

  return { fetch, requests, manifest, contents, get peakBlocks() { return peakBlocks; } };
}

/** Loads a fresh library instance (its own vm context, own wasmFS$ state) wired to `server`. */
function loadLibrary(server, { heapSize = 64 * 1024 * 1024 } = {}) {
  let captured = null;
  const HEAPU8 = new Uint8Array(heapSize);
  const sandbox = {
    addToLibrary: (obj) => { captured = obj; },
    wasmFS$backends: {},
    wasmFS$JSMemoryRanges: {},
    wasmFS$streamState: { manifestPromise: null, files: {} },
    // File handles are just their "/data/<path>" URL string: the test does not need real
    // pointer semantics, only that get_file_url + UTF8ToString round-trip it.
    __wasmfs_fetch_get_file_url: (file) => file,
    UTF8ToString: (ptr) => ptr,
    __wasmfs_fetch_get_chunk_size: () => 4 * 1024 * 1024,
    HEAPU8,
    self: { location: { origin: ORIGIN } },
    fetch: server.fetch,
    URL, URLSearchParams, Uint8Array, Promise, Math, Object, Array, JSON, TextEncoder, TextDecoder,
    console,
  };
  vm.createContext(sandbox);
  vm.runInContext(LIBRARY_SOURCE, sandbox, { filename: 'wasmfs_fetch.js' });
  return { captured, HEAPU8, backends: sandbox.wasmFS$backends, state: sandbox.wasmFS$streamState };
}

async function makeBackend(server, opts) {
  const lib = loadLibrary(server, opts);
  const backendId = 1;
  await lib.captured._wasmfs_create_fetch_backend_js(backendId);
  return { backend: lib.backends[backendId], HEAPU8: lib.HEAPU8, state: lib.state };
}

function fileFor(path) { return `/data/${path}`; }

test('getSize of two listed files: manifest sizes, one /data-manifest request', async () => {
  const server = makeServer({ listed: { 'CARS/VIPER/GEOMETRY.BIN': { size: 2_500_000, mtime: 1160000000 }, 'GLOBAL/GLOBALB.BUN': { size: 500_000, mtime: 1160000001 } } });
  const { backend } = await makeBackend(server);
  assert.equal(await backend.getSize(fileFor('CARS/VIPER/GEOMETRY.BIN')), 2_500_000);
  assert.equal(await backend.getSize(fileFor('GLOBAL/GLOBALB.BUN')), 500_000);
  const manifestHits = server.requests.filter((r) => r.path === '/data-manifest');
  assert.equal(manifestHits.length, 1);
});

test('a read spanning blocks 0..2 returns exact bytes and fetches exactly those blocks', async () => {
  const path = 'CARS/VIPER/GEOMETRY.BIN';
  const server = makeServer({ listed: { [path]: { size: 2_500_000, mtime: 1160000000 } } });
  const { backend, HEAPU8 } = await makeBackend(server);
  const buffer = 1000;
  const len = await backend.read(fileFor(path), buffer, 2_500_000, 0);
  assert.equal(len, 2_500_000);
  assert.deepEqual(HEAPU8.subarray(buffer, buffer + 2_500_000), server.contents[path]);
  const blockReqs = server.requests.filter((r) => r.path === `/data-blocks/v2/${path}`);
  assert.deepEqual(blockReqs.map((r) => new URLSearchParams(r.search).get('b')).sort(), ['0', '1', '2']);
  for (const r of blockReqs) {
    const qs = new URLSearchParams(r.search);
    assert.equal(qs.get('n'), '1');
    assert.equal(qs.get('m'), '1160000000');
    assert.equal(qs.get('s'), '2500000');
  }
  assert.equal(server.requests.some((r) => r.path.startsWith('/data/')), false);
  assert.equal(server.requests.some((r) => r.method === 'HEAD'), false);
});

test('reading the same range again makes no new request', async () => {
  const path = 'CARS/VIPER/GEOMETRY.BIN';
  const server = makeServer({ listed: { [path]: { size: 2_500_000, mtime: 1160000000 } } });
  const { backend } = await makeBackend(server);
  await backend.read(fileFor(path), 1000, 2_500_000, 0);
  const before = server.requests.length;
  await backend.read(fileFor(path), 1000, 2_500_000, 0);
  assert.equal(server.requests.length, before);
});

test('two concurrent reads of the same block make one request', async () => {
  const path = 'GLOBAL/GLOBALB.BUN';
  const server = makeServer({ listed: { [path]: { size: 500_000, mtime: 7 } } });
  const { backend } = await makeBackend(server);
  const [a, b] = await Promise.all([
    backend.read(fileFor(path), 0, 500_000, 0),
    backend.read(fileFor(path), 100000, 500_000, 0),
  ]);
  assert.equal(a, 500_000);
  assert.equal(b, 500_000);
  const blockReqs = server.requests.filter((r) => r.path === `/data-blocks/v2/${path}`);
  assert.equal(blockReqs.length, 1);
});

test('a read needing 12 blocks never has more than 8 in flight, and returns the right bytes', async () => {
  const path = 'CARS/BIG.BUN';
  const size = 12 * BLOCK_SIZE;
  const server = makeServer({ listed: { [path]: { size, mtime: 42 } } });
  const { backend, HEAPU8 } = await makeBackend(server);
  const buffer = 2000;
  const len = await backend.read(fileFor(path), buffer, size, 0);
  assert.equal(len, size);
  assert.deepEqual(HEAPU8.subarray(buffer, buffer + size), server.contents[path]);
  assert.ok(server.peakBlocks <= 8, `peak concurrent block requests was ${server.peakBlocks}`);
  assert.ok(server.peakBlocks >= 2, 'expected some batching to actually overlap');
});

test('a short or failed block answer is a negative error; no bytes are copied', async () => {
  const path = 'GLOBAL/GLOBALB.BUN';
  const server = makeServer({ listed: { [path]: { size: 500_000, mtime: 7 } }, shortBlock: { path, block: 0 } });
  const { backend, HEAPU8 } = await makeBackend(server);
  HEAPU8.fill(0xaa, 0, 1000);
  const len = await backend.read(fileFor(path), 0, 500_000, 0);
  assert.ok(len < 0, `expected a negative errno, got ${len}`);
  assert.ok(HEAPU8.subarray(0, 1000).every((b) => b === 0xaa), 'no bytes should have been copied');

  const server2 = makeServer({ listed: { [path]: { size: 500_000, mtime: 7 } }, failBlock: { path, block: 0 } });
  const { backend: backend2, HEAPU8: heap2 } = await makeBackend(server2);
  heap2.fill(0xaa, 0, 1000);
  const len2 = await backend2.read(fileFor(path), 0, 500_000, 0);
  assert.ok(len2 < 0, `expected a negative errno, got ${len2}`);
  assert.ok(heap2.subarray(0, 1000).every((b) => b === 0xaa));
});

test('a path the manifest does not list uses the stock HEAD + whole/range path', async () => {
  const path = 'SOME/OTHER.FILE';
  const server = makeServer({ listed: { 'CARS/VIPER/GEOMETRY.BIN': { size: 2_500_000, mtime: 1 } }, stock: { [path]: { size: 12345 } } });
  const { backend, HEAPU8 } = await makeBackend(server);
  const buffer = 500;
  const len = await backend.read(fileFor(path), buffer, 12345, 0);
  assert.equal(len, 12345);
  assert.deepEqual(HEAPU8.subarray(buffer, buffer + 12345), server.contents[path]);
  assert.ok(server.requests.some((r) => r.method === 'HEAD' && r.path === `/data/${path}`));
  assert.equal(server.requests.some((r) => r.path.startsWith('/data-blocks/')), false);
});

test('manifest 404: every file uses the stock path', async () => {
  const path = 'CARS/VIPER/GEOMETRY.BIN';
  const server = makeServer({ manifestStatus: 404, stock: { [path]: { size: 777 } } });
  const { backend, HEAPU8 } = await makeBackend(server);
  const buffer = 10;
  const len = await backend.read(fileFor(path), buffer, 777, 0);
  assert.equal(len, 777);
  assert.deepEqual(HEAPU8.subarray(buffer, buffer + 777), server.contents[path]);
  assert.ok(server.requests.some((r) => r.method === 'HEAD'));
});

test('the last block of a file shorter than 1 MiB is accepted at its true length', async () => {
  const path = 'GLOBAL/TAIL.BUN';
  const size = BLOCK_SIZE + 12345; // block 1 is 12345 bytes, not a full MiB
  const server = makeServer({ listed: { [path]: { size, mtime: 9 } } });
  const { backend, HEAPU8 } = await makeBackend(server);
  const buffer = 300;
  const len = await backend.read(fileFor(path), buffer, size, 0);
  assert.equal(len, size);
  assert.deepEqual(HEAPU8.subarray(buffer, buffer + size), server.contents[path]);
});

test('the block cache holds at most 64 blocks, dropping the least recently read', async () => {
  // Every block used to stay in memory for the whole session; Safari's page process grew past
  // what an iPhone allows and was killed while booting. A dropped block is read again from the
  // browser's HTTP cache (block addresses are immutable).
  const path = 'TRACKS/STREAML5RA.BUN';
  const server = makeServer({ listed: { [path]: { size: 70 * BLOCK_SIZE, mtime: 9 } } });
  const { backend, HEAPU8, state } = await makeBackend(server);
  const blockReqs = (b) => server.requests.filter((r) => r.path === `/data-blocks/v2/${path}` && new URLSearchParams(r.search).get('b') === String(b)).length;
  for (let b = 0; b < 70; b++) {
    assert.equal(await backend.read(fileFor(path), 0, BLOCK_SIZE, b * BLOCK_SIZE), BLOCK_SIZE);
    if (b === 40) await backend.read(fileFor(path), 0, 16, 0); // block 0 read again: recent now
  }
  assert.equal(state.files[fileFor(path)].blocks.filter(Boolean).length, 64);
  assert.equal(await backend.read(fileFor(path), 0, BLOCK_SIZE, 0), BLOCK_SIZE);
  assert.equal(blockReqs(0), 1, 'a block read again stays held');
  assert.equal(await backend.read(fileFor(path), 0, BLOCK_SIZE, BLOCK_SIZE), BLOCK_SIZE);
  assert.equal(blockReqs(1), 2, 'the least recently read block was dropped and is fetched again');
  assert.deepEqual(HEAPU8.subarray(0, BLOCK_SIZE), server.contents[path].subarray(BLOCK_SIZE, 2 * BLOCK_SIZE));
});

test('a single read larger than the block cache returns exact bytes', async () => {
  // Carbon reads some files in one call bigger than the cache; holding all of a read's blocks at
  // once made that read fail and the game recurse until the stack overflowed.
  const path = 'GLOBAL/GLOBALB.BUN';
  const size = 100 * BLOCK_SIZE + 123;
  const server = makeServer({ listed: { [path]: { size, mtime: 3 } } });
  const { backend, HEAPU8 } = await makeBackend(server, { heapSize: 128 * 1024 * 1024 });
  assert.equal(await backend.read(fileFor(path), 16, size, 0), size);
  assert.deepEqual(HEAPU8.subarray(16, 16 + size), server.contents[path]);
  assert.ok(server.peakBlocks <= 8);
});
