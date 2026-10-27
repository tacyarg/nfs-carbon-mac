// gpu.js: WebGPU calls the emdawnwebgpu glue makes that Firefox refuses.
//
// The glue hands setBindGroup the whole wasm heap (HEAPU32) as its dynamic-offsets array, plus a
// start and a count. Firefox rejects any typed-array view over 2 GB, our heap grows past that, and
// the throw kills the frame loop (black screen at 3 fps). Chrome accepts it. A library override
// cannot fix the glue: the port's library links after ours and wins, so wrap the method instead.

/** Wraps each prototype's setBindGroup so a (data, start, count) call passes just those offsets. */
export function fitBindGroupOffsets(...protos) {
  for (const p of protos) {
    if (!p || p.setBindGroup.fitted) continue;
    const set = p.setBindGroup;
    const fitted = function (index, group, data, start, count) {
      if (arguments.length < 5) return set.apply(this, arguments);
      return set.call(this, index, group, data.subarray(start, start + count), 0, count);
    };
    fitted.fitted = true;
    p.setBindGroup = fitted;
  }
}
