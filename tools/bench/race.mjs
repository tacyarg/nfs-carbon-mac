// race.mjs - the race benchmark: the same race every run, at full speed, measured per frame.
//
//   node tools/bench/race.mjs                      # baseline: steps to a race, then MEASURE frames
//   AB=params node tools/bench/race.mjs            # in-run A/B of a switch (kit/platform/ab_phase.h)
//   PROFILE=1 node tools/bench/race.mjs            # also a CPU profile of every thread in the window
//   HOLD=1 node tools/bench/race.mjs               # stay up after the steps: /key /shot /logs /eval /quit on :9334
//
// Why it is quick (about 80 s a run, 57 s of it to the race; the old way took 4-9 min):
// - A persistent Chrome profile (BENCH_PROFILE, default ~/.cache/nfsc-bench/chrome): the wasm code cache, the
//   HTTP cache of the streamed game files and the save (alias "Bench", made without prompts by
//   RECOMP_ONLINE_NAME) survive runs, so a run starts warm.
// - Steps wait for the game's own events (RECOMP_BENCH=1 logs "core.nfsc: flow <state>", mods/core/nfsc/bench.h)
//   instead of sleeping a guessed time.
// - RECOMP_HOST_PRESENT_PACING=immediate: frames are not held to the display, so a faster build shows above 60.
// Frame time is between two frames the game sealed; the presenter's repeats are left out.
// Noise: two runs of one build differ ~3% (52.1 / 53.9 fps); compare builds over several runs, or in one run
// with AB= (TSC: OLD 53.6, NEW 55.8). A pinned clock (RECOMP_PIN_CLOCK) does not work here: the app host never
// steps it per D3D9 frame, Carbon's boot sleeps on it, and the menus misread a key when game time runs 2x real.
//
// env: URL (default http://localhost:8000/), STEPS (below), MEASURE frames (default 1200, ~22 s at 55 fps),
// AB=<switches> (AB_PERIOD seconds, default 2), PARAMS (extra query, "&k=v..."), TAG, VW/VH.
// Steps, comma-separated: flow=<text> waits for a flow line containing text (FLOW_TIMEOUT s, default 180);
// Key~flow=<text> taps Key every GAP_MS until such a flow line; +N waits N frames; Key or Key*N taps (held KEY_MS,
// default 120, then GAP_MS, default 1000); shot=<name>; ms=N sleeps. SHOTS=1: a screenshot before every step.
import { chromium } from "/Users/tacyarg/Projects/nfsc-web/.tools/pw/node_modules/playwright-core/index.mjs";
import fs from "node:fs";
import os from "node:os";

const env = process.env;
const TAG = env.TAG || `race-${new Date().toTimeString().slice(0, 8).replace(/:/g, "")}`;
const OUT = `${os.homedir()}/.hermes/cache/scratch/bench/${TAG}`;
const MEASURE = +(env.MEASURE || 1200), AB_PERIOD = +(env.AB_PERIOD || 2);
const KEY_MS = +(env.KEY_MS || 120), GAP_MS = +(env.GAP_MS || 1000); // the menus take a 120 ms hold, not 40
// The "Bench" save (the first run makes it): the title takes input ~10 s in, main menu (Career), three right
// to Quick Race, its first race (Circuit, Main Street, 2 laps), the options as they are, then Enter until
// the front end unloads: Car Select, and the prompts a save may still ask (transmission, crew member).
// The race has loaded when the flow is idle again after WaitForSimulation; Enter skips the fly-by.
const STEPS = env.STEPS || "flow=state 00000000,ms=9000,Enter,ms=4000,ArrowRight*3,ms=500,Enter,ms=3000,Enter,ms=3000," +
  "Enter,ms=5000,Enter~flow=UnloadingFrontEnd,flow=WaitForSimulation,flow=state 00000000,ms=1000,Enter,ms=7000,shot=start";
let q = "?RECOMP_BENCH=1&RECOMP_ONLINE_NAME=Bench&RECOMP_HOST_PRESENT_PACING=immediate&RECOMP_SETTING_skip_intro=1&RECOMP_SETTING_performance_overlay=1&RECOMP_FRAME_TIMINGS=/dev/stderr";
if (env.AB) q += `&RECOMP_AB_PERIOD=${AB_PERIOD}&RECOMP_AB_SWITCHES=${env.AB}`;
const URL = (env.URL || "http://localhost:8000/") + q + (env.PARAMS || "");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
fs.mkdirSync(OUT, { recursive: true });
process.on("exit", () => fs.writeFileSync(`${OUT}/logs.txt`, logs.join("\n")));

