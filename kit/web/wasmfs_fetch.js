/**
 * recomp-kit: Emscripten's src/lib/libwasmfs_fetch.js (MIT, Emscripten 6.0.13) with two fixes
 * and one addition, all marked below; linked with --js-library, it replaces the built-in one.
 * The addition: a file the server's /data-manifest lists is read in 1 MiB blocks from
 * /data-blocks/v2/ instead of the stock HEAD-then-range path (see the block address contract
 * in planning/plans/2026-10-27-faster-loading.md). A file it does not list, or no manifest at
 * all, falls back to the stock path below unchanged.
 */
/**
 * @license
 * Copyright 2022 The Emscripten Authors
 * SPDX-License-Identifier: MIT
 */

addToLibrary({
  $wasmFS$JSMemoryRanges: {},

  // recomp-kit: manifest-driven block streaming state, one copy for the whole page.
  // manifestPromise resolves once to {byPath, blocks} or null (no usable manifest).
  // files[file] = { entry, blocks: [Uint8Array,...] (by block index), inflight: {idx: Promise} }
  // for a file the manifest lists; absent for everything else (stock path).
  $wasmFS$streamState: { manifestPromise: null, files: {} },

  // Fetch backend: On first access of the file (either a read or a getSize), it
  // will fetch() the data from the network asynchronously. Otherwise, after
  // that fetch it behaves just like JSFile (and it reuses the code from there).

  _wasmfs_create_fetch_backend_js__deps: [
    '$wasmFS$backends',
    '$wasmFS$JSMemoryRanges',
    '$wasmFS$streamState',
    '_wasmfs_fetch_get_file_url',
    '_wasmfs_fetch_get_chunk_size',
  ],
  _wasmfs_create_fetch_backend_js: async function(backend) {
    // recomp-kit: the file's URL, as the stock code below builds it, split out so the
    // manifest-lookup code can reuse it without duplicating the "//" collapse and the
    // absolute-vs-relative handling.
    function resolveFileUrl(file) {
      var fileUrl_p = __wasmfs_fetch_get_file_url(file);
      var fileUrl = UTF8ToString(fileUrl_p).replace(/([^:])\/{2,}/g, '$1/');
      var isAbs = fileUrl.indexOf('://') !== -1;
      return {fileUrl, isAbs};
    }

    // recomp-kit: load /data-manifest once per page. Resolves to null (use the stock
    // path for every file) when it 404s, or does not look like a usable manifest.
    function getManifest() {
      if (!wasmFS$streamState.manifestPromise) {
        wasmFS$streamState.manifestPromise = (async () => {
          try {
            var res = await fetch(new URL('/data-manifest', self.location.origin));
            if (!res.ok) return null;
            var m = await res.json();
            if (!m || !m.blocks || !m.blocks.url || !m.blocks.size || !Array.isArray(m.files) || m.files.length === 0) {
              return null;
            }
            var byPath = {};
            for (var i = 0; i < m.files.length; i++) {
              var f = m.files[i];
              if (!f.d) byPath[f.p.toLowerCase()] = f; // skip directory entries ({d:1}, no size)
            }
            return {byPath: byPath, blocks: m.blocks};
          } catch (_e) {
            return null;
          }
        })();
      }
      return wasmFS$streamState.manifestPromise;
    }

    // recomp-kit: the manifest's entry for this file, or null to use the stock path
    // (no manifest, the URL is not under the stream folder, or it does not list the file).
    async function manifestRoute(file) {
      var manifest = await getManifest();
      if (!manifest) return null;
      var {fileUrl} = resolveFileUrl(file);
      var pathname = new URL(fileUrl, self.location.origin).pathname;
      // The block address contract's lookup: the path relative to the "/data/" stream
      // folder, lower-cased (see planning/plans/2026-10-27-faster-loading.md).
      var m = /^\/data\/(.+)$/i.exec(pathname);
      if (!m) return null;
      var entry = manifest.byPath[m[1].toLowerCase()];
      if (!entry) return null;
      return {entry: entry, blocksCfg: manifest.blocks};
    }

    // recomp-kit: the contract address for block k of a manifest entry.
    function blockUrl(entry, blocksCfg, k) {
      var path = entry.p.split('/').map(encodeURIComponent).join('/');
      return blocksCfg.url + path + '?b=' + k + '&n=1&m=' + entry.m + '&s=' + entry.s;
    }

    // recomp-kit: fetch any of `indexes` not already held or in flight (at most 8 of this
    // read's requests in flight at once), then wait for every index in `indexes` to settle,
    // including ones another concurrent read already started. A short body or a non-ok
    // status fails the whole read (see read()'s catch); no bytes are copied either way.
    async function ensureBlocks(file, entry, blocksCfg, indexes) {
      var rec = wasmFS$streamState.files[file];
      var blockSize = blocksCfg.size;
      var toStart = indexes.filter((k) => rec.blocks[k] === undefined && !rec.inflight[k]);
      for (var i = 0; i < toStart.length; i += 8) {
        var batch = toStart.slice(i, i + 8);
        batch.forEach((k) => {
          var p = (async () => {
            var res = await fetch(blockUrl(entry, blocksCfg, k));
            if (!res.ok) throw {status: res.status};
            var bytes = res.bytes ? await res.bytes() : new Uint8Array(await res.arrayBuffer());
            var expected = Math.min(blockSize, entry.s - k * blockSize);
            if (bytes.byteLength !== expected) throw {status: 502};
            rec.blocks[k] = bytes;
          })();
          rec.inflight[k] = p.finally(() => { if (rec.inflight[k] === p) delete rec.inflight[k]; });
        });
        await Promise.all(batch.map((k) => rec.inflight[k]));
      }
      await Promise.all(indexes.map((k) => rec.inflight[k] || Promise.resolve()));
    }

    // recomp-kit: read() for a file the manifest lists. Throws like getFileRange on
    // failure, so read()'s existing catch turns it into the right negative errno.
    async function readFromBlocks(file, route, buffer, length, offset) {
      var {entry, blocksCfg} = route;
      length = Math.min(length, entry.s - offset);
      if (length <= 0) return 0;
      if (!wasmFS$streamState.files[file]) wasmFS$streamState.files[file] = {entry: entry, blocks: [], inflight: {}};
      var blockSize = blocksCfg.size;
      var firstBlock = (offset / blockSize) | 0;
      var lastBlock = ((offset + length - 1) / blockSize) | 0;
      var indexes = [];
      for (var k = firstBlock; k <= lastBlock; k++) indexes.push(k);
      await ensureBlocks(file, entry, blocksCfg, indexes);
      var blocks = wasmFS$streamState.files[file].blocks;
      var readLength = 0;
      for (var i = firstBlock; i <= lastBlock; i++) {
        var block = blocks[i];
        var start = Math.max(i * blockSize, offset);
        var blockStart = i * blockSize;
        var end = Math.min(blockStart + blockSize, offset + length);
        HEAPU8.set(block.subarray(start - blockStart, end - blockStart), buffer + (start - offset));
        readLength = end - offset;
      }
      return readLength;
    }

    // Get a promise that fetches the data and stores it in JS memory (if it has
    // not already been fetched).
    async function getFileRange(file, offset, len) {
      var url = '';
      var fileUrl_p = __wasmfs_fetch_get_file_url(file);
      // recomp-kit: the C++ getFileURL joins baseUrl + "/" + filePath and filePath
      // already starts with "/", so every streamed file URL carries "//"; strict
      // servers (ours) reject that. Collapse repeated slashes, but keep "://" intact.
      var fileUrl = UTF8ToString(fileUrl_p).replace(/([^:])\/{2,}/g, '$1/');
      var isAbs = fileUrl.indexOf('://') !== -1;
      if (isAbs) {
        url = fileUrl;
      } else {
        try {
          var u = new URL(fileUrl, self.location.origin);
          url = u.toString();
        } catch (_e) {
          throw {status: 404};
        }
      }
      var chunkSize = __wasmfs_fetch_get_chunk_size(file);
      offset ??= 0;
      len ??= chunkSize;
      // In which chunk does the seeked range start?  E.g., 5-14 with chunksize 8 will start in chunk 0.
      if (!(file in wasmFS$JSMemoryRanges)) {
        var fileInfo = await fetch(url, {method:'HEAD', headers:{'Range': 'bytes=0-'}});
        if (fileInfo.ok &&
            fileInfo.headers.has('Content-Length') &&
            fileInfo.headers.get('Accept-Ranges') == 'bytes' &&
            (parseInt(fileInfo.headers.get('Content-Length'), 10) > chunkSize*2)) {
          var size = parseInt(fileInfo.headers.get('Content-Length'), 10);
          wasmFS$JSMemoryRanges[file] = {
            size,
            chunks: [],
            chunkSize: chunkSize
          };
          len = Math.min(len, size-offset);
        } else {
          // may as well/forced to download the whole file
          var wholeFileReq = await fetch(url);
          if (!wholeFileReq.ok) {
            throw wholeFileReq;
          }
          var wholeFileData = new Uint8Array(await wholeFileReq.arrayBuffer());
          wasmFS$JSMemoryRanges[file] = {
            size: wholeFileData.byteLength,
            chunks: [wholeFileData],
            chunkSize: wholeFileData.byteLength
          };
          return;
        }
      }
      // recomp-kit: a file fetched whole (too small to chunk) is all in chunks[0]; chunking it
      // again here overwrote chunks[0] with its first chunk (reads past it came back short).
      if (wasmFS$JSMemoryRanges[file].chunkSize !== chunkSize) return;
      var firstChunk = (offset / chunkSize) | 0;
      // In which chunk does the seeked range end?  E.g., 5-14 with chunksize 8 will end in chunk 1, as will 5-16 (since byte 16 isn't requested).
      // This will always give us a chunk >= firstChunk since len > 0.
      var lastChunk = ((offset+len-1) / chunkSize) | 0;
      var allPresent = true;
      var i;
      // Do we have all the chunks already?  If so, we don't need to do any fetches.
      for (i = firstChunk; i <= lastChunk; i++) {
        if (!wasmFS$JSMemoryRanges[file].chunks[i]) {
          allPresent = false;
          break;
        }
      }
      if (allPresent) {
        // The data is already here, so nothing to do before we continue on to
        // the actual read.
        return;
      }
      // This is the first time we want the chunks' data.  We'll make
      // one request for all the chunks we need, rather than one
      // request per chunk.
      var start = firstChunk * chunkSize;
      // We must fetch *up to* the last byte of the last chunk.
      var end = (lastChunk+1) * chunkSize;
      var response = await fetch(url, {headers:{'Range': `bytes=${start}-${end-1}`}});
      if (!response.ok) {
        throw response;
      }
#if MIN_FIREFOX_VERSION < 128 || MIN_CHROME_VERSION < 132 || MIN_SAFARI_VERSION < 180000 || MIN_NODE_VERSION < 220300
      // Use the old .arrayBuffer() method when targeting old environments.
      var bytes = new Uint8Array(await response['arrayBuffer']());
#else
      // Use the new .bytes() method to save a bit of code size when all target environments have it. https://developer.mozilla.org/en-US/docs/Web/API/Response/bytes
      var bytes = await response['bytes']();
#endif
      for (i = firstChunk; i <= lastChunk; i++) {
        wasmFS$JSMemoryRanges[file].chunks[i] = bytes.slice(i*chunkSize-start,(i+1)*chunkSize-start);
      }
    }

    wasmFS$backends[backend] = {
      // alloc/free operations are not actually async. Just forward to the
      // parent class, but we must return a Promise as the caller expects.
      allocFile: async (file) => { /* nop */ },
      freeFile: async (file) => {
        // free memory
        wasmFS$JSMemoryRanges[file] = undefined;
        wasmFS$streamState.files[file] = undefined; // recomp-kit: also drop any held blocks
      },

      write: async (file, buffer, length, offset) => {
        console.error('TODO: file writing in fetch backend? read-only for now');
      },

      // read/getSize fetch the data, then forward to the parent class.
      read: async (file, buffer, length, offset) => {
        // This function assumes that offset is non-negative and length is positive.
        // C read() doesn't take an offset and so doesn't have to deal with the former situation,
        // and if the length is 0 or the offset is negative there's no reasonable read we can make.
        if (offset < 0 || length <= 0) {
          return 0;
        }
        // recomp-kit: a file the manifest lists is read in blocks instead of the stock
        // HEAD-then-range path below. A manifest-fetch hiccup (not a block failure) just
        // falls back to the stock path, same as "not listed".
        var route = null;
        try {
          route = await manifestRoute(file);
        } catch (_e) {
          route = null;
        }
        if (route) {
          try {
            return await readFromBlocks(file, route, buffer, length, offset);
          } catch (failedResponse) {
            return failedResponse.status === 404 ? -{{{ cDefs.ENOENT }}} : -{{{ cDefs.EBADF }}};
          }
        }
        try {
          await getFileRange(file, offset || 0, length);
        } catch (failedResponse) {
          return failedResponse.status === 404 ? -{{{ cDefs.ENOENT }}} : -{{{ cDefs.EBADF }}};
        }
        var fileInfo = wasmFS$JSMemoryRanges[file];
        length = Math.min(length, fileInfo.size-offset);
        // As above, we check the length just in case offset was beyond size and length is now negative.
        if (length <= 0) {
          return 0;
        }
        var chunks = fileInfo.chunks;
        var chunkSize = fileInfo.chunkSize;
        var firstChunk = (offset / chunkSize) | 0;
        // See comments in getFileRange.
        var lastChunk = ((offset+length-1) / chunkSize) | 0;
        var readLength = 0;
        for (var i = firstChunk; i <= lastChunk; i++) {
          var chunk = chunks[i];
          var start = Math.max(i*chunkSize, offset);
          var chunkStart = i*chunkSize;
          var end = Math.min(chunkStart+chunkSize, offset+length);
          HEAPU8.set(chunk.subarray(start-chunkStart, end-chunkStart), buffer+(start-offset));
          readLength = end - offset;
        }
        return readLength;
      },
      getSize: async (file) => {
        // recomp-kit: the manifest already carries the size, no HEAD needed.
        var route = null;
        try {
          route = await manifestRoute(file);
        } catch (_e) {
          route = null;
        }
        if (route) return route.entry.s;
        try {
          await getFileRange(file, 0, 0);
        } catch (failedResponse) {
          return 0;
        }
        return wasmFS$JSMemoryRanges[file].size;
      },
    };
  },

});
