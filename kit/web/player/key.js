// The player's key on nfscarbon.xyz: "nfsc-" and 8 of words.js's 1296 words (about 2^83), made here and kept in this
// browser. It is the only thing a player saves: one PBKDF2 run turns it into the key that seals their saves, the
// token the server keeps them under, and the login code that owns their name online
// (planning/specs/2026-10-26-quick-online-and-cloud-saves-design.md). The server never sees the key.
import { WORDS } from "./words.js";

export const PREFIX = "nfsc";
const N = 8, SALT = "nfscarbon.xyz key v1", ROUNDS = 100_000;
const SET = new Set(WORDS), enc = new TextEncoder();

export function newKey() {
  const words = [], limit = Math.floor(65536 / WORDS.length) * WORDS.length; // past it a value favours the first words
  while (words.length < N)
    for (const v of crypto.getRandomValues(new Uint16Array(N)))
      if (v < limit && words.length < N) words.push(WORDS[v % WORDS.length]);
  return [PREFIX, ...words].join("-");
}

/** a typed or pasted key, in any case and with any separators, the prefix optional; null when it is not one */
export function readKey(text) {
  const w = String(text).toLowerCase().split(/[^a-z]+/).filter(Boolean);
  if (w[0] === PREFIX) w.shift();
  return w.length === N && w.every((x) => SET.has(x)) ? [PREFIX, ...w].join("-") : null;
}

export async function derive(key) {
  const base = await crypto.subtle.importKey("raw", enc.encode(key), "PBKDF2", false, ["deriveBits"]);
  const bits = new Uint8Array(await crypto.subtle.deriveBits(
    { name: "PBKDF2", hash: "SHA-256", salt: enc.encode(SALT), iterations: ROUNDS }, base, 79 * 8));
  const code = [...bits.subarray(64, 79)].map((b) => b.toString(16).padStart(2, "0")).join("");
  return { saveKey: bits.slice(0, 32), token: bits.slice(32, 64), code };
}

export function randomName() {
  return "Racer" + String(crypto.getRandomValues(new Uint16Array(1))[0] % 10000).padStart(4, "0");
}