const ctx = await chromium.launchPersistentContext(env.BENCH_PROFILE || `${os.homedir()}/.cache/nfsc-bench/chrome`, {
  executablePath: "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  headless: false, viewport: { width: +(env.VW || 1280), height: +(env.VH || 720) },
  args: ["--enable-unsafe-webgpu", "--autoplay-policy=no-user-gesture-required", "--remote-debugging-port=9333"],
});
const frames = [], logs = [], flows = [];
const t0 = Date.now();
ctx.on("console", (m) => {
  for (const t of m.text().split("\n")) {
    const f = t.split(",");
    if (f.length === 26 && /^\d+$/.test(f[0])) { if (f[2] === "0") frames.push(f.map(Number)); } // not the presenter's repeats
    else if (t) {
      logs.push(t);
      if (t.includes("core.nfsc: flow ")) { flows.push(t); console.log(`${((Date.now() - t0) / 1000).toFixed(1).padStart(6)}s  frame ${frames.length}  ${t.slice(t.indexOf("flow "))}`); }
    }
  }
});
const page = ctx.pages()[0] ?? (await ctx.newPage());
await page.goto(URL);
await page.waitForFunction(() => !document.getElementById("play").disabled, null, { timeout: 120000 });
const b = await page.locator("#play").boundingBox();
await page.mouse.click(b.x + b.width / 2, b.y + b.height / 2);

const until = async (ok, secs, what) => {
  for (const end = Date.now() + secs * 1000; !ok(); await sleep(20))
    if (Date.now() > end) { await page.screenshot({ path: `${OUT}/stuck.png` }); throw new Error(`timed out: ${what} (see ${OUT}/stuck.png)`); }
};
const tap = async (key) => { await page.keyboard.down(key); await sleep(KEY_MS); await page.keyboard.up(key); };
const waitFrames = (n) => { const to = frames.length + n; return until(() => frames.length >= to, 10 + n / 10, `${n} frames`); };
let seen = 0; // flow lines a later flow= step may match
for (const [n, step] of STEPS.split(",").filter(Boolean).entries()) {
  if (env.SHOTS) await page.screenshot({ path: `${OUT}/step${String(n).padStart(2, "0")}-before.png` });
  const hit = (want) => flows.slice(seen).some((l) => l.includes(want));
  const take = (want) => (seen = flows.findIndex((l, i) => i >= seen && l.includes(want)) + 1);
  if (step.startsWith("flow=")) {
    const want = step.slice(5);
    await until(() => hit(want), +(env.FLOW_TIMEOUT || 180), step);
    take(want);
  } else if (step.includes("~flow=")) {
    const [key, want] = step.split("~flow=");
    for (const end = Date.now() + 1000 * +(env.FLOW_TIMEOUT || 180); !hit(want); ) {
      if (Date.now() > end) { await page.screenshot({ path: `${OUT}/stuck.png` }); throw new Error(`timed out: ${step}`); }
      await tap(key);
      for (let i = 0; i < 10 && !hit(want); i++) await sleep(GAP_MS / 10);
    }
    take(want);
  } else if (step.startsWith("+")) await waitFrames(+step.slice(1));
  else if (step.startsWith("shot=")) await page.screenshot({ path: `${OUT}/${step.slice(5)}.png` });
  else if (step.startsWith("ms=")) await sleep(+step.slice(3));
  else {
    const [key, n] = step.split("*");
    for (let i = 0; i < +(n || 1); i++) { await tap(key); await sleep(GAP_MS); }
  }
}
const secsToRace = (Date.now() - t0) / 1000;
console.log(`${secsToRace.toFixed(1).padStart(6)}s  in the race; measuring ${MEASURE} frames`);
await page.keyboard.down("ArrowUp"); // drive: the same throttle every run

