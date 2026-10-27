#!/usr/bin/env node
// Records the blocks the game reads from Play to the title: loads a player page, waits for Play,
// clicks it, watches every /data-blocks/v2/ request (the page's own fetches and its workers', since
// Playwright's context-level "request" event covers both in Chrome) for the given number of
// seconds, and prints kit/web/player/boot.json's shape, deduped in first-seen order.
//
// Usage: node kit/tools/record-boot.mjs <page url> [seconds=45] > kit/web/player/boot.json
import { chromium } from "../../.tools/pw/node_modules/playwright-core/index.mjs";
import fs from "node:fs";

const url = process.argv[2];
if (!url) { console.error("usage: node kit/tools/record-boot.mjs <page url> [seconds=45]"); process.exit(1); }
const seconds = +(process.argv[3] || 45);

const profileDir = fs.mkdtempSync("/Users/tacyarg/.hermes/cache/scratch/record-boot-");
const ctx = await chromium.launchPersistentContext(profileDir, {
  executablePath: "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  headless: true,
  args: ["--enable-unsafe-webgpu", "--autoplay-policy=no-user-gesture-required"],
  viewport: { width: 1280, height: 720 },
});

const seen = new Map(); // "path|block" -> true, in first-seen order
ctx.on("request", (req) => {
  const u = new URL(req.url());
  if (!u.pathname.startsWith("/data-blocks/v2/")) return;
  const p = decodeURIComponent(u.pathname.slice("/data-blocks/v2/".length));
  const b = +u.searchParams.get("b");
  const key = `${p}|${b}`;
  if (!seen.has(key)) seen.set(key, [p, b]);
});

try {
  const page = ctx.pages()[0] ?? (await ctx.newPage());
  await page.goto(url);
  while (await page.evaluate(() => document.getElementById("play").disabled)) await page.waitForTimeout(200);
  await page.evaluate(() => document.getElementById("play").click());
  await page.waitForTimeout(seconds * 1000);
} finally {
  console.log(JSON.stringify({ v: 1, blocks: [...seen.values()] }));
  await ctx.close();
}
