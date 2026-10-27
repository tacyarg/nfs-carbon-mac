// WebGPU calls Firefox refuses (kit/web/player/gpu.js): node --test kit/web/player/tests/gpu.test.mjs
import { test } from "node:test";
import assert from "node:assert/strict";
import { fitBindGroupOffsets } from "../gpu.js";

test("setBindGroup gets only its dynamic offsets, never the whole heap", () => {
  const calls = [];
  class Pass { setBindGroup(...a) { calls.push(a); } }
  fitBindGroupOffsets(Pass.prototype, undefined);
  const once = Pass.prototype.setBindGroup;
  fitBindGroupOffsets(Pass.prototype);
  assert.equal(Pass.prototype.setBindGroup, once, "a second call does not wrap again");

  const heap = new Uint32Array(64);
  heap.set([7, 9], 10);
  const pass = new Pass();
  pass.setBindGroup(1, "g", heap, 10, 2);
  pass.setBindGroup(0, "h");
  pass.setBindGroup(2, "i", [3]);

  assert.deepEqual([...calls[0][2]], [7, 9]);
  assert.equal(calls[0][2].length, 2, "just the offsets: Firefox refuses a view over 2 GB");
  assert.equal(calls[0][2].buffer, heap.buffer, "a view onto the heap, not a copy");
  assert.deepEqual(calls[0].slice(3), [0, 2]);
  assert.deepEqual(calls[1], [0, "h"]);
  assert.deepEqual(calls[2], [2, "i", [3]]);
});
