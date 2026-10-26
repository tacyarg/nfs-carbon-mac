import { test } from "node:test";
import assert from "node:assert/strict";
import { newKey, readKey, derive, randomName, PREFIX } from "../key.js";

test("a new key reads back as itself, in any case and with any separators", () => {
  const k = newKey();
  assert.match(k, /^nfsc(-[a-z]+){8}$/);
  assert.equal(readKey(k), k);
  assert.equal(readKey(k.toUpperCase().replaceAll("-", "  ")), k);
  assert.equal(readKey(k.slice(PREFIX.length + 1)), k, "the prefix is optional");
});

test("a key with a wrong word count or an unknown word is not a key", () => {
  const w = newKey().split("-").slice(1);
  assert.equal(readKey(w.slice(0, 7).join("-")), null);
  assert.equal(readKey([...w.slice(0, 7), "notaword"].join("-")), null);
});

test("derive gives a 30-hex login code, the same for the same key, another for another", async () => {
  const k = newKey(), a = await derive(k), b = await derive(k), c = await derive(newKey());
  assert.match(a.code, /^[0-9a-f]{30}$/);
  assert.equal(a.code, b.code);
  assert.notEqual(a.code, c.code);
  assert.equal(a.saveKey.length, 32);
  assert.equal(a.token.length, 32);
});

test("random names are valid online names", () => {
  for (let i = 0; i < 50; i++) assert.match(randomName(), /^Racer[0-9]{4}$/);
});
