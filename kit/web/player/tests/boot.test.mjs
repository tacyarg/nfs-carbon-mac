import { test } from "node:test";
import assert from "node:assert/strict";
import { bootUrls, prefetch } from "../boot.js";

// Shared contract fixture: the same manifest entry and block the plan's block address contract
// and Task 2's library use (planning/plans/2026-10-27-faster-loading.md). Task 2's test file
// (kit/web/tests/wasmfs_fetch.test.mjs) did not exist yet when this was written, so the expected
// address is hard-coded from the contract text instead of imported.
const MANIFEST = {
  v: 1,
  files: [
    { p: "CARS/VIPER/GEOMETRY.BIN", s: 20800000, m: 1160000000 },
    { p: "GLOBAL/GLOBALB.BUN", s: 2097152, m: 1160000001 },
  ],
  blocks: { url: "/data-blocks/v2/", size: 1048576, max: 8 },
};

test("bootUrls builds the contract address, case-insensitive lookup", () => {
  const boot = { v: 1, blocks: [["cars/viper/geometry.bin", 0], ["GLOBAL/GLOBALB.BUN", 1]] };
  const urls = bootUrls(MANIFEST, boot);
  assert.deepEqual(urls, [
    "/data-blocks/v2/CARS/VIPER/GEOMETRY.BIN?b=0&n=1&m=1160000000&s=20800000",
    "/data-blocks/v2/GLOBAL/GLOBALB.BUN?b=1&n=1&m=1160000001&s=2097152",
  ]);
});

test("bootUrls skips what it cannot place, never throws", () => {
  const boot = {
    v: 1,
    blocks: [
      ["NOT/LISTED.BIN", 0], // not in the manifest
      ["CARS/VIPER/GEOMETRY.BIN", 99], // past the file's end (size 20800000 / 1048576 blocks -> 19 blocks)
    ],
  };
  assert.deepEqual(bootUrls(MANIFEST, boot), []);
  assert.deepEqual(bootUrls({ v: 1, files: MANIFEST.files }, boot), []); // no blocks.url at all
  assert.deepEqual(bootUrls(MANIFEST, {}), []); // boot has no blocks list
  assert.deepEqual(bootUrls(MANIFEST, { v: 1 }), []);
});

test("bootUrls: the contract address matches the library's byte for byte", () => {
  // The exact string from planning/plans/2026-10-27-faster-loading.md's block address contract:
  // blocks.url + p.split('/').map(encodeURIComponent).join('/') + '?b=' + k + '&n=1&m=' + m + '&s=' + s
  const urls = bootUrls(MANIFEST, { v: 1, blocks: [["CARS/VIPER/GEOMETRY.BIN", 0]] });
  assert.deepEqual(urls, ["/data-blocks/v2/CARS/VIPER/GEOMETRY.BIN?b=0&n=1&m=1160000000&s=20800000"]);
});

test("prefetch never has more than inFlight requests at once, and settles", async () => {
  const urls = Array.from({ length: 14 }, (_, i) => `u${i}`);
  let active = 0, maxActive = 0;
  const calls = [];
  let resolvers = [];
  const fetchFn = (url) => {
    calls.push(url);
    active++;
    maxActive = Math.max(maxActive, active);
    return new Promise((resolve, reject) => {
      resolvers.push(() => {
        active--;
        if (url === "u3") reject(new Error("boom")); // a failure must not stop the rest
        else resolve({ arrayBuffer: async () => new ArrayBuffer(0) });
      });
    });
  };
  const done = prefetch(urls, fetchFn, { inFlight: 6 });
  for (let guard = 0; guard < 50 && calls.length < urls.length; guard++) {
    const batch = resolvers.splice(0);
    batch.forEach((r) => r());
    await Promise.resolve(); await Promise.resolve(); await Promise.resolve();
  }
  resolvers.splice(0).forEach((r) => r());
  await done;
  assert.equal(calls.length, urls.length, "every url was fetched");
  assert.ok(maxActive <= 6, `max concurrent was ${maxActive}`);
});

test("prefetch uses low priority and defaults to the global fetch and inFlight 6", async () => {
  const calls = [];
  const fetchFn = (url, opts) => {
    calls.push(opts);
    return Promise.resolve({ arrayBuffer: async () => new ArrayBuffer(0) });
  };
  await prefetch(["a", "b"], fetchFn);
  assert.ok(calls.every((o) => o.priority === "low"));
});
