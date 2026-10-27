// bursts.mjs - which thread holds the CPU in long unbroken stretches, and in what code.
//   node tools/bench/bursts.mjs <bench out dir>   (a PROFILE=1 run of race.mjs)
// A stretch is a run of samples that are not waits; one longer than MIN_MS (default 8) on a thread other than
// the game's is a stretch the game thread may wait out for the baton (guest_max_starve_ms in the frame timings).
// Each long stretch is named by the guest functions (fn_/body_<address>) seen most in it, innermost first.
import fs from "node:fs";

const dir = process.argv[2], MIN = +(process.env.MIN_MS || 8);
const WAIT = /\(idle\)|\(program\)|futex_wait|_emscripten_thread_mailbox|Atomics\.wait|__timedwait|sched_wait|emscripten_thread_sleep/;
for (const f of fs.readdirSync(`${dir}/prof`).sort()) {
  const p = JSON.parse(fs.readFileSync(`${dir}/prof/${f}`));
  const byId = new Map(p.nodes.map((n) => [n.id, n])), parent = new Map();
  for (const n of p.nodes) for (const c of n.children || []) parent.set(c, n.id);
  const stack = (id) => { const s = []; for (let x = id; x !== undefined; x = parent.get(x)) s.push(byId.get(x).callFrame.functionName); return s; };
  let busy = 0, run = null;
  const runs = [];
  p.samples.forEach((id, i) => {
    const dt = (p.timeDeltas[i + 1] ?? 0) / 1000, s = stack(id);
    if (WAIT.test(s.join(" "))) { if (run) runs.push(run); run = null; return; }
    busy += dt;
    (run ??= { ms: 0, fns: new Map() }).ms += dt;
    for (const fn of new Set(s.filter((n) => /^(fn|body)_[0-9a-f]{8}$/.test(n)).slice(0, 3))) run.fns.set(fn, (run.fns.get(fn) || 0) + dt);
  });
  if (run) runs.push(run);
  const secs = (p.endTime - p.startTime) / 1e6, long = runs.filter((r) => r.ms > MIN).sort((a, b) => b.ms - a.ms);
  if (busy / secs < 5) continue; // idle threads
  console.log(`${f}: busy ${(busy / secs).toFixed(0)} ms/s, ${long.length} stretches > ${MIN} ms, longest ${long[0]?.ms.toFixed(1) ?? 0} ms`);
  for (const r of long.slice(0, 4))
    console.log(`   ${r.ms.toFixed(1).padStart(6)} ms  ` + [...r.fns].sort((a, b) => b[1] - a[1]).slice(0, 4).map(([n, ms]) => `${n} ${ms.toFixed(1)}`).join("  "));
}