// Optional CPU profile of every thread (raw CDP: the page plus its workers, the game's threads).
let prof = null;
if (env.PROFILE) {
  const ver = await (await fetch("http://127.0.0.1:9333/json/version")).json();
  const ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0;
  const pending = new Map(), sessions = [];
  ws.onmessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
    else if (m.method === "Target.attachedToTarget") sessions.push(m.params.sessionId);
  };
  const send = (method, params = {}, sessionId) => new Promise((r) => { pending.set(++id, (m) => r(m.result || m)); ws.send(JSON.stringify({ id, method, params, sessionId })); });
  const pageT = (await send("Target.getTargets")).targetInfos.find((t) => t.type === "page" && t.url.startsWith(URL.split("?")[0]));
  const ps = (await send("Target.attachToTarget", { targetId: pageT.targetId, flatten: true })).sessionId;
  await send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, ps);
  await sleep(500);
  const all = [ps, ...sessions];
  for (const s of all) { await send("Profiler.enable", {}, s); await send("Profiler.setSamplingInterval", { interval: 500 }, s); await send("Profiler.start", {}, s); }
  prof = { ws, send, all };
}
const start = frames.length;
await waitFrames(MEASURE);
const win = frames.slice(start, start + MEASURE);
if (prof) {
  fs.mkdirSync(`${OUT}/prof`, { recursive: true });
  for (const [i, s] of prof.all.entries()) {
    const r = await prof.send("Profiler.stop", {}, s);
    if (r.profile) fs.writeFileSync(`${OUT}/prof/t${i}.cpuprofile`, JSON.stringify(r.profile));
  }
  prof.ws.close();
}
await page.screenshot({ path: `${OUT}/race.png` });
fs.writeFileSync(`${OUT}/frames.json`, JSON.stringify(win));
fs.writeFileSync(`${OUT}/logs.txt`, logs.join("\n"));

// Frame time = time between two frames the game sealed (col 3), so it holds every thread's share of a frame.
const pct = (a, p) => { const s = [...a].sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(s.length * p))]; };
const mean = (a) => a.reduce((x, y) => x + y, 0) / a.length;
const pairs = win.slice(1).map((f, k) => ({ f, ms: (f[3] - win[k][3]) * 1000, period: Math.floor(f[3] / AB_PERIOD), same: Math.floor(win[k][3] / AB_PERIOD) === Math.floor(f[3] / AB_PERIOD) }));
function summary(ps) {
  const ft = ps.map((p) => p.ms), col = (i) => ps.map((p) => p.f[i]), p50 = pct(ft, 0.5);
  return { frames: ps.length, fps: +(1000 / mean(ft)).toFixed(1), ms_p50: +p50.toFixed(2), ms_p95: +pct(ft, 0.95).toFixed(2),
    ms_p99: +pct(ft, 0.99).toFixed(2), ms_max: +Math.max(...ft).toFixed(1), hitches: ft.filter((t) => t > 2 * p50 && t > 25).length,
    game_cpu: +mean(col(21)).toFixed(2), game_cpu_p95: +pct(col(21), 0.95).toFixed(2), gpu: +mean(col(6)).toFixed(2),
    yield: +mean(col(17)).toFixed(2), blocked: +mean(col(18)).toFixed(2) };
}
const res = { tag: TAG, url: URL, secs_to_race: +secsToRace.toFixed(0), all: summary(pairs) };
if (env.AB) { // odd periods of the sealing clock run the OLD code (kit/platform/ab_phase.h); pairs across a switch are left out
  res.OLD = summary(pairs.filter((p) => p.same && p.period & 1));
  res.NEW = summary(pairs.filter((p) => p.same && !(p.period & 1)));
}
fs.writeFileSync(`${OUT}/summary.json`, JSON.stringify(res, null, 1));
for (const k of ["all", "OLD", "NEW"].filter((k) => res[k])) console.log(k.padEnd(4), JSON.stringify(res[k]));
console.log(`out: ${OUT}`);

if (env.HOLD) {
  const http = await import("node:http");
  http.createServer(async (req, rs) => {
    const u = new globalThis.URL(req.url, "http://x"), g = (k) => u.searchParams.get(k);
    try {
      if (u.pathname === "/key") await tap(g("k"));
      else if (u.pathname === "/shot") { const f = `${OUT}/${g("n") || "shot"}.png`; await page.screenshot({ path: f }); return rs.end(f + "\n"); }
      else if (u.pathname === "/eval") return rs.end(JSON.stringify(await page.evaluate(g("js"))) + "\n");
      else if (u.pathname === "/logs") return rs.end(logs.filter((l) => l.includes(g("grep") || "")).slice(-Number(g("n") || 20)).join("\n") + "\n");
      else if (u.pathname === "/quit") { rs.end("bye\n"); await ctx.close(); process.exit(0); }
      rs.end(`ok frame ${frames.length}\n`);
    } catch (e) { rs.end("error " + e.message + "\n"); }
  }).listen(9334);
  console.log("holding on :9334");
} else await ctx.close();
