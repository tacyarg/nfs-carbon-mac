// boot.js: the contract addresses for the game's boot blocks, and a small fetch pool to warm the
// browser's HTTP cache with them before the game's own reads land on the same addresses. See
// planning/plans/2026-10-27-faster-loading.md (the block address contract) and
// kit/web/wasmfs_fetch.js, which builds the identical string for the library's own reads.

/**
 * manifest: the /data-manifest response ({v, files:[{p,s,m}|{p,d:1}], blocks:{url,size,max}}).
 * boot: kit/web/player/boot.json's shape ({v, blocks:[[path, blockNumber], ...]}).
 * Returns the contract address for each boot entry the manifest can still place; a path the
 * manifest does not list (case-insensitive), a block past the file's end, or a manifest with no
 * usable blocks section is skipped, never thrown.
 */
export function bootUrls(manifest, boot) {
  const files = (manifest && manifest.files) || [];
  const blocks = manifest && manifest.blocks;
  const entries = (boot && boot.blocks) || [];
  if (!blocks || !blocks.url || !blocks.size) return [];
  const byPath = new Map(files.map((f) => [String(f.p).toLowerCase(), f]));
  const urls = [];
  for (const [path, k] of entries) {
    const f = byPath.get(String(path).toLowerCase());
    if (!f || f.s == null) continue; // not listed, or a directory entry
    if (k * blocks.size >= f.s) continue; // past the file's end
    urls.push(blocks.url + f.p.split("/").map(encodeURIComponent).join("/") +
      `?b=${k}&n=1&m=${f.m}&s=${f.s}`);
  }
  return urls;
}

/**
 * Fetches each url with fetchFn, at most opts.inFlight at once (default 6), low priority, reading
 * each body to the end so it lands in the cache. Resolves once every url has settled; a failed
 * fetch is ignored, not re-thrown.
 */
export async function prefetch(urls, fetchFn = fetch, { inFlight = 6 } = {}) {
  let next = 0;
  const workers = Array.from({ length: Math.min(inFlight, urls.length) }, async () => {
    while (next < urls.length) {
      const url = urls[next++];
      try {
        const r = await fetchFn(url, { priority: "low" });
        if (r && r.arrayBuffer) await r.arrayBuffer();
      } catch {
        // quiet: a missed prefetch just means the game's own read pays for that block later
      }
    }
  });
  await Promise.all(workers);
}
